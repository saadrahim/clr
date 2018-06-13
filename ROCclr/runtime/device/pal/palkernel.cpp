//
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
//
#include "device/pal/palkernel.hpp"
#include "device/pal/palprogram.hpp"
#include "device/pal/palblit.hpp"
#include "device/pal/palconstbuf.hpp"
#include "device/pal/palsched.hpp"
#include "platform/commandqueue.hpp"
#include "utils/options.hpp"

#include "acl.h"

#include <string>
#include <memory>
#include <fstream>
#include <sstream>
#include <iostream>
#include <ctime>
#include <algorithm>

namespace pal {

inline static HSAIL_ARG_TYPE GetHSAILArgType(const aclArgData* argInfo) {
  if (argInfo->argStr[0] == '_' && argInfo->argStr[1] == '.') {
    if (strcmp(&argInfo->argStr[2], "global_offset_0") == 0) {
      return HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_X;
    } else if (strcmp(&argInfo->argStr[2], "global_offset_1") == 0) {
      return HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Y;
    } else if (strcmp(&argInfo->argStr[2], "global_offset_2") == 0) {
      return HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Z;
    } else if (strcmp(&argInfo->argStr[2], "printf_buffer") == 0) {
      return HSAIL_ARGTYPE_HIDDEN_PRINTF_BUFFER;
    } else if (strcmp(&argInfo->argStr[2], "vqueue_pointer") == 0) {
      return HSAIL_ARGTYPE_HIDDEN_DEFAULT_QUEUE;
    } else if (strcmp(&argInfo->argStr[2], "aqlwrap_pointer") == 0) {
      return HSAIL_ARGTYPE_HIDDEN_COMPLETION_ACTION;
    }
    return HSAIL_ARGTYPE_HIDDEN_NONE;
  }

  switch (argInfo->type) {
    case ARG_TYPE_POINTER:
      return HSAIL_ARGTYPE_POINTER;
    case ARG_TYPE_QUEUE:
      return HSAIL_ARGTYPE_QUEUE;
    case ARG_TYPE_VALUE:
      return (argInfo->arg.value.data == DATATYPE_struct) ? HSAIL_ARGTYPE_REFERENCE
                                                          : HSAIL_ARGTYPE_VALUE;
    case ARG_TYPE_IMAGE:
      return HSAIL_ARGTYPE_IMAGE;
    case ARG_TYPE_SAMPLER:
      return HSAIL_ARGTYPE_SAMPLER;
    case ARG_TYPE_ERROR:
    default:
      return HSAIL_ARGTYPE_ERROR;
  }
}

inline static size_t GetHSAILArgAlignment(const aclArgData* argInfo) {
  switch (argInfo->type) {
    case ARG_TYPE_POINTER:
      return sizeof(void*);
    case ARG_TYPE_VALUE:
      switch (argInfo->arg.value.data) {
        case DATATYPE_i8:
        case DATATYPE_u8:
          return 1;
        case DATATYPE_u16:
        case DATATYPE_i16:
        case DATATYPE_f16:
          return 2;
        case DATATYPE_u32:
        case DATATYPE_i32:
        case DATATYPE_f32:
          return 4;
        case DATATYPE_i64:
        case DATATYPE_u64:
        case DATATYPE_f64:
          return 8;
        case DATATYPE_struct:
          return 128;
        case DATATYPE_ERROR:
        default:
          return -1;
      }
    case ARG_TYPE_IMAGE:
      return sizeof(cl_mem);
    case ARG_TYPE_SAMPLER:
      return sizeof(cl_sampler);
    default:
      return -1;
  }
}

inline static size_t GetHSAILArgPointeeAlignment(const aclArgData* argInfo) {
  if (argInfo->type == ARG_TYPE_POINTER) {
    return argInfo->arg.pointer.align;
  }
  return 1;
}

inline static HSAIL_ACCESS_TYPE GetHSAILArgAccessType(const aclArgData* argInfo) {
  aclAccessType accessType;

  if (argInfo->type == ARG_TYPE_POINTER) {
    accessType = argInfo->arg.pointer.type;
  } else if (argInfo->type == ARG_TYPE_IMAGE) {
    accessType = argInfo->arg.image.type;
  } else {
    return HSAIL_ACCESS_TYPE_NONE;
  }
  if (accessType == ACCESS_TYPE_RO) {
    return HSAIL_ACCESS_TYPE_RO;
  } else if (accessType == ACCESS_TYPE_WO) {
    return HSAIL_ACCESS_TYPE_WO;
  }

  return HSAIL_ACCESS_TYPE_RW;
}

inline static HSAIL_ADDRESS_QUALIFIER GetHSAILAddrQual(const aclArgData* argInfo) {
  if (argInfo->type == ARG_TYPE_POINTER) {
    switch (argInfo->arg.pointer.memory) {
      case PTR_MT_UAV_CONSTANT:
      case PTR_MT_CONSTANT_EMU:
      case PTR_MT_CONSTANT:
        return HSAIL_ADDRESS_CONSTANT;
      case PTR_MT_UAV:
      case PTR_MT_GLOBAL:
        return HSAIL_ADDRESS_GLOBAL;
      case PTR_MT_LDS_EMU:
      case PTR_MT_LDS:
        return HSAIL_ADDRESS_LOCAL;
      case PTR_MT_SCRATCH_EMU:
        return HSAIL_ADDRESS_GLOBAL;
      case PTR_MT_ERROR:
      default:
        LogError("Unsupported address type");
        return HSAIL_ADDRESS_ERROR;
    }
  } else if ((argInfo->type == ARG_TYPE_IMAGE) || (argInfo->type == ARG_TYPE_SAMPLER)) {
    return HSAIL_ADDRESS_GLOBAL;
  } else if (argInfo->type == ARG_TYPE_QUEUE) {
    return HSAIL_ADDRESS_GLOBAL;
  }
  return HSAIL_ADDRESS_ERROR;
}

/* f16 returns f32 - workaround due to comp lib */
inline static HSAIL_DATA_TYPE GetHSAILDataType(const aclArgData* argInfo) {
  aclArgDataType dataType;

  if (argInfo->type == ARG_TYPE_POINTER) {
    dataType = argInfo->arg.pointer.data;
  } else if (argInfo->type == ARG_TYPE_VALUE) {
    dataType = argInfo->arg.value.data;
  } else {
    return HSAIL_DATATYPE_ERROR;
  }
  switch (dataType) {
    case DATATYPE_i1:
      return HSAIL_DATATYPE_B1;
    case DATATYPE_i8:
      return HSAIL_DATATYPE_S8;
    case DATATYPE_i16:
      return HSAIL_DATATYPE_S16;
    case DATATYPE_i32:
      return HSAIL_DATATYPE_S32;
    case DATATYPE_i64:
      return HSAIL_DATATYPE_S64;
    case DATATYPE_u8:
      return HSAIL_DATATYPE_U8;
    case DATATYPE_u16:
      return HSAIL_DATATYPE_U16;
    case DATATYPE_u32:
      return HSAIL_DATATYPE_U32;
    case DATATYPE_u64:
      return HSAIL_DATATYPE_U64;
    case DATATYPE_f16:
      return HSAIL_DATATYPE_F32;
    case DATATYPE_f32:
      return HSAIL_DATATYPE_F32;
    case DATATYPE_f64:
      return HSAIL_DATATYPE_F64;
    case DATATYPE_struct:
      return HSAIL_DATATYPE_STRUCT;
    case DATATYPE_opaque:
      return HSAIL_DATATYPE_OPAQUE;
    case DATATYPE_ERROR:
    default:
      return HSAIL_DATATYPE_ERROR;
  }
}

inline static int GetHSAILArgSize(const aclArgData* argInfo) {
  switch (argInfo->type) {
    case ARG_TYPE_POINTER:
      return sizeof(void*);
    case ARG_TYPE_VALUE:
      switch (argInfo->arg.value.data) {
        case DATATYPE_i8:
        case DATATYPE_u8:
        case DATATYPE_struct:
          return 1 * argInfo->arg.value.numElements;
        case DATATYPE_u16:
        case DATATYPE_i16:
        case DATATYPE_f16:
          return 2 * argInfo->arg.value.numElements;
        case DATATYPE_u32:
        case DATATYPE_i32:
        case DATATYPE_f32:
          return 4 * argInfo->arg.value.numElements;
        case DATATYPE_i64:
        case DATATYPE_u64:
        case DATATYPE_f64:
          return 8 * argInfo->arg.value.numElements;
        case DATATYPE_ERROR:
        default:
          return -1;
      }
    case ARG_TYPE_IMAGE:
    case ARG_TYPE_SAMPLER:
    case ARG_TYPE_QUEUE:
      return sizeof(void*);
    default:
      return -1;
  }
}

inline static uint32_t GetOclArgumentType(const HSAILKernel::Argument* arg) {
  switch (arg->type_){
    case HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_X:
      return amd::KernelParameterDescriptor::HiddenGlobalOffsetX;
    case HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Y:
      return amd::KernelParameterDescriptor::HiddenGlobalOffsetY;
    case HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Z:
      return amd::KernelParameterDescriptor::HiddenGlobalOffsetZ;
    case HSAIL_ARGTYPE_HIDDEN_PRINTF_BUFFER:
      return amd::KernelParameterDescriptor::HiddenPrintfBuffer;
    case HSAIL_ARGTYPE_HIDDEN_DEFAULT_QUEUE:
      return amd::KernelParameterDescriptor::HiddenDefaultQueue;
    case HSAIL_ARGTYPE_HIDDEN_COMPLETION_ACTION:
      return amd::KernelParameterDescriptor::HiddenCompletionAction;
    case HSAIL_ARGTYPE_POINTER:
        return amd::KernelParameterDescriptor::MemoryObject;
    case HSAIL_ARGTYPE_IMAGE:
        return amd::KernelParameterDescriptor::ImageObject;
    case HSAIL_ARGTYPE_REFERENCE:
        return amd::KernelParameterDescriptor::ReferenceObject;
    case HSAIL_ARGTYPE_VALUE:
        return amd::KernelParameterDescriptor::ValueObject;
    case HSAIL_ARGTYPE_SAMPLER:
        return amd::KernelParameterDescriptor::SamplerObject;
    case HSAIL_ARGTYPE_QUEUE:
        return amd::KernelParameterDescriptor::QueueObject;
    default:
      return amd::KernelParameterDescriptor::HiddenNone;
  }
}

inline static clk_value_type_t GetOclType(const HSAILKernel::Argument* arg) {
  static const clk_value_type_t ClkValueMapType[6][6] = {
      {T_CHAR, T_CHAR2, T_CHAR3, T_CHAR4, T_CHAR8, T_CHAR16},
      {T_SHORT, T_SHORT2, T_SHORT3, T_SHORT4, T_SHORT8, T_SHORT16},
      {T_INT, T_INT2, T_INT3, T_INT4, T_INT8, T_INT16},
      {T_LONG, T_LONG2, T_LONG3, T_LONG4, T_LONG8, T_LONG16},
      {T_FLOAT, T_FLOAT2, T_FLOAT3, T_FLOAT4, T_FLOAT8, T_FLOAT16},
      {T_DOUBLE, T_DOUBLE2, T_DOUBLE3, T_DOUBLE4, T_DOUBLE8, T_DOUBLE16},
  };

  uint sizeType;
  uint numElements;
  if (arg->type_ == HSAIL_ARGTYPE_QUEUE) {
    return T_QUEUE;
  } else if (arg->type_ == HSAIL_ARGTYPE_POINTER || arg->type_ == HSAIL_ARGTYPE_IMAGE) {
    return T_POINTER;
  } else if (arg->type_ == HSAIL_ARGTYPE_VALUE || arg->type_ == HSAIL_ARGTYPE_REFERENCE) {
    switch (arg->dataType_) {
      case HSAIL_DATATYPE_S8:
      case HSAIL_DATATYPE_U8:
        sizeType = 0;
        numElements = arg->size_;
        break;
      case HSAIL_DATATYPE_S16:
      case HSAIL_DATATYPE_U16:
        sizeType = 1;
        numElements = arg->size_ / 2;
        break;
      case HSAIL_DATATYPE_S32:
      case HSAIL_DATATYPE_U32:
        sizeType = 2;
        numElements = arg->size_ / 4;
        break;
      case HSAIL_DATATYPE_S64:
      case HSAIL_DATATYPE_U64:
        sizeType = 3;
        numElements = arg->size_ / 8;
        break;
      case HSAIL_DATATYPE_F16:
        sizeType = 4;
        numElements = arg->size_ / 2;
        break;
      case HSAIL_DATATYPE_F32:
        sizeType = 4;
        numElements = arg->size_ / 4;
        break;
      case HSAIL_DATATYPE_F64:
        sizeType = 5;
        numElements = arg->size_ / 8;
        break;
      default:
        return T_VOID;
    }

    switch (numElements) {
      case 1:
        return ClkValueMapType[sizeType][0];
      case 2:
        return ClkValueMapType[sizeType][1];
      case 3:
        return ClkValueMapType[sizeType][2];
      case 4:
        return ClkValueMapType[sizeType][3];
      case 8:
        return ClkValueMapType[sizeType][4];
      case 16:
        return ClkValueMapType[sizeType][5];
      default:
        return T_VOID;
    }
  } else if (arg->type_ == HSAIL_ARGTYPE_SAMPLER) {
    return T_SAMPLER;
  } else {
    return T_VOID;
  }
}

inline static cl_kernel_arg_address_qualifier GetOclAddrQual(const HSAILKernel::Argument* arg) {
  if (arg->type_ == HSAIL_ARGTYPE_POINTER) {
    switch (arg->addrQual_) {
      case HSAIL_ADDRESS_GLOBAL:
        return CL_KERNEL_ARG_ADDRESS_GLOBAL;
      case HSAIL_ADDRESS_CONSTANT:
        return CL_KERNEL_ARG_ADDRESS_CONSTANT;
      case HSAIL_ADDRESS_LOCAL:
        return CL_KERNEL_ARG_ADDRESS_LOCAL;
      default:
        return CL_KERNEL_ARG_ADDRESS_PRIVATE;
    }
  } else if (arg->type_ == HSAIL_ARGTYPE_IMAGE) {
    return CL_KERNEL_ARG_ADDRESS_GLOBAL;
  }
  // default for all other cases
  return CL_KERNEL_ARG_ADDRESS_PRIVATE;
}

inline static cl_kernel_arg_access_qualifier GetOclAccessQual(const HSAILKernel::Argument* arg) {
  if (arg->type_ == HSAIL_ARGTYPE_IMAGE) {
    switch (arg->access_) {
      case HSAIL_ACCESS_TYPE_RO:
        return CL_KERNEL_ARG_ACCESS_READ_ONLY;
      case HSAIL_ACCESS_TYPE_WO:
        return CL_KERNEL_ARG_ACCESS_WRITE_ONLY;
      case HSAIL_ACCESS_TYPE_RW:
        return CL_KERNEL_ARG_ACCESS_READ_WRITE;
      default:
        return CL_KERNEL_ARG_ACCESS_NONE;
    }
  }
  return CL_KERNEL_ARG_ACCESS_NONE;
}

inline static cl_kernel_arg_type_qualifier GetOclTypeQual(const aclArgData* argInfo) {
  cl_kernel_arg_type_qualifier rv = CL_KERNEL_ARG_TYPE_NONE;
  if (argInfo->type == ARG_TYPE_POINTER) {
    if (argInfo->arg.pointer.isVolatile) {
      rv |= CL_KERNEL_ARG_TYPE_VOLATILE;
    }
    if (argInfo->arg.pointer.isRestrict) {
      rv |= CL_KERNEL_ARG_TYPE_RESTRICT;
    }
    if (argInfo->arg.pointer.isPipe) {
      rv |= CL_KERNEL_ARG_TYPE_PIPE;
    }
    if (argInfo->isConst) {
      rv |= CL_KERNEL_ARG_TYPE_CONST;
    }
    switch (argInfo->arg.pointer.memory) {
      case PTR_MT_CONSTANT:
      case PTR_MT_UAV_CONSTANT:
      case PTR_MT_CONSTANT_EMU:
        rv |= CL_KERNEL_ARG_TYPE_CONST;
        break;
      default:
        break;
    }
  }
  return rv;
}

bool HSAILKernel::aqlCreateHWInfo(amd::hsa::loader::Symbol* sym) {
  if (!sym) {
    return false;
  }
  if (!sym->GetInfo(HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, reinterpret_cast<void*>(&code_))) {
    return false;
  }

  amd_kernel_code_t* akc =
      reinterpret_cast<amd_kernel_code_t*>(prog().findHostKernelAddress(code_));
  cpuAqlCode_ = akc;
  if (!sym->GetInfo(HSA_EXT_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT_SIZE,
                    reinterpret_cast<void*>(&codeSize_))) {
    return false;
  }
  size_t akc_align = 0;
  if (!sym->GetInfo(HSA_EXT_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT_ALIGN,
                    reinterpret_cast<void*>(&akc_align))) {
    return false;
  }

  assert((akc->workitem_private_segment_byte_size & 3) == 0 && "Scratch must be DWORD aligned");
  workGroupInfo_.scratchRegs_ =
      amd::alignUp(akc->workitem_private_segment_byte_size, 16) / sizeof(uint);
  workGroupInfo_.privateMemSize_ = akc->workitem_private_segment_byte_size;
  workGroupInfo_.localMemSize_ = workGroupInfo_.usedLDSSize_ =
      akc->workgroup_group_segment_byte_size;
  workGroupInfo_.usedSGPRs_ = akc->wavefront_sgpr_count;
  workGroupInfo_.usedStackSize_ = 0;
  workGroupInfo_.usedVGPRs_ = akc->workitem_vgpr_count;

  if (!prog().isNull()) {
    workGroupInfo_.availableLDSSize_ = dev().properties().gfxipProperties.shaderCore.ldsSizePerCu;
    workGroupInfo_.availableSGPRs_ =
        dev().properties().gfxipProperties.shaderCore.numAvailableSgprs;
    workGroupInfo_.availableVGPRs_ =
        dev().properties().gfxipProperties.shaderCore.numAvailableVgprs;
    workGroupInfo_.preferredSizeMultiple_ = workGroupInfo_.wavefrontPerSIMD_ =
        dev().info().wavefrontWidth_;
  } else {
    workGroupInfo_.availableLDSSize_ = 64 * Ki;
    workGroupInfo_.availableSGPRs_ = 104;
    workGroupInfo_.availableVGPRs_ = 256;
    workGroupInfo_.preferredSizeMultiple_ = workGroupInfo_.wavefrontPerSIMD_ = 64;
  }
  return true;
}

void HSAILKernel::initArgList(const aclArgData* aclArg) {
  // Initialize the hsail argument list too
  initHsailArgs(aclArg);

  // Iterate through the arguments and insert into parameterList
  device::Kernel::parameters_t params;
  device::Kernel::parameters_t hiddenParams;
  amd::KernelParameterDescriptor desc;
  size_t offset = 0;
  size_t offsetStruct = argsBufferSize();

  for (uint i = 0; aclArg->struct_size != 0; i++, aclArg++) {
    // Allocate the hidden arguments, but abstraction layer will skip them
    if (arguments_[i]->index_ == uint(-1)) {
      offset = amd::alignUp(offset, arguments_[i]->alignment_);
      desc.offset_ = offset;
      desc.size_ = arguments_[i]->size_;
      offset += arguments_[i]->size_;
      desc.info_.oclObject_ = GetOclArgumentType(arguments_[i]);
      hiddenParams.push_back(desc);
      continue;
    }

    desc.name_ = arguments_[i]->name_.c_str();
    desc.type_ = GetOclType(arguments_[i]);
    desc.addressQualifier_ = GetOclAddrQual(arguments_[i]);
    desc.accessQualifier_ = GetOclAccessQual(arguments_[i]);
    desc.typeQualifier_ = GetOclTypeQual(aclArg);
    desc.typeName_ = arguments_[i]->typeName_.c_str();
    desc.info_.oclObject_ = GetOclArgumentType(arguments_[i]);
    desc.info_.arrayIndex_ = arguments_[i]->pointeeAlignment_;

    // Make a check if it is local or global
    if (desc.addressQualifier_ == CL_KERNEL_ARG_ADDRESS_LOCAL) {
      desc.size_ = 0;
    } else {
      desc.size_ = arguments_[i]->size_;
    }

    // Make offset alignment to match CPU metadata, since
    // in multidevice config abstraction layer has a single signature
    // and CPU sends the paramaters as they are allocated in memory
    size_t size = desc.size_;
    if (size == 0) {
      // Local memory for CPU
      size = sizeof(cl_mem);
    }
    // Check if HSAIL expects data by reference and allocate it behind
    if (arguments_[i]->type_ == HSAIL_ARGTYPE_REFERENCE) {
      desc.offset_ = offsetStruct;
      // Align the offset reference
      offset = amd::alignUp(offset, sizeof(size_t));
      patchReferences_.insert({desc.offset_, offset});
      offsetStruct += size;
      // Adjust the offset of arguments
      offset += sizeof(size_t);
    } else {
      // These objects have forced data size to uint64_t
      if ((desc.info_.oclObject_ == amd::KernelParameterDescriptor::ImageObject) ||
          (desc.info_.oclObject_ == amd::KernelParameterDescriptor::SamplerObject) ||
          (desc.info_.oclObject_ == amd::KernelParameterDescriptor::QueueObject)) {
        offset = amd::alignUp(offset, sizeof(uint64_t));
        desc.offset_ = offset;
        offset += sizeof(uint64_t);
      } else {
        offset = amd::alignUp(offset, arguments_[i]->alignment_);
        desc.offset_ = offset;
        offset += size;
      }
    }
    // Update read only flag
    desc.info_.readOnly_ = (arguments_[i]->access_ == HSAIL_ACCESS_TYPE_RO) ? true : false;

    params.push_back(desc);

    if (arguments_[i]->type_ == HSAIL_ARGTYPE_IMAGE) {
      flags_.imageEna_ = true;
      if (desc.accessQualifier_ != CL_KERNEL_ARG_ACCESS_READ_ONLY) {
        flags_.imageWriteEna_ = true;
      }
    }
  }

  createSignature(params, hiddenParams, amd::KernelSignature::ABIVersion_1);
}

void HSAILKernel::initHsailArgs(const aclArgData* aclArg) {
  // Iterate through the each kernel argument
  for (uint index = 0; aclArg->struct_size != 0; aclArg++) {
    Argument* arg = new Argument;

    // Initialize HSAIL kernel argument
    arg->name_ = aclArg->argStr;
    arg->typeName_ = aclArg->typeStr;
    arg->size_ = GetHSAILArgSize(aclArg);
    arg->type_ = GetHSAILArgType(aclArg);
    arg->addrQual_ = GetHSAILAddrQual(aclArg);
    arg->dataType_ = GetHSAILDataType(aclArg);
    arg->alignment_ = GetHSAILArgAlignment(aclArg);
    arg->access_ = GetHSAILArgAccessType(aclArg);
    arg->pointeeAlignment_ = GetHSAILArgPointeeAlignment(aclArg);

    bool isHidden = arg->type_ == HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_X ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Y ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Z ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_PRINTF_BUFFER ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_DEFAULT_QUEUE ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_COMPLETION_ACTION ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_NONE;

    arg->index_ = isHidden ? uint(-1) : index++;

    arguments_.push_back(arg);
  }
}

void HSAILKernel::initPrintf(const aclPrintfFmt* aclPrintf) {
  PrintfInfo info;
  uint index = 0;
  for (; aclPrintf->struct_size != 0; aclPrintf++) {
    index = aclPrintf->ID;
    if (printf_.size() <= index) {
      printf_.resize(index + 1);
    }
    std::string pfmt = aclPrintf->fmtStr;
    info.fmtString_.clear();
    bool need_nl = true;
    for (size_t pos = 0; pos < pfmt.size(); ++pos) {
      char symbol = pfmt[pos];
      need_nl = true;
      if (symbol == '\\') {
        // Rest of the C escape sequences (e.g. \') are handled correctly
        // by the MDParser, we are not sure exactly how!
        switch (pfmt[pos + 1]) {
          case 'a':
            pos++;
            symbol = '\a';
            break;
          case 'b':
            pos++;
            symbol = '\b';
            break;
          case 'f':
            pos++;
            symbol = '\f';
            break;
          case 'n':
            pos++;
            symbol = '\n';
            need_nl = false;
            break;
          case 'r':
            pos++;
            symbol = '\r';
            break;
          case 'v':
            pos++;
            symbol = '\v';
            break;
          case '7':
            if (pfmt[pos + 2] == '2') {
              pos += 2;
              symbol = '\72';
            }
            break;
          default:
            break;
        }
      }
      info.fmtString_.push_back(symbol);
    }
    if (need_nl) {
      info.fmtString_ += "\n";
    }
    uint32_t* tmp_ptr = const_cast<uint32_t*>(aclPrintf->argSizes);
    for (uint i = 0; i < aclPrintf->numSizes; i++, tmp_ptr++) {
      info.arguments_.push_back(*tmp_ptr);
    }
    printf_[index] = info;
    info.arguments_.clear();
  }
}

HSAILKernel::HSAILKernel(std::string name, HSAILProgram* prog, std::string compileOptions)
    : device::Kernel(name),
      compileOptions_(compileOptions),
      dev_(prog->dev()),
      prog_(*prog),
      index_(0),
      code_(0),
      codeSize_(0),
      waveLimiter_(
          this,
          (prog->isNull() ? 1
                          : dev().properties().gfxipProperties.shaderCore.numCusPerShaderArray) *
              dev().hwInfo()->simdPerCU_) {
  hsa_ = true;
}

HSAILKernel::~HSAILKernel() {
  while (!arguments_.empty()) {
    Argument* arg = arguments_.back();
    delete arg;
    arguments_.pop_back();
  }
}

bool HSAILKernel::init(amd::hsa::loader::Symbol* sym, bool finalize) {
#if defined(WITH_LIGHTNING_COMPILER)
  assert(!"Should not reach here");
#else  // !defined(WITH_LIGHTNING_COMPILER)
  acl_error error = ACL_SUCCESS;
  std::string openClKernelName = openclMangledName(name());
  flags_.internalKernel_ =
      (compileOptions_.find("-cl-internal-kernel") != std::string::npos) ? true : false;
  // compile kernel down to ISA
  if (finalize) {
    std::string options(compileOptions_.c_str());
    options.append(" -just-kernel=");
    options.append(openClKernelName.c_str());
    // Append an option so that we can selectively enable a SCOption on CZ
    // whenever IOMMUv2 is enabled.
    if (dev().settings().svmFineGrainSystem_) {
      options.append(" -sc-xnack-iommu");
    }
    error = aclCompile(dev().compiler(), prog().binaryElf(), options.c_str(), ACL_TYPE_CG,
                       ACL_TYPE_ISA, nullptr);
    buildLog_ += aclGetCompilerLog(dev().compiler());
    if (error != ACL_SUCCESS) {
      LogError("Failed to finalize kernel");
      return false;
    }
  }

  aqlCreateHWInfo(sym);

  // Pull out metadata from the ELF
  size_t sizeOfArgList;
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_ARGUMENT_ARRAY,
                       openClKernelName.c_str(), nullptr, &sizeOfArgList);
  if (error != ACL_SUCCESS) {
    return false;
  }

