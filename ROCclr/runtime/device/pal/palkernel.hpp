//
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
//
#pragma once

#include "device/device.hpp"
#include "utils/macros.hpp"
#include "platform/command.hpp"
#include "platform/program.hpp"
#include "platform/kernel.hpp"
#include "platform/sampler.hpp"
#include "device/pal/paldevice.hpp"
#include "device/pal/palvirtual.hpp"
#include "amd_hsa_kernel_code.h"
#include "device/pal/palprintf.hpp"
#include "device/pal/palwavelimiter.hpp"
#include "hsa.h"

namespace amd {
namespace hsa {
namespace loader {
class Symbol;
} // loader
} // hsa
} // amd

//! \namespace pal PAL Device Implementation
namespace pal {

class VirtualGPU;
class Device;
class NullDevice;
class HSAILProgram;

/*! \addtogroup pal PAL Device Implementation
 *  @{
 */

enum HSAIL_ADDRESS_QUALIFIER{
    HSAIL_ADDRESS_ERROR = 0,
    HSAIL_ADDRESS_GLOBAL,
    HSAIL_ADDRESS_LOCAL,
    HSAIL_MAX_ADDRESS_QUALIFIERS
} ;

enum HSAIL_ARG_TYPE{
    HSAIL_ARGTYPE_ERROR = 0,
    HSAIL_ARGTYPE_POINTER,
    HSAIL_ARGTYPE_VALUE,
    HSAIL_ARGTYPE_IMAGE,
    HSAIL_ARGTYPE_SAMPLER,
    HSAIL_ARGTYPE_QUEUE,
    HSAIL_ARGMAX_ARG_TYPES
};

enum HSAIL_DATA_TYPE{
    HSAIL_DATATYPE_ERROR = 0,
    HSAIL_DATATYPE_B1,
    HSAIL_DATATYPE_B8,
    HSAIL_DATATYPE_B16,
    HSAIL_DATATYPE_B32,
    HSAIL_DATATYPE_B64,
    HSAIL_DATATYPE_S8,
    HSAIL_DATATYPE_S16,
    HSAIL_DATATYPE_S32,
    HSAIL_DATATYPE_S64,
    HSAIL_DATATYPE_U8,
    HSAIL_DATATYPE_U16,
    HSAIL_DATATYPE_U32,
    HSAIL_DATATYPE_U64,
    HSAIL_DATATYPE_F16,
    HSAIL_DATATYPE_F32,
    HSAIL_DATATYPE_F64,
    HSAIL_DATATYPE_STRUCT,
    HSAIL_DATATYPE_OPAQUE,
    HSAIL_DATATYPE_MAX_TYPES
};

enum HSAIL_ACCESS_TYPE {
    HSAIL_ACCESS_TYPE_NONE = 0,
    HSAIL_ACCESS_TYPE_RO,
    HSAIL_ACCESS_TYPE_WO,
    HSAIL_ACCESS_TYPE_RW
};

class HSAILKernel : public device::Kernel
{
public:
    struct Argument
    {
        std::string name_;          //!< Argument's name
        std::string typeName_;      //!< Argument's type name
        uint        size_;          //!< Size in bytes
        uint        offset_;        //!< Argument's offset
        uint        alignment_;     //!< Argument's alignment
        HSAIL_ARG_TYPE type_;       //!< Type of the argument
        HSAIL_ADDRESS_QUALIFIER addrQual_;  //!< Address qualifier of the argument
        HSAIL_DATA_TYPE dataType_;  //!< The type of data
        uint        numElem_;       //!< Number of elements
        HSAIL_ACCESS_TYPE access_;  //!< Access type for the argument
    };

    // Max number of possible extra (hidden) kernel arguments
    static const uint MaxExtraArgumentsNum = 6;

    HSAILKernel(std::string name,
        HSAILProgram* prog,
        std::string compileOptions,
        uint extraArgsNum);

    virtual ~HSAILKernel();

    //! Initializes the metadata required for this kernel,
    //! finalizes the kernel if needed
    bool init(amd::hsa::loader::Symbol *sym, bool finalize = false);

    //! Returns true if memory is valid for execution
    virtual bool validateMemory(uint idx, amd::Memory* amdMem) const;

    //! Returns a pointer to the hsail argument
    const Argument* argument(size_t i) const { return arguments_[i]; }

    //! Returns the number of hsail arguments
    size_t numArguments() const { return arguments_.size(); }

    //! Returns GPU device object, associated with this kernel
    const Device& dev() const;

    //! Returns HSA program associated with this kernel
    const HSAILProgram& prog() const;

    //! Returns LDS size used in this kernel
    uint32_t ldsSize() const
        { return cpuAqlCode_->workgroup_group_segment_byte_size; }

