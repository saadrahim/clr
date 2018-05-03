//
// Copyright (c) 2009 Advanced Micro Devices, Inc. All rights reserved.
//
#pragma once

#include <memory>
#include "acl.h"
#include "rocprogram.hpp"
#include "top.hpp"
#include "rocprintf.hpp"

#ifndef WITHOUT_HSA_BACKEND

namespace roc {

#define MAX_INFO_STRING_LEN 0x40

enum ROC_ARG_TYPE {
  ROC_ARGTYPE_ERROR = 0,
  ROC_ARGTYPE_POINTER,
  ROC_ARGTYPE_VALUE,
  ROC_ARGTYPE_REFERENCE,
  ROC_ARGTYPE_IMAGE,
  ROC_ARGTYPE_SAMPLER,
  ROC_ARGTYPE_HIDDEN_GLOBAL_OFFSET_X,
  ROC_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Y,
  ROC_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Z,
  ROC_ARGTYPE_HIDDEN_PRINTF_BUFFER,
  ROC_ARGTYPE_HIDDEN_DEFAULT_QUEUE,
  ROC_ARGTYPE_HIDDEN_COMPLETION_ACTION,
  ROC_ARGTYPE_HIDDEN_NONE,
  ROC_ARGMAX_ARG_TYPES
};

enum ROC_ADDRESS_QUALIFIER {
  ROC_ADDRESS_ERROR = 0,
  ROC_ADDRESS_GLOBAL,
  ROC_ADDRESS_CONSTANT,
  ROC_ADDRESS_LOCAL,
  ROC_MAX_ADDRESS_QUALIFIERS
};

enum ROC_DATA_TYPE {
  ROC_DATATYPE_ERROR = 0,
  ROC_DATATYPE_B1,
  ROC_DATATYPE_B8,
  ROC_DATATYPE_B16,
  ROC_DATATYPE_B32,
  ROC_DATATYPE_B64,
  ROC_DATATYPE_S8,
  ROC_DATATYPE_S16,
  ROC_DATATYPE_S32,
  ROC_DATATYPE_S64,
  ROC_DATATYPE_U8,
  ROC_DATATYPE_U16,
  ROC_DATATYPE_U32,
  ROC_DATATYPE_U64,
  ROC_DATATYPE_F16,
  ROC_DATATYPE_F32,
  ROC_DATATYPE_F64,
  ROC_DATATYPE_STRUCT,
  ROC_DATATYPE_OPAQUE,
  ROC_DATATYPE_MAX_TYPES
};

enum ROC_ACCESS_TYPE {
  ROC_ACCESS_TYPE_NONE = 0,
  ROC_ACCESS_TYPE_RO,
  ROC_ACCESS_TYPE_WO,
  ROC_ACCESS_TYPE_RW
};

class Kernel : public device::Kernel {
 public:
  struct Argument {
    uint index_;                      //!< Argument's index in the OCL signature
    std::string name_;                //!< Argument's name
    std::string typeName_;            //!< Argument's type name
    uint size_;                       //!< Size in bytes
    uint alignment_;                  //!< Argument's alignment
    uint pointeeAlignment_;           //!< Alignment of the data pointed to
    ROC_ARG_TYPE type_;               //!< Type of the argument
    ROC_ADDRESS_QUALIFIER addrQual_;  //!< Address qualifier of the argument
    ROC_DATA_TYPE dataType_;          //!< The type of data
    ROC_ACCESS_TYPE access_;          //!< Access type for the argument
  };

  Kernel(std::string name, Program* prog, const uint64_t& kernelCodeHandle,
         const uint32_t workgroupGroupSegmentByteSize,
         const uint32_t workitemPrivateSegmentByteSize, const uint32_t kernargSegmentByteSize,
         const uint32_t kernargSegmentAlignment);

  const uint64_t& KernelCodeHandle() { return kernelCodeHandle_; }

  const uint32_t WorkgroupGroupSegmentByteSize() const { return workgroupGroupSegmentByteSize_; }

  const uint32_t workitemPrivateSegmentByteSize() const { return workitemPrivateSegmentByteSize_; }

  const uint64_t KernargSegmentByteSize() const { return kernargSegmentByteSize_ + 48; }