  char* aclArgList = new char[sizeOfArgList];
  if (nullptr == aclArgList) {
    return false;
  }
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_ARGUMENT_ARRAY,
                       openClKernelName.c_str(), aclArgList, &sizeOfArgList);
  if (error != ACL_SUCCESS) {
    return false;
  }
  // Set the argList
  initArgList(reinterpret_cast<const aclArgData*>(aclArgList));
  delete[] aclArgList;

  size_t sizeOfWorkGroupSize;
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_WORK_GROUP_SIZE,
                       openClKernelName.c_str(), nullptr, &sizeOfWorkGroupSize);
  if (error != ACL_SUCCESS) {
    return false;
  }
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_WORK_GROUP_SIZE,
                       openClKernelName.c_str(), workGroupInfo_.compileSize_, &sizeOfWorkGroupSize);
  if (error != ACL_SUCCESS) {
    return false;
  }

  // Copy wavefront size
  workGroupInfo_.wavefrontSize_ = dev().info().wavefrontWidth_;
  // Find total workgroup size
  if (workGroupInfo_.compileSize_[0] != 0) {
    workGroupInfo_.size_ = workGroupInfo_.compileSize_[0] * workGroupInfo_.compileSize_[1] *
        workGroupInfo_.compileSize_[2];
  } else {
    workGroupInfo_.size_ = dev().info().preferredWorkGroupSize_;
  }

  // Pull out printf metadata from the ELF
  size_t sizeOfPrintfList;
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_GPU_PRINTF_ARRAY,
                       openClKernelName.c_str(), nullptr, &sizeOfPrintfList);
  if (error != ACL_SUCCESS) {
    return false;
  }

  // Make sure kernel has any printf info
  if (0 != sizeOfPrintfList) {
    char* aclPrintfList = new char[sizeOfPrintfList];
    if (nullptr == aclPrintfList) {
      return false;
    }
    error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_GPU_PRINTF_ARRAY,
                         openClKernelName.c_str(), aclPrintfList, &sizeOfPrintfList);
    if (error != ACL_SUCCESS) {
      return false;
    }

    // Set the PrintfList
    initPrintf(reinterpret_cast<aclPrintfFmt*>(aclPrintfList));
    delete[] aclPrintfList;
  }

  aclMetadata md;
  md.enqueue_kernel = false;
  size_t sizeOfDeviceEnqueue = sizeof(md.enqueue_kernel);
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_DEVICE_ENQUEUE,
                       openClKernelName.c_str(), &md.enqueue_kernel, &sizeOfDeviceEnqueue);
  if (error != ACL_SUCCESS) {
    return false;
  }
  flags_.dynamicParallelism_ = md.enqueue_kernel;

  md.kernel_index = -1;
  size_t sizeOfIndex = sizeof(md.kernel_index);
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_KERNEL_INDEX,
                       openClKernelName.c_str(), &md.kernel_index, &sizeOfIndex);
  if (error != ACL_SUCCESS) {
    return false;
  }
  index_ = md.kernel_index;

  size_t sizeOfWavesPerSimdHint = sizeof(workGroupInfo_.wavesPerSimdHint_);
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_WAVES_PER_SIMD_HINT,
                       openClKernelName.c_str(), &workGroupInfo_.wavesPerSimdHint_,
                       &sizeOfWavesPerSimdHint);
  if (error != ACL_SUCCESS) {
    return false;
  }

  waveLimiter_.enable();

  size_t sizeOfWorkGroupSizeHint = sizeof(workGroupInfo_.compileSizeHint_);
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_WORK_GROUP_SIZE_HINT,
                       openClKernelName.c_str(), workGroupInfo_.compileSizeHint_,
                       &sizeOfWorkGroupSizeHint);
  if (error != ACL_SUCCESS) {
    return false;
  }

  size_t sizeOfVecTypeHint;
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_VEC_TYPE_HINT,
                       openClKernelName.c_str(), NULL, &sizeOfVecTypeHint);
  if (error != ACL_SUCCESS) {
    return false;
  }

  if (0 != sizeOfVecTypeHint) {
    char* VecTypeHint = new char[sizeOfVecTypeHint + 1];
    if (NULL == VecTypeHint) {
      return false;
    }
    error = aclQueryInfo(dev().compiler(), prog().binaryElf(), RT_VEC_TYPE_HINT,
                         openClKernelName.c_str(), VecTypeHint, &sizeOfVecTypeHint);
    if (error != ACL_SUCCESS) {
      return false;
    }
    VecTypeHint[sizeOfVecTypeHint] = '\0';
    workGroupInfo_.compileVecTypeHint_ = std::string(VecTypeHint);
    delete[] VecTypeHint;
  }

