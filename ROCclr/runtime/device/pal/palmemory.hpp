//
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
//
#pragma once

#include "top.hpp"
#include "thread/atomic.hpp"
#include "device/pal/palresource.hpp"
#include <map>

/*! \addtogroup GPU
 *  @{
 */
namespace device {
class Memory;
}

//! PAL Device Implementation
namespace pal {

class Device;
class Heap;
class Resource;
class Memory;
class VirtualGPU;

//! GPU memory object.
//  Wrapper that can contain a heap block or an interop buffer/image.
class Memory : public device::Memory, public Resource {
 public:
  //! Constructor (with owner)
  Memory(const Device& gpuDev,  //!< GPU device object
         amd::Memory& owner,    //!< Abstraction layer memory object
         size_t size            //!< Memory size for allocation
         );

  //! Constructor (nonfat version for local scratch mem use without heap block)
  Memory(const Device& gpuDev,  //!< GPU device object
         size_t size            //!< Memory size for allocation
         );

  //! Constructor memory for images (without global heap allocation)
  Memory(const Device& gpuDev,          //!< GPU device object
         amd::Memory& owner,            //!< Abstraction layer memory object
         size_t width,                  //!< Allocated memory width
         size_t height,                 //!< Allocated memory height
         size_t depth,                  //!< Allocated memory depth
         cl_image_format format,        //!< Memory format
         cl_mem_object_type imageType,  //!< CL image type
         uint mipLevels                 //!< The number of mip levels
         );

  //! Constructor memory for images (without global heap allocation)
  Memory(const Device& gpuDev,          //!< GPU device object
         size_t size,                   //!< Memory object size
         size_t width,                  //!< Allocated memory width
         size_t height,                 //!< Allocated memory height
         size_t depth,                  //!< Allocated memory depth
         cl_image_format format,        //!< Memory format
         cl_mem_object_type imageType,  //!< CL image type
         uint mipLevels                 //!< The number of mip levels
         );

  //! Default destructor
  ~Memory();

  //! Creates the interop memory
  bool createInterop();

  //! Overloads the resource create method
  virtual bool create(Resource::MemoryType memType,          //!< Memory type
                      Resource::CreateParams* params = NULL  //!< Prameters for create
                      );

  //! Allocate memory for API-level maps
  virtual void* allocMapTarget(const amd::Coord3D& origin,  //!< The map location in memory
                               const amd::Coord3D& region,  //!< The map region in memory
                               uint mapFlags,               //!< Map flags
                               size_t* rowPitch = NULL,     //!< Row pitch for the mapped memory
                               size_t* slicePitch = NULL    //!< Slice for the mapped memory
                               );

  //! Pins system memory associated with this memory object
  virtual bool pinSystemMemory(void* hostPtr,  //!< System memory address
                               size_t size     //!< Size of allocated system memory
                               );

  //! Releases indirect map surface
  virtual void releaseIndirectMap() { decIndMapCount(); }

  //! Map the device memory to CPU visible
  virtual void* cpuMap(device::VirtualDevice& vDev,  //!< Virtual device for map operaiton
                       uint flags = 0,               //!< flags for the map operation
                       // Optimization for multilayer map/unmap
                       uint startLayer = 0,       //!< Start layer for multilayer map
                       uint numLayers = 0,        //!< End layer for multilayer map
                       size_t* rowPitch = NULL,   //!< Row pitch for the device memory
                       size_t* slicePitch = NULL  //!< Slice pitch for the device memory
                       );

  //! Unmap the device memory
  virtual void cpuUnmap(device::VirtualDevice& vDev  //!< Virtual device for unmap operaiton
                        );

  //! Updates device memory from the owner's host allocation
  void syncCacheFromHost(VirtualGPU& gpu,  //!< Virtual GPU device object
                         //! Synchronization flags
                         device::Memory::SyncFlags syncFlags = device::Memory::SyncFlags());

  //! Updates the owner's host allocation from device memory
  virtual void syncHostFromCache(
      //! Synchronization flags
      device::Memory::SyncFlags syncFlags = device::Memory::SyncFlags());

