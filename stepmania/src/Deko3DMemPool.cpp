#include "global.h"
#include "Deko3DMemPool.h"
#include "RageLog.h"
#include "RageUtil.h"

namespace
{
	uint32_t AlignUp( uint32_t iValue, uint32_t iAlign )
	{
		return (iValue + iAlign - 1) & ~(iAlign - 1);
	}

	// DkMemBlocks themselves must be a multiple of DK_MEMBLOCK_ALIGNMENT (4096).
	uint32_t RoundBlockSize( uint32_t iSize )
	{
		return AlignUp( iSize, DK_MEMBLOCK_ALIGNMENT );
	}
}

// ---------------------------------------------------------------------
// Deko3DBumpPool
// ---------------------------------------------------------------------

Deko3DBumpPool::Deko3DBumpPool( dk::Device device, uint32_t iFlags, uint32_t iBlockSize ):
	m_Device(device),
	m_iFlags(iFlags),
	m_iBlockSize(RoundBlockSize(iBlockSize))
{
}

Deko3DBumpPool::~Deko3DBumpPool()
{
}

Deko3DBumpPool::Block &Deko3DBumpPool::AddBlock( uint32_t iMinSize )
{
	uint32_t iSize = RoundBlockSize( max(m_iBlockSize, iMinSize) );

	Block block;
	block.MemBlock = dk::MemBlockMaker(m_Device, iSize).setFlags(m_iFlags).create();
	block.pCpuAddr = (m_iFlags & DkMemBlockFlags_CpuUncached) || (m_iFlags & DkMemBlockFlags_CpuCached)
		? block.MemBlock.getCpuAddr() : nullptr;
	block.iGpuAddr = block.MemBlock.getGpuAddr();
	block.iSize = iSize;
	block.iUsed = 0;

	m_vBlocks.push_back( std::move(block) );
	return m_vBlocks.back();
}

Deko3DAlloc Deko3DBumpPool::Allocate( uint32_t iSize, uint32_t iAlign )
{
	// Try the most recently added block first; bump allocators only ever
	// grow forward, so earlier blocks are permanently full once we've moved on.
	if( !m_vBlocks.empty() )
	{
		Block &block = m_vBlocks.back();
		uint32_t iStart = AlignUp( block.iUsed, iAlign );
		if( iStart + iSize <= block.iSize )
		{
			block.iUsed = iStart + iSize;

			Deko3DAlloc alloc;
			alloc.hBlock = block.MemBlock;
			alloc.iOffset = iStart;
			alloc.iSize = iSize;
			alloc.pCpuAddr = block.pCpuAddr ? (uint8_t*)block.pCpuAddr + iStart : nullptr;
			alloc.iGpuAddr = block.iGpuAddr + iStart;
			return alloc;
		}
	}

	// Need a new block. Requested size may exceed our default block size
	// (e.g. an unusually large shader) - AddBlock rounds up to fit it.
	Block &block = AddBlock( iSize + iAlign );
	uint32_t iStart = AlignUp( block.iUsed, iAlign );
	ASSERT_M( iStart + iSize <= block.iSize, "Deko3DBumpPool: new block still too small" );
	block.iUsed = iStart + iSize;

	Deko3DAlloc alloc;
	alloc.hBlock = block.MemBlock;
	alloc.iOffset = iStart;
	alloc.iSize = iSize;
	alloc.pCpuAddr = block.pCpuAddr ? (uint8_t*)block.pCpuAddr + iStart : nullptr;
	alloc.iGpuAddr = block.iGpuAddr + iStart;
	return alloc;
}

void Deko3DBumpPool::Clear()
{
	for( Block &block : m_vBlocks )
		block.iUsed = 0;
}

// ---------------------------------------------------------------------
// Deko3DFreeListPool
// ---------------------------------------------------------------------

Deko3DFreeListPool::Deko3DFreeListPool( dk::Device device, uint32_t iFlags, uint32_t iBlockSize ):
	m_Device(device),
	m_iFlags(iFlags),
	m_iBlockSize(RoundBlockSize(iBlockSize))
{
}

Deko3DFreeListPool::~Deko3DFreeListPool()
{
}

void Deko3DFreeListPool::AddBlock( uint32_t iMinSize )
{
	uint32_t iSize = RoundBlockSize( max(m_iBlockSize, iMinSize) );

	Block block;
	block.MemBlock = dk::MemBlockMaker(m_Device, iSize).setFlags(m_iFlags).create();
	block.pCpuAddr = (m_iFlags & DkMemBlockFlags_CpuUncached) || (m_iFlags & DkMemBlockFlags_CpuCached)
		? block.MemBlock.getCpuAddr() : nullptr;
	block.iGpuAddr = block.MemBlock.getGpuAddr();
	block.iSize = iSize;

	m_vBlocks.push_back( std::move(block) );
	m_FreeList.emplace_back( m_vBlocks.size()-1, 0u, iSize );
}