#endif  // !defined(WITH_LIGHTNING_COMPILER)
  return true;
}

const Device& HSAILKernel::dev() const { return reinterpret_cast<const Device&>(dev_); }

const HSAILProgram& HSAILKernel::prog() const {
  return reinterpret_cast<const HSAILProgram&>(prog_);
}

void HSAILKernel::findLocalWorkSize(size_t workDim, const amd::NDRange& gblWorkSize,
                                    amd::NDRange& lclWorkSize) const {
  // Initialize the default workgoup info
  // Check if the kernel has the compiled sizes
  if (workGroupInfo()->compileSize_[0] == 0) {
    // Find the default local workgroup size, if it wasn't specified
    if (lclWorkSize[0] == 0) {
      bool b1DOverrideSet = !flagIsDefault(GPU_MAX_WORKGROUP_SIZE);
      bool b2DOverrideSet = !flagIsDefault(GPU_MAX_WORKGROUP_SIZE_2D_X) ||
          !flagIsDefault(GPU_MAX_WORKGROUP_SIZE_2D_Y);
      bool b3DOverrideSet = !flagIsDefault(GPU_MAX_WORKGROUP_SIZE_3D_X) ||
          !flagIsDefault(GPU_MAX_WORKGROUP_SIZE_3D_Y) ||
          !flagIsDefault(GPU_MAX_WORKGROUP_SIZE_3D_Z);

      bool overrideSet = ((workDim == 1) && b1DOverrideSet) || ((workDim == 2) && b2DOverrideSet) ||
          ((workDim == 3) && b3DOverrideSet);
      if (!overrideSet) {
        // Find threads per group
        size_t thrPerGrp = workGroupInfo()->size_;

        // Check if kernel uses images
        if (flags_.imageEna_ &&
            // and thread group is a multiple value of wavefronts
            ((thrPerGrp % workGroupInfo()->wavefrontSize_) == 0) &&
            // and it's 2 or 3-dimensional workload
            (workDim > 1) && ((dev().settings().partialDispatch_) ||
                              (((gblWorkSize[0] % 16) == 0) && ((gblWorkSize[1] % 16) == 0)))) {
          // Use 8x8 workgroup size if kernel has image writes
          if (flags_.imageWriteEna_ || (thrPerGrp != dev().info().preferredWorkGroupSize_)) {
            lclWorkSize[0] = 8;
            lclWorkSize[1] = 8;
          } else {
            lclWorkSize[0] = 16;
            lclWorkSize[1] = 16;
          }
          if (workDim == 3) {
            lclWorkSize[2] = 1;
          }
        } else {
          size_t tmp = thrPerGrp;
          // Split the local workgroup into the most efficient way
          for (uint d = 0; d < workDim; ++d) {
            size_t div = tmp;
            for (; (gblWorkSize[d] % div) != 0; div--)
              ;
            lclWorkSize[d] = div;
            tmp /= div;
          }

          // Assuming DWORD access
          const uint cacheLineMatch = dev().settings().cacheLineSize_ >> 2;

          // Check if partial dispatch is enabled and
          if (dev().settings().partialDispatch_ &&
            // we couldn't find optimal workload
            (((lclWorkSize.product() % workGroupInfo()->wavefrontSize_) != 0) ||
                // or size is too small for the cache line
            (lclWorkSize[0] < cacheLineMatch))) {
            size_t maxSize = 0;
            size_t maxDim = 0;
            for (uint d = 0; d < workDim; ++d) {
              if (maxSize < gblWorkSize[d]) {
                maxSize = gblWorkSize[d];
                maxDim = d;
              }
            }
            // Use X dimension as high priority. Runtime will assume that
            // X dimension is more important for the address calculation
            if ((maxDim != 0) && (gblWorkSize[0] >= (cacheLineMatch / 2))) {
              lclWorkSize[0] = cacheLineMatch;
              thrPerGrp /= cacheLineMatch;
              lclWorkSize[maxDim] = thrPerGrp;
              for (uint d = 1; d < workDim; ++d) {
                if (d != maxDim) {
                  lclWorkSize[d] = 1;
                }
              }
            }
            else {
              // Check if a local workgroup has the most optimal size
              if (thrPerGrp > maxSize) {
                thrPerGrp = maxSize;
              }
              lclWorkSize[maxDim] = thrPerGrp;
              for (uint d = 0; d < workDim; ++d) {
                if (d != maxDim) {
                  lclWorkSize[d] = 1;
                }
              }
            }
          }
        }
      } else {
        // Use overrides when app doesn't provide workgroup dimensions
        if (workDim == 1) {
          lclWorkSize[0] = GPU_MAX_WORKGROUP_SIZE;
        } else if (workDim == 2) {
          lclWorkSize[0] = GPU_MAX_WORKGROUP_SIZE_2D_X;
          lclWorkSize[1] = GPU_MAX_WORKGROUP_SIZE_2D_Y;
        } else if (workDim == 3) {
          lclWorkSize[0] = GPU_MAX_WORKGROUP_SIZE_3D_X;
          lclWorkSize[1] = GPU_MAX_WORKGROUP_SIZE_3D_Y;
          lclWorkSize[2] = GPU_MAX_WORKGROUP_SIZE_3D_Z;
        } else {
          assert(0 && "Invalid workDim!");
        }
      }
    }
  } else {
    for (uint d = 0; d < workDim; ++d) {
      lclWorkSize[d] = workGroupInfo()->compileSize_[d];
    }
  }
}

