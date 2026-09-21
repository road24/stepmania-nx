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
		{ /* R4G4B4A4 */ 16, { 0xF000, 0x0F00, 0x00F0, 0x000F } },
		{ /* R5G5B5A1 */ 16, { 0xF800, 0x07C0, 0x003E, 0x0001 } },
		{ /* R5G5B5X1 */ 16, { 0xF800, 0x07C0, 0x003E, 0x0000 } },
		{ /* R8G8B8   */ 24, { 0xFF0000, 0x00FF00, 0x0000FF, 0x000000 } },
		{ /* Paletted */ 8,  { 0, 0, 0, 0 } },
		{ /* B8G8R8   */ 24, { 0x0000FF, 0x00FF00, 0xFF0000, 0x000000 } },
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

	DkBlendFactor RageToDkBlendFactor( bool bSrc, BlendMode mode, bool bAlphaChannel )
	{
		// Only the src/dst factor PAIR actually varies by BlendMode; encode
		// both together per mode rather than trying to decompose src/dst
		// independently, since they're not independent choices in GL's own
		// fixed-function blend equations either.
		(void)bAlphaChannel;
		switch( mode )
		{
		case BLEND_ADD:
			return bSrc ? DkBlendFactor_SrcAlpha : DkBlendFactor_One;
		case BLEND_SUBTRACT: // handled via blend op, factors match ADD
			return bSrc ? DkBlendFactor_SrcAlpha : DkBlendFactor_One;
		case BLEND_MODULATE:
			return bSrc ? DkBlendFactor_DstColor : DkBlendFactor_Zero;
		case BLEND_COPY_SRC:
		case BLEND_NO_EFFECT:
			return bSrc ? DkBlendFactor_One : DkBlendFactor_Zero;
		case BLEND_NORMAL:
		default:
			return bSrc ? DkBlendFactor_SrcAlpha : DkBlendFactor_InvSrcAlpha;
		}
	}

	DkBlendOp RageToDkBlendOp( BlendMode mode )
	{
		switch( mode )
		{
		case BLEND_SUBTRACT: return DkBlendOp_RevSub;
		default: return DkBlendOp_Add;
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
	// m_SetupCmdBuf is used outside the per-frame lifecycle (texture loads
	// can happen before the first frame, or at arbitrary points during a
	// session), so it can't use the per-frame ring pools - it gets its own
	// small pool instead, reset (Clear()) and re-fed before every use.
	// Only BeginFrame()'s render-target bind and CreateTexture()'s load-time
	// upload share this buffer/pool now - CreateTexture() always waitIdle()s
	// before returning, so by the time either one runs again the GPU is
	// genuinely done with the previous use. Streaming updates (movie frames)
	// used to share this too and do NOT hold that invariant - BeginFrame()'s
	// submission is never waited on, so a mid-frame UpdateTexture() call
	// could reuse (Clear()) this same memory while the GPU was still reading
	// this frame's render-target-bind commands from it, corrupting the
	// command stream (a real, on-device "GPU method error" via
	// dkCmdBufBarrier, root-caused through the debug deko3d lib's cbDebug).
	// UpdateTexture() now has its own buffer/pool entirely
	// (m_StreamingCmdBuf/m_pStreamingCmdPool, RearmStreamingCmdBuf() below)
	// specifically so it can never race this one again.
	m_SetupCmdBuf.clear();
	m_pSetupCmdPool->Clear();
	Deko3DAlloc cmdMem = m_pSetupCmdPool->Allocate( SETUP_CMD_POOL_SIZE, DK_CMDMEM_ALIGNMENT );
	ASSERT_M( cmdMem.IsValid(), "RageDisplay_Deko3D: setup command pool allocation failed" );
	m_SetupCmdBuf.addMemory( cmdMem.hBlock, cmdMem.iOffset, cmdMem.iSize );
}

void RageDisplay_Deko3D::RearmStreamingCmdBuf()
{
	// Mirrors RearmSetupCmdBuf(), on m_StreamingCmdBuf/m_pStreamingCmdPool
	// instead - see the comment there for why UpdateTexture() needs its own,
	// separate from m_SetupCmdBuf. UpdateTexture() itself always
	// waitIdle()s before returning (mirroring CreateTexture()), so reusing
	// this same memory on the next call is safe by the same reasoning.
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
}

void RageDisplay_Deko3D::DestroySwapchain()
{
	if( !m_Swapchain )
		return;

	m_Queue.waitIdle();
	m_Swapchain.destroy();

	for( int i = 0; i < NUM_FRAMEBUFFERS; ++i )
		m_pImagePool->Free( m_FramebufferMem[i] );
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
	m_SetupCmdBuf.bindRenderTargets( { &colorTarget }, nullptr );

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
void RageDisplay_Deko3D::ClearZBuffer() { /* no depth buffer bound yet in Phase 1 (sprites don't need one); no-op */ }
void RageDisplay_Deko3D::SetTextureMode( TextureUnit /*tu*/, TextureMode tm ) { m_Pending.textureMode = tm; }
void RageDisplay_Deko3D::SetTextureWrapping( TextureUnit /*tu*/, bool b ) { m_Pending.bTextureWrap = b; }
void RageDisplay_Deko3D::SetTextureFiltering( TextureUnit /*tu*/, bool b ) { m_Pending.bTextureFilter = b; }
void RageDisplay_Deko3D::SetTexture( TextureUnit /*tu*/, uintptr_t iTexture ) { m_Pending.iBoundTexture = iTexture; }
void RageDisplay_Deko3D::ClearAllTextures() { m_Pending.iBoundTexture = 0; }

void RageDisplay_Deko3D::SetEffectMode( EffectMode em ) { m_Pending.effectMode = em; }
bool RageDisplay_Deko3D::IsEffectModeSupported( EffectMode em ) { return em == EffectMode_Normal; } // only Normal has a shader in Phase 1

// ---------------------------------------------------------------------
// Phase 2 stubs (Model lighting/materials) - see doc 09 S10
// ---------------------------------------------------------------------

void RageDisplay_Deko3D::SetMaterial( const RageColor &, const RageColor &, const RageColor &, const RageColor &, float )
{
	LOG->Warn( "RageDisplay_Deko3D::SetMaterial: Model lighting is Phase 2, not implemented yet" );
}
void RageDisplay_Deko3D::SetLighting( bool ) { }
void RageDisplay_Deko3D::SetLightOff( int ) { }
void RageDisplay_Deko3D::SetLightDirectional( int, const RageColor &, const RageColor &, const RageColor &, const RageVector3 & ) { }
void RageDisplay_Deko3D::SetSphereEnvironmentMapping( TextureUnit, bool ) { }
void RageDisplay_Deko3D::SetCelShaded( int ) { }

RageCompiledGeometry *RageDisplay_Deko3D::CreateCompiledGeometry()
{
	LOG->Warn( "RageDisplay_Deko3D::CreateCompiledGeometry: Model geometry is Phase 2, not implemented yet" );
	return nullptr;
}
void RageDisplay_Deko3D::DeleteCompiledGeometry( RageCompiledGeometry * ) { }

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

	dk::ImageLayout layout;
	dk::ImageLayoutMaker( m_Device )
		.setFlags( 0 )
		.setFormat( dkFormat )
		.setDimensions( pImg->w, pImg->h )
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

	RearmSetupCmdBuf();
	dk::ImageView imageView( pRec->Image );
	m_SetupCmdBuf.copyBufferToImage( { staging.iGpuAddr }, imageView, { 0, 0, 0, (uint32_t)pImg->w, (uint32_t)pImg->h, 1 } );
	m_Queue.submitCommands( m_SetupCmdBuf.finishList() );
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
	cmdbuf.bindDepthStencilState( depthState );

	SpriteShaderVariant variant = GetShaderVariantForCurrentState();
	std::array<DkShader const *, 2> shaders = { &m_VertexShader, &m_FragmentShaders[variant] };
	cmdbuf.bindShaders( DkStageFlag_Vertex | DkStageFlag_Fragment, shaders );

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

void RageDisplay_Deko3D::DrawQuadsInternal( const RageSpriteVertex v[], int iNumVerts )
{
	if( iNumVerts <= 0 )
		return;

	// NOTE: does NOT clear()/addMemory()/finishList()/submit the command
	// buffer here - that happens ONCE per frame, in BeginFrame()/EndFrame().
	// This call only RECORDS more commands into the frame's already-fed
	// buffer, so an arbitrary number of sprite draws can accumulate into
	// one frame without exhausting the ring (see BeginFrame()'s comment for
	// the bug this replaced).

	// Upload this draw's vertex data into the per-frame ring (doc 09 S9 /
	// doc 10 Pattern D) - Sprite.cpp rebuilds v[] fresh on every single
	// call, so there is no static buffer to reuse across frames here.
	uint32_t iVertBytes = sizeof(RageSpriteVertex) * iNumVerts;
	Deko3DAlloc vertMem = m_pDynamicDataPool->Allocate( iVertBytes, alignof(RageSpriteVertex) );
	ASSERT_M( vertMem.IsValid(), "RageDisplay_Deko3D: dynamic vertex ring exhausted for this frame" );
	std::memcpy( vertMem.pCpuAddr, v, iVertBytes );

	// Transform matrix: RageMatrix's raw bytes need no transpose for a GLSL
	// mat4 - confirmed against RageDisplay_Legacy's own glLoadMatrixf() call
	// sites (RageDisplay_OGL.cpp:1024 etc.), see doc 09's resolved finding.
	//
	// Grouping/order corrected against RageDisplay_OGL.cpp's own actual
	// usage (RageDisplay_OGL.cpp:1015,1028) rather than assumed - the
	// original version of this code grouped Centering with World and chained
	// all four matrices in one pass, which is wrong on both counts: the real
	// backend keeps two separate matrices (modelView = View-after-World,
	// projection = Centering-after-Projection), matching GL's separate
	// GL_MODELVIEW/GL_PROJECTION stacks. RageMatrixMultiply(pOut,pA,pB)
	// computes pOut = pB*pA (pB applied first, confirmed by reading
	// RageMath.cpp's actual multiply implementation, not assumed from the
	// header declaration alone).
	RageMatrix modelView;
	RageMatrixMultiply( &modelView, GetViewTop(), GetWorldTop() );   // World applied first, then View
	RageMatrix projection;
	RageMatrixMultiply( &projection, GetCentering(), GetProjectionTop() ); // Projection applied first, then Centering
	RageMatrix final;
	RageMatrixMultiply( &final, &projection, &modelView );           // modelView applied first, then projection

	Deko3DAlloc uniformMem = m_pDynamicDataPool->Allocate( sizeof(RageMatrix), DK_UNIFORM_BUF_ALIGNMENT );
	ASSERT_M( uniformMem.IsValid(), "RageDisplay_Deko3D: dynamic uniform ring exhausted for this frame" );
	std::memcpy( uniformMem.pCpuAddr, &final, sizeof(RageMatrix) );

	FlushState( m_DynamicCmdBuf );

	// Re-checked immediately at the bind, not just at allocation time above -
	// this is the actual GPU address deko3d receives, and nothing should be
	// allowed to reach it unvalidated.
	ASSERT_M( uniformMem.IsValid(), "RageDisplay_Deko3D: uniform buffer GPU address is invalid at bind time" );
	m_DynamicCmdBuf.bindUniformBuffer( DkStage_Vertex, 0, uniformMem.iGpuAddr, uniformMem.iSize );

	// DkVtxAttribState's aggregate initializer takes only its 6 NAMED
	// bitfields in order (bufferId, isFixed, offset, size, type, isBgra) -
	// the two anonymous padding bitfields in between are skipped, matching
	// every devkitPro example's own usage (e.g. Example04_TexturedCube.cpp:44-45).
	std::array<DkVtxAttribState, 3> attribs = {{
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, p), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, c), DkVtxAttribSize_4x8,  DkVtxAttribType_Unorm, 1 }, // isBgra=1, see doc 11
		{ 0, 0, (uint32_t)offsetof(RageSpriteVertex, t), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	}};
	std::array<DkVtxBufferState, 1> vtxBufState = {{ { sizeof(RageSpriteVertex), 0 } }};
	m_DynamicCmdBuf.bindVtxAttribState( attribs );
	m_DynamicCmdBuf.bindVtxBufferState( vtxBufState );
	ASSERT_M( vertMem.IsValid(), "RageDisplay_Deko3D: vertex buffer GPU address is invalid at bind time" );
	m_DynamicCmdBuf.bindVtxBuffer( 0, vertMem.iGpuAddr, vertMem.iSize );

	m_DynamicCmdBuf.draw( DkPrimitive_Quads, iNumVerts, 1, 0, 0 );

	// No finishList()/submitCommands() here - this quad's commands stay
	// recorded in the frame's shared buffer; EndFrame() submits the whole
	// frame's accumulated draws in one call.

	StatsAddVerts( iNumVerts );
}

