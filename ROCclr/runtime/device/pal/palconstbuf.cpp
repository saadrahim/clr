//
// Copyright (c) 2010 Advanced Micro Devices, Inc. All rights reserved.
//

#include "device/pal/palconstbuf.hpp"
#include "device/pal/palvirtual.hpp"
#include "device/pal/paldevice.hpp"
#include "device/pal/palsettings.hpp"

namespace pal {

// ================================================================================================
ManagedBuffer::ManagedBuffer(VirtualGPU& gpu, uint32_t size)
    : gpu_(gpu)
    , buffers_(MaxNumberOfBuffers)
    , activeBuffer_(0)
    , size_(size)
    , wrtOffset_(0)
    , wrtAddress_(nullptr) {}

// ================================================================================================
ManagedBuffer::~ManagedBuffer() {
  for (auto it : buffers_) {
    if (it->data() != nullptr) {
      it->unmap(&gpu_);
    }
    delete it;
  }
}

// ================================================================================================
bool ManagedBuffer::create(Resource::MemoryType type) {
  for (uint i = 0; i < buffers_.size(); ++i) {
    buffers_[i] = new Memory(const_cast<pal::Device&>(gpu_.dev()), size_);
    if (nullptr == buffers_[i] || !buffers_[i]->create(type)) {
      LogPrintfError("We couldn't create HW constant buffer, size(%d)!", size_);
      return false;
    }
    void* wrtAddress = buffers_[i]->map(&gpu_);
    if (wrtAddress == nullptr) {
        LogPrintfError("We couldn't map HW constant buffer, size(%d)!", size_);
        return false;
    }
    // Make sure OCL touches every buffer in the queue to avoid delays on the first submit
    uint dummy = 0;
    static constexpr bool Wait = true;
    // Write 0 for the buffer paging by VidMM
    buffers_[i]->writeRawData(gpu_, 0, sizeof(dummy), &dummy, Wait);
  }
  wrtAddress_ = buffers_[activeBuffer_]->data();
  return true;
}

// ================================================================================================
address ManagedBuffer::reserve(uint32_t size, uint64_t* gpu_address) {
  static constexpr uint32_t MemAlignment = 256;

  // Align reserve size on the vector's boundary
  uint32_t count = amd::alignUp(size, MemAlignment);

  // Check if buffer has enough space for reservation
  if ((wrtOffset_ + count) > size_) {
    // Get the next buffer in the list
    ++activeBuffer_;
    activeBuffer_ %= MaxNumberOfBuffers;
    // Make sure the buffer isn't busy
    buffers_[activeBuffer_]->wait(gpu_);
    wrtAddress_ = buffers_[activeBuffer_]->data();
    wrtOffset_ = 0;
  }

  *gpu_address = buffers_[activeBuffer_]->vmAddress() + wrtOffset_;
  address cpu_address = wrtAddress_ + wrtOffset_;
  
  // Adjust the offset by the reserved size
  wrtOffset_ += count;

  return cpu_address;
}

// ================================================================================================
ConstantBuffer::ConstantBuffer(ManagedBuffer& mbuf, uint32_t size)
    : mbuf_(mbuf)
    , sys_mem_copy_(nullptr)
    , size_(size)
{}

// ================================================================================================
ConstantBuffer::~ConstantBuffer() {
  amd::AlignedMemory::deallocate(sys_mem_copy_);
}

// ================================================================================================
bool ConstantBuffer::Create() {
  // Create sysmem copy for the constant buffer.
  sys_mem_copy_ = reinterpret_cast<address>(amd::AlignedMemory::allocate(size_, 256));
  if (sys_mem_copy_ == nullptr) {
    LogPrintfError("We couldn't allocate sysmem copy for constant buffer, size(%d)!", size_);
    return false;
  }
  memset(sys_mem_copy_, 0, size_);
  return true;
}

// ================================================================================================
uint64_t ConstantBuffer::UploadDataToHw(uint32_t size) const
{
  uint64_t  vm_address;
  address   cpu_address = mbuf_.reserve(size, &vm_address);
  // Update memory with new CB data
  memcpy(cpu_address, sys_mem_copy_, size);
  return vm_address;
}

}  // namespace pal