hsa_kernel_dispatch_packet_t* HSAILKernel::loadArguments(
    VirtualGPU& gpu, const amd::Kernel& kernel, const amd::NDRangeContainer& sizes,
    const_address parameters, size_t ldsAddress, uint64_t vmDefQueue, uint64_t* vmParentWrap) const {
  uint64_t argList;
  address aqlArgBuf = gpu.managedBuffer().reserve(
    argsBufferSize() + sizeof(hsa_kernel_dispatch_packet_t), &argList);
  gpu.addVmMemory(gpu.managedBuffer().activeMemory());

  if (dynamicParallelism()) {
    // Provide the host parent AQL wrap object to the kernel
    AmdAqlWrap wrap = {};
    wrap.state = AQL_WRAP_BUSY;
    *vmParentWrap = gpu.cb(1)->UploadDataToHw(&wrap, sizeof(AmdAqlWrap));
    gpu.addVmMemory(gpu.cb(1)->ActiveMemory());
  }

  const amd::KernelSignature& signature = kernel.signature();

  // Check if runtime has to setup hidden arguments
  for (const auto& it : signature.hiddenParameters()) {
    size_t offset;
    switch (it.info_.oclObject_) {
      case amd::KernelParameterDescriptor::HiddenNone:
        // void* zero = 0;
        // WriteAqlArgAt(const_cast<address>(parameters), &zero, it.size_, it.offset_);
        break;
      case amd::KernelParameterDescriptor::HiddenGlobalOffsetX:
        offset = sizes.offset()[0];
        WriteAqlArgAt(const_cast<address>(parameters), &offset, it.size_, it.offset_);
        break;
      case amd::KernelParameterDescriptor::HiddenGlobalOffsetY:
        if (sizes.dimensions() >= 2) {
            offset = sizes.offset()[1];
            WriteAqlArgAt(const_cast<address>(parameters), &offset, it.size_, it.offset_);
        }
        break;
      case amd::KernelParameterDescriptor::HiddenGlobalOffsetZ:
        if (sizes.dimensions() >= 3) {
          offset = sizes.offset()[2];
          WriteAqlArgAt(const_cast<address>(parameters), &offset, it.size_, it.offset_);
        }
        break;
      case amd::KernelParameterDescriptor::HiddenPrintfBuffer:
        if ((printfInfo().size() > 0) &&
            // and printf buffer was allocated
            (gpu.printfDbgHSA().dbgBuffer() != nullptr)) {
          // and set the fourth argument as the printf_buffer pointer
          size_t bufferPtr = static_cast<size_t>(gpu.printfDbgHSA().
            dbgBuffer()->vmAddress());
          gpu.addVmMemory(gpu.printfDbgHSA().dbgBuffer());
          WriteAqlArgAt(const_cast<address>(parameters), &bufferPtr, it.size_, it.offset_);
        }
        break;
      case amd::KernelParameterDescriptor::HiddenDefaultQueue:
        if (vmDefQueue != 0) {
          WriteAqlArgAt(const_cast<address>(parameters), &vmDefQueue, it.size_, it.offset_);
        }
        break;
      case amd::KernelParameterDescriptor::HiddenCompletionAction:
        if (*vmParentWrap != 0) {
          WriteAqlArgAt(const_cast<address>(parameters), vmParentWrap, it.size_, it.offset_);
        }
        break;
    }
  }

  // Load all kernel arguments
  WriteAqlArgAt(aqlArgBuf, parameters, argsBufferSize(), 0);
  // Note: In a case of structs the size won't match,
  // since HSAIL compiler expects a reference...
  assert(argsBufferSize() <= signature.paramsSize() &&
    "A mismatch of sizes of arguments between compiler and runtime!");

  //hsa_kernel_dispatch_packet_t disp;
  hsa_kernel_dispatch_packet_t* hsaDisp = reinterpret_cast<hsa_kernel_dispatch_packet_t*>(
    gpu.cb(0)->SysMemCopy());

  amd::NDRange local(sizes.local());
  const amd::NDRange& global = sizes.global();

  // Check if runtime has to find local workgroup size
  findLocalWorkSize(sizes.dimensions(), sizes.global(), local);

  constexpr uint16_t kDispatchPacketHeader =
    (HSA_PACKET_TYPE_KERNEL_DISPATCH << HSA_PACKET_HEADER_TYPE) |
    (1 << HSA_PACKET_HEADER_BARRIER) |
    (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_ACQUIRE_FENCE_SCOPE) |
    (HSA_FENCE_SCOPE_AGENT << HSA_PACKET_HEADER_RELEASE_FENCE_SCOPE);

  hsaDisp->header = kDispatchPacketHeader;
  hsaDisp->setup = sizes.dimensions();

  hsaDisp->workgroup_size_x = local[0];
  hsaDisp->workgroup_size_y = (sizes.dimensions() > 1) ? local[1] : 1;
  hsaDisp->workgroup_size_z = (sizes.dimensions() > 2) ? local[2] : 1;

  hsaDisp->grid_size_x = global[0];
  hsaDisp->grid_size_y = (sizes.dimensions() > 1) ? global[1] : 1;
  hsaDisp->grid_size_z = (sizes.dimensions() > 2) ? global[2] : 1;
  hsaDisp->reserved2 = 0;

  // Initialize kernel ISA and execution buffer requirements
  hsaDisp->private_segment_size = spillSegSize();
  hsaDisp->group_segment_size = ldsAddress - ldsSize();
  hsaDisp->kernel_object = gpuAqlCode();

  hsaDisp->kernarg_address = reinterpret_cast<void*>(argList);
  hsaDisp->reserved2 = 0;
  hsaDisp->completion_signal.handle = 0;
  memcpy(aqlArgBuf + argsBufferSize(), hsaDisp, sizeof(hsa_kernel_dispatch_packet_t));

  if (AMD_HSA_BITS_GET(cpuAqlCode_->kernel_code_properties,
      AMD_KERNEL_CODE_PROPERTIES_ENABLE_SGPR_QUEUE_PTR)) {
    gpu.addVmMemory(gpu.hsaQueueMem());
  }

  return hsaDisp;
}

