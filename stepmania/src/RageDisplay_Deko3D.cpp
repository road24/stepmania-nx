#include "global.h"
#include "RageDisplay.h"
#include "RageDisplay_Deko3D.h"
#include "RageUtil.h"
#include "RageLog.h"
#include "RageTypes.h"
#include "RageMath.h"
#include "RageSurface.h"
#include "RageSurfaceUtils.h"
#include "DisplaySpec.h"

#include <switch.h>
#include <cstring>

namespace
{
	// Copied verbatim from RageDisplay_GLES2.cpp/RageDisplay_OGL.cpp to avoid
	// re-deriving masks by hand. IMPORTANT: as written, these masks describe
	// the BIG-ENDIAN byte layout of each format. RageDisplay_OGL.cpp never
	// uses this table as-is either - its constructor calls FixLittleEndian()
	// (RageDisplay_OGL.cpp:218-249), which byte-swaps every 24/32bpp entry
	// on a little-endian host so the masks line up with what RageSurface
	// (and RageSurface_Load_PNG.cpp's Swap32BE() calls, RageSurface_Load_PNG.cpp:228-233)
	// actually produce in memory. This backend originally skipped that step,
	// so FindPixelFormat() (RageDisplay.cpp:658) never matched a single real
	// decoded texture - every texture silently fell back through
	// GetImgPixelFormat()'s "unsupported" branch. Worse than a missed fast
	// path: the fallback then re-tagged the converted surface as RGBA8 using
	// these same (wrong, un-swapped) masks, so RageSurfaceUtils::Blit() wrote
	// bytes in A,B,G,R memory order while RagePixelFormatToDkImageFormat()
	// uploaded them to the GPU declared as DkImageFormat_RGBA8_Unorm (real
	// R,G,B,A order) - i.e. every texture's channels were scrambled, not
	// just slow. FixDeko3DLittleEndian() below ports the exact same fix.
	RageDisplay::RagePixelFormatDesc PIXEL_FORMAT_DESC[NUM_RagePixelFormat] = {
		{ /* R8G8B8A8 */ 32, { 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF } },
		{ /* B8G8R8A8 */ 32, { 0x0000FF00, 0x00FF0000, 0xFF000000, 0x000000FF } },
		// Packed 16bpp nibble order is reversed (A,B,G,R, not R,G,B,A) -
		// deko3d's own source names these hw formats "A4B4G4R4"/"A1B5G5R5";
		// confirmed on-device via _dithertest.lua.
		{ /* R4G4B4A4 */ 16, { 0x000F, 0x00F0, 0x0F00, 0xF000 } },
		{ /* R5G5B5A1 */ 16, { 0x001F, 0x03E0, 0x7C00, 0x8000 } },
		{ /* R5G5B5X1 */ 16, { 0x001F, 0x03E0, 0x7C00, 0x0000 } },
		{ /* R8G8B8   */ 24, { 0xFF0000, 0x00FF00, 0x0000FF, 0x000000 } },
		{ /* Paletted */ 8,  { 0, 0, 0, 0 } },
		{ /* B8G8R8   */ 24, { 0x0000FF, 0x00FF00, 0xFF0000, 0x000000 } },
		// Unchanged: never natively uploaded (SupportsTextureFormat rejects
		// them), so the GPU nibble-order fix above doesn't apply here.
		{ /* A1R5G5B5 */ 16, { 0x7C00, 0x03E0, 0x001F, 0x8000 } },
		{ /* X1R5G5B5 */ 16, { 0x7C00, 0x03E0, 0x001F, 0x0000 } },
	};

	// Port of RageDisplay_OGL.cpp's FixLittleEndian() (RageDisplay_OGL.cpp:218-249).
	// Switch/libnx is unconditionally AArch64 little-endian (no big-endian
	// target exists), so unlike the OGL original this isn't guarded by
	// #if defined(ENDIAN_LITTLE) - it always applies. Only 24/32bpp entries
	// are byte-oriented in memory (16bpp formats are bit-packed values, not
	// byte sequences, so they need no swap; 8bpp Paletted has no color masks
	// to swap) - this mirrors the OGL original's
	// "g_GLPixFmtInfo[i].type != GL_UNSIGNED_BYTE || pf.bpp == 8" skip
	// condition, expressed directly in terms of bpp since this backend has
	// no GL-format-enum table to key off of.
	void FixDeko3DLittleEndian()
	{
		static bool bInitialized = false;
		if( bInitialized )
			return;
		bInitialized = true;

		for( int i = 0; i < NUM_RagePixelFormat; ++i )
		{
			RageDisplay::RagePixelFormatDesc &pf = PIXEL_FORMAT_DESC[i];
			if( pf.bpp != 24 && pf.bpp != 32 )
				continue;

			for( int mask = 0; mask < 4; ++mask )
			{
				unsigned m = pf.masks[mask];
				switch( pf.bpp )
				{
				case 24: m = Swap24(m); break;
				case 32: m = Swap32(m); break;
				}
				pf.masks[mask] = m;
			}
		}
	}

	// Pool sizes are explicitly-marked placeholders - see
	// 10-Deko3D-MemoryAllocator.md "Sizing the pools": real numbers need
	// profiling against actual theme/gameplay content, not guessing further
	// in source code.
	const uint32_t CODE_POOL_SIZE = 256 * 1024;
	const uint32_t IMAGE_POOL_SIZE = 64 * 1024 * 1024;
	const uint32_t SCRATCH_POOL_SIZE = 8 * 1024 * 1024;
	const uint32_t SETUP_CMD_POOL_SIZE = 64 * 1024;
	const uint32_t STREAMING_CMD_POOL_SIZE = 64 * 1024;
	const uint32_t DYNAMIC_CMD_SLICE_SIZE = 128 * 1024;
	const uint32_t DYNAMIC_DATA_SLICE_SIZE = 512 * 1024;
	const uint32_t MODEL_GEOMETRY_POOL_SIZE = 8 * 1024 * 1024;

	// RGB factors only, matching RageDisplay_Legacy::SetBlendMode's
	// iSourceRGB/iDestRGB switch (RageDisplay_OGL.cpp:1881-1923) case-for-case.
	// This previously only handled 5 of the 10 real BlendMode values (the
	// rest silently fell through to Normal's factors), had BLEND_MODULATE's
	// src/dst backwards (DstColor/Zero instead of Zero/SrcColor), and had
	// BLEND_SUBTRACT copying BLEND_ADD's factors instead of Normal's (the
	// "subtract" comes entirely from RageToDkBlendOp's Reverse-Subtract op,
	// not from different factors - OGL confirms BLEND_SUBTRACT's
	// iSourceRGB/iDestRGB are byte-for-byte identical to BLEND_NORMAL's).
	// Root-caused via a real theme (raveitout's "ScreenSelectPlayMode
	// background.lua") drawing an opaque white Quad with
	// BlendMode_WeightedMultiply over a bg video: falling through to
	// Normal's factors with an opaque white source (SrcAlpha=1) reduces to
	// "replace with white," flattening the video to a solid white
	// background - not a video-decoding bug, a missing blend-mode translation.
	DkBlendFactor RageToDkBlendFactor( bool bSrc, BlendMode mode, bool bAlphaChannel )
	{
		(void)bAlphaChannel; // alpha factors are handled separately, hardcoded, at the FlushCommonState() call site
		switch( mode )
		{
		case BLEND_ADD:
			return bSrc ? DkBlendFactor_SrcAlpha : DkBlendFactor_One;
		case BLEND_MODULATE:
			return bSrc ? DkBlendFactor_Zero : DkBlendFactor_SrcColor;
		case BLEND_COPY_SRC:
			return bSrc ? DkBlendFactor_One : DkBlendFactor_Zero;
		case BLEND_ALPHA_MASK:
		case BLEND_ALPHA_KNOCK_OUT:
		case BLEND_NO_EFFECT:
			// RGB-identical in OGL too (BLEND_ALPHA_MASK/KNOCK_OUT only
			// differ from each other in their alpha factors).
			return bSrc ? DkBlendFactor_Zero : DkBlendFactor_One;
		case BLEND_ALPHA_MULTIPLY:
			return bSrc ? DkBlendFactor_SrcAlpha : DkBlendFactor_Zero;
		case BLEND_WEIGHTED_MULTIPLY:
			// "out = 2*(dst*src)" - 0.5 gray is identity, darker darkens,
			// brighter (e.g. this session's white-Quad case) lightens.
			return bSrc ? DkBlendFactor_DstColor : DkBlendFactor_SrcColor;
		case BLEND_INVERT_DEST:
			// "out = src - dst"; both factors are ONE, the subtraction
			// itself comes from RageToDkBlendOp's DkBlendOp_Sub below.
			return DkBlendFactor_One;
		case BLEND_NORMAL:
		case BLEND_SUBTRACT: // see comment above - same factors as Normal
		default:
			return bSrc ? DkBlendFactor_SrcAlpha : DkBlendFactor_InvSrcAlpha;
		}
	}

	DkBlendOp RageToDkBlendOp( BlendMode mode )
	{
		switch( mode )
		{
		case BLEND_SUBTRACT: return DkBlendOp_RevSub;
		case BLEND_INVERT_DEST: return DkBlendOp_Sub;
		default: return DkBlendOp_Add;
		}
	}

	// Matches RageDisplay_Legacy::SetZTestMode's glDepthFunc mapping.
	DkCompareOp RageToDkCompareOp( ZTestMode mode )
	{
		switch( mode )
		{
		case ZTEST_WRITE_ON_PASS: return DkCompareOp_Lequal;
		case ZTEST_WRITE_ON_FAIL: return DkCompareOp_Greater;
		case ZTEST_OFF:
		default: return DkCompareOp_Always;
		}
	}

	// deko3d's own fatal-error path (dk::detail::RaiseError, seen in every
	// crash report so far as the frame right above svcBreak/User Break) logs
	// nothing on its own before aborting - the crash report only gives us
	// the DkResult numeric code embedded in Atmosphere's Result field and a
	// raw return-address stack trace. DkDeviceMaker::cbDebug fires with the
	// actual failing call's name and message *before* that abort, so wiring
	// it up is the only way to get a human-readable reason survivably into
	// log.txt instead of reverse-engineering addr2line output after the
	// fact for every new crash. Uses LOG->Info() specifically (not Trace())
	// because RageLog::Write() only forces an immediate Flush() for
	// WRITE_TO_INFO/WRITE_LOUD (RageLog.cpp:308) - Trace() writes are
	// buffered and are lost outright when the process is killed by
	// svcBreak(), which is exactly why the BeginFrame/EndFrame Trace calls
	// added earlier never once appeared in a captured log.txt.
	void Deko3DDebugCallback( void * /*userData*/, const char *context, DkResult result, const char *message )
	{
		LOG->Info( "deko3d: %s failed with DkResult %d: %s", context, (int)result, message );
	}
}