// The remaining primitive types aren't exercised by Sprite/Quad rendering
// (09-Deko3D-SpritePipelineCache.md S1) - stubbed rather than silently
// mis-drawing until a real caller shows up needing them.
void RageDisplay_Deko3D::DrawQuadStripInternal( const RageSpriteVertex[], int ) { LOG->Warn( "RageDisplay_Deko3D::DrawQuadStripInternal: not implemented yet" ); }
void RageDisplay_Deko3D::DrawFanInternal( const RageSpriteVertex[], int ) { LOG->Warn( "RageDisplay_Deko3D::DrawFanInternal: not implemented yet" ); }
void RageDisplay_Deko3D::DrawStripInternal( const RageSpriteVertex[], int ) { LOG->Warn( "RageDisplay_Deko3D::DrawStripInternal: not implemented yet" ); }
void RageDisplay_Deko3D::DrawTrianglesInternal( const RageSpriteVertex[], int ) { LOG->Warn( "RageDisplay_Deko3D::DrawTrianglesInternal: not implemented yet" ); }
void RageDisplay_Deko3D::DrawCompiledGeometryInternal( const RageCompiledGeometry *, int ) { LOG->Warn( "RageDisplay_Deko3D::DrawCompiledGeometryInternal: Phase 2, not implemented yet" ); }
void RageDisplay_Deko3D::DrawLineStripInternal( const RageSpriteVertex[], int, float ) { LOG->Warn( "RageDisplay_Deko3D::DrawLineStripInternal: not implemented yet" ); }
void RageDisplay_Deko3D::DrawSymmetricQuadStripInternal( const RageSpriteVertex[], int ) { LOG->Warn( "RageDisplay_Deko3D::DrawSymmetricQuadStripInternal: not implemented yet" ); }

/*
 * Copyright (c) 2026 the StepMania-nx contributors
 * Same license terms as the rest of stepmania/src (see LICENSE).
 */