#if defined(WITH_LIGHTNING_COMPILER)

using llvm::AMDGPU::HSAMD::AccessQualifier;
using llvm::AMDGPU::HSAMD::AddressSpaceQualifier;
using llvm::AMDGPU::HSAMD::ValueKind;
using llvm::AMDGPU::HSAMD::ValueType;

const LightningProgram& LightningKernel::prog() const {
  return reinterpret_cast<const LightningProgram&>(prog_);
}

void LightningKernel::initPrintf(const std::vector<std::string>& printfInfoStrings) {
  for (auto str : printfInfoStrings) {
    std::vector<std::string> tokens;

    size_t end, pos = 0;
    do {
      end = str.find_first_of(':', pos);
      tokens.push_back(str.substr(pos, end - pos));
      pos = end + 1;
    } while (end != std::string::npos);

    if (tokens.size() < 2) {
      LogPrintfWarning("Invalid PrintInfo string: \"%s\"", str.c_str());
      continue;
    }

    pos = 0;
    size_t printfInfoID = std::stoi(tokens[pos++]);
    if (printf_.size() <= printfInfoID) {
      printf_.resize(printfInfoID + 1);
    }
    PrintfInfo& info = printf_[printfInfoID];

    size_t numSizes = std::stoi(tokens[pos++]);
    end = pos + numSizes;

    // ensure that we have the correct number of tokens
    if (tokens.size() < end + 1 /*last token is the fmtString*/) {
      LogPrintfWarning("Invalid PrintInfo string: \"%s\"", str.c_str());
      continue;
    }

    // push the argument sizes
    while (pos < end) {
      info.arguments_.push_back(std::stoi(tokens[pos++]));
    }

    // FIXME: We should not need this! [
    std::string& fmt = tokens[pos];
    bool need_nl = true;

    for (pos = 0; pos < fmt.size(); ++pos) {
      char symbol = fmt[pos];
      need_nl = true;
      if (symbol == '\\') {
        switch (fmt[pos + 1]) {
          case 'a':
            pos++;
            symbol = '\a';
            break;
          case 'b':
            pos++;
            symbol = '\b';
            break;
          case 'f':
            pos++;
            symbol = '\f';
            break;
          case 'n':
            pos++;
            symbol = '\n';
            need_nl = false;
            break;
          case 'r':
            pos++;
            symbol = '\r';
            break;
          case 'v':
            pos++;
            symbol = '\v';
            break;
          case '7':
            if (fmt[pos + 2] == '2') {
              pos += 2;
              symbol = '\72';
            }
            break;
          default:
            break;
        }
      }
      info.fmtString_.push_back(symbol);
    }
    if (need_nl) {
      info.fmtString_ += "\n";
    }
    // ]
  }
}