    //! Returns pointer on CPU to AQL code info
    const amd_kernel_code_t* cpuAqlCode() const { return cpuAqlCode_; }

    //! Returns memory object with AQL code
    pal::Memory* gpuAqlCode() const { return code_; }

    //! Returns size of AQL code
    size_t aqlCodeSize() const { return codeSize_; }

    //! Returns the size of argument buffer
    size_t argsBufferSize() const
        { return cpuAqlCode_->kernarg_segment_byte_size; }

    //! Returns spill reg size per workitem
    int spillSegSize() const
        { return cpuAqlCode_->workitem_private_segment_byte_size; }

    //! Returns TRUE if kernel uses dynamic parallelism
    bool dynamicParallelism() const
        { return (flags_.dynamicParallelism_) ? true : false; }

    //! Returns TRUE if kernel is internal kernel
    bool isInternalKernel() const
        { return (flags_.internalKernel_) ? true : false; }

    //! Finds local workgroup size
    void findLocalWorkSize(
        size_t      workDim,            //!< Work dimension
        const amd::NDRange& gblWorkSize,//!< Global work size
        amd::NDRange& lclWorkSize       //!< Local work size
        ) const;

    //! Returns AQL packet in CPU memory
    //! if the kerenl arguments were successfully loaded, otherwise NULL
    hsa_kernel_dispatch_packet_t* loadArguments(
        VirtualGPU&                     gpu,        //!< Running GPU context
        const amd::Kernel&              kernel,     //!< AMD kernel object
        const amd::NDRangeContainer&    sizes,      //!< NDrange container
        const_address               parameters,     //!< Application arguments for the kernel
        bool                        nativeMem,      //!< Native memory objectes are passed
        uint64_t                    vmDefQueue,     //!< GPU VM default queue pointer
        uint64_t*                   vmParentWrap,   //!< GPU VM parent aql wrap object
        std::vector<const Memory*>&     memList     //!< Memory list for GSL/VidMM handles
        ) const;

    //! Returns pritnf info array
    const std::vector<PrintfInfo>& printfInfo() const { return printf_; }

    //! Returns the kernel index in the program
    uint index() const { return index_; }

    //! Returns kernel's extra argument count
    uint extraArgumentsNum() const { return extraArgumentsNum_; }

    //! Get profiling callback object
    virtual amd::ProfilingCallback* getProfilingCallback(
        const device::VirtualDevice *vdev) {
        return waveLimiter_.getProfilingCallback(vdev);
    }

    //! Get waves per shader array to be used for kernel execution.
    uint getWavesPerSH(const device::VirtualDevice *vdev) const {
        return waveLimiter_.getWavesPerSH(vdev);
    }

private:
    //! Disable copy constructor
    HSAILKernel(const HSAILKernel&);

    //! Disable operator=
    HSAILKernel& operator=(const HSAILKernel&);

    //! Creates AQL kernel HW info
    bool aqlCreateHWInfo(amd::hsa::loader::Symbol *sym);

    //! Initializes arguments_ and the abstraction layer kernel parameters
    void initArgList(
        const aclArgData* aclArg    //!< List of ACL arguments
        );

    //! Initializes Hsail Argument metadata and info
    void initHsailArgs(
        const aclArgData* aclArg    //!< List of ACL arguments
        );

    //! Initializes Hsail Printf metadata and info
    void initPrintf(
        const aclPrintfFmt* aclPrintf   //!< List of ACL printfs
        );

    std::vector<Argument*> arguments_;  //!< Vector list of HSAIL Arguments
    std::string compileOptions_;        //!< compile used for finalizing this kernel
    amd_kernel_code_t*  cpuAqlCode_;    //!< AQL kernel code on CPU
    const NullDevice&   dev_;           //!< GPU device object
    const HSAILProgram& prog_;          //!< Reference to the parent program
    std::vector<PrintfInfo> printf_;    //!< Format strings for GPU printf support
    uint    index_;                     //!< Kernel index in the program

    pal::Memory*    code_;      //!< Memory object with ISA code
    size_t          codeSize_;  //!< Size of ISA code

    char*       hwMetaData_;    //!< SI metadata

    uint extraArgumentsNum_; //! Number of extra (hidden) kernel arguments

    union Flags {
        struct {
            uint    imageEna_: 1;           //!< Kernel uses images
            uint    imageWriteEna_: 1;      //!< Kernel uses image writes
            uint    dynamicParallelism_: 1; //!< Dynamic parallelism enabled
            uint    internalKernel_: 1;     //!< True: internal kernel
        };
        uint    value_;
        Flags(): value_(0) {}
    } flags_;

    WaveLimiterManager waveLimiter_; //!< adaptively control number of waves
};

/*@}*/} // namespace pal

