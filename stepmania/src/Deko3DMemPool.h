/* Deko3DMemPool - GPU memory sub-allocation for the deko3d RageDisplay backend.
 *
 * deko3d hands out whole DkMemBlocks (page-aligned, flags fixed per block);
 * everything above that is our own responsibility. Three allocation
 * patterns are needed, matched to how each resource actually lives and
 * dies (see docs/architecture/10-Deko3D-MemoryAllocator.md for the design
 * rationale):
 *
 *  - Deko3DBumpPool:      allocate-and-forget, or allocate/Clear() in bulk.
 *                         Used for shader code (allocated once, never freed)
 *                         and the texture-upload staging buffer (Clear()d
 *                         after each synchronous upload completes).
 *  - Deko3DFreeListPool:  general alloc/free with coalescing. Used for
 *                         texture image storage, whose lifetime is driven
 *                         by RageTextureManager's cache and does not follow
 *                         a stack/arena discipline (see doc 10's reasoning
 *                         on why textures can't just use a bump pool).
 *  - Deko3DRingPool:      N fixed-size slices, each guarded by a DkFence,
 *                         handed out round-robin. Used for per-frame
 *                         command-buffer memory and per-frame vertex/
 *                         uniform data (as separate instances of this class).
 *
 * Written fresh for this engine's C++11 target and existing idioms (plain
 * structs, explicit Allocate()/Free() calls) rather than porting deko3d's
 * own C++17-flavored SampleFramework utilities.
 */
#ifndef DEKO3D_MEM_POOL_H
#define DEKO3D_MEM_POOL_H

#include <deko3d.hpp>
#include <vector>
#include <list>

/** @brief One sub-allocation handed out by any of the pools below. */
struct Deko3DAlloc
{
	Deko3DAlloc(): hBlock(nullptr), iOffset(0), iSize(0), pCpuAddr(nullptr), iGpuAddr(DK_GPU_ADDR_INVALID) { }

	// hBlock!=nullptr alone does NOT prove iGpuAddr is a real, usable GPU
	// address - it only proves *some* pool call filled in the struct. Every
	// bind call that reaches the GPU (bindVtxBuffer, bindUniformBuffer,
	// copyBufferToImage, ...) uses iGpuAddr directly, so this checks that
	// field explicitly rather than trusting hBlock as a proxy for it.
	// DK_GPU_ADDR_INVALID is UINT64_MAX (deko3d.h), not 0 - checked for
	// explicitly since a genuinely-uninitialized DkGpuAddr defaults there,
	// not to 0. iGpuAddr==0 is checked too: it is not deko3d's own "invalid"
	// sentinel, but a real GPU virtual address of exactly 0 for one of our
	// own pool allocations would itself be a symptom of something never
	// having been computed, so it is treated as invalid here as well.
	bool IsValid() const { return hBlock != nullptr && iGpuAddr != DK_GPU_ADDR_INVALID && iGpuAddr != 0; }

	DkMemBlock hBlock;
	uint32_t iOffset;
	uint32_t iSize;
	void *pCpuAddr;      // nullptr if this pool's memory isn't CPU-visible
	DkGpuAddr iGpuAddr;
};

/**
 * @brief Bump-allocates out of one or more growable DkMemBlocks.
 *
 * No individual Free(); either the whole pool is destroyed, or Clear() resets
 * every block's bump pointer to zero at once. Clear() is only safe once the
 * caller knows the GPU is done with every allocation handed out since the
 * last Clear() (e.g. after a synchronous dkQueueWaitIdle()) - this class does
 * not track that itself.
 */
class Deko3DBumpPool
{
public:
	Deko3DBumpPool( dk::Device device, uint32_t iFlags, uint32_t iBlockSize );
	~Deko3DBumpPool();

	// Non-copyable: owns DkMemBlock handles.
	Deko3DBumpPool( const Deko3DBumpPool & ) = delete;
	Deko3DBumpPool &operator=( const Deko3DBumpPool & ) = delete;

	Deko3DAlloc Allocate( uint32_t iSize, uint32_t iAlign );
	void Clear();

private:
	struct Block
	{
		dk::UniqueMemBlock MemBlock;
		void *pCpuAddr;
		DkGpuAddr iGpuAddr;
		uint32_t iSize;
		uint32_t iUsed;
	};

	Block &AddBlock( uint32_t iMinSize );

	dk::Device m_Device;
	uint32_t m_iFlags;
	uint32_t m_iBlockSize;
	std::vector<Block> m_vBlocks;
};

/**
 * @brief Free-list allocator with adjacent-slice coalescing, over one or
 * more growable DkMemBlocks.
 *
 * Used where allocations and frees are genuinely unpredictable in size and
 * timing (texture load/unload driven by RageTextureManager's cache). See
 * docs/architecture/10-Deko3D-MemoryAllocator.md for why this is needed
 * instead of a simpler scheme: long-lived and short-lived textures coexist
 * in the same pool at overlapping times, so a "reset everything at a scene
 * boundary" scheme would free memory still in use.
 */
class Deko3DFreeListPool
{
public:
	Deko3DFreeListPool( dk::Device device, uint32_t iFlags, uint32_t iBlockSize );
	~Deko3DFreeListPool();

	Deko3DFreeListPool( const Deko3DFreeListPool & ) = delete;
	Deko3DFreeListPool &operator=( const Deko3DFreeListPool & ) = delete;