static inline HSAIL_ARG_TYPE GetKernelArgType(const KernelArgMD& lcArg) {
  switch (lcArg.mValueKind) {
    case ValueKind::GlobalBuffer:
    case ValueKind::DynamicSharedPointer:
    case ValueKind::Pipe:
      return HSAIL_ARGTYPE_POINTER;
    case ValueKind::ByValue:
      return HSAIL_ARGTYPE_VALUE;
    case ValueKind::Image:
      return HSAIL_ARGTYPE_IMAGE;
    case ValueKind::Sampler:
      return HSAIL_ARGTYPE_SAMPLER;
    case ValueKind::HiddenGlobalOffsetX:
      return HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_X;
    case ValueKind::HiddenGlobalOffsetY:
      return HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Y;
    case ValueKind::HiddenGlobalOffsetZ:
      return HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Z;
    case ValueKind::HiddenPrintfBuffer:
      return HSAIL_ARGTYPE_HIDDEN_PRINTF_BUFFER;
    case ValueKind::HiddenDefaultQueue:
      return HSAIL_ARGTYPE_HIDDEN_DEFAULT_QUEUE;
    case ValueKind::HiddenCompletionAction:
      return HSAIL_ARGTYPE_HIDDEN_COMPLETION_ACTION;
    case ValueKind::HiddenNone:
      return HSAIL_ARGTYPE_HIDDEN_NONE;
    default:
      return HSAIL_ARGTYPE_ERROR;
  }
}