Deko3DAlloc Deko3DFreeListPool::Allocate( uint32_t iSize, uint32_t iAlign )
{
	// Best-fit search: smallest free span that still satisfies size+alignment.
	// Linear scan is fine at this engine's scale (RageTextureManager caches
	// dozens to a few hundred live textures per doc 10, not millions of
	// allocations); a tree-based free list would be premature here.
	std::list<FreeSpan>::iterator best = m_FreeList.end();
	uint32_t iBestAlignedStart = 0;
	uint32_t iBestWaste = 0xFFFFFFFFu;

	for( std::list<FreeSpan>::iterator it = m_FreeList.begin(); it != m_FreeList.end(); ++it )
	{
		uint32_t iAlignedStart = AlignUp( it->iStart, iAlign );
		if( iAlignedStart + iSize > it->iEnd )
			continue; // doesn't fit once aligned

		uint32_t iWaste = it->GetSize() - iSize;
		if( iWaste < iBestWaste )
		{
			best = it;
			iBestAlignedStart = iAlignedStart;
			iBestWaste = iWaste;
		}
	}

	if( best == m_FreeList.end() )
	{
		// No existing span fits; grow. Re-run the search once against the
		// single new span rather than duplicating the search loop.
		AddBlock( iSize + iAlign );
		best = m_FreeList.end();
		--best; // AddBlock always emplace_back's exactly one new span
		iBestAlignedStart = AlignUp( best->iStart, iAlign );
		ASSERT_M( iBestAlignedStart + iSize <= best->iEnd, "Deko3DFreeListPool: new block still too small" );
	}

	FreeSpan span = *best;
	size_t iBlock = span.iBlock;
	uint32_t iAllocEnd = iBestAlignedStart + iSize;

	// Split the span: keep [iStart, iBestAlignedStart) and (iAllocEnd, iEnd)
	// as free, if non-empty; consume the middle.
	m_FreeList.erase( best );
	if( iBestAlignedStart > span.iStart )
		m_FreeList.emplace_back( iBlock, span.iStart, iBestAlignedStart );
	if( iAllocEnd < span.iEnd )
		m_FreeList.emplace_back( iBlock, iAllocEnd, span.iEnd );

	const Block &block = m_vBlocks[iBlock];
	Deko3DAlloc alloc;
	alloc.hBlock = block.MemBlock;
	alloc.iOffset = iBestAlignedStart;
	alloc.iSize = iSize;
	alloc.pCpuAddr = block.pCpuAddr ? (uint8_t*)block.pCpuAddr + iBestAlignedStart : nullptr;
	alloc.iGpuAddr = block.iGpuAddr + iBestAlignedStart;
	return alloc;
}

void Deko3DFreeListPool::Free( const Deko3DAlloc &alloc )
{
	if( !alloc.IsValid() )
		return;

	// Find which block this allocation belongs to.
	size_t iBlock = m_vBlocks.size();
	for( size_t i = 0; i < m_vBlocks.size(); ++i )
	{
		if( (DkMemBlock)m_vBlocks[i].MemBlock == alloc.hBlock )
		{
			iBlock = i;
			break;
		}
	}
	ASSERT_M( iBlock < m_vBlocks.size(), "Deko3DFreeListPool::Free: allocation not from this pool" );

	uint32_t iStart = alloc.iOffset;
	uint32_t iEnd = alloc.iOffset + alloc.iSize;

	// Insert, then coalesce with any adjacent free span(s) in the same block.
	for( std::list<FreeSpan>::iterator it = m_FreeList.begin(); it != m_FreeList.end(); )
	{
		if( it->iBlock != iBlock )
		{
			++it;
			continue;
		}
		if( it->iEnd == iStart )
		{
			iStart = it->iStart;
			it = m_FreeList.erase(it);
			continue;
		}
		if( it->iStart == iEnd )
		{
			iEnd = it->iEnd;
			it = m_FreeList.erase(it);
			continue;
		}
		++it;
	}

	m_FreeList.emplace_back( iBlock, iStart, iEnd );
}

// ---------------------------------------------------------------------
// Deko3DRingPool
// ---------------------------------------------------------------------

Deko3DRingPool::Deko3DRingPool( dk::Device device, uint32_t iFlags, uint32_t iNumSlices, uint32_t iSliceSize ):
	m_pCpuAddr(nullptr),
	m_iGpuAddr(DK_GPU_ADDR_INVALID),
	m_iNumSlices(iNumSlices),
	m_iSliceSize(AlignUp(iSliceSize, DK_CMDMEM_ALIGNMENT)),
	m_iCurSlice(0),
	m_iCurSliceUsed(0),
	m_vFences(iNumSlices)
{
	ASSERT_M( iNumSlices > 0, "Deko3DRingPool: need at least one slice" );
	uint32_t iTotalSize = RoundBlockSize( m_iNumSlices * m_iSliceSize );
	m_MemBlock = dk::MemBlockMaker(device, iTotalSize).setFlags(iFlags).create();
	m_pCpuAddr = m_MemBlock.getCpuAddr();
	m_iGpuAddr = m_MemBlock.getGpuAddr();
}

