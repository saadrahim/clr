//
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
//
#pragma once

#include "top.hpp"
#include "device/device.hpp"
#include "device/pal/paldevice.hpp"
#include "palPerfExperiment.h"

namespace pal {

class VirtualGPU;

class PalCounterReference : public amd::ReferenceCountedObject
{
public:
    static PalCounterReference* Create(VirtualGPU& gpu);

    //! Default constructor
    PalCounterReference(
        VirtualGPU&     gpu //!< Virtual GPU device object
        )
        : perfExp_(nullptr)
        , gpu_(gpu)
        , memory_(nullptr)
        , cpuAddr_(nullptr)
        , layout_(nullptr) {}

    //! Get PAL counter
    Pal::IPerfExperiment* iPerf() const { return perfExp_; }

    //! Returns the virtual GPU device
    const VirtualGPU& gpu() const { return gpu_; }

    //! Prepare for execution
    bool finalize();

    //! Returns the PAL counter results
    uint64_t result(uint index);

protected:
    //! Default destructor
    ~PalCounterReference();

private:
    //! Disable copy constructor
    PalCounterReference(const PalCounterReference&);

    //! Disable operator=
    PalCounterReference& operator=(const PalCounterReference&);

    VirtualGPU&                 gpu_;           //!< The virtual GPU device object
    Pal::IPerfExperiment*       perfExp_;       //!< PAL performance experiment object
    Pal::GlobalCounterLayout*   layout_;        //!< Layout of the result
    Memory*                     memory_;
    void*                       cpuAddr_;       //!< CPU address of memory_
};

//! Performance counter implementation on GPU
class PerfCounter : public device::PerfCounter
{
public:
    //! The performance counter info
    struct Info : public amd::EmbeddedObject
    {
        uint        blockIndex_;    //!< Index of the block to configure
        uint        counterIndex_;  //!< Index of the hardware counter
        uint        eventIndex_;    //!< Event you wish to count with the counter
    };

    //! Constructor for the GPU PerfCounter object
    PerfCounter(
        const Device&       device,         //!< A GPU device object
        PalCounterReference* palRef,        //!< Counter Reference
        cl_uint             blockIndex,     //!< HW block index
        cl_uint             counterIndex,   //!< Counter index within the block
        cl_uint             eventIndex)     //!< Event index for profiling
        : gpuDevice_(device)
        , palRef_(palRef)
        , index_(0)
    {
        info_.blockIndex_   = blockIndex;
        info_.counterIndex_ = counterIndex;
        info_.eventIndex_   = eventIndex;
    }

    //! Destructor for the GPU PerfCounter object
    virtual ~PerfCounter();

    //! Creates the current object
    bool create();

    //! Returns the specific information about the counter
    uint64_t getInfo(
        uint64_t infoType   //!< The type of returned information
        ) const;

    //! Returns the GPU device, associated with the current object
    const Device& dev() const { return gpuDevice_; }

    //! Returns the virtual GPU device
    const VirtualGPU& gpu() const { return palRef_->gpu(); }

    //! Returns the CAL performance counter descriptor
    const Info* info() const { return &info_; }

    //! Returns the Info structure for performance counter
    Pal::IPerfExperiment* iPerf() const { return palRef_->iPerf(); }

private:
    //! Disable default copy constructor
    PerfCounter(const PerfCounter&);

    //! Disable default operator=
    PerfCounter& operator=(const PerfCounter&);

    const Device&           gpuDevice_; //!< The backend device
    PalCounterReference*    palRef_;    //!< Reference counter
    Info                    info_;      //!< The info structure for perfcounter
    uint                    index_;     //!< Counter index in the CAL container
};

} // namespace pal