  const uint8_t KernargSegmentAlignment() const { return kernargSegmentAlignment_; }

  ~Kernel();

  //! Initializes the metadata required for this kernel
  virtual bool init() = 0;

  const Program* program() const { return static_cast<const Program*>(program_); }

  //! Returns the kernel argument list
  const std::vector<Argument*>& hsailArgs() const { return hsailArgList_; }

  //! Returns a pointer to the hsail argument at the specified index
  Argument* hsailArgAt(size_t index) const {
    for (auto arg : hsailArgList_)
      if (arg->index_ == index) return arg;
    assert(!"Should not reach here");
    return nullptr;
  }

  //! Return printf info array
  const std::vector<PrintfInfo>& printfInfo() const { return printf_; }

  //! Return TRUE if kernel is internal blit kernel
  bool isInternalKernel() const { return (flags_.internalKernel_) ? true : false; }

  //! set internal kernel flag
  void setInternalKernelFlag(bool flag) { flags_.internalKernel_ = flag; }

  //! Return TRUE if kernel uses images
  bool imageEnable() const { return (flags_.imageEnable_) ? true : false; }

  //! Return TRUE if kernel wirtes images
  bool imageWrite() const { return (flags_.imageWrite_) ? true : false; }

 protected:
  union Flags {
    struct {
      uint internalKernel_ : 1;  //!< Is a blit kernel?
      uint imageEnable_    : 1;  //!< Kernel uses images
      uint imageWrite_     : 1;  //!< Kernel writes images
    };
    uint value_;
    Flags() : value_(0) {}
  } flags_;


  Program* program_;                //!< The roc::Program context
  std::vector<Argument*> hsailArgList_;  //!< Vector list of HSAIL Arguments
  uint64_t kernelCodeHandle_;            //!< Kernel code handle (aka amd_kernel_code_t)
  const uint32_t workgroupGroupSegmentByteSize_;
  const uint32_t workitemPrivateSegmentByteSize_;
  const uint32_t kernargSegmentByteSize_;
  const uint32_t kernargSegmentAlignment_;
  size_t kernelDirectiveOffset_;
  std::vector<PrintfInfo> printf_;
};

#if defined(WITH_COMPILER_LIB)
class HSAILKernel : public roc::Kernel {
 public:
  HSAILKernel(std::string name, Program* prog, const uint64_t& kernelCodeHandle,
              const uint32_t workgroupGroupSegmentByteSize,
              const uint32_t workitemPrivateSegmentByteSize,
              const uint32_t kernargSegmentByteSize,
              const uint32_t kernargSegmentAlignment)
   : roc::Kernel(name, prog, kernelCodeHandle, workgroupGroupSegmentByteSize,
                 workitemPrivateSegmentByteSize, kernargSegmentByteSize, kernargSegmentAlignment) {
  }

  //! Initializes the metadata required for this kernel
  virtual bool init() final;

 private:
  //! Populates hsailArgList_
  void initArguments(const aclArgData* aclArg);

  //! Initializes HSAIL Printf metadata and info
  void initPrintf(const aclPrintfFmt* aclPrintf);
};
#endif // defined(WITH_COMPILER_LIB)

#if defined(WITH_LIGHTNING_COMPILER)
class LightningKernel : public roc::Kernel {
 public:
  LightningKernel(std::string name, Program* prog, const uint64_t& kernelCodeHandle,
                  const uint32_t workgroupGroupSegmentByteSize,
                  const uint32_t workitemPrivateSegmentByteSize,
                  const uint32_t kernargSegmentByteSize,
                  const uint32_t kernargSegmentAlignment)
   : roc::Kernel(name, prog, kernelCodeHandle, workgroupGroupSegmentByteSize,
                 workitemPrivateSegmentByteSize, kernargSegmentByteSize, kernargSegmentAlignment) {
  }
  //! Initializes the metadata required for this kernel
  virtual bool init() final;

private:
  //! Initializes Hsail Argument metadata and info for LC
  void initArguments(const KernelMD& kernelMD);

  //! Initializes HSAIL Printf metadata and info for LC
  void initPrintf(const std::vector<std::string>& printfInfoStrings);
};
#endif // defined(WITH_LIGHTNING_COMPILER)

}  // namespace roc

#endif  // WITHOUT_HSA_BACKEND