Deko3DRingPool::~Deko3DRingPool()
{
}

void Deko3DRingPool::BeginFrame()
{
	m_iCurSlice = (m_iCurSlice + 1) % m_iNumSlices;
	m_iCurSliceUsed = 0;
	// Wait for the GPU to be done with whatever we wrote into this slice
	// the last time it came around (iNumSlices frames ago). On the very
	// first pass through, the fence is default-constructed/unsignalled;
	// dk::Fence::wait() on a never-signalled fence returns immediately
	// rather than blocking forever (deko3d treats an unused fence as
	// already satisfied).
	m_vFences[m_iCurSlice].wait();
}

Deko3DAlloc Deko3DRingPool::Allocate( uint32_t iSize, uint32_t iAlign )
{
	uint32_t iStart = AlignUp( m_iCurSliceUsed, iAlign );
	Deko3DAlloc alloc; // left invalid (IsValid() == false) if it doesn't fit
	if( iStart + iSize > m_iSliceSize )
	{
		LOG->Warn( "Deko3DRingPool: slice exhausted (wanted %u more bytes, slice is %u bytes) - "
			"increase this ring's slice size", iSize, m_iSliceSize );
		return alloc;
	}

	m_iCurSliceUsed = iStart + iSize;

	uint32_t iSliceOffset = m_iCurSlice * m_iSliceSize;
	alloc.hBlock = (DkMemBlock)m_MemBlock;
	alloc.iOffset = iSliceOffset + iStart;
	alloc.iSize = iSize;
	alloc.pCpuAddr = (uint8_t*)m_pCpuAddr + iSliceOffset + iStart;
	alloc.iGpuAddr = m_iGpuAddr + iSliceOffset + iStart;
	return alloc;
}

void Deko3DRingPool::EndFrame( dk::Queue queue )
{
	queue.signalFence( m_vFences[m_iCurSlice] );
}

// ---------------------------------------------------------------------
// Deko3DDescriptorTable
// ---------------------------------------------------------------------

Deko3DDescriptorTable::Deko3DDescriptorTable( dk::Device device, uint32_t iNumSlots, uint32_t iDescriptorSize, uint32_t iDescriptorAlign ):
	m_pCpuAddr(nullptr),
	m_iGpuAddr(DK_GPU_ADDR_INVALID),
	m_iNumSlots(iNumSlots),
	m_iDescriptorSize(AlignUp(iDescriptorSize, iDescriptorAlign)),
	m_vSlotUsed(iNumSlots, false),
	m_iNextFreeHint(0)
{
	uint32_t iTotalSize = RoundBlockSize( m_iNumSlots * m_iDescriptorSize );
	m_MemBlock = dk::MemBlockMaker(device, iTotalSize)
		.setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
		.create();
	m_pCpuAddr = m_MemBlock.getCpuAddr();
	m_iGpuAddr = m_MemBlock.getGpuAddr();
}

Deko3DDescriptorTable::~Deko3DDescriptorTable()
{
}

int32_t Deko3DDescriptorTable::AllocateSlot()
{
	for( uint32_t i = 0; i < m_iNumSlots; ++i )
	{
		uint32_t iSlot = (m_iNextFreeHint + i) % m_iNumSlots;
		if( !m_vSlotUsed[iSlot] )
		{
			m_vSlotUsed[iSlot] = true;
			m_iNextFreeHint = (iSlot + 1) % m_iNumSlots;
			return (int32_t)iSlot;
		}
	}

	// Table full. Compatibility-first: this is a real, reportable error,
	// not something to silently corrupt a neighboring slot over - see
	// 11-Deko3D-TextureFormatAndShaderContract.md and doc 10 open item #2.
	LOG->Warn( "Deko3DDescriptorTable: out of slots (all %u in use); "
		"this texture/sampler will not be created", m_iNumSlots );
	return -1;
}

void Deko3DDescriptorTable::FreeSlot( int32_t iSlot )
{
	if( iSlot < 0 )
		return;
	ASSERT_M( (uint32_t)iSlot < m_iNumSlots, "Deko3DDescriptorTable::FreeSlot: bad slot index" );
	ASSERT_M( m_vSlotUsed[iSlot], "Deko3DDescriptorTable::FreeSlot: slot already free" );
	m_vSlotUsed[iSlot] = false;
	m_iNextFreeHint = (uint32_t)iSlot;
}

/*
 * Copyright (c) 2026 the StepMania-nx contributors
 * Same license terms as the rest of stepmania/src (see LICENSE).
 */