// ---------------------------------------------------------------------
// PendingState
// ---------------------------------------------------------------------

RageDisplay_Deko3D::PendingState::PendingState():
	blendMode(BLEND_NORMAL),
	cullMode(CULL_NONE),
	zTestMode(ZTEST_OFF),
	bZTest(false),
	bZWrite(false),
	bAlphaTest(false),
	fZBias(0),
	textureMode(TextureMode_Modulate),
	effectMode(EffectMode_Normal),
	iBoundTexture(0),
	bTextureWrap(false),
	bTextureFilter(true)
{
}

// ---------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------

RageDisplay_Deko3D::RageDisplay_Deko3D():
	m_iWhiteTextureDescriptorSlot(-1),
	m_pCodePool(nullptr),
	m_pImagePool(nullptr),
	m_pScratchPool(nullptr),
	m_pSetupCmdPool(nullptr),
	m_pStreamingCmdPool(nullptr),
	m_pDynamicCmdPool(nullptr),
	m_pDynamicDataPool(nullptr),
	m_pImageDescriptors(nullptr),
	m_pSamplerDescriptors(nullptr),
	m_pModelGeometryPool(nullptr),
	m_VertexShader(),
	m_iWidth(0),
	m_iHeight(0),
	m_iBeginFrameCount(0),
	m_iEndFrameCount(0)
{
	LOG->Info( "RageDisplay_Deko3D: constructing (Phase 1: Sprite/Quad only)" );
	FixDeko3DLittleEndian();
	for( int i = 0; i < 2; ++i )
		for( int j = 0; j < 2; ++j )
			m_iSamplerSlot[i][j] = -1;
}

RageDisplay_Deko3D::~RageDisplay_Deko3D()
{
	for( TextureRecord *pRec : m_vTextures )
		delete pRec;

	delete m_pImageDescriptors;
	delete m_pSamplerDescriptors;
	delete m_pModelGeometryPool;
	delete m_pDynamicDataPool;
	delete m_pDynamicCmdPool;
	delete m_pStreamingCmdPool;
	delete m_pSetupCmdPool;
	delete m_pScratchPool;
	delete m_pImagePool;
	delete m_pCodePool;
}

// ---------------------------------------------------------------------
// Init / video mode
// ---------------------------------------------------------------------

RString RageDisplay_Deko3D::Init( const VideoModeParams &p, bool /*bAllowUnacceleratedRenderer*/ )
{
	// __DATE__/__TIME__ are filled in by the COMPILER at the moment this
	// specific .cpp is actually compiled, tied to normal source-file
	// dependency tracking - unlike generated/verstub.cpp's "Compiled ..."
	// line (StepMania.cpp:1001-1003), which comes from a CMake
	// configure_file() that only re-runs on a full "cmake -G ..." configure
	// pass, not on an incremental "cmake --build"/"make" (confirmed: editing
	// and rebuilding this file alone left verstub.cpp's timestamp
	// unchanged). This line is the one to trust when checking whether a
	// given log.txt actually came from the binary with your latest edit,
	// regardless of how the build was invoked.
	LOG->Info( "RageDisplay_Deko3D: source last compiled %s %s", __DATE__, __TIME__ );

	CreateDeviceAndQueue();
	CreatePools();
	BindDescriptorSets();
	LoadShaders();
	CreateSamplers();
	CreateWhiteTexture();

	bool bNewDeviceOut = false;
	RString sError = TryVideoMode( p, bNewDeviceOut );
	if( !sError.empty() )
		return sError;

	return RString();
}

void RageDisplay_Deko3D::CreateDeviceAndQueue()
{
	m_Device = dk::DeviceMaker().setCbDebug( Deko3DDebugCallback ).create();
	m_Queue = dk::QueueMaker(m_Device).setFlags(DkQueueFlags_Graphics).create();

	m_SetupCmdBuf = dk::CmdBufMaker(m_Device).create();
	m_DynamicCmdBuf = dk::CmdBufMaker(m_Device).create();
	m_StreamingCmdBuf = dk::CmdBufMaker(m_Device).create();
}