	Deko3DAlloc Allocate( uint32_t iSize, uint32_t iAlign );
	void Free( const Deko3DAlloc &alloc );

private:
	struct Block
	{
		dk::UniqueMemBlock MemBlock;
		void *pCpuAddr;
		DkGpuAddr iGpuAddr;
		uint32_t iSize;
	};

	// A free byte range within one Block. iBlock indexes m_vBlocks.
	struct FreeSpan
	{
		FreeSpan( size_t iBlock_, uint32_t iStart_, uint32_t iEnd_ ):
			iBlock(iBlock_), iStart(iStart_), iEnd(iEnd_) { }
		size_t iBlock;
		uint32_t iStart, iEnd;
		uint32_t GetSize() const { return iEnd - iStart; }
	};

	void AddBlock( uint32_t iMinSize );

	dk::Device m_Device;
	uint32_t m_iFlags;
	uint32_t m_iBlockSize;
	std::vector<Block> m_vBlocks;
	// Sorted by (iBlock, iStart); linear scan is fine at this engine's scale
	// (dozens to a few hundred live textures, not millions of allocations).
	std::list<FreeSpan> m_FreeList;
};

/**
 * @brief N fixed-size slices, each guarded by a fence, handed out
 * round-robin - one per frame in flight.
 *
 * Mirrors the shape confirmed against devkitPro's own CCmdMemRing example
 * (docs/architecture/09-Deko3D-SpritePipelineCache.md S9), written fresh
 * for this engine. Used for both per-frame command-buffer memory and
 * per-frame vertex/uniform data (as two separate instances - a command
 * list and the data it references have the same lifetime shape but are
 * logically distinct allocations).
 */
class Deko3DRingPool
{
public:
	// iNumSlices should match (or exceed) the number of frames that may be
	// in flight at once - 2 or 3, matching swapchain buffering.
	Deko3DRingPool( dk::Device device, uint32_t iFlags, uint32_t iNumSlices, uint32_t iSliceSize );
	~Deko3DRingPool();

	Deko3DRingPool( const Deko3DRingPool & ) = delete;
	Deko3DRingPool &operator=( const Deko3DRingPool & ) = delete;

	// Call once at the start of each frame's use of this ring. Waits for
	// the slice about to be reused to be free (the GPU must have finished
	// reading whatever was written to it last time this slice came around).
	void BeginFrame();

	// Sub-allocate out of the CURRENT slice (set by the last BeginFrame()).
	// Returns an invalid Deko3DAlloc (IsValid() == false) if the slice is
	// out of space; callers should size iSliceSize generously and treat
	// this as a real error (ASSERT), not silently drop data.
	Deko3DAlloc Allocate( uint32_t iSize, uint32_t iAlign );

	// Call once at the end of each frame's use of this ring, after the
	// commands that reference this frame's allocations have been recorded
	// (not necessarily submitted yet). Records the fence to wait on before
	// this slice is reused N frames from now.
	void EndFrame( dk::Queue queue );

private:
	dk::UniqueMemBlock m_MemBlock;
	void *m_pCpuAddr;
	DkGpuAddr m_iGpuAddr;
	uint32_t m_iNumSlices;
	uint32_t m_iSliceSize;
	uint32_t m_iCurSlice;
	uint32_t m_iCurSliceUsed;
	std::vector<dk::Fence> m_vFences;
};

/**
 * @brief Fixed-size table of same-size descriptor slots (image or sampler
 * descriptors), allocated once at Init() and never resized.
 *
 * See docs/architecture/10-Deko3D-MemoryAllocator.md's reasoning for why a
 * generous fixed size (not a resizable table) is the right Phase 1 choice:
 * DkResHandles already baked into in-flight command lists reference slot
 * indices directly, so resizing mid-session would need index-stability
 * guarantees a simple resize can't give for free.
 */
class Deko3DDescriptorTable
{
public:
	Deko3DDescriptorTable( dk::Device device, uint32_t iNumSlots, uint32_t iDescriptorSize, uint32_t iDescriptorAlign );
	~Deko3DDescriptorTable();

	Deko3DDescriptorTable( const Deko3DDescriptorTable & ) = delete;
	Deko3DDescriptorTable &operator=( const Deko3DDescriptorTable & ) = delete;

	// Returns -1 if the table is full. Callers must treat that as a real,
	// reportable error (see 11-Deko3D-TextureFormatAndShaderContract.md and
	// doc 10 open item #2) - not silently overwrite a neighboring slot.
	int32_t AllocateSlot();
	void FreeSlot( int32_t iSlot );

	DkGpuAddr GetSlotAddr( int32_t iSlot ) const { return m_iGpuAddr + iSlot * m_iDescriptorSize; }
	void *GetSlotCpuAddr( int32_t iSlot ) const { return (uint8_t*)m_pCpuAddr + iSlot * m_iDescriptorSize; }
	DkGpuAddr GetBaseAddr() const { return m_iGpuAddr; }
	uint32_t GetNumSlots() const { return m_iNumSlots; }

private:
	dk::UniqueMemBlock m_MemBlock;
	void *m_pCpuAddr;
	DkGpuAddr m_iGpuAddr;
	uint32_t m_iNumSlots;
	uint32_t m_iDescriptorSize;
	std::vector<bool> m_vSlotUsed;
	uint32_t m_iNextFreeHint; // avoids re-scanning from 0 on every allocation
};

#endif

/*
 * Copyright (c) 2026 the StepMania-nx contributors
 * Same license terms as the rest of stepmania/src (see LICENSE).
 */
