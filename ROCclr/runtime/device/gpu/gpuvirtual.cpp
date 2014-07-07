//
// Copyright (c) 2008 Advanced Micro Devices, Inc. All rights reserved.
//

#include "platform/perfctr.hpp"
#include "platform/threadtrace.hpp"
#include "platform/kernel.hpp"
#include "platform/commandqueue.hpp"
#include "device/gpu/gpuconstbuf.hpp"
#include "device/gpu/gpuvirtual.hpp"
#include "device/gpu/gpukernel.hpp"
#include "device/gpu/gpuprogram.hpp"
#include "device/gpu/gpucounters.hpp"
#include "device/gpu/gputhreadtrace.hpp"
#include "device/gpu/gputimestamp.hpp"
#include "device/gpu/gpublit.hpp"
#include "newcore.h"
#include "sc-hsa/Interface/SCHSAInterface.h"
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <d3d10_1.h>
#include "amdocl/cl_d3d9_amd.hpp"
#include "amdocl/cl_d3d10_amd.hpp"
#include "amdocl/cl_d3d11_amd.hpp"
#endif // _WIN32

namespace gpu {

bool
VirtualGPU::MemoryDependency::create(size_t numMemObj)
{
    if (numMemObj > 0) {
        // Allocate the array of memory objects for dependency tracking
        memObjectsInQueue_ = new MemoryState[numMemObj];
        if (NULL == memObjectsInQueue_) {
            return false;
        }
        memset(memObjectsInQueue_, 0, sizeof(MemoryState) * numMemObj);
        maxMemObjectsInQueue_ = numMemObj;
    }

    return true;
}

void
VirtualGPU::MemoryDependency::validate(
    VirtualGPU&     gpu,
    const Memory*   memory,
    bool            readOnly)
{
    bool    flushL1Cache = false;

    if (maxMemObjectsInQueue_ == 0) {
        return;
    }

    uint64_t curStart = memory->hbOffset();
    uint64_t curEnd = curStart + memory->hbSize();

    // Loop through all memory objects in the queue and find dependency
    // @note don't include objects from the current kernel
    for (size_t j = 0; j < endMemObjectsInQueue_; ++j) {
        // Check if the queue already contains this mem object and
        // GPU operations aren't readonly
        uint64_t busyStart = memObjectsInQueue_[j].start_;
        uint64_t busyEnd = memObjectsInQueue_[j].end_;

        // Check if the start inside the busy region
        if ((((curStart >= busyStart) && (curStart < busyEnd)) ||
            // Check if the end inside the busy region
             ((curEnd > busyStart) && (curEnd <= busyEnd)) ||
            // Check if the start/end cover the busy region
             ((curStart <= busyStart) && (curEnd >= busyEnd))) &&
            // If the buys region was written or the current one is for write
            (!memObjectsInQueue_[j].readOnly_ || !readOnly)) {
            flushL1Cache = true;
            break;
        }
    }

    // Did we reach the limit?
    if (maxMemObjectsInQueue_ <= (numMemObjectsInQueue_ + 1)) {
        flushL1Cache = true;
    }

    if (flushL1Cache) {
        // Flush cache
        gpu.flushL1Cache();

        // Clear memory dependency state
        const static bool All = true;
        clear(!All);
    }

    // Insert current memory object into the queue always,
    // since runtime calls flush before kernel execution and it has to keep
    // current kernel in tracking
    memObjectsInQueue_
        [numMemObjectsInQueue_].start_ = curStart;
    memObjectsInQueue_
        [numMemObjectsInQueue_].end_ = curEnd;
    memObjectsInQueue_
        [numMemObjectsInQueue_].readOnly_ = readOnly;
    numMemObjectsInQueue_++;
}

void
VirtualGPU::MemoryDependency::clear(bool all)
{
    if (numMemObjectsInQueue_ > 0) {
        size_t  i, j;
        if (all) {
            endMemObjectsInQueue_ = numMemObjectsInQueue_;
        }

        // Preserve all objects from the current kernel
        for (i = 0, j = endMemObjectsInQueue_; j < numMemObjectsInQueue_; i++, j++) {
            memObjectsInQueue_[i].start_ = memObjectsInQueue_[j].start_;
            memObjectsInQueue_[i].end_ = memObjectsInQueue_[j].end_;
            memObjectsInQueue_[i].readOnly_ = memObjectsInQueue_[j].readOnly_;
        }
        // Clear all objects except current kernel
        memset(&memObjectsInQueue_[i], 0, sizeof(amd::Memory*) * numMemObjectsInQueue_);
        numMemObjectsInQueue_ -= endMemObjectsInQueue_;
        endMemObjectsInQueue_ = 0;
    }
}

VirtualGPU::DmaFlushMgmt::DmaFlushMgmt(const Device& dev)
    : cbWorkload_(0)
    , dispatchSplitSize_(0)
{
    aluCnt_ = dev.info().simdPerCU_ * dev.info().simdWidth_ * dev.info().maxComputeUnits_;
    maxDispatchWorkload_ = static_cast<uint64_t>(dev.info().maxClockFrequency_) *
        // find time in us
        100 * dev.settings().maxWorkloadTime_ *
        aluCnt_;
    resetCbWorkload(dev);
}

void
VirtualGPU::DmaFlushMgmt::resetCbWorkload(const Device& dev)
{
    cbWorkload_ = 0;
    maxCbWorkload_ = static_cast<uint64_t>(dev.info().maxClockFrequency_) *
        // find time in us
        100 * dev.settings().minWorkloadTime_ * aluCnt_;
}

void
VirtualGPU::DmaFlushMgmt::findSplitSize(
    const Device& dev, uint64_t threads, uint instructions)
{
    uint64_t workload = threads * instructions;
    if (maxDispatchWorkload_ < workload) {
        dispatchSplitSize_ = static_cast<uint>(maxDispatchWorkload_ / instructions);
        uint    fullLoad = dev.info().maxComputeUnits_ * dev.info().maxWorkGroupSize_;
        if ((dispatchSplitSize_ % fullLoad) != 0) {
            dispatchSplitSize_ = (dispatchSplitSize_ / fullLoad + 1) * fullLoad;
        }
    }
    else {
        dispatchSplitSize_ = (threads > dev.settings().workloadSplitSize_) ?
            dev.settings().workloadSplitSize_ : 0;
    }
}

bool
VirtualGPU::DmaFlushMgmt::isCbReady(
    VirtualGPU& gpu, uint64_t threads, uint instructions)
{
    bool    cbReady = false;
    uint64_t workload = amd::alignUp(threads, 4 * aluCnt_) * instructions;
    // Add current workload to the overall workload in the current DMA
    cbWorkload_ += workload;
    // Did it exceed maximum?
    if (cbWorkload_ > maxCbWorkload_) {
        // Reset DMA workload
        cbWorkload_ = 0;
        // Increase workload of the next DMA buffer by 50%
        maxCbWorkload_ = maxCbWorkload_ * 3 / 2;
        if (maxCbWorkload_ > maxDispatchWorkload_) {
            maxCbWorkload_ = maxDispatchWorkload_;
        }
        cbReady = true;
    }
    return cbReady;
}

bool
VirtualGPU::gslOpen(uint nEngines, gslEngineDescriptor *engines)
{
    // GSL device initialization
    dev().PerformFullInitialization();

    // Wait the event
    m_waitType = dev().settings().syncObject_
        ? CAL_WAIT_LOW_CPU_UTILIZATION
        : CAL_WAIT_POLLING;

    if (!open(&dev(), nEngines, engines)) {
        return false;
    }

    return true;
}

void
VirtualGPU::gslDestroy()
{
    closeVideoSession();
    close(dev().getNative());
}

void
VirtualGPU::addXferWrite(Resource& resource)
{
    if (xferWriteBuffers_.size() > 7) {
        dev().xferWrite().release(*this, *xferWriteBuffers_.front());
        xferWriteBuffers_.pop_front();
    }

    // Delay destruction
    xferWriteBuffers_.push_back(&resource);
}

void
VirtualGPU::releaseXferWrite()
{
    for (std::list<Resource*>::iterator it = xferWriteBuffers_.begin();
            it != xferWriteBuffers_.end(); ++it) {
        dev().xferWrite().release(*this, *(*it));
    }
    xferWriteBuffers_.clear();
}

void
VirtualGPU::addPinnedMem(amd::Memory* mem)
{
    if (pinnedMems_.size() > 7) {
        pinnedMems_.front()->release();
        pinnedMems_.pop_front();
    }

    // Start operation, since we should release mem object
    flushDMA(getGpuEvent(dev().getGpuMemory(mem))->engineId_);

    // Delay destruction
    pinnedMems_.push_back(mem);
}

void
VirtualGPU::releasePinnedMem()
{
    for (std::list<amd::Memory*>::iterator it = pinnedMems_.begin();
            it != pinnedMems_.end(); ++it) {
        (*it)->release();
    }
    pinnedMems_.clear();
}

bool
VirtualGPU::createVirtualQueue(uint deviceQueueSize)
{
    uint    numSlots = deviceQueueSize / sizeof(AmdAqlWrap);
    uint    allocSize = deviceQueueSize;

    // Add the virtual queue header
    allocSize += sizeof(AmdVQueueHeader);
    allocSize = amd::alignUp(allocSize, 128);

    uint    argOffs = allocSize;

    // Add the kernel arguments and wait events
    uint singleArgSize = amd::alignUp(dev().info().maxParameterSize_ + 64 +
        dev().settings().numWaitEvents_ * sizeof(uint64_t), 128);
    allocSize += singleArgSize * numSlots;

    uint    eventsOffs = allocSize;
    // Add the device events
    allocSize += dev().settings().numDeviceEvents_ * sizeof(AmdEvent);

    uint    eventMaskOffs = allocSize;
    // Add mask array for events
    allocSize += amd::alignUp(dev().settings().numDeviceEvents_, 32) / 8;

    uint    slotMaskOffs = allocSize;
    // Add mask array for AmdAqlWrap slots
    allocSize += amd::alignUp(numSlots, 32) / 8;

    virtualQueue_ = new Memory(dev(), allocSize);
    Resource::MemoryType type = (GPU_PRINT_CHILD_KERNEL == 0) ?
        Resource::Local : Resource::Remote;
    if  ((virtualQueue_ == NULL) || !virtualQueue_->create(type)) {
        return false;
    }
    address ptr  = reinterpret_cast<address>(
        virtualQueue_->map(this, Resource::WriteOnly));
    if (NULL == ptr) {
        return false;
    }
    // Clear memory
    memset(ptr, 0, allocSize);
    uint64_t    vaBase = virtualQueue_->vmAddress();
    AmdVQueueHeader* header = reinterpret_cast<AmdVQueueHeader*>(ptr);

    // Initialize the virtual queue header
    header->aql_slot_num    = numSlots;
    header->event_slot_num  = dev().settings().numDeviceEvents_;
    header->event_slot_mask = vaBase + eventMaskOffs;
    header->event_slots     = vaBase + eventsOffs;
    header->aql_slot_mask   = vaBase + slotMaskOffs;
    header->wait_size       = dev().settings().numWaitEvents_;
    header->arg_size        = dev().info().maxParameterSize_ + 64;
    vqHeader_ = new AmdVQueueHeader;
    if (NULL == vqHeader_) {
        return false;
    }
    *vqHeader_ = *header;

    // Go over all slots and perform initialization
    AmdAqlWrap* slots = reinterpret_cast<AmdAqlWrap*>(&header[1]);
    for (uint i = 0; i < numSlots; ++i) {
        uint64_t argStart = vaBase + argOffs + i * singleArgSize;
        slots[i].aql.kernel_arg_address = argStart;
        slots[i].wait_list = argStart + dev().info().maxParameterSize_ + 64;
    }
    // Upload data back to local memory
    if (GPU_PRINT_CHILD_KERNEL == 0) {
        virtualQueue_->unmap(this);
    }

    schedParams_ = new Memory(dev(), 64 * Ki);
    if ((schedParams_ == NULL) || !schedParams_->create(Resource::RemoteUSWC)) {
        return false;
    }

    ptr  = reinterpret_cast<address>(schedParams_->map(this));

    return true;
}

VirtualGPU::VirtualGPU(
    Device&  device)
    : device::VirtualDevice(device)
    , CALGSLContext()
    , engineID_(MainEngine)
    , activeKernelDesc_(NULL)
    , gpuDevice_(static_cast<Device&>(device))
    , execution_("Virtual GPU execution lock", true)
    , printfDbg_(NULL)
    , printfDbgHSA_(NULL)
    , tsCache_(NULL)
    , vmMems_(NULL)
    , numVmMems_(0)
    , dmaFlushMgmt_(device)
    , numGrpCb_(NULL)
    , scratchRegNum_(0)
    , hwRing_(0)
    , readjustTimeGPU_(0)
    , currTs_(NULL)
    , vqHeader_(NULL)
    , virtualQueue_(NULL)
    , schedParams_(NULL)
    , schedParamIdx_(0)
    , hsaQueueMem_(NULL)
{
    memset(&cal_, 0, sizeof(CalVirtualDesc));
    for (uint i = 0; i < AllEngines; ++i) {
        cal_.events_[i].invalidate();
    }
    memset(&cal_.samplersState_, 0xff, sizeof(cal_.samplersState_));

    // Note: Virtual GPU device creation must be a thread safe operation
    index_ = gpuDevice_.numOfVgpus_++;
    gpuDevice_.vgpus_.resize(gpuDevice_.numOfVgpus());
    gpuDevice_.vgpus_[index()] = this;
}

bool
VirtualGPU::create(
    bool    profiling
#if cl_amd_open_video
    , void* calVideoProperties
#endif // cl_amd_open_video
    , uint  deviceQueueSize
    )
{
    device::BlitManager::Setup  blitSetup;
    gslEngineDescriptor engines[2];
    uint    engineMask = 0;
    uint32_t    num = 0;

    if (index() >= GPU_MAX_COMMAND_QUEUES) {
        // Cap the maximum number of concurrent Virtual GPUs.
        return false;
    }

    // Virtual GPU will have profiling enabled
    state_.profiling_ = profiling;

#if cl_amd_open_video
    if(calVideoProperties) {
        cl_video_encode_desc_amd* ptr_ovSessionProperties  =
            reinterpret_cast<cl_video_encode_desc_amd *>(calVideoProperties);
        CALvideoProperties* ptr_calVideoProperties =
            reinterpret_cast<CALvideoProperties *>(ptr_ovSessionProperties->calVideoProperties);

        switch (ptr_calVideoProperties->VideoEngine_name) {
        case CAL_CONTEXT_VIDEO:
            engineMask = dev().engines().getMask(GSL_ENGINEID_UVD);
            num = dev().engines().getRequested(engineMask, engines);
            // Open GSL context
            if ((num == 0) || !gslOpen(num, engines)) {
                return false;
            }
            openVideoSession(*ptr_calVideoProperties);
            break;
        case CAL_CONTEXT_VIDEO_VCE:
            engineMask = dev().engines().getMask(GSL_ENGINEID_VCE);
            num = dev().engines().getRequested(engineMask, engines);
            // Open GSL context
            if ((num == 0) || !gslOpen(num, engines)) {
                return false;
            }
            break;
        default:
            assert(false && "Unknown video engine!");
            break;
        }
        if (ptr_calVideoProperties->VideoEngine_name == CAL_CONTEXT_VIDEO_VCE) {
            CALEncodeCreateVCE encodeVCE;
            createVCE(&encodeVCE, 0);

            CAL_VID_PROFILE_LEVEL encode_profile_level;
            encode_profile_level.profile = ptr_ovSessionProperties->attrib.profile;
            encode_profile_level.level = ptr_ovSessionProperties->attrib.level;
            createEncodeSession(
                0,
                (CALencodeMode)ptr_ovSessionProperties->encodeMode,//CAL_VID_encode_AVC_FULL
                encode_profile_level,
                (CAL_VID_PICTURE_FORMAT)ptr_ovSessionProperties->attrib.format, //CAL_VID_PICTURE_NV12
                ptr_ovSessionProperties->image_width,
                ptr_ovSessionProperties->image_height,
                ptr_ovSessionProperties->frameRateNumerator,
                ptr_ovSessionProperties->frameRateDenominator,
                (CAL_VID_ENCODE_JOB_PRIORITY)ptr_ovSessionProperties->priority); //CAL_VID_ENCODE_JOB_PRIORITY_LEVEL1
        }
    }
    else
#endif // !cl_amd_open_video
    {
        if (dev().engines().numComputeRings()) {
            //!@note: Add 1 to account the device queue for transfers
            uint idx = (index() + 1) % dev().engines().numComputeRings();

            // hwRing_ should be set 0 if forced to have single scratch buffer
            hwRing_ = (dev().settings().useSingleScratch_) ? 0 : idx;

            engineMask = dev().engines().getMask((gslEngineID)(GSL_ENGINEID_COMPUTE0 + idx));
            if (dev().canDMA()) {
                if (idx & 0x1) {
                    engineMask |= dev().engines().getMask(GSL_ENGINEID_DRMDMA1);
                }
                else {
                    engineMask |= dev().engines().getMask(GSL_ENGINEID_DRMDMA0);
                }
            }
        }
        else {
            engineMask = dev().engines().getMask(GSL_ENGINEID_3DCOMPUTE0);
            if (dev().canDMA()) {
                engineMask |= dev().engines().getMask(GSL_ENGINEID_DRMDMA0);
            }
        }
        num = dev().engines().getRequested(engineMask, engines);

        // Open GSL context
        if ((num == 0) || !gslOpen(num, engines)) {
            return false;
        }
    }

    // Diable double copy optimization,
    // since UAV read from nonlocal is fast enough
    blitSetup.disableCopyBufferToImageOpt_ = true;
    if (!allocConstantBuffers()) {
        return false;
    }

    // Create Printf class
    printfDbg_ = new PrintfDbg(gpuDevice_);
    if ((NULL == printfDbg_) || !printfDbg_->create()) {
        delete printfDbg_;
        LogError("Could not allocate debug buffer for printf()!");
        return false;
    }

    // Create HSAILPrintf class
    printfDbgHSA_ = new PrintfDbgHSA(gpuDevice_);
    if (NULL == printfDbgHSA_) {
        delete printfDbgHSA_;
        LogError("Could not create PrintfDbgHSA class!");
        return false;
    }

    // Choose the appropriate class for blit engine
    switch (dev().settings().blitEngine_) {
        default:
            // Fall through ...
        case Settings::BlitEngineHost:
            blitSetup.disableAll();
            // Fall through ...
        case Settings::BlitEngineCAL:
        case Settings::BlitEngineKernel:
            if (!dev().heap()->isVirtual()) {
                blitSetup.disableReadBufferRect_    = true;
                blitSetup.disableWriteBufferRect_   = true;
            }
            blitMgr_ = new KernelBlitManager(*this, blitSetup);
            break;
    }
    if ((NULL == blitMgr_) || !blitMgr_->create(gpuDevice_)) {
        LogError("Could not create BlitManager!");
        return false;
    }

    tsCache_ = new TimeStampCache(*this);
    if (NULL == tsCache_) {
        LogError("Could not create TimeStamp cache!");
        return false;
    }

    if (!memoryDependency().create(dev().settings().numMemDependencies_)) {
        LogError("Could not create the array of memory objects!");
        return false;
    }

    if(!allocHsaQueueMem()) {
        LogError("Could not create hsaQueueMem object!");
        return false;
    }

    //! @todo for testing only
    //deviceQueueSize = (deviceQueueSize == 0) ? (128 * Ki) : deviceQueueSize;
    // Check if the app requested a device queue creation
    if ((0 != deviceQueueSize) && !createVirtualQueue(deviceQueueSize)) {
        LogError("Could not create a virtual queue!");
        return false;
    }


    return true;
}

bool
VirtualGPU::allocHsaQueueMem()
{
    amd_queue_t queue = {0};
    hsaQueueMem_ = new gpu::Memory(dev(), sizeof(queue));
    if (hsaQueueMem_ == NULL) {
        return false;
    }
    if (!hsaQueueMem_->create(gpu::Resource::Local)) {
        delete hsaQueueMem_;
        return false;
    }
    void* cpuPtr = hsaQueueMem_->map(NULL, gpu::Resource::WriteOnly);
    queue.private_segment_aperture_base_hi =
        static_cast<uint32>(dev().gslCtx()->getPrivateApertureBase()>>32);
    queue.group_segment_aperture_base_hi =
        static_cast<uint32>(dev().gslCtx()->getSharedApertureBase()>>32);
    memcpy(cpuPtr, &queue, sizeof(queue));
    hsaQueueMem_->unmap(NULL);
    return true;
}

VirtualGPU::~VirtualGPU()
{
    // Not safe to remove a queue. So lock the device
    amd::ScopedLock k(dev().lockAsyncOps());
    amd::ScopedLock lock(dev().vgpusAccess());

    uint    i;
    // Destroy all kernels
    for (GslKernels::const_iterator it = gslKernels_.begin();
            it != gslKernels_.end(); ++it) {
        if (it->first != 0) {
            freeKernelDesc(it->second);
        }
    }
    gslKernels_.clear();

    // Destroy all memories
    releaseMemObjects();

    // Destroy printf object
    delete printfDbg_;

    // Destroy printfHSA object
    delete printfDbgHSA_;

    // Destroy BlitManager object
    delete blitMgr_;

    // Destroy TimeStamp cache
    delete tsCache_;

    // Destroy resource list with the constant buffers
    for (i = 0; i < constBufs_.size(); ++i) {
        delete constBufs_[i];
    }
    delete numGrpCb_;

    gslDestroy();

    gpuDevice_.numOfVgpus_--;
    gpuDevice_.vgpus_.erase(gpuDevice_.vgpus_.begin() + index());
    for (uint idx = index(); idx < dev().vgpus().size(); ++idx) {
        dev().vgpus()[idx]->index_--;
    }

    // Release scratch buffer memory to reduce memory pressure
    //!@note OCLtst uses single device with multiple tests
    //! Release memory only if it's the last command queue.
    //! The first queue is reserved for the transfers on device
    if ((scratchRegNum_ > 0) && (gpuDevice_.numOfVgpus_ <= 1)) {
        gpuDevice_.destroyScratchBuffers();
    }

    delete [] vmMems_;
    //! @todo Temporarily keep the buffer mapped for debug purpose
    if (NULL != schedParams_) {
        schedParams_->unmap(this);
    }
    delete vqHeader_;
    delete virtualQueue_;
    delete schedParams_;
    delete hsaQueueMem_;
}

void
VirtualGPU::submitReadMemory(amd::ReadMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    // Translate memory references and ensure cache up-to-date
    gpu::Memory* memory = dev().getGpuMemory(&vcmd.source());

    size_t offset = 0;
    // Find if virtual address is a CL allocation
    gpu::Memory* hostMemory = dev().findMemoryFromVA(vcmd.destination(), &offset);

    profilingBegin(vcmd, true);

    memory->syncCacheFromHost(*this);
    cl_command_type type = vcmd.type();
    bool result = false;
    amd::Memory* bufferFromImage = NULL;

    // Force buffer read for IMAGE1D_BUFFER
    if ((type == CL_COMMAND_READ_IMAGE) &&
        (vcmd.source().getType() == CL_MEM_OBJECT_IMAGE1D_BUFFER)) {
        bufferFromImage = createBufferFromImage(vcmd.source());
        if (NULL == bufferFromImage) {
            LogError("We should not fail buffer creation from image_buffer!");
        }
        else {
            type = CL_COMMAND_READ_BUFFER;
            bufferFromImage->setVirtualDevice(this);
            memory = dev().getGpuMemory(bufferFromImage);
        }
    }

    // Process different write commands
    switch (type) {
    case CL_COMMAND_READ_BUFFER: {
        amd::Coord3D    origin(vcmd.origin()[0]);
        amd::Coord3D    size(vcmd.size()[0]);
        if (NULL != bufferFromImage) {
            size_t  elemSize =
                vcmd.source().asImage()->getImageFormat().getElementSize();
            origin.c[0] *= elemSize;
            size.c[0]   *= elemSize;
        }
        if (hostMemory != NULL) {
            // Accelerated transfer without pinning
            amd::Coord3D dstOrigin(offset);
            result = blitMgr().copyBuffer(*memory, *hostMemory,
                origin, dstOrigin, size, vcmd.isEntireMemory());
        }
        else {
            result = blitMgr().readBuffer(
                *memory, vcmd.destination(),
                origin, size, vcmd.isEntireMemory());
        }
        if (NULL != bufferFromImage) {
            bufferFromImage->release();
        }
    }
        break;
    case CL_COMMAND_READ_BUFFER_RECT:
        // Runtime can't use accelerated copy if offset isn't 0 for the rect copy
        if ((hostMemory != NULL) && (offset == 0)) {
            result = blitMgr().copyBufferRect(*memory, *hostMemory,
                vcmd.bufRect(), vcmd.hostRect(), vcmd.size(),
                vcmd.isEntireMemory());
        }
        else {
            result = blitMgr().readBufferRect(*memory,
                vcmd.destination(), vcmd.bufRect(), vcmd.hostRect(), vcmd.size(),
                vcmd.isEntireMemory());
        }
        break;
    case CL_COMMAND_READ_IMAGE:
        if (hostMemory != NULL) {
            // Accelerated image to buffer transfer without pinning
            amd::Coord3D dstOrigin(offset);
            result = blitMgr().copyImageToBuffer(*memory, *hostMemory,
                vcmd.origin(), dstOrigin, vcmd.size(),
                vcmd.isEntireMemory());
        }
        else {
            result = blitMgr().readImage(*memory, vcmd.destination(),
                vcmd.origin(), vcmd.size(), vcmd.rowPitch(), vcmd.slicePitch(),
                vcmd.isEntireMemory());
        }
        break;
    default:
        LogError("Unsupported type for the read command");
        break;
    }

    if (!result) {
        LogError("submitReadMemory failed!");
        vcmd.setStatus(CL_INVALID_OPERATION);
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitWriteMemory(amd::WriteMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    // Translate memory references and ensure cache up to date
    gpu::Memory* memory = dev().getGpuMemory(&vcmd.destination());
    size_t offset = 0;
    // Find if virtual address is a CL allocation
    gpu::Memory* hostMemory = dev().findMemoryFromVA(vcmd.source(), &offset);

    profilingBegin(vcmd, true);

    bool    entire  = vcmd.isEntireMemory();

    // Synchronize memory from host if necessary
    device::Memory::SyncFlags syncFlags;
    syncFlags.skipEntire_ = entire;
    memory->syncCacheFromHost(*this, syncFlags);

    cl_command_type type = vcmd.type();
    bool result = false;
    amd::Memory* bufferFromImage = NULL;

    // Force buffer write for IMAGE1D_BUFFER
    if ((type == CL_COMMAND_WRITE_IMAGE) &&
        (vcmd.destination().getType() == CL_MEM_OBJECT_IMAGE1D_BUFFER)) {
        bufferFromImage = createBufferFromImage(vcmd.destination());
        if (NULL == bufferFromImage) {
            LogError("We should not fail buffer creation from image_buffer!");
        }
        else {
            type = CL_COMMAND_WRITE_BUFFER;
            bufferFromImage->setVirtualDevice(this);
            memory = dev().getGpuMemory(bufferFromImage);
        }
    }

    // Process different write commands
    switch (type) {
    case CL_COMMAND_WRITE_BUFFER: {
        amd::Coord3D    origin(vcmd.origin()[0]);
        amd::Coord3D    size(vcmd.size()[0]);
        if (NULL != bufferFromImage) {
            size_t  elemSize =
                vcmd.destination().asImage()->getImageFormat().getElementSize();
            origin.c[0] *= elemSize;
            size.c[0]   *= elemSize;
        }
        if (hostMemory != NULL) {
            // Accelerated transfer without pinning
            amd::Coord3D srcOrigin(offset);
            result = blitMgr().copyBuffer(*hostMemory, *memory,
                srcOrigin, origin, size, vcmd.isEntireMemory());
        }
        else {
            result = blitMgr().writeBuffer(vcmd.source(), *memory,
                origin, size, vcmd.isEntireMemory());
        }
        if (NULL != bufferFromImage) {
            bufferFromImage->release();
        }
    }
        break;
    case CL_COMMAND_WRITE_BUFFER_RECT:
        // Runtime can't use accelerated copy if offset isn't 0 for the rect copy
        if ((hostMemory != NULL) && (offset == 0)) {
            result = blitMgr().copyBufferRect(*hostMemory, *memory,
                vcmd.hostRect(), vcmd.bufRect(), vcmd.size(),
                vcmd.isEntireMemory());
        }
        else {
            result = blitMgr().writeBufferRect(vcmd.source(), *memory,
                vcmd.hostRect(), vcmd.bufRect(), vcmd.size(),
                vcmd.isEntireMemory());
        }
        break;
    case CL_COMMAND_WRITE_IMAGE:
        if (hostMemory != NULL) {
            // Accelerated buffer to image transfer without pinning
            amd::Coord3D srcOrigin(offset);
            result = blitMgr().copyBufferToImage(*hostMemory, *memory,
                srcOrigin, vcmd.origin(), vcmd.size(),
                vcmd.isEntireMemory());
        }
        else {
            result = blitMgr().writeImage(vcmd.source(), *memory,
                vcmd.origin(), vcmd.size(), vcmd.rowPitch(), vcmd.slicePitch(),
                vcmd.isEntireMemory());
        }
        break;
    default:
        LogError("Unsupported type for the write command");
        break;
    }

    if (!result) {
        LogError("submitWriteMemory failed!");
        vcmd.setStatus(CL_INVALID_OPERATION);
    }
    else {
        // Mark this as the most-recently written cache of the destination
        vcmd.destination().signalWrite(&gpuDevice_);
    }
    profilingEnd(vcmd);
}

bool
VirtualGPU::copyMemory(cl_command_type type
            , amd::Memory& srcMem
            , amd::Memory& dstMem
            , bool entire
            , const amd::Coord3D& srcOrigin
            , const amd::Coord3D& dstOrigin
            , const amd::Coord3D& size
            , const amd::BufferRect& srcRect
            , const amd::BufferRect& dstRect
            )
{
    // Translate memory references and ensure cache up-to-date
    gpu::Memory* dstMemory = dev().getGpuMemory(&dstMem);
    gpu::Memory* srcMemory = dev().getGpuMemory(&srcMem);

    // Synchronize source and destination memory
    device::Memory::SyncFlags syncFlags;
    syncFlags.skipEntire_ = entire;
    dstMemory->syncCacheFromHost(*this, syncFlags);
    srcMemory->syncCacheFromHost(*this);

    amd::Memory* bufferFromImageSrc = NULL;
    amd::Memory* bufferFromImageDst = NULL;

    // Force buffer read for IMAGE1D_BUFFER
    if ((srcMem.getType() == CL_MEM_OBJECT_IMAGE1D_BUFFER)) {
        bufferFromImageSrc = createBufferFromImage(srcMem);
        if (NULL == bufferFromImageSrc) {
            LogError("We should not fail buffer creation from image_buffer!");
        }
        else {
            type = CL_COMMAND_COPY_BUFFER;
            bufferFromImageSrc->setVirtualDevice(this);
            srcMemory = dev().getGpuMemory(bufferFromImageSrc);
       }
    }
    // Force buffer write for IMAGE1D_BUFFER
    if ((dstMem.getType() == CL_MEM_OBJECT_IMAGE1D_BUFFER)) {
        bufferFromImageDst = createBufferFromImage(dstMem);
        if (NULL == bufferFromImageDst) {
            LogError("We should not fail buffer creation from image_buffer!");
        }
        else {
            type = CL_COMMAND_COPY_BUFFER;
            bufferFromImageDst->setVirtualDevice(this);
            dstMemory = dev().getGpuMemory(bufferFromImageDst);
        }
    }

    bool result = false;

    // Check if HW can be used for memory copy
    switch (type) {
    case CL_COMMAND_SVM_MEMCPY:
    case CL_COMMAND_COPY_BUFFER: {
        amd::Coord3D    realSrcOrigin(srcOrigin[0]);
        amd::Coord3D    realDstOrigin(dstOrigin[0]);
        amd::Coord3D    realSize(size.c[0],size.c[1],size.c[2]);

        if (NULL != bufferFromImageSrc) {
            size_t  elemSize =
                srcMem.asImage()->getImageFormat().getElementSize();
            realSrcOrigin.c[0] *= elemSize;
            if (NULL != bufferFromImageDst) {
                realDstOrigin.c[0] *= elemSize;
            }
            realSize.c[0] *= elemSize;
        }
        else if (NULL != bufferFromImageDst) {
            size_t  elemSize =
                dstMem.asImage()->getImageFormat().getElementSize();
            realDstOrigin.c[0] *= elemSize;
            realSize.c[0]   *= elemSize;
        }

        result = blitMgr().copyBuffer(*srcMemory, *dstMemory,
            realSrcOrigin, realDstOrigin, realSize, entire);

        if (NULL != bufferFromImageSrc) {
            bufferFromImageSrc->release();
        }
        if (NULL != bufferFromImageDst) {
            bufferFromImageDst->release();
        }
    }
        break;
    case CL_COMMAND_COPY_BUFFER_RECT:
        result = blitMgr().copyBufferRect(*srcMemory, *dstMemory,
            srcRect, dstRect, size, entire);
        break;
    case CL_COMMAND_COPY_IMAGE_TO_BUFFER:
        result = blitMgr().copyImageToBuffer(*srcMemory, *dstMemory,
            srcOrigin, dstOrigin, size, entire);
        break;
    case CL_COMMAND_COPY_BUFFER_TO_IMAGE:
        result = blitMgr().copyBufferToImage(*srcMemory, *dstMemory,
            srcOrigin, dstOrigin, size, entire);
        break;
    case CL_COMMAND_COPY_IMAGE:
        result = blitMgr().copyImage(*srcMemory, *dstMemory,
            srcOrigin, dstOrigin, size, entire);
        break;
    default:
        LogError("Unsupported command type for memory copy!");
        break;
    }

    if (!result) {
        LogError("submitCopyMemory failed!");
        return false;
    }
    else {
        // Mark this as the most-recently written cache of the destination
        dstMem.signalWrite(&gpuDevice_);
    }
    return true;
}

void
VirtualGPU::submitCopyMemory(amd::CopyMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd);

    cl_command_type type = vcmd.type();
    bool entire  = vcmd.isEntireMemory();

    if (!copyMemory(type, vcmd.source(), vcmd.destination(), entire,
            vcmd.srcOrigin(), vcmd.dstOrigin(), vcmd.size(), vcmd.srcRect(),
            vcmd.dstRect())) {
        vcmd.setStatus(CL_INVALID_OPERATION);
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitSvmCopyMemory(amd::SvmCopyMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());
    profilingBegin(vcmd);

    cl_command_type type = vcmd.type();
    amd::Memory* srcMem = amd::SvmManager::FindSvmBuffer(vcmd.src());
    amd::Memory* dstMem = amd::SvmManager::FindSvmBuffer(vcmd.dst());
    if (NULL == srcMem || NULL == dstMem) {
        vcmd.setStatus(CL_INVALID_OPERATION);
        return;
    }

    amd::Coord3D srcOrigin(0, 0, 0);
    amd::Coord3D dstOrigin(0, 0, 0);
    amd::Coord3D size(vcmd.srcSize(), 1, 1);
    amd::BufferRect srcRect;
    amd::BufferRect dstRect;

    srcOrigin.c[0] = static_cast<const_address>(vcmd.src()) - static_cast<address>(srcMem->getSvmPtr());
    dstOrigin.c[0] = static_cast<const_address>(vcmd.dst()) - static_cast<address>(dstMem->getSvmPtr());

    if (!(srcMem->validateRegion(srcOrigin, size)) || !(dstMem->validateRegion(dstOrigin, size))) {
        vcmd.setStatus(CL_INVALID_OPERATION);
        return;
    }

    bool entire  = srcMem->isEntirelyCovered(srcOrigin, size) &&
                   dstMem->isEntirelyCovered(dstOrigin, size);

    if (!copyMemory(type, *srcMem, *dstMem, entire,
        srcOrigin, dstOrigin, size, srcRect, dstRect)) {
        vcmd.setStatus(CL_INVALID_OPERATION);
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitMapMemory(amd::MapMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd, true);

    gpu::Memory* memory = dev().getGpuMemory(&vcmd.memory());

    // Save write map info for unmap copy
    if (vcmd.mapFlags() & (CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION)) {
        memory->saveWriteMapInfo(vcmd.origin(),
            vcmd.size(), vcmd.isEntireMemory());
    }

    // If we have host memory, use it
    if (memory->owner()->getHostMem() != NULL) {
        if (!memory->isHostMemDirectAccess()) {
            // Make sure GPU finished operation before
            // synchronization with the backing store
            memory->wait(*this);
        }

        // Target is the backing store, so just ensure that owner is up-to-date
        memory->owner()->cacheWriteBack();

        // Add memory to VA cache, so rutnime can detect direct access to VA
        dev().addVACache(memory);
    }
    else if (memory->isPersistentDirectMap()) {
        // Nothing to do here
    }
    else if (memory->mapMemory() != NULL) {
        // Target is a remote resource, so copy
        assert(memory->mapMemory() != NULL);
        if (vcmd.mapFlags() & (CL_MAP_READ | CL_MAP_WRITE)) {
            amd::Coord3D dstOrigin(0, 0, 0);
            if (memory->cal()->buffer_) {
                if (!blitMgr().copyBuffer(*memory,
                    *memory->mapMemory(), vcmd.origin(), dstOrigin,
                    vcmd.size(), vcmd.isEntireMemory())) {
                    LogError("submitMapMemory() - copy failed");
                    vcmd.setStatus(CL_MAP_FAILURE);
                }
            }
            else if ((vcmd.memory().getType() == CL_MEM_OBJECT_IMAGE1D_BUFFER)) {
                amd::Memory* bufferFromImage = NULL;
                Memory* memoryBuf = memory;
                amd::Coord3D    origin(vcmd.origin()[0]);
                amd::Coord3D    size(vcmd.size()[0]);
                size_t  elemSize =
                    vcmd.memory().asImage()->getImageFormat().getElementSize();
                origin.c[0] *= elemSize;
                size.c[0]   *= elemSize;

                bufferFromImage = createBufferFromImage(vcmd.memory());
                if (NULL == bufferFromImage) {
                    LogError("We should not fail buffer creation from image_buffer!");
                }
                else {
                    bufferFromImage->setVirtualDevice(this);
                    memoryBuf = dev().getGpuMemory(bufferFromImage);
                }
                if (!blitMgr().copyBuffer(*memoryBuf,
                    *memory->mapMemory(), origin, dstOrigin,
                    size, vcmd.isEntireMemory())) {
                    LogError("submitMapMemory() - copy failed");
                    vcmd.setStatus(CL_MAP_FAILURE);
                }
                if (NULL != bufferFromImage) {
                    bufferFromImage->release();
                }
            }
            else {
                if (!blitMgr().copyImageToBuffer(*memory,
                    *memory->mapMemory(), vcmd.origin(), dstOrigin,
                    vcmd.size(), vcmd.isEntireMemory())) {
                    LogError("submitMapMemory() - copy failed");
                    vcmd.setStatus(CL_MAP_FAILURE);
                }
            }
        }
    }
    else {
        LogError("Unhandled map!");
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitUnmapMemory(amd::UnmapMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd, true);
    gpu::Memory* memory = dev().getGpuMemory(&vcmd.memory());
    amd::Memory* owner = memory->owner();

    // We used host memory
    if (owner->getHostMem() != NULL) {
        if (memory->isUnmapWrite()) {
            // Target is the backing store, so sync
            owner->signalWrite(NULL);
            memory->syncCacheFromHost(*this);
        }
        // Remove memory from VA cache
        dev().removeVACache(memory);
    }
    // data check was added for persistent memory that failed to get aperture
    // and therefore are treated like a remote resource
    else if (memory->isPersistentDirectMap() && (memory->data() != NULL)) {
        memory->unmap(this);
    }
    else if (memory->mapMemory() != NULL) {
        if (memory->isUnmapWrite()) {
            amd::Coord3D srcOrigin(0, 0, 0);
            // Target is a remote resource, so copy
            assert(memory->mapMemory() != NULL);
            if (memory->cal()->buffer_) {
                if (!blitMgr().copyBuffer(
                    *memory->mapMemory(), *memory,
                    srcOrigin,
                    memory->writeMapInfo()->origin_,
                    memory->writeMapInfo()->region_,
                    memory->writeMapInfo()->entire_)) {
                    LogError("submitUnmapMemory() - copy failed");
                    vcmd.setStatus(CL_OUT_OF_RESOURCES);
                }
            }
            else if ((vcmd.memory().getType() == CL_MEM_OBJECT_IMAGE1D_BUFFER)) {
                amd::Memory* bufferFromImage = NULL;
                Memory* memoryBuf = memory;
                amd::Coord3D    origin(memory->writeMapInfo()->origin_[0]);
                amd::Coord3D    size(memory->writeMapInfo()->region_[0]);
                size_t  elemSize =
                    vcmd.memory().asImage()->getImageFormat().getElementSize();
                origin.c[0] *= elemSize;
                size.c[0]   *= elemSize;

                bufferFromImage = createBufferFromImage(vcmd.memory());
                if (NULL == bufferFromImage) {
                    LogError("We should not fail buffer creation from image_buffer!");
                }
                else {
                    bufferFromImage->setVirtualDevice(this);
                    memoryBuf = dev().getGpuMemory(bufferFromImage);
                }
                if (!blitMgr().copyBuffer(
                    *memory->mapMemory(), *memoryBuf,
                    srcOrigin, origin, size,
                    memory->writeMapInfo()->entire_)) {
                    LogError("submitUnmapMemory() - copy failed");
                    vcmd.setStatus(CL_OUT_OF_RESOURCES);
                }
                if (NULL != bufferFromImage) {
                    bufferFromImage->release();
                }
            }
            else {
                if (!blitMgr().copyBufferToImage(
                    *memory->mapMemory(), *memory,
                    srcOrigin,
                    memory->writeMapInfo()->origin_,
                    memory->writeMapInfo()->region_,
                    memory->writeMapInfo()->entire_)) {
                    LogError("submitUnmapMemory() - copy failed");
                    vcmd.setStatus(CL_OUT_OF_RESOURCES);
                }
            }
        }
    }
    else {
        LogError("Unhandled unmap!");
        vcmd.setStatus(CL_INVALID_VALUE);
    }

    // Clear read only flag
    memory->clearUnmapWrite();

    profilingEnd(vcmd);
}

bool
VirtualGPU::fillMemory(cl_command_type type, amd::Memory* amdMemory, const void* pattern,
                       size_t patternSize, const amd::Coord3D& origin, const amd::Coord3D& size)
{
    gpu::Memory* memory = dev().getGpuMemory(amdMemory);
    bool    entire = amdMemory->isEntirelyCovered(origin, size);

    // Synchronize memory from host if necessary
    device::Memory::SyncFlags syncFlags;
    syncFlags.skipEntire_ = entire;
    memory->syncCacheFromHost(*this, syncFlags);

    bool result = false;
    amd::Memory* bufferFromImage = NULL;
    float fillValue[4];

    // Force fill buffer for IMAGE1D_BUFFER
    if ((type == CL_COMMAND_FILL_IMAGE) &&
        (amdMemory->getType() == CL_MEM_OBJECT_IMAGE1D_BUFFER)) {
        bufferFromImage = createBufferFromImage(*amdMemory);
        if (NULL == bufferFromImage) {
            LogError("We should not fail buffer creation from image_buffer!");
        }
        else {
            type = CL_COMMAND_FILL_BUFFER;
            bufferFromImage->setVirtualDevice(this);
            memory = dev().getGpuMemory(bufferFromImage);
        }
    }

    // Find the the right fill operation
    switch (type) {
    case CL_COMMAND_FILL_BUFFER :
    case CL_COMMAND_SVM_MEMFILL : {
        amd::Coord3D    realOrigin(origin[0]);
        amd::Coord3D    realSize(size[0]);
        // Reprogram fill parameters if it's an IMAGE1D_BUFFER object
        if (NULL != bufferFromImage) {
            size_t  elemSize =
                amdMemory->asImage()->getImageFormat().getElementSize();
            realOrigin.c[0] *= elemSize;
            realSize.c[0]   *= elemSize;
            memset(fillValue, 0, sizeof(fillValue));
            amdMemory->asImage()->getImageFormat().formatColor(pattern, fillValue);
            pattern = fillValue;
            patternSize = elemSize;
        }
        result = blitMgr().fillBuffer(*memory, pattern,
            patternSize, realOrigin, realSize, amdMemory->isEntirelyCovered(origin, size));
        if (NULL != bufferFromImage) {
            bufferFromImage->release();
        }
    }
        break;
    case CL_COMMAND_FILL_IMAGE:
        result = blitMgr().fillImage(*memory, pattern,
            origin, size, amdMemory->isEntirelyCovered(origin, size));
        break;
    default:
        LogError("Unsupported command type for FillMemory!");
        break;
    }

    if (!result) {
        LogError("fillMemory failed!");
        return false;
    }

    // Mark this as the most-recently written cache of the destination
    amdMemory->signalWrite(&gpuDevice_);
    return true;
}

void
VirtualGPU::submitFillMemory(amd::FillMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd, true);

    if (!fillMemory(vcmd.type(), &vcmd.memory(),vcmd.pattern(),
        vcmd.patternSize(), vcmd.origin(), vcmd.size())) {
        vcmd.setStatus(CL_INVALID_OPERATION);
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitSvmMapMemory(amd::SvmMapMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd, true);

    //check if the ptr is in the svm space
    amd::Memory* svmMem = vcmd.getSvmMem();
    if (NULL == svmMem) {
        LogWarning("wrong svm address ");
        vcmd.setStatus(CL_INVALID_VALUE);
        return;
    }

    // Make sure we have memory for the command execution
    gpu::Memory* memory = dev().getGpuMemory(svmMem);

    if (vcmd.mapFlags() & (CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION)) {
        memory->saveWriteMapInfo(vcmd.origin(), vcmd.size(), vcmd.isEntireMemory());
    }

    if (memory->mapMemory() != NULL) {
        if (vcmd.mapFlags() & (CL_MAP_READ | CL_MAP_WRITE)) {
            amd::Coord3D dstOrigin(0, 0, 0);
            if (memory->cal()->buffer_) {
                if (!blitMgr().copyBuffer(*memory,
                    *memory->mapMemory(), vcmd.origin(), dstOrigin,
                    vcmd.size(), vcmd.isEntireMemory())) {
                    LogError("submitSVMMapMemory() - copy failed");
                    vcmd.setStatus(CL_MAP_FAILURE);
                }
            }
        }
    }
    else {
        LogError("Unhandled svm map!");
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitSvmUnmapMemory(amd::SvmUnmapMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());
    profilingBegin(vcmd, true);

    amd::Memory* svmMem = vcmd.getSvmMem();
    if (NULL == svmMem) {
        LogWarning("wrong svm address ");
        vcmd.setStatus(CL_INVALID_VALUE);
        return;
    }

    gpu::Memory* memory = dev().getGpuMemory(svmMem);

    if (memory->mapMemory() != NULL) {
        if (memory->isUnmapWrite()) {
            amd::Coord3D srcOrigin(0, 0, 0);
            // Target is a remote resource, so copy
            assert(memory->mapMemory() != NULL);
            if (memory->cal()->buffer_) {
                if (!blitMgr().copyBuffer(
                    *memory->mapMemory(), *memory,
                    srcOrigin,
                    memory->writeMapInfo()->origin_,
                    memory->writeMapInfo()->region_,
                    memory->writeMapInfo()->entire_)) {
                    LogError("submitUnmapMemory() - copy failed");
                    vcmd.setStatus(CL_OUT_OF_RESOURCES);
                }
            }
        }
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitSvmFillMemory(amd::SvmFillMemoryCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd, true);

    amd::Memory* dstMemory = amd::SvmManager::FindSvmBuffer(vcmd.dst());
    assert(dstMemory&&"No svm Buffer to fill with!");
    size_t offset = reinterpret_cast<uintptr_t>(vcmd.dst())
                    - reinterpret_cast<uintptr_t>(dstMemory->getSvmPtr());
    assert((offset >= 0)&&"wrong svm ptr to fill with!");

    gpu::Memory* memory = dev().getGpuMemory(dstMemory);
    size_t fillSize = vcmd.patternSize() * vcmd.times();

    amd::Coord3D    origin(offset, 0, 0);
    amd::Coord3D    size(fillSize, 1, 1);
    assert((dstMemory->validateRegion(origin, size))&&"The incorrect fill size!");

    if (!fillMemory(vcmd.type(), dstMemory, vcmd.pattern(),
                    vcmd.patternSize(), origin, size)) {
        vcmd.setStatus(CL_INVALID_OPERATION);
    }
    profilingEnd(vcmd);
}

void
VirtualGPU::submitMigrateMemObjects(amd::MigrateMemObjectsCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd, true);

    std::vector<amd::Memory*>::const_iterator itr;
    for (itr = vcmd.memObjects().begin(); itr != vcmd.memObjects().end(); itr++) {
        // Find device memory
        gpu::Memory* memory = dev().getGpuMemory(*itr);

        if (vcmd.migrationFlags() & CL_MIGRATE_MEM_OBJECT_HOST) {
            memory->mgpuCacheWriteBack();
        }
        else if (vcmd.migrationFlags() & CL_MIGRATE_MEM_OBJECT_CONTENT_UNDEFINED) {
            // Synchronize memory from host if necessary.
            // The sync function will perform memory migration from
            // another device if necessary
            device::Memory::SyncFlags syncFlags;
            memory->syncCacheFromHost(*this, syncFlags);
        }
        else {
            LogWarning("Unknown operation for memory migration!");
        }
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitSvmFreeMemory(amd::SvmFreeMemoryCommand& vcmd)
{
    // in-order semantics: previous commands need to be done before we start
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd);
    std::vector<void*>& svmPointers = vcmd.svmPointers();
    if (vcmd.pfnFreeFunc() == NULL) {
        // pointers allocated using clSVMAlloc
        for (cl_uint i = 0; i < svmPointers.size(); i++) {
            dev().svmFree(svmPointers[i]);
        }
    }
    else {
        vcmd.pfnFreeFunc()(as_cl(vcmd.queue()->asCommandQueue()), svmPointers.size(),
                static_cast<void**>(&(svmPointers[0])), vcmd.userData());
    }
    profilingEnd(vcmd);
}

void
VirtualGPU::findIterations(
    const amd::NDRangeContainer& sizes,
    const amd::NDRange&   local,
    amd::NDRange&   groups,
    amd::NDRange&   remainder,
    size_t&         extra)
{
    size_t  dimensions = sizes.dimensions();

    if (cal()->iterations_ > 1) {
        size_t  iterations = cal()->iterations_;
        cal_.iterations_ = 1;

        // Find the total amount of all groups
        groups = sizes.global() / local;
        if (dev().settings().partialDispatch_) {
            for (uint j = 0; j < dimensions; ++j) {
                if ((sizes.global()[j] % local[j]) != 0) {
                    groups[j]++;
                }
            }
        }

        // Calculate the real number of required iterations and
        // the workgroup size of each iteration
        for (int j = (dimensions - 1); j >= 0; --j) {
            // Find possible size of each iteration
            size_t tmp = (groups[j] / iterations);
            // Make sure the group size is more than 1
            if (tmp > 0) {
                remainder = groups;
                remainder[j] = (groups[j] % tmp);

                extra = ((groups[j] / tmp) +
                    // Check for the remainder
                    ((remainder[j] != 0) ? 1 : 0));
                // Recalculate the number of iterations
                cal_.iterations_ *= extra;
                if (remainder[j] == 0) {
                    extra = 0;
                }
                groups[j] = tmp;
                break;
            }
            else {
                iterations = ((iterations / groups[j]) +
                    (((iterations % groups[j]) != 0) ? 1 : 0));
                cal_.iterations_ *= groups[j];
                groups[j] = 1;
            }
        }
    }
}

void
VirtualGPU::setupIteration(
    uint            iteration,
    const amd::NDRangeContainer& sizes,
    Kernel&         gpuKernel,
    amd::NDRange&   global,
    amd::NDRange&   offsets,
    amd::NDRange&   local,
    amd::NDRange&   groups,
    amd::NDRange&   groupOffset,
    amd::NDRange&   divider,
    amd::NDRange&   remainder,
    size_t          extra)
{
    size_t  dimensions = sizes.dimensions();

    // Calculate the workload size for the remainder
    if ((extra != 0) && ((iteration % extra) == 0)) {
        groups = remainder;
    }
    else {
        groups = divider;
    }
    global = groups * local;

    if (dev().settings().partialDispatch_) {
        for (uint j = 0; j < dimensions; ++j) {
            size_t offset = groupOffset[j] * local[j];
            if ((offset + global[j]) > sizes.global()[j]) {
                global[j] = sizes.global()[j] - offset;
            }
        }
    }

    // Reprogram the kernel parameters for the GPU execution
    gpuKernel.setupProgramGrid(*this, dimensions,
        offsets, global, local, groupOffset,
        sizes.offset(), sizes.global());

    // Update the constant buffers
    gpuKernel.bindConstantBuffers(*this);

    uint sub = 0;
    // Find the offsets for the next execution
    for (uint j = 0; j < dimensions; ++j) {
        groupOffset[j] += groups[j];
        // Make sure the offset doesn't go over the size limit
        if (sizes.global()[j] <= groupOffset[j] * local[j]) {
            // Check if we counted a group in one dimension already
            if (sub) {
                groupOffset[j] -= groups[j];
            }
            else {
                groupOffset[j] = 0;
            }
        }
        else {
            groupOffset[j] -= sub;
            // We already counted elements in one dimension
            sub = 1;
        }

        offsets[j] = groupOffset[j] * local[j] +
            sizes.offset()[j];
    }
}

void
VirtualGPU::submitKernel(amd::NDRangeKernelCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd);

    // Submit kernel to HW
    if (!submitKernelInternal(vcmd.sizes(), vcmd.kernel(), vcmd.parameters(), false)) {
        vcmd.setStatus(CL_INVALID_OPERATION);
    }

    profilingEnd(vcmd);
}

bool
VirtualGPU::submitKernelInternalHSA(
    const amd::NDRangeContainer& sizes,
    const amd::Kernel&  kernel,
    const_address parameters,
    bool    nativeMem)
{
    uint64_t    vmParentWrap = 0;
    uint64_t    vmDefQueue = 0;
    amd::DeviceQueue*  defQueue = kernel.program().context().defDeviceQueue(dev());
    VirtualGPU*  gpuDefQueue = NULL;

    // Get the HSA kernel object
    const HSAILKernel& hsaKernel =
        static_cast<const HSAILKernel&>(*(kernel.getDeviceKernel(dev())));
    std::vector<const Resource*>    memList;

    bool printfEnabled = (hsaKernel.printfInfo().size() > 0) ? true:false;
    if (!printfDbgHSA().init(*this, printfEnabled )){
        LogError( "Printf debug buffer initialization failed!");
        return false;
    }

    bool deviceSupportFGS = 0 != (dev().info().svmCapabilities_ & CL_DEVICE_SVM_FINE_GRAIN_SYSTEM);
    bool supportFineGrainedSystem = deviceSupportFGS;
    FGSStatus status = kernel.parameters().getSvmSystemPointersSupport();
    switch (status) {
        case FGS_YES:
            if (!deviceSupportFGS) {
                return false;
            }
            supportFineGrainedSystem = true;
            break;
        case FGS_NO:
            supportFineGrainedSystem = false;
            break;
        case FGS_DEFAULT:
        default:
            break;
    }

    size_t count = kernel.parameters().getNumberOfSvmPtr();
    size_t execInfoOffset = kernel.parameters().getExecInfoOffset();
    amd::Memory* memory = NULL;
    //get svm non arugment information
    void* const* svmPtrArray = reinterpret_cast<void* const*>(parameters + execInfoOffset);
    for (size_t i = 0; i < count; i++) {
        memory =  amd::SvmManager::FindSvmBuffer(svmPtrArray[i]);
        if (NULL == memory) {
            if (!supportFineGrainedSystem) {
                return false;
            }
        }
        else {
            Memory* gpuMemory = dev().getGpuMemory(memory);
            if (NULL != gpuMemory) {
                memList.push_back(gpuMemory);
            }
            else {
                return false;
            }
        }
    }

    // Check memory dependency and cache coherency
    processMemObjectsHSA(kernel, parameters, nativeMem);
    cal_.memCount_ = 0;

    if (hsaKernel.dynamicParallelism()) {
        if (NULL == defQueue) {
            LogError("Default device queue wasn't allocated");
            return false;
        }
        else {
            gpuDefQueue = static_cast<VirtualGPU*>(defQueue->vDev());
        }
        vmDefQueue = gpuDefQueue->virtualQueue_->vmAddress();
        if (gpuDefQueue->hwRing() == hwRing()) {
            LogError("Can't submit the child kernels to the same HW ring as the host queue!");
            return false;
        }

        // Add memory handles before the actual dispatch
        memList.push_back(gpuDefQueue->virtualQueue_);
        memList.push_back(gpuDefQueue->schedParams_);
        memList.push_back(hsaKernel.prog().kernelTable());
        gpuDefQueue->writeVQueueHeader(*this,
            hsaKernel.prog().kernelTable()->vmAddress());
    }

    // Program the kernel arguments for the GPU execution
    HsaAqlDispatchPacket*   aqlPkt =
        hsaKernel.loadArguments(*this, kernel, sizes, parameters, nativeMem,
        vmDefQueue, &vmParentWrap, memList);
    if (NULL == aqlPkt) {
        LogError("Couldn't load kernel arguments");
        return false;
    }

    gslMemObject    scratch = NULL;
    // Check if the device allocated more registers than the old setup
    if (hsaKernel.workGroupInfo()->scratchRegs_ > 0) {
        const std::vector<Memory*>& mems = dev().scratch(hwRing())->memObjs_;
        scratch = mems[0]->gslResource();
        memList.push_back(mems[0]);
        scratchRegNum_ = dev().scratch(hwRing())->regNum_;
    }

    // Add GSL handle to the memory list for VidMM
    for (uint i = 0; i < memList.size(); ++i) {
        addVmMemory(memList[i]);
    }

    GpuEvent    gpuEvent;
    // Run AQL dispatch in HW
    runAqlDispatch(gpuEvent, aqlPkt, vmMems(), cal_.memCount_,
        scratch, hsaKernel.cpuAqlCode(), hsaQueueMem_->vmAddress());

    if (hsaKernel.dynamicParallelism()) {
        // Make sure exculsive access to the device queue
        amd::ScopedLock(defQueue->lock());
        //! \todo Remove flush. We start parent earlier.
        flushDMA(MainEngine);

        if (GPU_PRINT_CHILD_KERNEL != 0) {
            waitForEvent(&gpuEvent);

            AmdAqlWrap* wraps =  (AmdAqlWrap*)(&((AmdVQueueHeader*)gpuDefQueue->virtualQueue_->data())[1]);
            uint p = 0;
            for (uint i = 0; i < gpuDefQueue->vqHeader_->aql_slot_num; ++i) {
                if (wraps[i].state != 0) {
                    if (p == GPU_PRINT_CHILD_KERNEL) {
                        break;
                    }
                    p++;
                    std::stringstream print;
                    print.flags(std::ios::right | std::ios_base::hex | std::ios_base::uppercase);
                    print << "Slot#: "  << i << "\n";
                    print << "\tenqueue_flags: "  << wraps[i].enqueue_flags   << "\n";
                    print << "\tcommand_id: "     << wraps[i].command_id      << "\n";
                    print << "\tchild_counter: "  << wraps[i].child_counter   << "\n";
                    print << "\tcompletion: "     << wraps[i].completion      << "\n";
                    print << "\tparent_wrap: "    << wraps[i].parent_wrap     << "\n";
                    print << "\twait_list: "      << wraps[i].wait_list       << "\n";
                    print << "\twait_num: "       << wraps[i].wait_num        << "\n";
                    print << "WorkGroupSize[ " << wraps[i].aql.workgroup_size[0] << ", ";
                    print << wraps[i].aql.workgroup_size[1] << ", ";
                    print << wraps[i].aql.workgroup_size[2] << "]\n";
                    print << "GridSize[ " << wraps[i].aql.grid_size[0] << ", ";
                    print << wraps[i].aql.grid_size[1] << ", ";
                    print << wraps[i].aql.grid_size[2] << "]\n";

                    uint64_t* kernels = (uint64_t*)(
                        const_cast<Memory*>(hsaKernel.prog().kernelTable())->map(this));
                    uint j;
                    for (j = 0; j < hsaKernel.prog().kernels().size(); ++j) {
                        if (kernels[j] == wraps[i].aql.kernel_object_address) {
                            break;
                        }
                    }
                    const_cast<Memory*>(hsaKernel.prog().kernelTable())->unmap(this);
                    HSAILKernel* child = NULL;
                    for (auto it = hsaKernel.prog().kernels().begin();
                         it != hsaKernel.prog().kernels().end(); ++it) {
                        if (j == static_cast<HSAILKernel*>(it->second)->index()) {
                            child = static_cast<HSAILKernel*>(it->second);
                        }
                    }
                    if (child == NULL) {
                        printf("Error: couldn't find child kernel!\n");
                        continue;
                    }
                    uint offsArg = wraps[i].aql.kernel_arg_address -
                        gpuDefQueue->virtualQueue_->vmAddress();
                    address argum = gpuDefQueue->virtualQueue_->data() + offsArg;
                    print << "Kernel: " << child->name() << "\n";
                    static const char* Names[HSAILKernel::ExtraArguments] = {
                    "Offset0: ", "Offset1: ","Offset2: ","PrintfBuf: ", "VqueuePtr: ", "AqlWarap: "};
                    for (j = 0; j < HSAILKernel::ExtraArguments; ++j) {
                        print << "\t" << Names[j] << *(size_t*)argum;
                        print << "\n";
                        argum += sizeof(size_t);
                    }
                    for (j = 0; j < child->numArguments(); ++j) {
                        print << "\t" << child->argument(j)->name_ << ": ";
                        for (int s = child->argument(j)->size_ - 1; s >= 0; --s) {
                            print.width(2);
                            print.fill('0');
                            print << (uint32_t)(argum[s]);
                        }
                        argum += child->argument(j)->size_;
                        print << "\n";
                    }
                    printf("%s", print.str().c_str());
                }
            }
        }

        // Get the global loop start before the scheduler
        mcaddr loopStart = gpuDefQueue->virtualQueueDispatcherStart();
        static_cast<KernelBlitManager&>(gpuDefQueue->blitMgr()).runScheduler(
            *gpuDefQueue->virtualQueue_,
            *gpuDefQueue->schedParams_, gpuDefQueue->schedParamIdx_,
            gpuDefQueue->vqHeader_->aql_slot_num);

        // Get the address of PM4 template and add write it to params
        //! @note DMA flush must not occur between patch and the scheduler
        mcaddr patchStart = gpuDefQueue->virtualQueueDispatcherStart();
        // Program parameters for the scheduler
        SchedulerParam* param = &reinterpret_cast<SchedulerParam*>
            (gpuDefQueue->schedParams_->data())[gpuDefQueue->schedParamIdx_];
        param->signal = 1;
        // Scale clock to 1024 to avoid 64 bit div in the scheduler
        param->eng_clk = (1000 * 1024) / dev().info().maxClockFrequency_;
        param->hw_queue = patchStart + sizeof(uint32_t)/* Rewind packet*/;
        param->hsa_queue = gpuDefQueue->hsaQueueMem()->vmAddress();
        param->launch = 0;
        // Fill the scratch buffer information
        if (hsaKernel.prog().maxScratchRegs() > 0) {
            gpu::Memory* scratchBuf = dev().scratch(gpuDefQueue->hwRing())->memObjs_[0];
            param->scratchSize = scratchBuf->size();
            param->scratch = scratchBuf->vmAddress();
            param->numMaxWaves = 32 * dev().info().maxComputeUnits_;
            memList.push_back(scratchBuf);
        }
        else {
            param->numMaxWaves = 0;
            param->scratchSize = 0;
            param->scratch = 0;
        }

        // Add all kernels in the program to the mem list.
        //! \note Runtime doesn't know which one will be called
        hsaKernel.prog().fillResListWithKernels(memList);

        // Add GSL handle to the memory list for VidMM
        for (uint i = 0; i < memList.size(); ++i) {
            gpuDefQueue->addVmMemory(memList[i]);
        }

        mcaddr  signalAddr = gpuDefQueue->schedParams_->vmAddress() +
            gpuDefQueue->schedParamIdx_ * sizeof(SchedulerParam);
        gpuDefQueue->virtualQueueDispatcherEnd(gpuEvent,
            gpuDefQueue->vmMems(), gpuDefQueue->cal_.memCount_,
            signalAddr, loopStart);
        // Set GPU event for the used resources
        for (uint i = 0; i < memList.size(); ++i) {
            memList[i]->setBusy(*gpuDefQueue, gpuEvent);
        }

        // Add the termination handshake to the host queue
        virtualQueueHandshake(gpuEvent, gpuDefQueue->schedParams_->gslResource(),
            vmParentWrap + offsetof(AmdAqlWrap, state), AQL_WRAP_DONE,
            vmParentWrap + offsetof(AmdAqlWrap, child_counter),
            signalAddr);
        ++gpuDefQueue->schedParamIdx_ %=
            gpuDefQueue->schedParams_->size() / sizeof(SchedulerParam);
        //! \todo optimize the wrap around
        if (gpuDefQueue->schedParamIdx_ == 0) {
            gpuDefQueue->schedParams_->wait(*gpuDefQueue);
        }
    }

    // Set GPU event for the used resources
    for (uint i = 0; i < memList.size(); ++i) {
        memList[i]->setBusy(*this, gpuEvent);
    }

    // Update the global GPU event
    setGpuEvent(gpuEvent);

    if (!printfDbgHSA().output(*this, printfEnabled, hsaKernel.printfInfo())) {
        LogError("Couldn't read printf data from the buffer!\n");
        return false;
    }

    // Runtime submitted a HSAIL kernel
    state_.hsailKernel_ = true;

    return true;
}

bool
VirtualGPU::submitKernelInternal(
    const amd::NDRangeContainer& sizes,
    const amd::Kernel&  kernel,
    const_address parameters,
    bool    nativeMem)
{
    bool            result = true;
    uint            i;
    size_t          dimensions = sizes.dimensions();
    amd::NDRange    local(sizes.local());
    amd::NDRange    groupOffset(dimensions);
    GpuEvent        gpuEvent;
    groupOffset = 0;

    // Get the GPU kernel object with optimization enabled
    bool    noAlias = true;
    device::Kernel* devKernel = const_cast<device::Kernel*>
        (kernel.getDeviceKernel(dev(), noAlias));
    Kernel& gpuKernelOpt = static_cast<gpu::Kernel&>(*devKernel);

    if (gpuKernelOpt.hsa()) {
        return submitKernelInternalHSA(sizes, kernel, parameters, nativeMem);
    }
    else if (state_.hsailKernel_) {
        // Reload GSL state to HW, so runtime could run AMDIL kernel
        flushDMA(MainEngine);
        // Reset HSAIL state
        state_.hsailKernel_ = false;
    }

    // Find if arguments contain memory aliases or a dependency in the queue
    if (gpuKernelOpt.processMemObjects(*this, kernel, parameters, nativeMem)) {
        // Try to obtain a kernel object without optimization
        noAlias = false;
        devKernel = const_cast<device::Kernel*>
            (kernel.getDeviceKernel(dev(), noAlias));
        if (devKernel == NULL) {
            // We don't have any, so rebuild kernel
            if (!kernel.program().buildNoOpt(dev(), gpuKernelOpt.name())) {
                LogWarning("Kernel recompilation without noAlias failed!");
                noAlias = true;
            }

            // Get the GPU kernel object for the final execution
            devKernel = const_cast<device::Kernel*>
                (kernel.getDeviceKernel(dev(), noAlias));
        }
    }

    Kernel& gpuKernel = static_cast<gpu::Kernel&>(*devKernel);
    bool printfEnabled = (gpuKernel.flags() &
                          gpu::NullKernel::PrintfOutput) ? true:false;
    // Set current kernel CAL descriptor as active
    if (!setActiveKernelDesc(sizes, &gpuKernel) ||
        // Initialize printf support
        !printfDbg().init(*this, printfEnabled , sizes.global())) {
        LogPrintfError("We couldn't set \"%s\" kernel as active!",
            gpuKernel.name().data());
        return false;
    }

    // Find if we have to split workload
    dmaFlushMgmt_.findSplitSize(dev(), sizes.global().product(), gpuKernel.instructionCnt());

    // Program the kernel parameters for the GPU execution
    cal_.memCount_ = 0;
    gpuKernel.setupProgramGrid(*this, dimensions,
        sizes.offset(), sizes.global(),
        local, groupOffset, sizes.offset(), sizes.global());

    // Load kernel arguments
    if (gpuKernel.loadParameters(*this, kernel, parameters, nativeMem)) {
        amd::NDRange    global(sizes.global());
        amd::NDRange    groups(dimensions);
        amd::NDRange    offsets(sizes.offset());
        amd::NDRange    divider(dimensions);
        amd::NDRange    remainder(dimensions);
        size_t          extra = 0;

        // Split the workload if necessary for local/private emulation or printf
        findIterations(sizes, local, groups, remainder, extra);

        divider = groups;
        i = 0;
        do {
            bool    lastRun = (i == (cal()->iterations_ - 1)) ? true : false;
            // Reprogram the CAL grid and constant buffers if
            // the workload split is on
            if (cal()->iterations_ > 1) {
                // Initialize printf support
                if (!printfDbg().init(*this, printfEnabled, local)) {
                    result = false;
                    break;
                }

                // Reprogram the CAL grid and constant buffers
                setupIteration(i, sizes,
                    gpuKernel, global, offsets, local,
                    groups, groupOffset, divider, remainder, extra);
            }

            // Execute the kernel
            if (gpuKernel.run(*this, &gpuEvent, lastRun)) {
                //! @todo A flush is necessary to make sure
                // that 2 consecutive runs won't access to the same
                // private/local memory. CAL has to generate cache flush
                // and wait for idle commands
                bool flush = ((cal()->iterations_ > 1) ||
                    dmaFlushMgmt_.isCbReady(*this, global.product(),
                        gpuKernel.instructionCnt())) ? true : false;

                // Update the global GPU event
                setGpuEvent(gpuEvent, flush);

                // This code for the kernel execution debugging
                if (dev().settings().debugFlags_ & Settings::LockGlobalMemory) {
                    gpuKernel.debug(*this);
                }
            }
            else {
                result = false;
                break;
            }

            // Print the debug buffer output result
            if (printfDbg().output(*this, printfEnabled,
                (cal()->iterations_ > 1) ? local : sizes.global(),
                gpuKernel.prog().printfInfo())) {
                // Go to the next iteration
                ++i;
            }
            else {
                result = false;
                break;
            }
        }
        // Check if we have to make multiple iterations
        while (i < cal()->iterations_);
    }
    else {
        result = false;
    }

    if (!result) {
        LogPrintfError("submitKernel failed to execute the \"%s\" kernel on HW!",
            gpuKernel.name().data());
    }

    return result;
}

void
VirtualGPU::submitNativeFn(amd::NativeFnCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    Unimplemented();    //!< @todo: Unimplemented
}

void
VirtualGPU::submitMarker(amd::Marker& vcmd)
{
    //!@note runtime doesn't need to lock this command on execution

    if (vcmd.waitingEvent() != NULL) {
        bool foundEvent = false;

        // Loop through all outstanding command batches
        while (!cbList_.empty()) {
            CommandBatchList::const_iterator it = cbList_.begin();
            // Wait for completion
            foundEvent = awaitCompletion(*it, vcmd.waitingEvent());
            // Release a command batch
            delete *it;
            // Remove command batch from the list
            cbList_.pop_front();
            // Early exit if we found a command
            if (foundEvent) break;
        }

        // Event should be in the current command batch
        if (!foundEvent) {
            state_.forceWait_ = true;
        }
        // If we don't have any more batches, then assume GPU is idle
        else if (cbList_.empty()) {
            dmaFlushMgmt_.resetCbWorkload(dev());
        }
    }
}

void
VirtualGPU::releaseMemory(gslMemObject gslResource, bool wait)
{
    bool result = true;
    if (wait) {
        waitForEvent(&gpuEvents_[gslResource]);
    }

    // Unbind resource if it's active kernel desc
    for (uint i = 0; i < MaxUavArguments; ++i) {
        if (gslResource == cal_.uavs_[i]) {
            result = setUAVBuffer(i, 0, GSL_UAV_TYPE_UNKNOWN);
            cal_.uavs_[i] = 0;
        }
    }
    for (uint i = 0; i < MaxReadImage; ++i) {
        if (gslResource == cal_.readImages_[i]) {
            result = setInput(i, 0);
            cal_.readImages_[i] = 0;
        }
    }
    for (uint i = 0; i < MaxConstBuffers; ++i) {
        if (gslResource == cal_.constBuffers_[i]) {
            result = setConstantBuffer(i, 0, 0, 0);
            cal_.constBuffers_[i] = 0;
        }
    }

    //!@todo optimize unbind
    if (numGrpCb_ != NULL) {
        setConstantBuffer(SC_INFO_CONSTANTBUFFER, NULL, 0, 0);
    }

    if ((dev().scratch(hwRing()) != NULL) &&
        (dev().scratch(hwRing())->regNum_ > 0)) {
        // Unbind scratch memory
        const std::vector<Memory*>& mems = dev().scratch(hwRing())->memObjs_;
        for (uint i = 0; i < mems.size(); ++i) {
            if ((mems[i] != NULL) && (mems[i]->gslResource() == gslResource)) {
                setScratchBuffer(NULL, i);
                scratchRegNum_ = 0;
            }
        }
    }

    gpuEvents_.erase(gslResource);
}

void
VirtualGPU::releaseKernel(CALimage calImage)
{
    GslKernelDesc* desc = gslKernels_[calImage];
    if (desc != NULL) {
        freeKernelDesc(desc);
    }
    gslKernels_.erase(calImage);
}

void
VirtualGPU::submitPerfCounter(amd::PerfCounterCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    gslQueryObject  gslCounter;

    const amd::PerfCounterCommand::PerfCounterList counters = vcmd.getCounters();

    // Create a HW counter
    gslCounter = createCounter(GSL_PERFORMANCE_COUNTERS_ATI);
    if (0 == gslCounter) {
        LogError("We failed to allocate memory for the GPU perfcounter");
        vcmd.setStatus(CL_INVALID_OPERATION);
        return;
    }
    CalCounterReference* calRef = new CalCounterReference(*this, gslCounter);
    if (calRef == NULL) {
        LogError("We failed to allocate memory for the GPU perfcounter");
        vcmd.setStatus(CL_INVALID_OPERATION);
        return;
    }
    gslCounter = 0;

    for (uint i = 0; i < vcmd.getNumCounters(); ++i) {
        amd::PerfCounter* amdCounter =
            static_cast<amd::PerfCounter*>(counters[i]);
        const PerfCounter* counter =
            static_cast<const PerfCounter*>(amdCounter->getDeviceCounter());

        // Make sure we have a valid gpu performance counter
        if (NULL == counter) {
            amd::PerfCounter::Properties prop = amdCounter->properties();
            PerfCounter* gpuCounter = new PerfCounter(
                gpuDevice_,
                *this,
                prop[CL_PERFCOUNTER_GPU_BLOCK_INDEX],
                prop[CL_PERFCOUNTER_GPU_COUNTER_INDEX],
                prop[CL_PERFCOUNTER_GPU_EVENT_INDEX]);
            if (NULL == gpuCounter) {
                LogError("We failed to allocate memory for the GPU perfcounter");
                vcmd.setStatus(CL_INVALID_OPERATION);
                return;
            }
            else if (gpuCounter->create(calRef)) {
                amdCounter->setDeviceCounter(gpuCounter);
            }
            else {
                LogPrintfError("We failed to allocate a perfcounter in CAL.\
                    Block: %d, counter: #d, event: %d",
                    gpuCounter->info()->blockIndex_,
                    gpuCounter->info()->counterIndex_,
                    gpuCounter->info()->eventIndex_);
                delete gpuCounter;
                vcmd.setStatus(CL_INVALID_OPERATION);
                return;
            }
            counter = gpuCounter;
        }
    }

    calRef->release();

    for (uint i = 0; i < vcmd.getNumCounters(); ++i) {
        amd::PerfCounter* amdCounter =
            static_cast<amd::PerfCounter*>(counters[i]);
        const PerfCounter* counter =
            static_cast<const PerfCounter*>(amdCounter->getDeviceCounter());

        if (gslCounter != counter->gslCounter()) {
            gslCounter = counter->gslCounter();
            // Find the state and sends the command to CAL
            if (vcmd.getState() == amd::PerfCounterCommand::Begin) {
                beginCounter(gslCounter, GSL_PERFORMANCE_COUNTERS_ATI);
            }
            else if (vcmd.getState() == amd::PerfCounterCommand::End) {
                GpuEvent event;
                endCounter(gslCounter, event);
                setGpuEvent(event);
            }
            else {
                LogError("Unsupported performance counter state");
                vcmd.setStatus(CL_INVALID_OPERATION);
                return;
            }
        }
    }
}
void
VirtualGPU::submitThreadTraceMemObjects(amd::ThreadTraceMemObjectsCommand& cmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(cmd);

    switch(cmd.type()) {
    case CL_COMMAND_THREAD_TRACE_MEM:
        {
            amd::ThreadTrace* amdThreadTrace = &cmd.getThreadTrace();
            ThreadTrace* threadTrace =
                static_cast<ThreadTrace*>(amdThreadTrace->getDeviceThreadTrace());

            if (threadTrace == NULL) {
                gslQueryObject  gslThreadTrace;
                // Create a HW thread trace query object
                gslThreadTrace = createThreadTrace();
                if (0 == gslThreadTrace) {
                    LogError("Failure in memory allocation for the GPU threadtrace");
                    cmd.setStatus(CL_INVALID_OPERATION);
                    return;
                }
                CalThreadTraceReference* calRef = new CalThreadTraceReference(*this,gslThreadTrace);
                if (calRef == NULL) {
                    LogError("Failure in memory allocation for the GPU threadtrace");
                    cmd.setStatus(CL_INVALID_OPERATION);
                    return;
                }
                size_t seNum = amdThreadTrace->deviceSeNumThreadTrace();
                ThreadTrace* gpuThreadTrace = new ThreadTrace(
                    gpuDevice_,
                    *this,
                    seNum);
                if (NULL == gpuThreadTrace) {
                    LogError("Failure in memory allocation for the GPU threadtrace");
                    cmd.setStatus(CL_INVALID_OPERATION);
                    return;
                }
                if (gpuThreadTrace->create(calRef)) {
                    amdThreadTrace->setDeviceThreadTrace(gpuThreadTrace);
                }
                else {
                    LogError("Failure in memory allocation for the GPU threadtrace");
                    delete gpuThreadTrace;
                    cmd.setStatus(CL_INVALID_OPERATION);
                    return;
                }
                threadTrace = gpuThreadTrace;
                calRef->release();
            }
            gslShaderTraceBufferObject* threadTraceBufferObjects = threadTrace->getThreadTraceBufferObjects();
            const size_t memObjSize = cmd.getMemoryObjectSize();
            const std::vector<amd::Memory*>& memObj = cmd.getMemList();
            size_t se = 0;
            for (std::vector<amd::Memory*>::const_iterator itMemObj = memObj.begin();itMemObj != memObj.end();++itMemObj,++se) {
                // Find GSL Mem Object
                gslMemObject gslMemObj = dev().getGpuMemory(*itMemObj)->gslResource();

                // Bind GSL MemObject to the appropriate SE Thread Trace Buffer Object
                configMemThreadTrace(threadTraceBufferObjects[se],gslMemObj,se,memObjSize);
            }
            break;
        }
    default:
        LogError("Unsupported command type for ThreadTraceMemObjects!");
        break;
    }
}

void
VirtualGPU::submitThreadTrace(amd::ThreadTraceCommand& cmd)
{
     // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(cmd);

    switch(cmd.type()) {
    case CL_COMMAND_THREAD_TRACE:
        {
            amd::ThreadTrace* amdThreadTrace =
                static_cast<amd::ThreadTrace*>(&cmd.getThreadTrace());
            ThreadTrace* threadTrace =
                static_cast<ThreadTrace*>(amdThreadTrace->getDeviceThreadTrace());

            // gpu thread trace object had to be generated prior to begin/end/pause/resume due
            // to ThreadTraceMemObjectsCommand execution
            if (threadTrace == NULL) {
                return;
            }
            else {
                gslQueryObject  gslThreadTrace;
                gslThreadTrace = threadTrace->gslThreadTrace();
                // Find the state and sends the command to CAL
                if (cmd.getState() == amd::ThreadTraceCommand::Begin) {
                    size_t amdMemObjsNumThreadTrace = amdThreadTrace->deviceSeNumThreadTrace();
                    amd::ThreadTrace::ThreadTraceConfig* amdThreadTraceConfig =
                        static_cast<amd::ThreadTrace::ThreadTraceConfig*>(cmd.threadTraceConfig());
                    CALthreadTraceConfig calTthreadTraceConfig;

                    calTthreadTraceConfig.cu = amdThreadTraceConfig->cu_;
                    calTthreadTraceConfig.sh = amdThreadTraceConfig->sh_;
                    calTthreadTraceConfig.simd_mask = amdThreadTraceConfig->simdMask_;
                    calTthreadTraceConfig.vm_id_mask = amdThreadTraceConfig->vmIdMask_;
                    calTthreadTraceConfig.token_mask = amdThreadTraceConfig->tokenMask_;
                    calTthreadTraceConfig.reg_mask = amdThreadTraceConfig->regMask_;
                    calTthreadTraceConfig.inst_mask = amdThreadTraceConfig->instMask_;
                    calTthreadTraceConfig.random_seed = amdThreadTraceConfig->randomSeed_;
                    calTthreadTraceConfig.user_data = amdThreadTraceConfig->userData_;
                    calTthreadTraceConfig.capture_mode = amdThreadTraceConfig->captureMode_;
                    if (amdThreadTraceConfig->isUserData_) {
                        calTthreadTraceConfig.is_user_data = CAL_TRUE;
                    }
                    else {
                        calTthreadTraceConfig.is_user_data = CAL_FALSE;
                    }
                    if (amdThreadTraceConfig->isWrapped_) {
                        calTthreadTraceConfig.is_wrapped = CAL_TRUE;
                    }
                    else {
                        calTthreadTraceConfig.is_wrapped = CAL_FALSE;
                    }
                    beginThreadTrace(gslThreadTrace,0,GSL_SHADER_TRACE_BYTES_WRITTEN,amdMemObjsNumThreadTrace,calTthreadTraceConfig);
                }
                else if (cmd.getState() == amd::ThreadTraceCommand::End) {
                    endThreadTrace(gslThreadTrace,2);
                }
                else if (cmd.getState() == amd::ThreadTraceCommand::Pause) {
                    pauseThreadTrace(2);
                }
                else if (cmd.getState() == amd::ThreadTraceCommand::Resume) {
                    resumeThreadTrace(2);
                }
            }
            break;
        }
    default:
        LogError("Unsupported command type for ThreadTrace!");
        break;
    }
}

void
VirtualGPU::submitAcquireExtObjects(amd::AcquireExtObjectsCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd);

    for (std::vector<amd::Memory*>::const_iterator it = vcmd.getMemList().begin();
         it != vcmd.getMemList().end(); it++) {
        // amd::Memory object should never be NULL
        assert(*it && "Memory object for interop is NULL");
        gpu::Memory* memory = dev().getGpuMemory(*it);

        // If resource is a shared copy of original resource, then
        // runtime needs to copy data from original resource
        (*it)->getInteropObj()->copyOrigToShared();

        // Check if OpenCL has direct access to the interop memory
        if (memory->interopType() == Memory::InteropDirectAccess) {
            continue;
        }

        // Does interop use HW emulation?
        if (memory->interopType() == Memory::InteropHwEmulation) {
            static const bool Entire  = true;
            amd::Coord3D    origin(0, 0, 0);
            amd::Coord3D    region(memory->size());

            // Synchronize the object
            if (!blitMgr().copyBuffer(*memory->interop(),
                *memory, origin, origin, region, Entire)) {
                LogError("submitAcquireExtObjects - Interop synchronization failed!");
                vcmd.setStatus(CL_INVALID_OPERATION);
                return;
            }
        }
    }

    profilingEnd(vcmd);
}

void
VirtualGPU::submitReleaseExtObjects(amd::ReleaseExtObjectsCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd);

    for (std::vector<amd::Memory*>::const_iterator it = vcmd.getMemList().begin();
         it != vcmd.getMemList().end(); it++) {
        // amd::Memory object should never be NULL
        assert(*it && "Memory object for interop is NULL");
        gpu::Memory* memory = dev().getGpuMemory(*it);

        // Check if we can use HW interop
        if (memory->interopType() == Memory::InteropHwEmulation) {
            static const bool Entire  = true;
            amd::Coord3D    origin(0, 0, 0);
            amd::Coord3D    region(memory->size());

            // Synchronize the object
            if (!blitMgr().copyBuffer(*memory, *memory->interop(),
                origin, origin, region, Entire)) {
                LogError("submitReleaseExtObjects interop synchronization failed!");
                vcmd.setStatus(CL_INVALID_OPERATION);
                return;
            }
        }
        else {
            if (memory->interopType() != Memory::InteropDirectAccess) {
                LogError("None interop release!");
            }
        }

        // If resource is a shared copy of original resource, then
        // runtime needs to copy data back to original resource
        (*it)->getInteropObj()->copySharedToOrig();
    }

    profilingEnd(vcmd);
}

#if cl_amd_open_video
void
VirtualGPU::submitRunVideoProgram(amd::RunVideoProgramCommand& vcmd)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    profilingBegin(vcmd);

    switch(vcmd.type()) {
    case CL_COMMAND_VIDEO_DECODE_AMD: {
        CALprogramVideoDecode calVideoData;
        cl_video_decode_data_amd* clVideoData =
            static_cast<cl_video_decode_data_amd*>(vcmd.videoData());

        //Convert cl_video_program_type_amd to CALvideoType
        calVideoData.videoType.type = CAL_VIDEO_DECODE;
        calVideoData.videoType.size = sizeof(CALprogramVideoDecode);
        // Copy video data from CL to CAL structure
        calVideoData.videoType.flags = clVideoData->video_type.flags;
        calVideoData.picture_parameter_1 = clVideoData->picture_parameter_1;
        calVideoData.picture_parameter_2 = clVideoData->picture_parameter_2;
        calVideoData.picture_parameter_2_size = clVideoData->picture_parameter_2_size;
        calVideoData.bitstream_data = clVideoData->bitstream_data;
        calVideoData.bitstream_data_size = clVideoData->bitstream_data_size;
        calVideoData.slice_data_control = clVideoData->slice_data_control;
        calVideoData.slice_data_size = clVideoData->slice_data_control_size;

        gpu::Memory* gpuMem = dev().getGpuMemory(&vcmd.memory());

        GpuEvent event;
        if (!runProgramVideoDecode(event, gpuMem->gslResource(),
            reinterpret_cast<CALprogramVideoDecode&>(calVideoData))) {
            vcmd.setStatus(CL_INVALID_OPERATION);
            return;
        }
        // Mark source and destination as busy
        gpuMem->setBusy(*this, event);

        // Update the global GPU event and flush the DMA buffer,
        // so runtime can synchronize UVD and SDMA engines
        // @todo - do we need to flush here?
        setGpuEvent(event, true);
    }
    break;
    case CL_COMMAND_VIDEO_ENCODE_AMD: {
        cl_video_encode_data_amd* clVideoData =
            static_cast<cl_video_encode_data_amd*>(vcmd.videoData());

        CAL_VID_ENCODE_PARAMETERS_H264 *ppicture_parameter =
            reinterpret_cast<CAL_VID_ENCODE_PARAMETERS_H264*>(clVideoData->pictureParam2);
        uint num_of_encode_task_input_buffer =
            (uint)(clVideoData->pictureParam1Size);
        CAL_VID_BUFFER_DESCRIPTION *encode_task_input_buffer_list =
            reinterpret_cast<CAL_VID_BUFFER_DESCRIPTION *>(clVideoData->pictureParam1);

        CAL_VID_BUFFER_DESCRIPTION *encode_task_input_buffer_listbackup =
            new CAL_VID_BUFFER_DESCRIPTION [num_of_encode_task_input_buffer];
        if (encode_task_input_buffer_listbackup == NULL) {
            LogError("calCtxRunProgramVideo unable to allocate memory");
            vcmd.setStatus(CL_OUT_OF_RESOURCES);
            return;
        }

        // Entropy mode
        cl_mem  buffer_surface;
        gpu::Memory* gpuMem;

        // Convert cl_mem object to gslMemObject object
        for (uint i = 0; i < num_of_encode_task_input_buffer; i++) {
            encode_task_input_buffer_listbackup[i] = encode_task_input_buffer_list[i];
            buffer_surface = (cl_mem)encode_task_input_buffer_list[i].buffer.pPicture;
            gpuMem = dev().getGpuMemory(as_amd(buffer_surface));
            encode_task_input_buffer_listbackup[i].buffer.pPicture = gpuMem->gslResource();
        }

        gpuMem     = dev().getGpuMemory(&(vcmd.memory()));

        // Encode the picture - call QueryTask to get the results...
        GpuEvent event;
        EncodeePicture(event, num_of_encode_task_input_buffer,
            encode_task_input_buffer_listbackup, ppicture_parameter,
            &(clVideoData->uiTaskID),
            gpuMem->gslResource(), 0);

        // Mark source and destination as busy
        gpuMem->setBusy(*this, event);

        // Update the global GPU event and flush the DMA buffer,
        // so runtime can synchronize VCE and SDMA engines
        // @todo - do we need to flush here?
        setGpuEvent(event, true);
        delete[] encode_task_input_buffer_listbackup;
    }
    break;
    default:
        vcmd.setStatus(CL_INVALID_VIDEO_CONFIG_TYPE_AMD);
        LogError("Invalid video command type");
        return;
    }
    profilingEnd(vcmd);
}

void
VirtualGPU::submitSetVideoSession(amd::SetVideoSessionCommand& cmd)
{
    switch (cmd.operation()) {
    case amd::SetVideoSessionCommand::CloseSession:
        closeVideoEncodeSession(0);
        destroyVCE(0);
        break;
    case amd::SetVideoSessionCommand::ConfigTypePictureControl:
        getPictureConfig(
            (CALEncodeGetPictureControlConfig*)(cmd.paramValue()), 0);
        break;
    case amd::SetVideoSessionCommand::ConfigTypeRateControl:
        getRateControlConfig(
            (CALEncodeGetRateControlConfig*)(cmd.paramValue()), 0);
        break;
    case amd::SetVideoSessionCommand::ConfigTypeMotionEstimation:
        getMotionEstimationConfig(
            (CALEncodeGetMotionEstimationConfig*)(cmd.paramValue()), 0);
        break;
    case amd::SetVideoSessionCommand::ConfigTypeRDO:
        getRDOConfig(
            (CALEncodeGetRDOControlConfig*)(cmd.paramValue()), 0);
        break;
    case amd::SetVideoSessionCommand::SendEncodeConfig:
        SendConfig(
            cmd.numBuffers(), (CAL_VID_CONFIG*)(cmd.paramValue()), 0);
        break;
    case amd::SetVideoSessionCommand::GetDeviceCapVCE: {
        CALEncodeGetDeviceCAP    EncodeCAP;
        EncodeCAP.num_of_encode_cap = 1;
        EncodeCAP.encode_caps = (CAL_VID_ENCODE_CAPS *)(cmd.paramValue());
        getDeviceCAPVCE(0, cmd.numBuffers(), &EncodeCAP, 0);
    }
        break;
    case amd::SetVideoSessionCommand::EncodeQueryTaskDescription:
        QueryTaskDescription(
            cmd.numBuffers(), cmd.paramValue2(),
            (CAL_VID_OUTPUT_DESCRIPTION *)cmd.paramValue(), 0);
        break;
    case amd::SetVideoSessionCommand::ReleaseOutputResource:
        ReleaseOutputResource(cmd.numBuffers(), 0);
        break;
    default:
        break;
    }
}
#endif // cl_amd_open_video

void
VirtualGPU::submitSignal(amd::SignalCommand & vcmd)
{
    bool res = true;
    amd::ScopedLock lock(execution());
    profilingBegin(vcmd);
    gpu::Memory* gpuMemory = dev().getGpuMemory(&vcmd.memory());
    if (vcmd.type() == CL_COMMAND_WAIT_SIGNAL_AMD) {
        res = WaitSignal(gpuMemory->gslResource(), vcmd.markerValue());
    }
    else if (vcmd.type() == CL_COMMAND_WRITE_SIGNAL_AMD) {
        res = WriteSignal(gpuMemory->gslResource(), vcmd.markerValue(),
            vcmd.markerOffset());
    }
    if(res != true) {
        LogError("submitSignal failed");
        vcmd.setStatus(CL_INVALID_OPERATION);
    }
    profilingEnd(vcmd);
}

void
VirtualGPU::submitMakeBuffersResident(amd::MakeBuffersResidentCommand & vcmd)
{
    amd::ScopedLock lock(execution());
    profilingBegin(vcmd);
    std::vector<amd::Memory*> memObjects = vcmd.memObjects();
    cl_uint numObjects = memObjects.size();
    gslMemObject* pGSLMemObjects = new gslMemObject[numObjects];

    for(cl_uint i = 0; i < numObjects; ++i)
    {
        gpu::Memory* gpuMemory = dev().getGpuMemory(memObjects[i]);
        pGSLMemObjects[i] = gpuMemory->gslResource();
        gpuMemory->syncCacheFromHost(*this);
    }

    cl_ulong* surfBusAddr = new cl_ulong[numObjects];
    cl_ulong* markerBusAddr = new cl_ulong[numObjects];
    bool res = MakeBuffersResident(
                    numObjects,
                    pGSLMemObjects,
                    (CALuint64*)surfBusAddr,
                    (CALuint64*)markerBusAddr);
    if(res != true) {
        LogError("MakeBuffersResident failed");
        vcmd.setStatus(CL_INVALID_OPERATION);
    }
    else {
        cl_bus_address_amd* busAddr = vcmd.busAddress();
        for(cl_uint i = 0; i < numObjects; ++i)
        {
            busAddr[i].surface_bus_address = surfBusAddr[i];
            busAddr[i].marker_bus_address = markerBusAddr[i];
        }
    }
    delete[] pGSLMemObjects;
    delete[] surfBusAddr;
    delete[] markerBusAddr;
    profilingEnd(vcmd);
}


bool
VirtualGPU::awaitCompletion(CommandBatch* cb, const amd::Event* waitingEvent)
{
    bool found = false;
    amd::Command*   current;
    amd::Command*   head = cb->head_;

    // Make sure that profiling is enabled
    if (head->profilingInfo().enabled_) {
        return profilingCollectResults(cb, waitingEvent);
    }
    // Mark the first command in the batch as running
    if (head != NULL) {
        head->setStatus(CL_RUNNING);
    }
    else {
        return found;
    }

    // Wait for the last known GPU event
    waitEventLock(cb);

    while (NULL != head) {
        current = head->getNext();
        if (head->status() == CL_SUBMITTED) {
            head->setStatus(CL_RUNNING);
            head->setStatus(CL_COMPLETE);
        }
        else if (head->status() == CL_RUNNING) {
            head->setStatus(CL_COMPLETE);
        }
        else if ((head->status() != CL_COMPLETE) && (current != NULL)) {
            LogPrintfError("Unexpected command status - %d!", head->status());
        }

        // Check if it's a waiting command
        if (head == waitingEvent) {
            found = true;
        }

        head->release();
        head = current;
    }

    return found;
}

void
VirtualGPU::flush(amd::Command* list, bool wait)
{
    CommandBatch* cb = NULL;
    bool    gpuCommand = false;

    for (uint i = 0; i < AllEngines; ++i) {
        if (cal_.events_[i].isValid()) {
            gpuCommand = true;
        }
    }

    // If the batch doesn't have any GPU command and the list is empty
    if (!gpuCommand && cbList_.empty()) {
        state_.forceWait_ = true;
    }

    // Insert the current batch into a list
    if (NULL != list) {
        cb = new CommandBatch(list, cal()->events_, cal()->lastTS_);
    }

    for (uint i = 0; i < AllEngines; ++i) {
        flushDMA(i);
        // Reset event so we won't try to wait again,
        // if runtime didn't submit any commands
        // @note: it's safe to invalidate events, since
        // we already saved them with the batch creation step above
        cal_.events_[i].invalidate();
    }

    // Mark last TS as NULL, so runtime won't process empty batches with the old TS
    cal_.lastTS_ = NULL;
    if (NULL != cb) {
        cbList_.push_back(cb);
    }

    wait |= state_.forceWait_;
    // Loop through all outstanding command batches
    while (!cbList_.empty()) {
        CommandBatchList::const_iterator it = cbList_.begin();
        // Check if command batch finished without a wait
        bool    finished = true;
        for (uint i = 0; i < AllEngines; ++i) {
            finished &= isDone(&(*it)->events_[i]);
        }
        if (finished || wait) {
            // Wait for completion
            awaitCompletion(*it);
            // Release a command batch
            delete *it;
            // Remove command batch from the list
            cbList_.pop_front();
        }
        else {
            // Early exit if no finished
            break;
        }
    }
    state_.forceWait_ = false;
}

void
VirtualGPU::enableSyncedBlit() const
{
    return blitMgr_->enableSynchronization();
}

void
VirtualGPU::releaseMemObjects()
{
    for (GpuEvents::const_iterator it = gpuEvents_.begin();
            it != gpuEvents_.end(); ++it) {
        GpuEvent event = it->second;
        waitForEvent(&event);
    }
    // Unbind all resources.So the queue won't have any bound mem objects
    for (uint i = 0; i < MaxUavArguments; ++i) {
        if (NULL != cal_.uavs_[i]) {
            setUAVBuffer(i, 0, GSL_UAV_TYPE_UNKNOWN);
            cal_.uavs_[i] = 0;
        }
    }
    for (uint i = 0; i < MaxReadImage; ++i) {
        if (NULL != cal_.readImages_[i]) {
            setInput(i, 0);
            cal_.readImages_[i] = 0;
        }
    }
    for (uint i = 0; i < MaxConstBuffers; ++i) {
        if (NULL != cal_.constBuffers_[i]) {
            setConstantBuffer(i, 0, 0, 0);
            cal_.constBuffers_[i] = 0;
        }
    }
    //!@todo optimize unbind
    if (numGrpCb_ != NULL) {
        setConstantBuffer(SC_INFO_CONSTANTBUFFER, NULL, 0, 0);
    }
    gpuEvents_.clear();
}

void
VirtualGPU::setGpuEvent(
    GpuEvent    gpuEvent,
    bool        flush)
{
    cal_.events_[engineID_] = gpuEvent;

    // Flush current DMA buffer if requested
    if (flush || GPU_FLUSH_ON_EXECUTION) {
        flushDMA(engineID_);
    }
}

void
VirtualGPU::flushDMA(uint engineID)
{
    if (engineID == MainEngine) {
        // Clear memory dependency state, since runtime flushes compute
        // memoryDependency().clear();
        //!@todo Keep memory dependency alive even if we flush DMA,
        //! since only L2 cache is flushed in KMD frame,
        //! but L1 still has to be invalidated.
    }

    //! \note Use CtxIsEventDone, so we won't flush compute for DRM engine
    isDone(&cal_.events_[engineID]);
}

bool
VirtualGPU::waitAllEngines(CommandBatch* cb)
{
    uint i;
    GpuEvent*   events;    //!< GPU events for the batch

    // If command batch is NULL then wait for the current
    if (NULL == cb) {
        events = cal_.events_;
    }
    else {
        events = cb->events_;
    }

    bool earlyDone = true;
    // The first loop is to flush all engines and/or check if
    // engines are idle already
    for (i = 0; i < AllEngines; ++i) {
        earlyDone &= isDone(&events[i]);
    }

    // Release all transfer buffers on this command queue
    releaseXferWrite();

    // Rlease all pinned memory
    releasePinnedMem();

    // The second loop is to wait all engines
    for (i = 0; i < AllEngines; ++i) {
        waitForEvent(&events[i]);
    }

    return earlyDone;
}

void
VirtualGPU::waitEventLock(CommandBatch* cb)
{
    // Make sure VirtualGPU has an exclusive access to the resources
    amd::ScopedLock lock(execution());

    bool earlyDone = waitAllEngines(cb);

    // Free resource cache if we have too many entries
    //! \note we do it here, when all engines are idle,
    // because Vista/Win7 idles GPU on a resource destruction
    static const size_t MinCacheEntries = 4096;
    dev().resourceCache().free(MinCacheEntries);

    // Find the timestamp object of the last command in the batch
    if (cb->lastTS_ != NULL) {
        // If earlyDone is TRUE, then CPU didn't wait for GPU.
        // Thus the sync point between CPU and GPU is unclear and runtime
        // will use an older adjustment value to maintain the same timeline
        if (!earlyDone ||
            //! \note Workaround for APU(s).
            //! GPU-CPU timelines may go off too much, thus always
            //! force calibration with the last batch in the list
            (cbList_.size() <= 1) ||
            (readjustTimeGPU_ == 0)) {
            uint64_t    startTimeStampGPU = 0;
            uint64_t    endTimeStampGPU = 0;

            // Get the timestamp value of the last command in the batch
            cb->lastTS_->value(&startTimeStampGPU, &endTimeStampGPU);

            uint64_t    endTimeStampCPU = amd::Os::timeNanos();
            // Make sure the command batch has a valid GPU TS
            if (!GPU_RAW_TIMESTAMP) {
                // Adjust the base time by the execution time
                readjustTimeGPU_ = endTimeStampGPU - endTimeStampCPU;
            }
        }
    }
}

void
VirtualGPU::validateScratchBuffer(const Kernel* kernel)
{
    // Check if the device allocated more registers than the old setup
    if (dev().scratch(hwRing())->regNum_ > scratchRegNum_) {
        const std::vector<Memory*>& mems = dev().scratch(hwRing())->memObjs_;
        for (uint i = 0; i < mems.size(); ++i) {
            // Setup scratch buffer
            setScratchBuffer(mems[i]->gslResource(), i);
        }
        scratchRegNum_ = dev().scratch(hwRing())->regNum_;
    }
}

bool
VirtualGPU::setActiveKernelDesc(
    const amd::NDRangeContainer& sizes,
    const Kernel* kernel)
{
    bool    result = true;
    CALimage calImage = kernel->calImage();

    GslKernelDesc* desc = gslKernels_[calImage];

    validateScratchBuffer(kernel);

    // Early exit
    if ((activeKernelDesc_ == desc) && (desc != NULL)) {
        return result;
    }

    // Does the kernel descriptor for this virtual device exist?
    if (desc == NULL) {
        desc = allocKernelDesc(kernel, calImage);
        if (desc == NULL) {
            return false;
        }
        gslKernels_[calImage] = desc;
    }

    // Update UAV mask if it has a different set of bits
    if ((activeKernelDesc_ == NULL) ||
        (activeKernelDesc_->uavMask_.mask[0] != desc->uavMask_.mask[0])) {
        setUavMask(desc->uavMask_);
    }

    // Set the descriptor as active
    activeKernelDesc_ = desc;

    // Program the samplers defined in the kernel
    if (!kernel->setInternalSamplers(*this)) {
        result = false;
    }

    // Bind global HW constant buffers
    if (!kernel->bindGlobalHwCb(*this, desc)) {
        result = false;
    }

    if (result) {
        // Set program in GSL
        setProgram(desc->func_);

        // Update internal constant buffer
        if (desc->intCb_ != 0) {
            setConstants(desc->intCb_);
        }
    }

    return result;
}

bool
VirtualGPU::allocConstantBuffers()
{
    // Allocate/reallocate constant buffers
    const static size_t MinCbSize = 64 * Ki;
    uint    i;

    // Create/reallocate constant buffer resources
    for (i = 0; i < MaxConstBuffersArguments; ++i) {
        ConstBuffer* constBuf = new ConstBuffer(*this, ((MinCbSize +
            ConstBuffer::VectorSize - 1) / ConstBuffer::VectorSize));

        if ((constBuf != NULL) && constBuf->create()) {
            addConstBuffer(constBuf);
        }
        else {
            // We failed to create a constant buffer
            delete constBuf;
            return false;
        }
    }

    // 8xx workaround for num workgroups
    if (!dev().settings().siPlus_) {
        numGrpCb_ = new ConstBuffer(*this, ((MinCbSize +
                ConstBuffer::VectorSize - 1) / ConstBuffer::VectorSize));
        if ((numGrpCb_ == NULL) || !numGrpCb_->create()) {
            LogError("Could not allocate num groups constant buffer!");
            return false;
        }
    }

    return true;
}

VirtualGPU::GslKernelDesc*
VirtualGPU::allocKernelDesc(const Kernel* kernel, CALimage calImage)
{
    // Sanity checks
    assert(kernel != NULL);
    GslKernelDesc*  desc = new GslKernelDesc;

    if (desc != NULL) {
        memset(desc, 0, sizeof(GslKernelDesc));

        if (kernel->calImage() != calImage) {
            desc->image_ = calImage;
        }

        if (!moduleLoad(calImage, &desc->func_, &desc->intCb_, &desc->uavMask_)) {
            LogPrintfError("calModuleLoad failed for \"%s\" kernel!",
                kernel->name().c_str());
            delete desc;
            return NULL;
        }

        //
        // prime the func info in the func object.
        //
        getFuncInfo(desc->func_, GSL_COMPUTE_PROGRAM, &desc->funcInfo_);
    }

    if (kernel->argSize() > slots_.size()) {
        slots_.resize(kernel->argSize());
    }

    return desc;
}

void
VirtualGPU::freeKernelDesc(VirtualGPU::GslKernelDesc* desc)
{
    if (desc) {
        if (gslKernelDesc() == desc) {
            // Clear active kernel desc
            activeKernelDesc_ = NULL;
            setProgram(0);
        }

        if (desc->image_ != 0) {
            // Free CAL image
            free(desc->image_);
        }

        if (desc->func_ != 0) {
            if (desc->intCb_ != 0) {
                destroyConstants(desc->intCb_);
            }
            destroyProgramObject(desc->func_);
        }

        delete desc;
    }
}

void
VirtualGPU::profilingBegin(amd::Command& command, bool drmProfiling)
{
    // Is profiling enabled?
    if (command.profilingInfo().enabled_) {
        // Allocate a timestamp object from the cache
        TimeStamp* ts = tsCache_->allocTimeStamp();
        if (NULL == ts) {
            return;
        }
        // Save the TimeStamp object in the current OCL event
        command.setData(ts);
        currTs_ = ts;
    }
}

void
VirtualGPU::profilingEnd(amd::Command& command)
{
    // Get the TimeStamp object associated witht the current command
    TimeStamp* ts = reinterpret_cast<TimeStamp*>(command.data());
    if (ts != NULL) {
        // Check if the command actually did any GPU submission
        if (ts->isValid()) {
            cal_.lastTS_ = ts;
        }
        else {
            // Destroy the TimeStamp object
            tsCache_->freeTimeStamp(ts);
            command.setData(NULL);
        }
    }
}

bool
VirtualGPU::profilingCollectResults(CommandBatch* cb, const amd::Event* waitingEvent)
{
    bool    found = false;
    amd::Command*   current;
    amd::Command*   first = cb->head_;

    // If the command list is, empty then exit
    if (NULL == first) {
        return found;
    }

    // Wait for the last known GPU events on all engines
    waitEventLock(cb);

    // Find the CPU base time of the entire command batch execution
    uint64_t    endTimeStamp = amd::Os::timeNanos();
    uint64_t    startTimeStamp = endTimeStamp;

    // First step, walk the command list to find the first valid command
    //! \note The batch may have empty markers at the beginning.
    //! So the start/end of the empty commands is equal to
    //! the start of the first valid command in the batch.
    first = cb->head_;
    while (NULL != first) {
        // Get the TimeStamp object associated witht the current command
        TimeStamp* ts = reinterpret_cast<TimeStamp*>(first->data());

        if (ts != NULL) {
            ts->value(&startTimeStamp, &endTimeStamp);
            endTimeStamp -= readjustTimeGPU_;
            startTimeStamp -= readjustTimeGPU_;
            // Assign to endTimeStamp the start of the first valid command
            endTimeStamp = startTimeStamp;
            break;
        }
        first = first->getNext();
    }

    // Second step, walk the command list to construct the time line
    first = cb->head_;
    while (NULL != first) {
        // Get the TimeStamp object associated witht the current command
        TimeStamp* ts = reinterpret_cast<TimeStamp*>(first->data());

        current = first->getNext();

        if (ts != NULL) {
            ts->value(&startTimeStamp, &endTimeStamp);
            endTimeStamp -= readjustTimeGPU_;
            startTimeStamp -= readjustTimeGPU_;
            // Destroy the TimeStamp object
            tsCache_->freeTimeStamp(ts);
            first->setData(NULL);
        }
        else {
            // For empty commands start/end is equal to
            // the end of the last valid command
            startTimeStamp = endTimeStamp;
        }

        // Update the command status with the proper timestamps
        if (first->status() == CL_SUBMITTED) {
            first->setStatus(CL_RUNNING, startTimeStamp);
            first->setStatus(CL_COMPLETE, endTimeStamp);
        }
        else if (first->status() == CL_RUNNING) {
            first->setStatus(CL_COMPLETE, endTimeStamp);
        }
        else if ((first->status() != CL_COMPLETE) && (current != NULL)) {
            LogPrintfError("Unexpected command status - %d!", first->status());
        }

        // Do we wait this event?
        if (first == waitingEvent) {
            found = true;
        }

        first->release();
        first = current;
    }

    return found;
}

bool
VirtualGPU::addVmMemory(const Resource* resource)
{
    if (dev().heap()->isVirtual()) {
        uint*    cnt = &cal_.memCount_;
        (*cnt)++;
        // Reallocate array if kernel uses more memory objects
        if (numVmMems_ < *cnt) {
            gslMemObject* tmp;
            tmp = new gslMemObject [*cnt];
            if (tmp == NULL) {
                return false;
            }
            memcpy(tmp, vmMems_, sizeof(gslMemObject) * numVmMems_);
            delete [] vmMems_;
            vmMems_ = tmp;
            numVmMems_ = *cnt;
        }
        vmMems_[*cnt - 1] = resource->gslResource();
    }

    return true;
}

void
VirtualGPU::profileEvent(EngineType engine, bool type) const
{
    if (NULL == currTs_) {
        return;
    }
    if (type) {
        currTs_->begin((engine == SdmaEngine) ? true : false);
    }
    else {
        currTs_->end((engine == SdmaEngine) ? true : false);
    }
}

void
VirtualGPU::processMemObjectsHSA(
    const amd::Kernel&  kernel,
    const_address       params,
    bool                nativeMem)
{
    static const bool NoAlias = true;
    const HSAILKernel& hsaKernel = static_cast<const HSAILKernel&>
        (*(kernel.getDeviceKernel(dev(), NoAlias)));

    // Mark the tracker with a new kernel,
    // so we can avoid checks of the aliased objects
    memoryDependency().newKernel();

    const amd::KernelSignature& signature = kernel.signature();
    const amd::KernelParameters& kernelParams = kernel.parameters();

    // Check all parameters for the current kernel
    for (size_t i = 0; i < signature.numParameters(); ++i) {
        const amd::KernelParameterDescriptor& desc = signature.at(i);
        const HSAILKernel::Argument*  arg = hsaKernel.argument(i);
        Memory* memory = NULL;
        bool    readOnly = false;

        // Find if current argument is a buffer
        if ((desc.type_ == T_POINTER) && (arg->addrQual_ != HSAIL_ADDRESS_LOCAL)) {
            if (kernelParams.boundToSvmPointer(dev(), params, i)) {
                //!\todo Do we have to sync cache coherency or wait for SDMA?
                flushL1Cache();
                break;
            }

            if (nativeMem) {
                memory = *reinterpret_cast<Memory* const*>(params + desc.offset_);
            }
            else if (*reinterpret_cast<amd::Memory* const*>
                    (params + desc.offset_) != NULL) {
                amd::Memory* svmMem = amd::SvmManager::FindSvmBuffer(
                    *reinterpret_cast<void* const*>(params + desc.offset_));
                if (NULL == svmMem) {
                    memory = dev().getGpuMemory(*reinterpret_cast<amd::Memory* const*>
                            (params + desc.offset_));
                }
                else {
                    memory = dev().getGpuMemory(svmMem);
                }
                // Synchronize data with other memory instances if necessary
                memory->syncCacheFromHost(*this);
            }

            if (memory != NULL) {
                //!@todo The code below can handle images only,
                //! but the qualifier is broken anyway
                readOnly = (desc.accessQualifier_ ==
                    CL_KERNEL_ARG_ACCESS_READ_ONLY) ? true : false;
                // Validate memory for a dependency in the queue
                memoryDependency().validate(*this, memory, readOnly);
            }
        }
    }

    if (hsaKernel.prog().globalStore() != NULL) {
        const static bool IsReadOnly = false;
        // Validate global store for a dependency in the queue
        memoryDependency().validate(*this, hsaKernel.prog().globalStore(), IsReadOnly);
    }
}

amd::Memory*
VirtualGPU::createBufferFromImage(amd::Memory& amdImage) const
{
    amd::Memory* mem = new(amdImage.getContext())
        amd::Buffer(amdImage, 0, 0, amdImage.getSize());

    if ((mem != NULL) && !mem->create()) {
        mem->release();
    }

    return mem;
}

void
VirtualGPU::writeVQueueHeader(VirtualGPU& hostQ, uint64_t kernelTable)
{
    const static bool Wait = true;
    vqHeader_->kernel_table = kernelTable;
    virtualQueue_->writeRawData(hostQ, sizeof(AmdVQueueHeader), vqHeader_, !Wait);
}

} // namespace gpu