  //! Creates a view from current resource
  virtual Memory* createBufferView(
      amd::Memory& subBufferOwner  //!< The abstraction layer subbuf owner
      );

  //! Allocates host memory for synchronization with MGPU context
  void mgpuCacheWriteBack();

  //! Transfers objects data to the destination object
  bool moveTo(Memory& dst);

  //! Accessors for indirect map memory object
  Memory* mapMemory() const;

  // Decompress GL depth-stencil/MSAA resources for CL access
  // Invalidates any FBOs the resource may be bound to, otherwise the GL driver may crash.
  virtual bool processGLResource(GLResourceOP operation);

  //! Returns the interop resource for this memory object
  const Memory* parent() const { return parent_; }

  //! Returns TRUE if direct map is acceaptable. The method detects
  //! forced USWC memory on APU and will cause a switch to
  //! indirect map for allocations with a possibility of host read
  bool isDirectMap() {
    return (isCacheable() || !isHostMemDirectAccess() ||
            (owner()->getMemFlags() &
             (CL_MEM_ALLOC_HOST_PTR | CL_MEM_HOST_WRITE_ONLY | CL_MEM_READ_ONLY)));
  }

  //! Quick view update for managed buffers. It should avoid expensive object allocations
  void updateView(Resource* view, size_t offset, size_t size) {
    size_ = size;
    flags_ |= HostMemoryDirectAccess;
    Resource::updateView(view, offset, size);
  }

 protected:
  //! Decrement map count
  void decIndMapCount();

 private:
  //! Disable copy constructor
  Memory(const Memory&);

  //! Disable operator=
  Memory& operator=(const Memory&);

  Memory* pinnedMemory_;     //!< Memory used as pinned system memory
  const Memory* parent_;     //!< Parent memory object
};

class Buffer : public pal::Memory {
 public:
  //! Buffer constructor
  Buffer(const Device& gpuDev,  //!< GPU device object
         amd::Memory& owner,    //!< Abstraction layer memory object
         size_t size            //!< Buffer size
         )
      : pal::Memory(gpuDev, owner, size) {}

  //! Creates a view from current resource
  virtual Memory* createBufferView(
      amd::Memory& subBufferOwner  //!< The abstraction layer subbuf owner
      ) const;

 private:
  //! Disable copy constructor
  Buffer(const Buffer&);

  //! Disable operator=
  Buffer& operator=(const Buffer&);
};

class Image : public pal::Memory {
 public:
  //! Image constructor
  Image(const Device& gpuDev,          //!< GPU device object
        amd::Memory& owner,            //!< Abstraction layer memory object
        size_t width,                  //!< Allocated memory width
        size_t height,                 //!< Allocated memory height
        size_t depth,                  //!< Allocated memory depth
        cl_image_format format,        //!< Memory format
        cl_mem_object_type imageType,  //!< CL image type
        uint mipLevels                 //!< The number of mip levels
        )
      : pal::Memory(gpuDev, owner, width, height, depth, format, imageType, mipLevels) {}

  //! Image constructor
  Image(const Device& gpuDev,          //!< GPU device object
        size_t size,                   //!< Memory size
        size_t width,                  //!< Allocated memory width
        size_t height,                 //!< Allocated memory height
        size_t depth,                  //!< Allocated memory depth
        cl_image_format format,        //!< Memory format
        cl_mem_object_type imageType,  //!< CL image type
        uint mipLevels                 //!< The number of mip levels
        )
      : pal::Memory(gpuDev, size, width, height, depth, format, imageType, mipLevels) {}

  //! Allocate memory for API-level maps
  virtual void* allocMapTarget(const amd::Coord3D& origin,  //!< The map location in memory
                               const amd::Coord3D& region,  //!< The map region in memory
                               uint mapFlags,               //!< Map flags
                               size_t* rowPitch = NULL,     //!< Row pitch for the mapped memory
                               size_t* slicePitch = NULL    //!< Slice for the mapped memory
                               );

 private:
  //! Disable copy constructor
  Image(const Image&);

  //! Disable operator=
  Image& operator=(const Image&);
};

}  // namespace pal