void RageDisplay_Deko3D::CreatePools()
{
	m_pCodePool = new Deko3DBumpPool( m_Device,
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
		CODE_POOL_SIZE );
	m_pImagePool = new Deko3DFreeListPool( m_Device,
		DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
		IMAGE_POOL_SIZE );
	m_pScratchPool = new Deko3DBumpPool( m_Device,
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		SCRATCH_POOL_SIZE );
	m_pSetupCmdPool = new Deko3DBumpPool( m_Device,
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		SETUP_CMD_POOL_SIZE );
	m_pStreamingCmdPool = new Deko3DBumpPool( m_Device,
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		STREAMING_CMD_POOL_SIZE );

	uint32_t iCmdMem = DYNAMIC_CMD_SLICE_SIZE;
	m_pDynamicCmdPool = new Deko3DRingPool( m_Device,
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NUM_DYNAMIC_SLICES, iCmdMem );
	m_pDynamicDataPool = new Deko3DRingPool( m_Device,
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NUM_DYNAMIC_SLICES, DYNAMIC_DATA_SLICE_SIZE );

	m_pImageDescriptors = new Deko3DDescriptorTable( m_Device, MAX_LIVE_TEXTURES,
		sizeof(DkImageDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT );
	m_pSamplerDescriptors = new Deko3DDescriptorTable( m_Device, 4, // exactly 4: TextureWrapping x TextureFiltering
		sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT );

	// CPU-visible: memcpy'd directly in Change(), no texture-style staging.
	m_pModelGeometryPool = new Deko3DFreeListPool( m_Device,
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		MODEL_GEOMETRY_POOL_SIZE );
}

void RageDisplay_Deko3D::BindDescriptorSets()
{
	// One-time, queue-persistent setup - matches Example04_TexturedCube.cpp's
	// "Configure persistent state in the queue" block exactly (submit once
	// via the static command buffer, waitIdle(), never touch again). This
	// was the actual root cause of the GPU page fault (address 0x0, Read)
	// that survived every allocation/slot-index validation added at every
	// other bind site: dkMakeTextureHandle()'s packed slot indices are
	// meaningless without first telling the GPU which physical memory
	// address the descriptor table they index into actually lives at. Every
	// FlushState() texture bind before this fix was indexing into whatever
	// descriptor-set base address the GPU happened to default to (i.e. none)
	// - not a data problem, a missing setup call.
	RearmSetupCmdBuf();
	m_SetupCmdBuf.bindImageDescriptorSet( m_pImageDescriptors->GetBaseAddr(), m_pImageDescriptors->GetNumSlots() );
	m_SetupCmdBuf.bindSamplerDescriptorSet( m_pSamplerDescriptors->GetBaseAddr(), m_pSamplerDescriptors->GetNumSlots() );
	m_Queue.submitCommands( m_SetupCmdBuf.finishList() );
	m_Queue.waitIdle();
}

void RageDisplay_Deko3D::RearmSetupCmdBuf()
{
	// m_SetupCmdBuf is now used ONLY by BeginFrame()'s render-target bind
	// (plus the one-time BindDescriptorSets() call at Init(), before any
	// frame exists) - it gets its own small pool, reset (Clear()) and
	// re-fed before every use.
	//
	// Texture uploads (CreateTexture() and UpdateTexture()) used to share
	// this buffer/pool too, on the assumption that they only ever ran at
	// safe, serialized load time. That assumption was wrong twice over, in
	// two separate real on-device crashes (both a "GPU method error" via
	// dkCmdBufBarrier, root-caused through the debug deko3d lib's cbDebug):
	// UpdateTexture() runs mid-frame for streaming updates (movie frames),
	// and CreateTexture() turned out to also run mid-frame in practice (e.g.
	// entering ScreenGameplay creates textures after that frame's
	// BeginFrame() has already run). BeginFrame()'s render-target-bind
	// submission on THIS buffer is never waitIdle()'d, so either one
	// reusing (Clear()ing) this same memory later in the same frame could
	// corrupt render-target-bind commands the GPU might still be reading.
	// Both texture-upload paths now share m_StreamingCmdBuf/
	// m_pStreamingCmdPool instead (RearmStreamingCmdBuf() below) - safe
	// between themselves (both fully synchronous, submit+waitIdle before
	// returning, and StepMania's render loop is single-threaded so they
	// can't overlap each other), just not safe sharing with this one.
	m_SetupCmdBuf.clear();
	m_pSetupCmdPool->Clear();
	Deko3DAlloc cmdMem = m_pSetupCmdPool->Allocate( SETUP_CMD_POOL_SIZE, DK_CMDMEM_ALIGNMENT );
	ASSERT_M( cmdMem.IsValid(), "RageDisplay_Deko3D: setup command pool allocation failed" );
	m_SetupCmdBuf.addMemory( cmdMem.hBlock, cmdMem.iOffset, cmdMem.iSize );
}

void RageDisplay_Deko3D::RearmStreamingCmdBuf()
{
	// Mirrors RearmSetupCmdBuf(), on m_StreamingCmdBuf/m_pStreamingCmdPool
	// instead - shared by both CreateTexture() and UpdateTexture() (see the
	// comment on RearmSetupCmdBuf() for why neither can share that one).
	// Both always waitIdle() before returning, so reusing this same memory
	// on the next call - by either of them - is safe by the same reasoning.
	m_StreamingCmdBuf.clear();
	m_pStreamingCmdPool->Clear();
	Deko3DAlloc cmdMem = m_pStreamingCmdPool->Allocate( STREAMING_CMD_POOL_SIZE, DK_CMDMEM_ALIGNMENT );
	ASSERT_M( cmdMem.IsValid(), "RageDisplay_Deko3D: streaming command pool allocation failed" );
	m_StreamingCmdBuf.addMemory( cmdMem.hBlock, cmdMem.iOffset, cmdMem.iSize );
}

void RageDisplay_Deko3D::LoadShaders()
{
	// Phase 1 shader set (11-Deko3D-TextureFormatAndShaderContract.md):
	// one vertex shader shared by all sprite draws, one fragment shader per
	// TextureMode variant actually reachable from Sprite::DrawTexture()
	// (09-Deko3D-SpritePipelineCache.md S1/S6).
	//
	// The compiled .dksh files are produced from
	// src/deko3d_shaders/*.glsl by a uam build step (CMakeData-rage.cmake),
	// written directly into Data/Shaders/Deko3D/ - this project has no
	// RomFS anywhere else (everything, including the sibling
	// Data/Shaders/GLSL/ theme-effect shaders, loads from the real
	// filesystem via a CWD-relative path, per RageFileManager.cpp's
	// ChangeToDirOfExecutable() using GetCwd() directly on __SWITCH__), so
	// these follow that same convention rather than introducing RomFS just
	// for these three files.
	auto loadOne = [this]( dk::Shader *pOut, const char *sPath )
	{
		FILE *f = fopen( sPath, "rb" );
		if( f == nullptr )
		{
			LOG->Warn( "RageDisplay_Deko3D: could not open shader '%s' - "
				"has the .glsl->.dksh build step been wired up yet?", sPath );
			return;
		}
		fseek( f, 0, SEEK_END );
		long iSize = ftell( f );
		rewind( f );

		Deko3DAlloc alloc = m_pCodePool->Allocate( (uint32_t)iSize, DK_SHADER_CODE_ALIGNMENT );
		fread( alloc.pCpuAddr, (size_t)iSize, 1, f );
		fclose( f );

		dk::ShaderMaker maker( (DkMemBlock)alloc.hBlock, alloc.iOffset );
		maker.initialize( *pOut );
	};

	loadOne( &m_VertexShader, "Data/Shaders/Deko3D/sprite_vsh.dksh" );
	loadOne( &m_FragmentShaders[SpriteShader_Modulate], "Data/Shaders/Deko3D/sprite_modulate_fsh.dksh" );
	loadOne( &m_FragmentShaders[SpriteShader_Glow], "Data/Shaders/Deko3D/sprite_glow_fsh.dksh" );
	loadOne( &m_ModelVertexShader, "Data/Shaders/Deko3D/model_vsh.dksh" );
	loadOne( &m_ModelFragmentShader, "Data/Shaders/Deko3D/model_fsh.dksh" );
}

dk::Sampler RageDisplay_Deko3D::MakeSampler( bool bWrap, bool bFilter ) const
{
	dk::Sampler sampler;
	sampler.setFilter( bFilter ? DkFilter_Linear : DkFilter_Nearest,
	                    bFilter ? DkFilter_Linear : DkFilter_Nearest );
	DkWrapMode wrap = bWrap ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
	sampler.setWrapMode( wrap, wrap, wrap );
	return sampler;
}

void RageDisplay_Deko3D::CreateSamplers()
{
	// All 4 combinations created eagerly at Init() - see doc 09 S4/S5: this
	// is the one part of the design that's small enough (4 entries) to be
	// eager-by-simplicity rather than lazy-by-necessity.
	for( int iWrap = 0; iWrap < 2; ++iWrap )
	{
		for( int iFilter = 0; iFilter < 2; ++iFilter )
		{
			int32_t iSlot = m_pSamplerDescriptors->AllocateSlot();
			ASSERT_M( iSlot >= 0, "RageDisplay_Deko3D: could not allocate a sampler slot at Init() - this should never happen (only 4 are ever needed)" );

			dk::Sampler sampler = MakeSampler( iWrap != 0, iFilter != 0 );
			dk::SamplerDescriptor *pDesc = (dk::SamplerDescriptor *)m_pSamplerDescriptors->GetSlotCpuAddr( iSlot );
			pDesc->initialize( sampler );

			m_iSamplerSlot[iWrap][iFilter] = iSlot;
		}
	}
}

int32_t RageDisplay_Deko3D::GetOrCreateSamplerSlot( bool bWrap, bool bFilter )
{
	// All 4 are created eagerly in CreateSamplers(); this is just the lookup.
	return m_iSamplerSlot[bWrap ? 1 : 0][bFilter ? 1 : 0];
}

void RageDisplay_Deko3D::CreateWhiteTexture()
{
	// See the m_iWhiteTextureDescriptorSlot comment in the header for why
	// this exists: FlushState() must always bind *something* to the
	// fragment stage's texture slot, even for "no texture" draws, because
	// both shader variants unconditionally sample it.
	const RagePixelFormatDesc *pDesc = GetPixelFormatDesc( RagePixelFormat_RGBA8 );
	RageSurface *pImg = CreateSurface( 1, 1, pDesc->bpp,
		pDesc->masks[0], pDesc->masks[1], pDesc->masks[2], pDesc->masks[3] );
	// All 4 bytes 0xFF - opaque white regardless of which byte is which
	// channel, so this doesn't depend on the mask layout above at all.
	std::memset( pImg->pixels, 0xFF, 4 );

	uintptr_t iHandle = CreateTexture( RagePixelFormat_RGBA8, pImg, false );
	ASSERT_M( iHandle != 0, "RageDisplay_Deko3D: failed to create the 1x1 white fallback texture at Init()" );
	m_iWhiteTextureDescriptorSlot = m_vTextures[iHandle - 1]->iImageDescriptorSlot;
	// Every untextured draw for the rest of the process's lifetime trusts
	// this slot without re-checking it (FlushState() only re-validates that
	// it's >=0, not that it's a real, initialized descriptor) - confirm the
	// invariant holds right here, once, at the one place it's established.
	ASSERT_M( m_iWhiteTextureDescriptorSlot >= 0,
		"RageDisplay_Deko3D: white fallback texture was created but has no valid image descriptor slot" );

	delete pImg;
}

RString RageDisplay_Deko3D::TryVideoMode( const VideoModeParams &p, bool &bNewDeviceOut )
{
	bNewDeviceOut = false; // deko3d device is created once in Init(), not per mode change

	DestroySwapchain();
	CreateSwapchain( p.width, p.height );

	m_iWidth = p.width;
	m_iHeight = p.height;
	m_CurrentParams = ActualVideoModeParams( p, p.width, p.height, false );

	// p.rate is PREFSMAN->m_iRefreshRate, which defaults to REFRESH_DEFAULT
	// (0, meaning "unspecified" - RageDisplay.h:13) unless the user has set
	// an explicit rate. GetActualVideoModeParams() is contractually supposed
	// to report the *resolved* rate, not echo the raw preference back -
	// LowLevelWindow_SDL.cpp:183-186 (used by the OGL/GLES2 backends) never
	// leaves CurrentParams.rate at 0, falling back to 60 when the real
	// display doesn't report one either. deko3d has no swapchain/display
	// query for the physical refresh rate wired up yet, so mirror that same
	// fallback rather than leave 0 (which would otherwise misreport as a
	// literal "0Hz" display, e.g. in StepMania.cpp's GetActualGraphicOptionsString()).
	if( m_CurrentParams.rate == REFRESH_DEFAULT )
		m_CurrentParams.rate = 60;

	return RString();
}

void RageDisplay_Deko3D::CreateSwapchain( int iWidth, int iHeight )
{
	dk::ImageLayout layout;
	dk::ImageLayoutMaker( m_Device )
		.setFlags( DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression )
		.setFormat( DkImageFormat_RGBA8_Unorm )
		.setDimensions( iWidth, iHeight )
		.initialize( layout );

	uint64_t iSize = layout.getSize();
	uint32_t iAlign = layout.getAlignment();

	std::array<DkImage const *, NUM_FRAMEBUFFERS> images;
	for( int i = 0; i < NUM_FRAMEBUFFERS; ++i )
	{
		m_FramebufferMem[i] = m_pImagePool->Allocate( (uint32_t)iSize, iAlign );
		// Same unguarded-allocation gap as the per-texture image memory
		// below, but for the swapchain's own render targets specifically -
		// these are what every single frame binds and presents, so a bad
		// allocation here would GPU-fault on literally the first frame.
		ASSERT_M( m_FramebufferMem[i].IsValid(),
			ssprintf("RageDisplay_Deko3D: framebuffer %d image pool allocation failed (%llu bytes) - "
				"m_pImagePool likely exhausted, see IMAGE_POOL_SIZE", i, (unsigned long long)iSize).c_str() );
		m_Framebuffers[i].initialize( layout, m_FramebufferMem[i].hBlock, m_FramebufferMem[i].iOffset );
		images[i] = &m_Framebuffers[i];
	}

	m_Swapchain = dk::SwapchainMaker( m_Device, nwindowGetDefault(), images ).create();

	// Z24S8 (no stencil use, but it's the precedented, known-working format).
	dk::ImageLayout depthLayout;
	dk::ImageLayoutMaker( m_Device )
		.setFlags( DkImageFlags_UsageRender | DkImageFlags_HwCompression )
		.setFormat( DkImageFormat_Z24S8 )
		.setDimensions( iWidth, iHeight )
		.initialize( depthLayout );

	m_DepthBufferMem = m_pImagePool->Allocate( (uint32_t)depthLayout.getSize(), depthLayout.getAlignment() );
	ASSERT_M( m_DepthBufferMem.IsValid(),
		ssprintf("RageDisplay_Deko3D: depth buffer image pool allocation failed (%llu bytes) - "
			"m_pImagePool likely exhausted, see IMAGE_POOL_SIZE", (unsigned long long)depthLayout.getSize()).c_str() );
	m_DepthBuffer.initialize( depthLayout, m_DepthBufferMem.hBlock, m_DepthBufferMem.iOffset );
}

void RageDisplay_Deko3D::DestroySwapchain()
{
	if( !m_Swapchain )
		return;

	m_Queue.waitIdle();
	m_Swapchain.destroy();

	for( int i = 0; i < NUM_FRAMEBUFFERS; ++i )
		m_pImagePool->Free( m_FramebufferMem[i] );

	m_pImagePool->Free( m_DepthBufferMem );
}

// ---------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------

bool RageDisplay_Deko3D::BeginFrame()
{
	m_iBeginFrameCount++;
	if( m_iBeginFrameCount - m_iEndFrameCount > 1 )
	{
		LOG->Warn( "RageDisplay_Deko3D::BeginFrame: called without a matching "
			"EndFrame() completing first (BeginFrame count=%llu, EndFrame count=%llu) "
			"- a swapchain image is being held longer than one frame",
			(unsigned long long)m_iBeginFrameCount, (unsigned long long)m_iEndFrameCount );
	}

	m_pDynamicCmdPool->BeginFrame();
	m_pDynamicDataPool->BeginFrame();

	// The swapchain image must be acquired and bound as the render target
	// BEFORE any of this frame's DrawQuadsInternal() calls, not after -
	// every draw call between BeginFrame() and EndFrame() needs somewhere
	// to render into. Matches the acquire-then-bind-then-draw order used
	// throughout every devkitPro example read during design (e.g.
	// deko_basic/main.c:178-181).
	//
	// Diagnostic logging added around this specific call: it's the site of
	// a crash (dk::detail::RaiseError inside dkQueueAcquireImage) that only
	// manifests after several prior frames render successfully - this log
	// line lets the next crash's log show exactly which frame number it was
	// on, cross-referenced against the BeginFrame/EndFrame counters above.
	LOG->Trace( "RageDisplay_Deko3D::BeginFrame: frame #%llu, acquiring swapchain image",
		(unsigned long long)m_iBeginFrameCount );
	m_iCurFramebufferSlot = m_Queue.acquireImage( m_Swapchain );
	LOG->Trace( "RageDisplay_Deko3D::BeginFrame: frame #%llu, acquired slot %d",
		(unsigned long long)m_iBeginFrameCount, m_iCurFramebufferSlot );

	// Validate the swapchain framebuffer's own backing memory before binding
	// it as a render target - every bind that hands deko3d a GPU address
	// gets checked at the point of use now (not just trusted from wherever
	// it was allocated), per the same reasoning as Deko3DAlloc::IsValid().
	ASSERT_M( m_FramebufferMem[m_iCurFramebufferSlot].IsValid(),
		ssprintf("RageDisplay_Deko3D: framebuffer slot %d has no valid backing memory", m_iCurFramebufferSlot).c_str() );

	RearmSetupCmdBuf();
	dk::ImageView colorTarget( m_Framebuffers[m_iCurFramebufferSlot] );
	dk::ImageView depthTarget( m_DepthBuffer );
	m_SetupCmdBuf.bindRenderTargets( { &colorTarget }, &depthTarget );

	// Same class of bug as the missing descriptor-set binding: deko3d has no
	// default viewport/scissor derived from the bound render target's size -
	// without this, clip-space coordinates map to whatever tiny/undefined
	// region the GPU happened to have configured, which is exactly the
	// symptom seen on real hardware (a black screen with a handful of
	// scattered colored pixels in one corner, instead of the actual UI).
	// Set every frame (not once at Init()) so a resolution change picks up
	// m_iWidth/m_iHeight automatically, consistent with bindRenderTargets()
	// above also being redone every frame rather than once.
	m_SetupCmdBuf.setViewports( 0, { { 0.0f, 0.0f, (float)m_iWidth, (float)m_iHeight, 0.0f, 1.0f } } );
	m_SetupCmdBuf.setScissors( 0, { { 0, 0, (uint32_t)m_iWidth, (uint32_t)m_iHeight } } );

	// No deko3d default clear, unlike RageDisplay_OGL.cpp:843-845's
	// glClear() every frame - without it, stale content from 2 frames back
	// (NUM_FRAMEBUFFERS==2) bleeds through.
	m_SetupCmdBuf.clearColor( 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f );

	m_Queue.submitCommands( m_SetupCmdBuf.finishList() );

	// Feed the dynamic command buffer ONE chunk of memory for the WHOLE
	// frame, once, here - not per draw call. Every DrawQuadsInternal() this
	// frame just records more commands into this same buffer; the whole
	// frame's worth of draws are submitted together in EndFrame(). This
	// corrects a real bug found by tracing through runtime behavior rather
	// than just compiling: the first version of this code re-fed and
	// re-submitted the buffer inside DrawQuadsInternal() itself, asking for
	// the FULL slice size on every single draw call - which exhausted the
	// slice after exactly one quad and crashed (ASSERT_M) on the second
	// quad drawn in the same frame. Since Sprite::DrawTexture() alone can
	// issue up to 3 quads per sprite (shadow/diffuse/glow), this would have
	// crashed on effectively every real frame.
	m_DynamicCmdBuf.clear();
	Deko3DAlloc cmdMem = m_pDynamicCmdPool->Allocate( DYNAMIC_CMD_SLICE_SIZE, DK_CMDMEM_ALIGNMENT );
	ASSERT_M( cmdMem.IsValid(), "RageDisplay_Deko3D: dynamic command ring exhausted (increase DYNAMIC_CMD_SLICE_SIZE)" );
	m_DynamicCmdBuf.addMemory( cmdMem.hBlock, cmdMem.iOffset, cmdMem.iSize );

	return RageDisplay::BeginFrame();
}

void RageDisplay_Deko3D::EndFrame()
{
	// Submit this frame's accumulated draw commands BEFORE signalling the
	// ring pools' fences below - the fence must only signal once the GPU
	// work that reads this frame's slice has actually been enqueued, or
	// BeginFrame()'s fence.wait() N frames from now would return early
	// while the GPU could still be reading data this slice is about to be
	// overwritten with.
	m_Queue.submitCommands( m_DynamicCmdBuf.finishList() );

	m_pDynamicCmdPool->EndFrame( m_Queue );
	m_pDynamicDataPool->EndFrame( m_Queue );

	LOG->Trace( "RageDisplay_Deko3D::EndFrame: frame #%llu, presenting slot %d",
		(unsigned long long)m_iBeginFrameCount, m_iCurFramebufferSlot );
	m_Queue.presentImage( m_Swapchain, m_iCurFramebufferSlot );
	m_iEndFrameCount++;

	RageDisplay::EndFrame();
}

ActualVideoModeParams RageDisplay_Deko3D::GetActualVideoModeParams() const
{
	return m_CurrentParams;
}

void RageDisplay_Deko3D::GetDisplaySpecs( DisplaySpecs &out ) const
{
	// The Switch has exactly two fixed display modes (handheld/docked); it
	// does not expose a display-selection UI the way desktop platforms do.
	// Stubbed empty for Phase 1 - see 08-Deko3D-Feasibility.md /
	// 09-Deko3D-SpritePipelineCache.md open items on docked/handheld
	// (onOperationMode) handling, which is what should populate this
	// properly once implemented.
	out.clear();
}

const RageDisplay::RagePixelFormatDesc *RageDisplay_Deko3D::GetPixelFormatDesc( RagePixelFormat pf ) const
{
	ASSERT( pf < NUM_RagePixelFormat );
	return &PIXEL_FORMAT_DESC[pf];
}

// ---------------------------------------------------------------------
// Fixed-function state setters - just record; FlushState() does the work
// ---------------------------------------------------------------------

void RageDisplay_Deko3D::SetBlendMode( BlendMode mode ) { m_Pending.blendMode = mode; }
void RageDisplay_Deko3D::SetCullMode( CullMode mode ) { m_Pending.cullMode = mode; }
void RageDisplay_Deko3D::SetZTestMode( ZTestMode mode ) { m_Pending.zTestMode = mode; m_Pending.bZTest = (mode != ZTEST_OFF); }
void RageDisplay_Deko3D::SetZWrite( bool b ) { m_Pending.bZWrite = b; }
void RageDisplay_Deko3D::ClearZBuffer()
{
	// clearDepthStencil() always writes, unlike glClear(GL_DEPTH_BUFFER_BIT)
	// (which respects glDepthMask) - no SetZWrite(true)/restore dance needed.
	m_DynamicCmdBuf.clearDepthStencil( true, 1.0f, 0xFF, 0 );
}
void RageDisplay_Deko3D::SetTextureMode( TextureUnit /*tu*/, TextureMode tm ) { m_Pending.textureMode = tm; }
void RageDisplay_Deko3D::SetTextureWrapping( TextureUnit /*tu*/, bool b ) { m_Pending.bTextureWrap = b; }
void RageDisplay_Deko3D::SetTextureFiltering( TextureUnit /*tu*/, bool b ) { m_Pending.bTextureFilter = b; }
void RageDisplay_Deko3D::SetTexture( TextureUnit /*tu*/, uintptr_t iTexture ) { m_Pending.iBoundTexture = iTexture; }
void RageDisplay_Deko3D::ClearAllTextures() { m_Pending.iBoundTexture = 0; }

void RageDisplay_Deko3D::SetEffectMode( EffectMode em ) { m_Pending.effectMode = em; }
bool RageDisplay_Deko3D::IsEffectModeSupported( EffectMode em ) { return em == EffectMode_Normal; } // only Normal has a shader in Phase 1

// ---------------------------------------------------------------------
// Model lighting/materials (doc 09 S10). Only light index 0 is tracked -
// the only index any real caller uses (see header comment).
// ---------------------------------------------------------------------

RageDisplay_Deko3D::ModelLightState::ModelLightState():
	emissive(0,0,0,1), ambient(0,0,0,1), diffuse(1,1,1,1),
	bLightingEnabled(false), bLight0Enabled(false),
	light0Ambient(0,0,0,1), light0Diffuse(1,1,1,1),
	light0Dir(0,0,-1)
{
}

void RageDisplay_Deko3D::SetMaterial( const RageColor &emissive, const RageColor &ambient,
	const RageColor &diffuse, const RageColor &/*specular*/, float /*shininess*/ )
{
	// Specular/shininess not tracked - no eye-position uniform to compute it with.
	m_ModelLight.emissive = emissive;
	m_ModelLight.ambient = ambient;
	m_ModelLight.diffuse = diffuse;
}

void RageDisplay_Deko3D::SetLighting( bool b ) { m_ModelLight.bLightingEnabled = b; }

void RageDisplay_Deko3D::SetLightOff( int index )
{
	if( index == 0 )
		m_ModelLight.bLight0Enabled = false;
}

void RageDisplay_Deko3D::SetLightDirectional( int index, const RageColor &ambient,
	const RageColor &diffuse, const RageColor &/*specular*/, const RageVector3 &dir )
{
	if( index != 0 )
		return;
	m_ModelLight.bLight0Enabled = true;
	m_ModelLight.light0Ambient = ambient;
	m_ModelLight.light0Diffuse = diffuse;
	m_ModelLight.light0Dir = dir;
}

void RageDisplay_Deko3D::SetSphereEnvironmentMapping( TextureUnit, bool ) { }
void RageDisplay_Deko3D::SetCelShaded( int ) { }

namespace
{
	// std140-friendly (vec4/mat4-sized fields) - matches model_*.glsl's uniform blocks.
	struct ModelTransformUniform
	{
		RageMatrix mvp;
		RageMatrix world;
	};

	struct ModelMaterialLightUniform
	{
		float matEmissive[4];
		float matAmbient[4];
		float matDiffuse[4];
		float flags[4]; // x=lighting enabled, y=light0 enabled
		float lightAmbient[4];
		float lightDiffuse[4];
		float lightDirWorld[4];
	};

	// RageModelVertex has no color (models use material color); GPU-side
	// layout for model_vsh.glsl's inPosition/inNormal/inTexCoord.
	struct ModelVertexGpu
	{
		RageVector3 p;
		RageVector3 n;
		RageVector2 t;
	};
}

// Private per-backend subclass, same pattern as RageCompiledGeometrySWOGL/
// HWOGL and RageCompiledGeometrySWD3D - not a new engine-wide abstraction.
class RageCompiledGeometryDeko3D : public RageCompiledGeometry
{
public:
	RageCompiledGeometryDeko3D( RageDisplay_Deko3D *pOwner ): m_pOwner(pOwner) { }
	~RageCompiledGeometryDeko3D()
	{
		if( m_VertMem.IsValid() )
			m_pOwner->m_pModelGeometryPool->Free( m_VertMem );
		if( m_IdxMem.IsValid() )
			m_pOwner->m_pModelGeometryPool->Free( m_IdxMem );
	}

	void Allocate( const vector<msMesh> &/*vMeshes*/ )
	{
		if( m_VertMem.IsValid() )
			m_pOwner->m_pModelGeometryPool->Free( m_VertMem );
		if( m_IdxMem.IsValid() )
			m_pOwner->m_pModelGeometryPool->Free( m_IdxMem );

		size_t iNumVerts = max( 1u, GetTotalVertices() );
		size_t iNumTriangles = max( 1u, GetTotalTriangles() );
		uint32_t iVertBytes = (uint32_t)( iNumVerts * sizeof(ModelVertexGpu) );
		uint32_t iIdxBytes = (uint32_t)( iNumTriangles * 3 * sizeof(uint16_t) );

		m_VertMem = m_pOwner->m_pModelGeometryPool->Allocate( iVertBytes, alignof(ModelVertexGpu) );
		m_IdxMem = m_pOwner->m_pModelGeometryPool->Allocate( iIdxBytes, alignof(uint16_t) );
		ASSERT_M( m_VertMem.IsValid() && m_IdxMem.IsValid(),
			"RageDisplay_Deko3D: model geometry pool allocation failed - see MODEL_GEOMETRY_POOL_SIZE" );
	}

	void Change( const vector<msMesh> &vMeshes )
	{
		ModelVertexGpu *pVerts = (ModelVertexGpu *)m_VertMem.pCpuAddr;
		uint16_t *pIdx = (uint16_t *)m_IdxMem.pCpuAddr;

		for( unsigned i = 0; i < vMeshes.size(); ++i )
		{
			const MeshInfo &meshInfo = m_vMeshInfo[i];
			const msMesh &mesh = vMeshes[i];
			const vector<RageModelVertex> &Vertices = mesh.Vertices;
			const vector<msTriangle> &Triangles = mesh.Triangles;

			for( unsigned j = 0; j < Vertices.size(); ++j )
			{
				ModelVertexGpu &v = pVerts[meshInfo.iVertexStart + j];
				v.p = Vertices[j].p;
				v.n = Vertices[j].n;
				v.t = Vertices[j].t;
			}

			// Indices are absolute (offset by iVertexStart) - all meshes share one buffer pair.
			for( unsigned j = 0; j < Triangles.size(); ++j )
				for( int k = 0; k < 3; ++k )
					pIdx[(meshInfo.iTriangleStart+j)*3+k] = (uint16_t)(meshInfo.iVertexStart + Triangles[j].nVertexIndices[k]);
		}
	}

	void Draw( int iMeshIndex ) const
	{
		const MeshInfo &meshInfo = m_vMeshInfo[iMeshIndex];
		if( meshInfo.iTriangleCount <= 0 )
			return;

		dk::CmdBuf cmdbuf = m_pOwner->m_DynamicCmdBuf;
		std::array<DkVtxAttribState, 3> attribs = {{
			{ 0, 0, (uint32_t)offsetof(ModelVertexGpu, p), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
			{ 0, 0, (uint32_t)offsetof(ModelVertexGpu, n), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
			{ 0, 0, (uint32_t)offsetof(ModelVertexGpu, t), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
		}};
		std::array<DkVtxBufferState, 1> vtxBufState = {{ { sizeof(ModelVertexGpu), 0 } }};
		cmdbuf.bindVtxAttribState( attribs );
		cmdbuf.bindVtxBufferState( vtxBufState );
		cmdbuf.bindVtxBuffer( 0, m_VertMem.iGpuAddr, m_VertMem.iSize );
		cmdbuf.bindIdxBuffer( DkIdxFormat_Uint16, m_IdxMem.iGpuAddr );
		// vertexOffset=0: indices already store absolute vertex indices (see Change()).
		cmdbuf.drawIndexed( DkPrimitive_Triangles, meshInfo.iTriangleCount * 3, 1, meshInfo.iTriangleStart * 3, 0, 0 );
	}

private:
	RageDisplay_Deko3D *m_pOwner;
	Deko3DAlloc m_VertMem;
	Deko3DAlloc m_IdxMem;
};

RageCompiledGeometry *RageDisplay_Deko3D::CreateCompiledGeometry()
{
	return new RageCompiledGeometryDeko3D( this );
}
void RageDisplay_Deko3D::DeleteCompiledGeometry( RageCompiledGeometry *p ) { delete p; }

void RageDisplay_Deko3D::DrawCompiledGeometryInternal( const RageCompiledGeometry *p, int iMeshIndex )
{
	ModelTransformUniform transform;
	RageMatrix modelView;
	RageMatrixMultiply( &modelView, GetViewTop(), GetWorldTop() );
	RageMatrix projection;
	RageMatrixMultiply( &projection, GetCentering(), GetProjectionTop() );
	RageMatrixMultiply( &transform.mvp, &projection, &modelView );
	transform.world = *GetWorldTop();

	Deko3DAlloc transformMem = m_pDynamicDataPool->Allocate( sizeof(transform), DK_UNIFORM_BUF_ALIGNMENT );
	ASSERT_M( transformMem.IsValid(), "RageDisplay_Deko3D: dynamic uniform ring exhausted for this frame (model transform)" );
	std::memcpy( transformMem.pCpuAddr, &transform, sizeof(transform) );

	ModelMaterialLightUniform ml;
	std::memcpy( ml.matEmissive, (const float *)m_ModelLight.emissive, sizeof(ml.matEmissive) );
	std::memcpy( ml.matAmbient, (const float *)m_ModelLight.ambient, sizeof(ml.matAmbient) );
	std::memcpy( ml.matDiffuse, (const float *)m_ModelLight.diffuse, sizeof(ml.matDiffuse) );
	ml.flags[0] = m_ModelLight.bLightingEnabled ? 1.0f : 0.0f;
	ml.flags[1] = m_ModelLight.bLight0Enabled ? 1.0f : 0.0f;
	ml.flags[2] = ml.flags[3] = 0.0f;
	std::memcpy( ml.lightAmbient, (const float *)m_ModelLight.light0Ambient, sizeof(ml.lightAmbient) );
	std::memcpy( ml.lightDiffuse, (const float *)m_ModelLight.light0Diffuse, sizeof(ml.lightDiffuse) );
	ml.lightDirWorld[0] = m_ModelLight.light0Dir.x;
	ml.lightDirWorld[1] = m_ModelLight.light0Dir.y;
	ml.lightDirWorld[2] = m_ModelLight.light0Dir.z;
	ml.lightDirWorld[3] = 0.0f;

	Deko3DAlloc mlMem = m_pDynamicDataPool->Allocate( sizeof(ml), DK_UNIFORM_BUF_ALIGNMENT );
	ASSERT_M( mlMem.IsValid(), "RageDisplay_Deko3D: dynamic uniform ring exhausted for this frame (model material/light)" );
	std::memcpy( mlMem.pCpuAddr, &ml, sizeof(ml) );

	FlushCommonState( m_DynamicCmdBuf );
	std::array<DkShader const *, 2> shaders = { &m_ModelVertexShader, &m_ModelFragmentShader };
	m_DynamicCmdBuf.bindShaders( DkStageFlag_Vertex | DkStageFlag_Fragment, shaders );
	m_DynamicCmdBuf.bindUniformBuffer( DkStage_Vertex, 0, transformMem.iGpuAddr, transformMem.iSize );
	m_DynamicCmdBuf.bindUniformBuffer( DkStage_Fragment, 1, mlMem.iGpuAddr, mlMem.iSize );

	p->Draw( iMeshIndex );
}

RageSurface *RageDisplay_Deko3D::CreateScreenshot()
{
	// Needs a readback-direction memory pool (CPU-read/GPU-write) distinct
	// from the upload-staging scratch pool - see doc 10 "sixth memory
	// pattern" note. Not implemented in Phase 1.
	LOG->Warn( "RageDisplay_Deko3D::CreateScreenshot: not implemented yet" );
	return nullptr;
}

// ---------------------------------------------------------------------
// Texture format handling (11-Deko3D-TextureFormatAndShaderContract.md)
// ---------------------------------------------------------------------

DkImageFormat RageDisplay_Deko3D::RagePixelFormatToDkImageFormat( RagePixelFormat pixfmt )
{
	switch( pixfmt )
	{
	case RagePixelFormat_RGBA8:  return DkImageFormat_RGBA8_Unorm;
	case RagePixelFormat_BGRA8:  return DkImageFormat_BGRA8_Unorm;
	case RagePixelFormat_RGBA4:  return DkImageFormat_RGBA4_Unorm;
	case RagePixelFormat_RGB5A1: return DkImageFormat_RGB5A1_Unorm;
	case RagePixelFormat_RGB5:   return DkImageFormat_RGB5_Unorm;
	default:
		FAIL_M( ssprintf("RageDisplay_Deko3D: %s has no native DkImageFormat; "
			"GetImgPixelFormat() should have converted this to RGBA8 already",
			RagePixelFormatToString(pixfmt).c_str()) );
		return DkImageFormat_RGBA8_Unorm;
	}
}

bool RageDisplay_Deko3D::SupportsTextureFormat( RagePixelFormat pixfmt, bool /*bRealtime*/ )
{
	// Compatibility-first (11-Deko3D-TextureFormatAndShaderContract.md):
	// only report the formats deko3d handles natively. RageBitmapTexture.cpp
	// already falls back to RGBA8/RGBA4 on its own when this returns false
	// (RageBitmapTexture.cpp:234-240) - no change needed there.
	switch( pixfmt )
	{
	case RagePixelFormat_RGBA8:
	case RagePixelFormat_BGRA8:
	case RagePixelFormat_RGBA4:
	case RagePixelFormat_RGB5A1:
	case RagePixelFormat_RGB5:
		return true;
	default:
		return false;
	}
}

RagePixelFormat RageDisplay_Deko3D::GetImgPixelFormat( RageSurface *&pImg, bool &bFreeImg, int iWidth, int iHeight, bool bPalettedTexture )
{
	// Modeled directly on RageDisplay_Legacy::GetImgPixelFormat()
	// (RageDisplay_OGL.cpp:2131-2166) - see
	// 11-Deko3D-TextureFormatAndShaderContract.md for why this shape was
	// chosen instead of inventing a new one.
	RagePixelFormat pixfmt = FindPixelFormat( pImg->format->BitsPerPixel,
		pImg->format->Rmask, pImg->format->Gmask, pImg->format->Bmask, pImg->format->Amask );

	// RageSurface_Load_PNG.cpp produces 32bpp surfaces with Amask=0 for any
	// source PNG that has no alpha channel (RageSurface_Load_PNG.cpp:198-233):
	// png_set_filler(png, 0xff, PNG_FILLER_AFTER) still writes a real 4th
	// byte per pixel, always 0xFF, at the same position an alpha byte would
	// occupy - the surface just doesn't *advertise* that byte as alpha.
	// FindPixelFormat()'s exact memcmp can never match this against RGBA8/
	// BGRA8 (whose descs require a non-zero Amask), even though the pixel
	// bytes it would convert (via the Blit() fallback right below) are
	// already byte-identical to a fully-opaque RGBA8/BGRA8 image. Recognize
	// this directly instead of paying for, and warning about, a full
	// software conversion that only ever produces the same bytes it started
	// with. Every opaque (non-transparent) background/UI PNG hits this path,
	// so on Switch this was not the rare case the comment above assumed.
	if( pixfmt == RagePixelFormat_Invalid && pImg->format->BitsPerPixel == 32 && pImg->format->Amask == 0 )
	{
		const RagePixelFormatDesc *pRGBA8 = GetPixelFormatDesc( RagePixelFormat_RGBA8 );
		const RagePixelFormatDesc *pBGRA8 = GetPixelFormatDesc( RagePixelFormat_BGRA8 );
		if( pImg->format->Rmask == pRGBA8->masks[0] && pImg->format->Gmask == pRGBA8->masks[1] &&
			pImg->format->Bmask == pRGBA8->masks[2] )
			pixfmt = RagePixelFormat_RGBA8;
		else if( pImg->format->Rmask == pBGRA8->masks[0] && pImg->format->Gmask == pBGRA8->masks[1] &&
			pImg->format->Bmask == pBGRA8->masks[2] )
			pixfmt = RagePixelFormat_BGRA8;
	}

	// deko3d has no hardware palette/indexed image format at all (checked
	// against the full DkImageFormat enum) - never "supported" natively.
	bool bSupported = !bPalettedTexture
		&& pixfmt != RagePixelFormat_Invalid
		&& SupportsTextureFormat( pixfmt );

	if( !bSupported )
	{
		// pImg->format->BitsPerPixel==8 is checked directly here rather than
		// trusting bPalettedTexture: RageBitmapTexture.cpp:198-231 only sets
		// pixfmt to RagePixelFormat_PAL (which is what bPalettedTexture is
		// derived from, CreateTexture() above) when SupportsTextureFormat()
		// already agreed to accept paletted textures. Since deko3d never
		// does, that branch is always skipped there and pixfmt comes back as
		// an ordinary RGB5A1/RGBA4/RGBA8 choice instead - even though pImg
		// itself is still the original 8bpp paletted surface at this point.
		// Without this check that case logged as "(unrecognized)", the exact
		// same label a genuine mask-matching failure gets, even though it's
		// neither unrecognized nor a bug: RageSurfaceUtils::Blit() has a
		// dedicated, correct PAL->RGBA path (RageSurfaceUtils.cpp:670-672)
		// for it - just no native indexed *GPU* texture format to upload to.
		if( pImg->format->BitsPerPixel == 8 )
			LOG->Warn(
				"Paletted (indexed) texture has no native deko3d equivalent - deko3d "
				"has no hardware indexed texture format at all, so this always converts "
				"to RGBA8 in software. This is slower to load; there is no re-export fix." );
		else
			LOG->Warn(
				"Texture format %s has no native deko3d equivalent; converting to RGBA8 "
				"in software. This is slower to load. For better load times, re-export "
				"this asset as RGBA8, BGRA8, RGBA4, RGB5A1, or RGB5.",
				pixfmt == RagePixelFormat_Invalid ? "(unrecognized)" : RagePixelFormatToString(pixfmt).c_str() );

		pixfmt = RagePixelFormat_RGBA8;
		const RagePixelFormatDesc *pfd = GetPixelFormatDesc( pixfmt );
		RageSurface *pConv = CreateSurface( pImg->w, pImg->h, pfd->bpp,
			pfd->masks[0], pfd->masks[1], pfd->masks[2], pfd->masks[3] );
		RageSurfaceUtils::Blit( pImg, pConv, iWidth, iHeight );
		pImg = pConv;
		bFreeImg = true;
	}
	else
	{
		bFreeImg = false;
	}

	return pixfmt;
}

uintptr_t RageDisplay_Deko3D::CreateTexture( RagePixelFormat pixfmt, RageSurface *pImg, bool /*bGenerateMipMaps*/ )
{
	ASSERT( pixfmt < NUM_RagePixelFormat );

	bool bFreeImg = false;
	RagePixelFormat actualFmt = GetImgPixelFormat( pImg, bFreeImg, pImg->w, pImg->h, pixfmt == RagePixelFormat_PAL );
	DkImageFormat dkFormat = RagePixelFormatToDkImageFormat( actualFmt );

	// RageTexture's UV math (RageTexture.h: GetImageToTexCoordsRatioX() =
	// 1.0f/GetTextureWidth()) assumes every texture is allocated at a
	// power-of-two size, with the real image content occupying only the
	// top-left (pImg->w x pImg->h) sub-rect of it - RageBitmapTexture.cpp
	// (the path every ordinary sprite/PNG texture goes through) pads pImg
	// itself to that size before ever calling CreateTexture(), so pImg->w/h
	// already equals the padded size there. MovieTexture_Generic.cpp does
	// NOT do this - it computes m_iTextureWidth/Height as power-of-two
	// (used for the same UV math) but hands CreateTexture() a surface at
	// the raw, unpadded m_iImageWidth/Height. Since this function's image
	// allocation used to just take pImg->w/h directly, movie textures ended
	// up allocated at their real (non-padded) size while Sprite still
	// divided by the padded size when computing UV coordinates - sampling
	// only the top-left fraction of the texture and stretching it across
	// the whole quad (a crop/zoom-in artifact, not a transform/scale bug).
	// Padding the GPU allocation here - independent of whatever pImg->w/h
	// already is - fixes movies and is a no-op for regular textures, which
	// arrive already at a power-of-two size (power_of_two() of an existing
	// power-of-two value returns that same value).
	int iAllocWidth = power_of_two( pImg->w );
	int iAllocHeight = power_of_two( pImg->h );

	dk::ImageLayout layout;
	dk::ImageLayoutMaker( m_Device )
		.setFlags( 0 )
		.setFormat( dkFormat )
		.setDimensions( iAllocWidth, iAllocHeight )
		.initialize( layout );

	TextureRecord *pRec = new TextureRecord();
	pRec->iWidth = pImg->w;
	pRec->iHeight = pImg->h;
	pRec->ImageMem = m_pImagePool->Allocate( (uint32_t)layout.getSize(), layout.getAlignment() );
	// This was previously unguarded, unlike the vertex/uniform ring
	// allocations - a real gap: if m_pImagePool is exhausted (IMAGE_POOL_SIZE
	// too small for the live texture set), Allocate() returns an invalid
	// Deko3DAlloc and this would silently hand pRec->Image.initialize() a
	// null/garbage memory block, producing a texture that GPU-faults the
	// instant anything samples it - every real (non-fallback) texture goes
	// through this exact path, so this was a real, previously-unchecked
	// candidate for the null-address page fault, not just a theoretical one.
	ASSERT_M( pRec->ImageMem.IsValid(),
		ssprintf("RageDisplay_Deko3D: texture image pool allocation failed (%ux%u, %u bytes) - "
			"m_pImagePool likely exhausted, see IMAGE_POOL_SIZE", pImg->w, pImg->h, (uint32_t)layout.getSize()).c_str() );
	pRec->Image.initialize( layout, pRec->ImageMem.hBlock, pRec->ImageMem.iOffset );

	pRec->iImageDescriptorSlot = m_pImageDescriptors->AllocateSlot();
	if( pRec->iImageDescriptorSlot < 0 )
	{
		// Table exhausted - a real, reportable failure (doc 10 open item #2),
		// not something to silently corrupt a neighboring slot over.
		m_pImagePool->Free( pRec->ImageMem );
		delete pRec;
		if( bFreeImg )
			delete pImg;
		return 0;
	}

	// Upload: staging buffer (CPU-writable) -> copyBufferToImage -> final
	// block-linear image, exactly the pattern confirmed in
	// SampleFramework/CExternalImage.cpp (09-Deko3D-SpritePipelineCache.md S9,
	// 10-Deko3D-MemoryAllocator.md Pattern C).
	uint32_t iRowBytes = pImg->pitch;
	uint32_t iUploadSize = iRowBytes * pImg->h;
	Deko3DAlloc staging = m_pScratchPool->Allocate( iUploadSize, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT );
	ASSERT_M( staging.IsValid(),
		ssprintf("RageDisplay_Deko3D: texture upload staging allocation failed (%u bytes) - "
			"m_pScratchPool likely exhausted, see SCRATCH_POOL_SIZE", iUploadSize).c_str() );
	std::memcpy( staging.pCpuAddr, pImg->pixels, iUploadSize );

	// Uses m_StreamingCmdBuf, NOT m_SetupCmdBuf - this was the actual cause
	// of a second real on-device "GPU method error" crash (same signature as
	// the one UpdateTexture() originally caused): CreateTexture() is not
	// reliably called only at safe, pre-frame load time as originally
	// assumed - e.g. entering ScreenGameplay creates textures mid-frame too,
	// after BeginFrame()'s render-target-bind submission on m_SetupCmdBuf
	// (which is never waitIdle()'d) but before that frame ends. Sharing
	// m_SetupCmdBuf here would let this Clear()+reuse the same memory the
	// GPU could still be reading. m_StreamingCmdBuf is fine to share with
	// UpdateTexture() (Deko3D backend's own comment on RearmStreamingCmdBuf()):
	// both are fully synchronous (submit+waitIdle before returning) and
	// StepMania's render loop is single-threaded, so one always completes
	// before the next begins - they just can't share with BeginFrame()'s
	// un-waited submission.
	RearmStreamingCmdBuf();
	dk::ImageView imageView( pRec->Image );
	m_StreamingCmdBuf.copyBufferToImage( { staging.iGpuAddr }, imageView, { 0, 0, 0, (uint32_t)pImg->w, (uint32_t)pImg->h, 1 } );
	m_Queue.submitCommands( m_StreamingCmdBuf.finishList() );
	m_Queue.waitIdle(); // synchronous, matching RageTextureManager's existing load-time (not per-frame) usage
	m_pScratchPool->Clear();

	dk::ImageDescriptor *pDesc = (dk::ImageDescriptor *)m_pImageDescriptors->GetSlotCpuAddr( pRec->iImageDescriptorSlot );
	pDesc->initialize( imageView );

	if( bFreeImg )
		delete pImg;

	m_vTextures.push_back( pRec );
	return (uintptr_t)m_vTextures.size(); // 1-based; 0 means "no texture" throughout RageDisplay
}

void RageDisplay_Deko3D::UpdateTexture( uintptr_t iTexHandle, RageSurface *pImg, int iXOffset, int iYOffset, int iWidth, int iHeight )
{
	// This was a no-op stub through Phase 1's initial static-sprite-only
	// scope, which meant every streaming texture consumer (movies, chiefly
	// MovieTexture_Generic::UpdateFrame(), MovieTexture_Generic.cpp:467/481)
	// uploaded exactly one (blank/garbage, since CreateTexture() is first
	// called with a nullptr-pixels placeholder surface - MovieTexture_Generic.cpp:160-168)
	// frame and then silently never updated again: movies rendered as a
	// frozen or blank/white texture instead of playing.
	if( iTexHandle == 0 || iTexHandle > m_vTextures.size() )
		return;
	TextureRecord *pRec = m_vTextures[iTexHandle - 1];
	if( pRec == nullptr )
		return;

	// Unlike CreateTexture(), this deliberately does NOT run pImg through
	// GetImgPixelFormat()'s conversion path: the GPU-side image's format was
	// fixed for good at creation time, and every caller (MovieTexture_Generic
	// reuses the same RageSurfaceFormat, built once, for every frame it
	// decodes into) is expected to keep handing back that same format -
	// re-deriving/silently changing it here would corrupt what gets uploaded.
	uint32_t iBytesPerPixel = (uint32_t)pImg->fmt.BytesPerPixel;
	uint32_t iRowBytes = (uint32_t)iWidth * iBytesPerPixel;
	uint32_t iUploadSize = iRowBytes * (uint32_t)iHeight;
	if( iUploadSize == 0 )
		return;

	Deko3DAlloc staging = m_pScratchPool->Allocate( iUploadSize, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT );
	ASSERT_M( staging.IsValid(),
		ssprintf("RageDisplay_Deko3D: streaming texture upload staging allocation failed (%u bytes) - "
			"m_pScratchPool likely exhausted, see SCRATCH_POOL_SIZE", iUploadSize).c_str() );

	// Copied row-by-row into the staging buffer rather than one memcpy:
	// pImg->pitch can include padding beyond iWidth*BytesPerPixel, and the
	// update region doesn't necessarily start at the surface's own origin,
	// but copyBufferToImage()'s source buffer must be tightly packed to
	// iWidth - unlike pImg itself, which may not be.
	const uint8_t *pSrc = (const uint8_t *)pImg->pixels + (size_t)iYOffset * pImg->pitch + (size_t)iXOffset * iBytesPerPixel;
	uint8_t *pDst = (uint8_t *)staging.pCpuAddr;
	for( int y = 0; y < iHeight; ++y )
	{
		std::memcpy( pDst, pSrc, iRowBytes );
		pSrc += pImg->pitch;
		pDst += iRowBytes;
	}

	// Uses its own command buffer/pool (RearmStreamingCmdBuf(), not
	// RearmSetupCmdBuf()) - see the comment on RearmSetupCmdBuf() for the
	// real, on-device race this avoids: this can run mid-frame (from a
	// movie's Sprite::Draw()), and BeginFrame()'s render-target bind on
	// m_SetupCmdBuf is never waited on before that happens.
	RearmStreamingCmdBuf();
	dk::ImageView imageView( pRec->Image );
	m_StreamingCmdBuf.copyBufferToImage( { staging.iGpuAddr }, imageView,
		{ (uint32_t)iXOffset, (uint32_t)iYOffset, 0, (uint32_t)iWidth, (uint32_t)iHeight, 1 } );
	m_Queue.submitCommands( m_StreamingCmdBuf.finishList() );
	m_Queue.waitIdle(); // synchronous, matching CreateTexture()'s existing load-time pattern
	m_pScratchPool->Clear();
}

void RageDisplay_Deko3D::DeleteTexture( uintptr_t iTexHandle )
{
	if( iTexHandle == 0 || iTexHandle > m_vTextures.size() )
		return;

	TextureRecord *pRec = m_vTextures[iTexHandle - 1];
	if( pRec == nullptr )
		return;

	m_pImageDescriptors->FreeSlot( pRec->iImageDescriptorSlot );
	m_pImagePool->Free( pRec->ImageMem );
	delete pRec;
	m_vTextures[iTexHandle - 1] = nullptr;
}

// ---------------------------------------------------------------------
// Draw path (09-Deko3D-SpritePipelineCache.md S7)
// ---------------------------------------------------------------------

RageDisplay_Deko3D::SpriteShaderVariant RageDisplay_Deko3D::GetShaderVariantForCurrentState() const
{
	// Only the two TextureMode values Sprite::DrawTexture() actually uses
	// have shaders in Phase 1 (Sprite.cpp:633,658, see doc 09 S1/S6).
	switch( m_Pending.textureMode )
	{
	case TextureMode_Glow: return SpriteShader_Glow;
	case TextureMode_Modulate:
	default: return SpriteShader_Modulate;
	}
}

void RageDisplay_Deko3D::FlushState( dk::CmdBuf cmdbuf )
{
	FlushCommonState( cmdbuf );

	SpriteShaderVariant variant = GetShaderVariantForCurrentState();
	std::array<DkShader const *, 2> shaders = { &m_VertexShader, &m_FragmentShaders[variant] };
	cmdbuf.bindShaders( DkStageFlag_Vertex | DkStageFlag_Fragment, shaders );
}

void RageDisplay_Deko3D::FlushCommonState( dk::CmdBuf cmdbuf )
{
	dk::RasterizerState rasterizerState;
	switch( m_Pending.cullMode )
	{
	case CULL_FRONT: rasterizerState.setCullMode( DkFace_Front ); break;
	case CULL_BACK:  rasterizerState.setCullMode( DkFace_Back ); break;
	case CULL_NONE:
	default:         rasterizerState.setCullMode( DkFace_None ); break;
	}
	cmdbuf.bindRasterizerState( rasterizerState );

	// Blend: needs BOTH a DkBlendState AND a separate DkColorState enable
	// bit - a gap the first pass at this design missed, caught by reading
	// deko_basic (never enables blending) against Example05_Tessellation
	// (does), see 09-Deko3D-SpritePipelineCache.md S9/S8.
	dk::ColorState colorState;
	bool bBlendOn = (m_Pending.blendMode != BLEND_NO_EFFECT);
	colorState.setBlendEnable( 0, bBlendOn );
	cmdbuf.bindColorState( colorState );

	dk::ColorWriteState colorWriteState;
	cmdbuf.bindColorWriteState( colorWriteState );

	if( bBlendOn )
	{
		dk::BlendState blendState;
		blendState.setOps( RageToDkBlendOp(m_Pending.blendMode), RageToDkBlendOp(m_Pending.blendMode) );
		blendState.setFactors(
			RageToDkBlendFactor(true, m_Pending.blendMode, false),
			RageToDkBlendFactor(false, m_Pending.blendMode, false),
			DkBlendFactor_One, DkBlendFactor_Zero );
		cmdbuf.bindBlendStates( 0, blendState );
	}

	dk::DepthStencilState depthState;
	depthState.setDepthTestEnable( m_Pending.bZTest );
	depthState.setDepthWriteEnable( m_Pending.bZWrite );
	depthState.setDepthCompareOp( RageToDkCompareOp( m_Pending.zTestMode ) );
	cmdbuf.bindDepthStencilState( depthState );

	// ALWAYS bind something to fragment-stage slot 0, even for "no texture"
	// draws (m_Pending.iBoundTexture==0) - both shader variants unconditionally
	// sample it (deko3d_shaders/sprite_*_fsh.glsl have no untextured variant),
	// and unlike OpenGL's texture-unit model, deko3d's bindTextures() is a
	// raw, persistent hardware register: skipping the bind here (the
	// original behavior) left it holding whatever it last held, which on the
	// very first draw of the very first frame - before any real texture had
	// ever been bound - was never written at all. That produced a real GPU
	// page fault (confirmed via Queue::checkError() after switching to the
	// debug deko3d lib: "GPU page fault, Address: 0x0000000000, Access
	// type: Read"), not just a cosmetic no-op. m_iWhiteTextureDescriptorSlot
	// (CreateWhiteTexture(), called once at Init()) makes
	// texture(tex,...)*inColor reduce to exactly inColor for untextured
	// draws, which is also the semantically correct result, not just a
	// crash-safe placeholder.
	int32_t iImageDescriptorSlot = m_iWhiteTextureDescriptorSlot;
	if( m_Pending.iBoundTexture != 0 && m_Pending.iBoundTexture <= m_vTextures.size() )
	{
		TextureRecord *pRec = m_vTextures[m_Pending.iBoundTexture - 1];
		if( pRec != nullptr )
			iImageDescriptorSlot = pRec->iImageDescriptorSlot;
	}
	int32_t iSamplerSlot = GetOrCreateSamplerSlot( m_Pending.bTextureWrap, m_Pending.bTextureFilter );

	// dkMakeTextureHandle() below packs these two ints into an opaque
	// DkResHandle with no validation of its own - a negative slot (e.g.
	// m_iWhiteTextureDescriptorSlot still at its -1 "never created" sentinel,
	// or an allocator returning -1 for "table exhausted") silently becomes
	// (uint32_t)-1 = 0xFFFFFFFF, an out-of-range descriptor-table index the
	// GPU has no way to reject before dereferencing it. Catch that on the
	// CPU, synchronously, at the exact bind call, instead of letting it
	// surface as an unattributable async GPU page fault frames later.
	ASSERT_M( iImageDescriptorSlot >= 0,
		ssprintf("RageDisplay_Deko3D: about to bind an invalid image descriptor slot (%d) - "
			"iBoundTexture=%llu, m_iWhiteTextureDescriptorSlot=%d",
			iImageDescriptorSlot, (unsigned long long)m_Pending.iBoundTexture, m_iWhiteTextureDescriptorSlot).c_str() );
	ASSERT_M( iSamplerSlot >= 0,
		ssprintf("RageDisplay_Deko3D: about to bind an invalid sampler slot (%d) - bWrap=%d bFilter=%d",
			iSamplerSlot, m_Pending.bTextureWrap, m_Pending.bTextureFilter).c_str() );

	DkResHandle hTex = dkMakeTextureHandle( (uint32_t)iImageDescriptorSlot, (uint32_t)iSamplerSlot );
	cmdbuf.bindTextures( DkStage_Fragment, 0, hTex );
}

// Shared by every non-indexed Draw*Internal (see header). RageMatrix
// grouping/order and vertex attrib layout are as established for
// DrawQuadsInternal originally - see git history for the citations
// (RageDisplay_OGL.cpp:1015,1028 matrix order; doc 11 for isBgra).
void RageDisplay_Deko3D::DrawPrimitive( DkPrimitive prim, const RageSpriteVertex v[], int iNumVerts )
{
	if( iNumVerts <= 0 )
		return;

	uint32_t iVertBytes = sizeof(RageSpriteVertex) * iNumVerts;
	Deko3DAlloc vertMem = m_pDynamicDataPool->Allocate( iVertBytes, alignof(RageSpriteVertex) );
	ASSERT_M( vertMem.IsValid(), "RageDisplay_Deko3D: dynamic vertex ring exhausted for this frame" );
	std::memcpy( vertMem.pCpuAddr, v, iVertBytes );

	RageMatrix modelView;
	RageMatrixMultiply( &modelView, GetViewTop(), GetWorldTop() );
	RageMatrix projection;
	RageMatrixMultiply( &projection, GetCentering(), GetProjectionTop() );
	RageMatrix final;
	RageMatrixMultiply( &final, &projection, &modelView );

	Deko3DAlloc uniformMem = m_pDynamicDataPool->Allocate( sizeof(RageMatrix), DK_UNIFORM_BUF_ALIGNMENT );
	ASSERT_M( uniformMem.IsValid(), "RageDisplay_Deko3D: dynamic uniform ring exhausted for this frame" );
	std::memcpy( uniformMem.pCpuAddr, &final, sizeof(RageMatrix) );

	FlushState( m_DynamicCmdBuf );

	ASSERT_M( uniformMem.IsValid(), "RageDisplay_Deko3D: uniform buffer GPU address is invalid at bind time" );
	m_DynamicCmdBuf.bindUniformBuffer( DkStage_Vertex, 0, uniformMem.iGpuAddr, uniformMem.iSize );

	std::array<DkVtxAttribState, 3> attribs = {{
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, p), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, c), DkVtxAttribSize_4x8,  DkVtxAttribType_Unorm, 1 }, // isBgra=1
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, t), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	}};
	std::array<DkVtxBufferState, 1> vtxBufState = {{ { sizeof(RageSpriteVertex), 0 } }};
	m_DynamicCmdBuf.bindVtxAttribState( attribs );
	m_DynamicCmdBuf.bindVtxBufferState( vtxBufState );
	ASSERT_M( vertMem.IsValid(), "RageDisplay_Deko3D: vertex buffer GPU address is invalid at bind time" );
	m_DynamicCmdBuf.bindVtxBuffer( 0, vertMem.iGpuAddr, vertMem.iSize );

	m_DynamicCmdBuf.draw( prim, iNumVerts, 1, 0, 0 );

	StatsAddVerts( iNumVerts );
}