static inline size_t GetKernelArgAlignment(const KernelArgMD& lcArg) { return lcArg.mAlign; }

static inline size_t GetKernelArgPointeeAlignment(const KernelArgMD& lcArg) {
  if (lcArg.mValueKind == ValueKind::DynamicSharedPointer) {
    uint32_t align = lcArg.mPointeeAlign;
    if (align == 0) {
      LogWarning("Missing DynamicSharedPointer alignment");
      align = 128; /* worst case alignment */
      ;
    }
    return align;
  }
  return 1;
}

static inline HSAIL_ACCESS_TYPE GetKernelArgAccessType(const KernelArgMD& lcArg) {
  if (lcArg.mValueKind == ValueKind::GlobalBuffer || lcArg.mValueKind == ValueKind::Image) {
    switch (lcArg.mAccQual) {
      case AccessQualifier::ReadOnly:
        return HSAIL_ACCESS_TYPE_RO;
      case AccessQualifier::WriteOnly:
        return HSAIL_ACCESS_TYPE_WO;
      case AccessQualifier::ReadWrite:
      default:
        return HSAIL_ACCESS_TYPE_RW;
    }
  }
  return HSAIL_ACCESS_TYPE_NONE;
}

static inline HSAIL_ADDRESS_QUALIFIER GetKernelAddrQual(const KernelArgMD& lcArg) {
  if (lcArg.mValueKind == ValueKind::DynamicSharedPointer) {
    return HSAIL_ADDRESS_LOCAL;
  } else if (lcArg.mValueKind == ValueKind::GlobalBuffer) {
    if (lcArg.mAddrSpaceQual == AddressSpaceQualifier::Global) {
      return HSAIL_ADDRESS_GLOBAL;
    } else if (lcArg.mAddrSpaceQual == AddressSpaceQualifier::Constant) {
      return HSAIL_ADDRESS_CONSTANT;
    }
    LogError("Unsupported address type");
    return HSAIL_ADDRESS_ERROR;
  } else if (lcArg.mValueKind == ValueKind::Image ||
             lcArg.mValueKind == ValueKind::Sampler ||
             lcArg.mValueKind == ValueKind::Pipe) {
    return HSAIL_ADDRESS_GLOBAL;
  }
  return HSAIL_ADDRESS_ERROR;
}

static inline HSAIL_DATA_TYPE GetKernelDataType(const KernelArgMD& lcArg) {
  if (lcArg.mValueKind != ValueKind::ByValue) {
    return HSAIL_DATATYPE_ERROR;
  }

  switch (lcArg.mValueType) {
    case ValueType::I8:
      return HSAIL_DATATYPE_S8;
    case ValueType::I16:
      return HSAIL_DATATYPE_S16;
    case ValueType::I32:
      return HSAIL_DATATYPE_S32;
    case ValueType::I64:
      return HSAIL_DATATYPE_S64;
    case ValueType::U8:
      return HSAIL_DATATYPE_U8;
    case ValueType::U16:
      return HSAIL_DATATYPE_U16;
    case ValueType::U32:
      return HSAIL_DATATYPE_U32;
    case ValueType::U64:
      return HSAIL_DATATYPE_U64;
    case ValueType::F16:
      return HSAIL_DATATYPE_F16;
    case ValueType::F32:
      return HSAIL_DATATYPE_F32;
    case ValueType::F64:
      return HSAIL_DATATYPE_F64;
    case ValueType::Struct:
      return HSAIL_DATATYPE_STRUCT;
    default:
      return HSAIL_DATATYPE_ERROR;
  }
}

static inline cl_kernel_arg_type_qualifier GetOclTypeQual(const KernelArgMD& lcArg) {
  cl_kernel_arg_type_qualifier rv = CL_KERNEL_ARG_TYPE_NONE;
  if (lcArg.mValueKind == ValueKind::GlobalBuffer ||
      lcArg.mValueKind == ValueKind::DynamicSharedPointer) {
    if (lcArg.mIsVolatile) {
      rv |= CL_KERNEL_ARG_TYPE_VOLATILE;
    }
    if (lcArg.mIsRestrict) {
      rv |= CL_KERNEL_ARG_TYPE_RESTRICT;
    }
    if (lcArg.mIsConst) {
      rv |= CL_KERNEL_ARG_TYPE_CONST;
    }
  }
  else if (lcArg.mIsPipe) {
    assert(lcArg.mValueKind == ValueKind::Pipe);
    rv |= CL_KERNEL_ARG_TYPE_PIPE;
  }
  return rv;
}

void LightningKernel::initArgList(const KernelMD& kernelMD) {
  device::Kernel::parameters_t params;
  device::Kernel::parameters_t hiddenParams;
  size_t offsetStruct = argsBufferSize();

  size_t offset = 0;

  for (size_t i = 0; i < kernelMD.mArgs.size(); ++i) {
    const KernelArgMD& lcArg = kernelMD.mArgs[i];

    // Initialize HSAIL kernel argument
    auto arg = new HSAILKernel::Argument;
    arg->name_ = lcArg.mName;
    arg->typeName_ = lcArg.mTypeName;
    arg->size_ = lcArg.mSize;
    arg->type_ = GetKernelArgType(lcArg);
    arg->addrQual_ = GetKernelAddrQual(lcArg);
    arg->dataType_ = GetKernelDataType(lcArg);
    arg->alignment_ = GetKernelArgAlignment(lcArg);
    arg->access_ = GetKernelArgAccessType(lcArg);
    arg->pointeeAlignment_ = GetKernelArgPointeeAlignment(lcArg);

    bool isHidden = arg->type_ == HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_X ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Y ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_GLOBAL_OFFSET_Z ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_PRINTF_BUFFER ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_DEFAULT_QUEUE ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_COMPLETION_ACTION ||
        arg->type_ == HSAIL_ARGTYPE_HIDDEN_NONE;

    arg->index_ = isHidden ? uint(-1) : params.size();
    arguments_.push_back(arg);
    // Initialize Device kernel parameters
    amd::KernelParameterDescriptor desc;

    if (isHidden) {
      offset = amd::alignUp(offset, arguments_[i]->alignment_);
      desc.offset_ = offset;
      desc.size_ = arguments_[i]->size_;
      offset += arguments_[i]->size_;
      desc.info_.oclObject_ = GetOclArgumentType(arguments_[i]);
      hiddenParams.push_back(desc);
      continue;
    }

    desc.name_ = lcArg.mName.c_str();
    desc.type_ = GetOclType(arg);
    desc.addressQualifier_ = GetOclAddrQual(arg);
    desc.accessQualifier_ = GetOclAccessQual(arg);
    desc.typeQualifier_ = GetOclTypeQual(lcArg);
    desc.typeName_ = lcArg.mTypeName.c_str();
    desc.info_.oclObject_ = GetOclArgumentType(arg);
    desc.info_.arrayIndex_ = arg->pointeeAlignment_;

    // Make a check if it is local or global
    if (desc.addressQualifier_ == CL_KERNEL_ARG_ADDRESS_LOCAL) {
      desc.size_ = 0;
    } else {
      desc.size_ = arg->size_;
    }

    // Make offset alignment to match CPU metadata, since
    // in multidevice config abstraction layer has a single signature
    // and CPU sends the parameters as they are allocated in memory
    size_t size = desc.size_;
    if (size == 0) {
      // Local memory for CPU
      size = sizeof(cl_mem);
    }
    // Check if HSAIL expects data by reference and allocate it behind
    if (arguments_[i]->type_ == HSAIL_ARGTYPE_REFERENCE) {
      desc.offset_ = offsetStruct;
      // Align the offset reference
      offset = amd::alignUp(offset, sizeof(size_t));
      patchReferences_.insert({ desc.offset_, offset });
      offsetStruct += size;
      // Adjust the offset of arguments
      offset += sizeof(size_t);
    }
    else {
      // These objects have forced data size to uint64_t
      if ((desc.info_.oclObject_ == amd::KernelParameterDescriptor::ImageObject) ||
          (desc.info_.oclObject_ == amd::KernelParameterDescriptor::SamplerObject) ||
          (desc.info_.oclObject_ == amd::KernelParameterDescriptor::QueueObject)) {
        offset = amd::alignUp(offset, sizeof(uint64_t));
        desc.offset_ = offset;
        offset += sizeof(uint64_t);
      } else {
        offset = amd::alignUp(offset, arguments_[i]->alignment_);
        desc.offset_ = offset;
        offset += size;
      }
    }
    // Update read only flag
    desc.info_.readOnly_ = (arguments_[i]->access_ == HSAIL_ACCESS_TYPE_RO) ? true : false;

    params.push_back(desc);
  }

  createSignature(params, hiddenParams, amd::KernelSignature::ABIVersion_1);
}

static const KernelMD* FindKernelMetadata(const CodeObjectMD* programMD, const std::string& name) {
  for (const KernelMD& kernelMD : programMD->mKernels) {
    if (kernelMD.mName == name) {
      return &kernelMD;
    }
  }
  return nullptr;
}

bool LightningKernel::init(amd::hsa::loader::Symbol* symbol) {
  flags_.internalKernel_ =
      (compileOptions_.find("-cl-internal-kernel") != std::string::npos) ? true : false;

  aqlCreateHWInfo(symbol);

  const CodeObjectMD* programMD = prog().metadata();
  assert(programMD != nullptr);

  const KernelMD* kernelMD = FindKernelMetadata(programMD, name());

  if (kernelMD == nullptr) {
    return false;
  }

  // Set the argList
  initArgList(*kernelMD);

  if (!kernelMD->mAttrs.mReqdWorkGroupSize.empty()) {
    const auto& requiredWorkgroupSize = kernelMD->mAttrs.mReqdWorkGroupSize;
    workGroupInfo_.compileSize_[0] = requiredWorkgroupSize[0];
    workGroupInfo_.compileSize_[1] = requiredWorkgroupSize[1];
    workGroupInfo_.compileSize_[2] = requiredWorkgroupSize[2];
  }

  if (!kernelMD->mAttrs.mWorkGroupSizeHint.empty()) {
    const auto& workgroupSizeHint = kernelMD->mAttrs.mWorkGroupSizeHint;
    workGroupInfo_.compileSizeHint_[0] = workgroupSizeHint[0];
    workGroupInfo_.compileSizeHint_[1] = workgroupSizeHint[1];
    workGroupInfo_.compileSizeHint_[2] = workgroupSizeHint[2];
  }

  if (!kernelMD->mAttrs.mVecTypeHint.empty()) {
    workGroupInfo_.compileVecTypeHint_ = kernelMD->mAttrs.mVecTypeHint.c_str();
  }

  // Copy wavefront size
  workGroupInfo_.wavefrontSize_ = dev().info().wavefrontWidth_;

  workGroupInfo_.size_ = kernelMD->mCodeProps.mMaxFlatWorkGroupSize;
  if (workGroupInfo_.size_ == 0) {
    return false;
  }

  initPrintf(programMD->mPrintf);

  /*FIXME_lmoriche:
  size_t sizeOfWavesPerSimdHint = sizeof(workGroupInfo_.wavesPerSimdHint_);
  error = aclQueryInfo(dev().compiler(), prog().binaryElf(),
      RT_WAVES_PER_SIMD_HINT, openClKernelName.c_str(),
      &workGroupInfo_.wavesPerSimdHint_, &sizeOfWavesPerSimdHint);
  if (error != ACL_SUCCESS) {
      return false;
  }

  waveLimiter_.enable();
  */

  return true;
}
#endif  // defined(WITH_LIGHTNING_COMPILER)


}  // namespace pal