void RageDisplay_Deko3D::DrawQuadsInternal( const RageSpriteVertex v[], int iNumVerts ) { DrawPrimitive( DkPrimitive_Quads, v, iNumVerts ); }
void RageDisplay_Deko3D::DrawQuadStripInternal( const RageSpriteVertex v[], int iNumVerts ) { DrawPrimitive( DkPrimitive_QuadStrip, v, iNumVerts ); }
void RageDisplay_Deko3D::DrawFanInternal( const RageSpriteVertex v[], int iNumVerts ) { DrawPrimitive( DkPrimitive_TriangleFan, v, iNumVerts ); }
void RageDisplay_Deko3D::DrawStripInternal( const RageSpriteVertex v[], int iNumVerts ) { DrawPrimitive( DkPrimitive_TriangleStrip, v, iNumVerts ); }
void RageDisplay_Deko3D::DrawTrianglesInternal( const RageSpriteVertex v[], int iNumVerts ) { DrawPrimitive( DkPrimitive_Triangles, v, iNumVerts ); }

// Same index pattern as RageDisplay_OGL.cpp:1505-1531 (4 triangles per
// 3-vertex "piece", used for hold/roll bodies - NoteDisplay.cpp:727).
// deko3d has no native symmetric-quad-strip topology either, so this is
// indexed DkPrimitive_Triangles, same as the GL glDrawElements() fallback.
void RageDisplay_Deko3D::DrawSymmetricQuadStripInternal( const RageSpriteVertex v[], int iNumVerts )
{
	if( iNumVerts <= 0 )
		return;

	int iNumPieces = (iNumVerts-3)/3;
	int iNumIndices = iNumPieces*4*3;
	if( iNumIndices <= 0 )
		return;

	uint32_t iVertBytes = sizeof(RageSpriteVertex) * iNumVerts;
	Deko3DAlloc vertMem = m_pDynamicDataPool->Allocate( iVertBytes, alignof(RageSpriteVertex) );
	ASSERT_M( vertMem.IsValid(), "RageDisplay_Deko3D: dynamic vertex ring exhausted for this frame" );
	std::memcpy( vertMem.pCpuAddr, v, iVertBytes );

	uint32_t iIdxBytes = sizeof(uint16_t) * iNumIndices;
	Deko3DAlloc idxMem = m_pDynamicDataPool->Allocate( iIdxBytes, alignof(uint16_t) );
	ASSERT_M( idxMem.IsValid(), "RageDisplay_Deko3D: dynamic index ring exhausted for this frame" );
	uint16_t *pIdx = (uint16_t *)idxMem.pCpuAddr;
	for( int i = 0; i < iNumPieces; i++ )
	{
		pIdx[i*12+0] = i*3+1; pIdx[i*12+1] = i*3+3; pIdx[i*12+2] = i*3+0;
		pIdx[i*12+3] = i*3+1; pIdx[i*12+4] = i*3+4; pIdx[i*12+5] = i*3+3;
		pIdx[i*12+6] = i*3+1; pIdx[i*12+7] = i*3+5; pIdx[i*12+8] = i*3+4;
		pIdx[i*12+9] = i*3+1; pIdx[i*12+10] = i*3+2; pIdx[i*12+11] = i*3+5;
	}

	RageMatrix modelView;
	RageMatrixMultiply( &modelView, GetViewTop(), GetWorldTop() );
	RageMatrix projection;
	RageMatrixMultiply( &projection, GetCentering(), GetProjectionTop() );
	RageMatrix final;
	RageMatrixMultiply( &final, &projection, &modelView );

	Deko3DAlloc uniformMem = m_pDynamicDataPool->Allocate( sizeof(RageMatrix), DK_UNIFORM_BUF_ALIGNMENT );
	ASSERT_M( uniformMem.IsValid(), "RageDisplay_Deko3D: dynamic uniform ring exhausted for this frame" );
	std::memcpy( uniformMem.pCpuAddr, &final, sizeof(RageMatrix) );

	FlushState( m_DynamicCmdBuf );
	m_DynamicCmdBuf.bindUniformBuffer( DkStage_Vertex, 0, uniformMem.iGpuAddr, uniformMem.iSize );

	std::array<DkVtxAttribState, 3> attribs = {{
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, p), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, c), DkVtxAttribSize_4x8,  DkVtxAttribType_Unorm, 1 },
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, t), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	}};
	std::array<DkVtxBufferState, 1> vtxBufState = {{ { sizeof(RageSpriteVertex), 0 } }};
	m_DynamicCmdBuf.bindVtxAttribState( attribs );
	m_DynamicCmdBuf.bindVtxBufferState( vtxBufState );
	m_DynamicCmdBuf.bindVtxBuffer( 0, vertMem.iGpuAddr, vertMem.iSize );

	ASSERT_M( idxMem.IsValid(), "RageDisplay_Deko3D: index buffer GPU address is invalid at bind time" );
	m_DynamicCmdBuf.bindIdxBuffer( DkIdxFormat_Uint16, idxMem.iGpuAddr );
	m_DynamicCmdBuf.drawIndexed( DkPrimitive_Triangles, iNumIndices, 1, 0, 0, 0 );

	StatsAddVerts( iNumVerts );
}

/*
 * Copyright (c) 2026 the StepMania-nx contributors
 * Same license terms as the rest of stepmania/src (see LICENSE).
 */
