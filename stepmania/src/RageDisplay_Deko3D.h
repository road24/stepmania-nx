/* RageDisplay_Deko3D - Nintendo Switch deko3d renderer.
 *
 * Phase 1 scope (see docs/architecture/08-Deko3D-Feasibility.md and
 * 09-Deko3D-SpritePipelineCache.md): Sprite/Quad drawing only. Model
 * lighting/cel-shading (Phase 2) is not implemented yet - those entry
 * points exist because they're part of RageDisplay's required interface,
 * but are stubbed to fail loudly rather than silently mis-render.
 */
#ifndef RAGE_DISPLAY_DEKO3D_H
#define RAGE_DISPLAY_DEKO3D_H

#include "Deko3DMemPool.h"
#include <deko3d.hpp>
#include <array>

class RageDisplay_Deko3D: public RageDisplay
{
public:
	RageDisplay_Deko3D();
	virtual ~RageDisplay_Deko3D();

	virtual RString Init( const VideoModeParams &p, bool bAllowUnacceleratedRenderer );

	virtual RString GetApiDescription() const { return "deko3d"; }
	virtual void GetDisplaySpecs( DisplaySpecs &out ) const;
	const RagePixelFormatDesc *GetPixelFormatDesc( RagePixelFormat pf ) const;

	virtual bool BeginFrame();
	virtual void EndFrame();
	virtual ActualVideoModeParams GetActualVideoModeParams() const;

	virtual void SetBlendMode( BlendMode mode );
	virtual bool SupportsTextureFormat( RagePixelFormat pixfmt, bool bRealtime = false );
	virtual bool SupportsPerVertexMatrixScale() { return false; }

	virtual uintptr_t CreateTexture( RagePixelFormat pixfmt, RageSurface *pImg, bool bGenerateMipMaps );
	virtual void UpdateTexture( uintptr_t iTexHandle, RageSurface *pImg, int iXOffset, int iYOffset, int iWidth, int iHeight );
	virtual void DeleteTexture( uintptr_t iTexHandle );
	virtual void ClearAllTextures();
	virtual int GetNumTextureUnits() { return 1; } // Phase 1: one bound sprite texture at a time
	virtual void SetTexture( TextureUnit tu, uintptr_t iTexture );
	virtual void SetTextureMode( TextureUnit tu, TextureMode tm );
	virtual void SetTextureWrapping( TextureUnit tu, bool b );
	virtual int GetMaxTextureSize() const { return 4096; } // conservative; not yet queried from the device
	virtual void SetTextureFiltering( TextureUnit tu, bool b );

	virtual bool IsZTestEnabled() const { return m_Pending.bZTest; }
	virtual bool IsZWriteEnabled() const { return m_Pending.bZWrite; }
	virtual void SetZWrite( bool b );
	virtual void SetZTestMode( ZTestMode mode );
	virtual void SetZBias( float f ) { m_Pending.fZBias = f; }
	virtual void ClearZBuffer();

	virtual void SetCullMode( CullMode mode );
	virtual void SetAlphaTest( bool b ) { m_Pending.bAlphaTest = b; }
	virtual void SetEffectMode( EffectMode em );
	virtual bool IsEffectModeSupported( EffectMode em );

	// Phase 2 (Model lighting/materials) - not implemented yet. See
	// 09-Deko3D-SpritePipelineCache.md S10 for the design (Material x Light
	// must be multiplied in-shader; no such shader exists yet).
	virtual void SetMaterial( const RageColor &emissive, const RageColor &ambient,
		const RageColor &diffuse, const RageColor &specular, float shininess );
	virtual void SetLighting( bool b );
	virtual void SetLightOff( int index );
	virtual void SetLightDirectional( int index, const RageColor &ambient,
		const RageColor &diffuse, const RageColor &specular, const RageVector3 &dir );
	virtual void SetSphereEnvironmentMapping( TextureUnit tu, bool b );
	virtual void SetCelShaded( int stage );

	virtual RageCompiledGeometry *CreateCompiledGeometry();
	virtual void DeleteCompiledGeometry( RageCompiledGeometry *p );

protected:
	virtual void DrawQuadsInternal( const RageSpriteVertex v[], int iNumVerts );
	virtual void DrawQuadStripInternal( const RageSpriteVertex v[], int iNumVerts );
	virtual void DrawFanInternal( const RageSpriteVertex v[], int iNumVerts );
	virtual void DrawStripInternal( const RageSpriteVertex v[], int iNumVerts );
	virtual void DrawTrianglesInternal( const RageSpriteVertex v[], int iNumVerts );
	virtual void DrawCompiledGeometryInternal( const RageCompiledGeometry *p, int iMeshIndex );
	virtual void DrawLineStripInternal( const RageSpriteVertex v[], int iNumVerts, float fLineWidth );
	virtual void DrawSymmetricQuadStripInternal( const RageSpriteVertex v[], int iNumVerts );

	virtual RString TryVideoMode( const VideoModeParams &p, bool &bNewDeviceOut );
	virtual RageSurface *CreateScreenshot(); // Phase 1: not implemented, see doc 10 open item on readback pools

private:
	// Non-copyable.
	RageDisplay_Deko3D( const RageDisplay_Deko3D & );
	RageDisplay_Deko3D &operator=( const RageDisplay_Deko3D & );

	// -- Setup helpers (Init) --
	void CreateDeviceAndQueue();
	void CreatePools();
	// Tells the GPU where m_pImageDescriptors/m_pSamplerDescriptors actually
	// live (dkCmdBufBindImageDescriptorSet/BindSamplerDescriptorSet) - this
	// is what dkMakeTextureHandle()'s packed (imageSlot, samplerSlot)
	// indices are resolved against. Every DkResHandle FlushState() builds is
	// meaningless without it: a slot index alone doesn't tell the GPU which
	// table to index into. Root-caused via SampleFramework/CDescriptorSet.h
	// and Example04_TexturedCube.cpp:191-213 ("Configure persistent state in
	// the queue") after Deko3DAlloc/slot-index validation at every other
	// bind site turned up nothing - every allocation and slot index really
	// was valid, which is what pointed at a missing GPU-side binding rather
	// than bad data. Called once at Init(), via the static setup command
	// buffer: like the shader/pipeline state examples bind once and never
	// touch again, this is queue-persistent state, not per-frame state.
	void BindDescriptorSets();
	void CreateSwapchain( int iWidth, int iHeight );
	void DestroySwapchain();
	void LoadShaders();
	void CreateSamplers();

	// -- Texture format handling (11-Deko3D-TextureFormatAndShaderContract.md) --
	// Returns the RagePixelFormat to actually upload as, converting pImg in
	// place (and pointing it at a new, caller-owned surface) if the source
	// format has no native deko3d equivalent. Never fails - always produces
	// something uploadable, logging a warning when a conversion happens.
	RagePixelFormat GetImgPixelFormat( RageSurface *&pImg, bool &bFreeImg, int iWidth, int iHeight, bool bPalettedTexture );
	static DkImageFormat RagePixelFormatToDkImageFormat( RagePixelFormat pixfmt );

	// -- Per-draw state translation (09-Deko3D-SpritePipelineCache.md S2-3) --
	// These just record what the next draw should use; the actual DK state
	// objects are built and bound lazily, in FlushState(), right before the
	// draw call that needs them - mirroring RageDisplay's own public/
	// ...Internal() split (RageDisplay.h) rather than touching the command
	// buffer on every individual Set*() call.
	struct PendingState
	{
		PendingState();
		BlendMode blendMode;
		CullMode cullMode;
		ZTestMode zTestMode;
		bool bZTest, bZWrite, bAlphaTest;
		float fZBias;
		TextureMode textureMode;
		EffectMode effectMode;
		uintptr_t iBoundTexture; // TextureUnit_1 only, Phase 1
		bool bTextureWrap, bTextureFilter;
	};
	PendingState m_Pending;

	void FlushState( dk::CmdBuf cmdbuf );
	dk::Sampler MakeSampler( bool bWrap, bool bFilter ) const;
	int32_t GetOrCreateSamplerSlot( bool bWrap, bool bFilter );

	// -- Shader selection (doc 09 S4/S6) --
	enum SpriteShaderVariant
	{
		SpriteShader_Modulate,
		SpriteShader_Glow,
		NUM_SpriteShaderVariant
	};
	SpriteShaderVariant GetShaderVariantForCurrentState() const;

	// -- Per-texture bookkeeping --
	struct TextureRecord
	{
		dk::Image Image;
		Deko3DAlloc ImageMem;
		int32_t iImageDescriptorSlot;
		int iWidth, iHeight;
	};
	std::vector<TextureRecord *> m_vTextures; // indexed by (handle - 1); see CreateTexture

	// 1x1 opaque-white texture, always bound to the fragment stage's texture
	// slot 0 when m_Pending.iBoundTexture==0 (RageDisplay's "no texture"
	// convention - solid-color quads, fades, rects). Both sprite shaders
	// (deko3d_shaders/sprite_*_fsh.glsl) unconditionally call
	// texture(tex, inTexCoord) - there is no untextured shader variant - so
	// skipping the bind entirely on an untextured draw (the original
	// FlushState() behavior) left deko3d's fragment-stage texture register
	// at whatever it last held. On the very first draw of the very first
	// frame, before any real texture had ever been bound, that register was
	// never written at all, and the GPU faulted reading a null descriptor -
	// root-caused via Queue::checkError()'s page-fault report (address
	// 0x0000000000, Read) after switching to the debug deko3d lib. White
	// makes texture(tex,...)*inColor reduce to exactly inColor, so this is
	// correct for untextured draws, not just crash-safe.
	int32_t m_iWhiteTextureDescriptorSlot;
	void CreateWhiteTexture();

	// -- deko3d core objects --
	dk::UniqueDevice m_Device;
	dk::UniqueQueue m_Queue;

	// One static command buffer for setup/teardown work (framebuffer binds,
	// texture uploads); one dynamic command buffer + ring for per-frame
	// sprite draw commands. See doc 09 S9 (CCmdMemRing pattern) and
	// doc 10 (Pattern D pools).
	dk::UniqueCmdBuf m_SetupCmdBuf;
	dk::UniqueCmdBuf m_DynamicCmdBuf;

	// -- Memory pools (10-Deko3D-MemoryAllocator.md) --
	Deko3DBumpPool *m_pCodePool;       // Pattern A: shaders
	Deko3DFreeListPool *m_pImagePool;  // Pattern B: texture storage
	Deko3DBumpPool *m_pScratchPool;    // Pattern C: texture upload staging (Clear()d between uploads)
	Deko3DBumpPool *m_pSetupCmdPool;   // Pattern C: m_SetupCmdBuf's own command memory (Clear()d/re-fed before each use)
	Deko3DRingPool *m_pDynamicCmdPool; // Pattern D: per-frame command memory
	Deko3DRingPool *m_pDynamicDataPool;// Pattern D: per-frame vertex + uniform data
	Deko3DDescriptorTable *m_pImageDescriptors;
	Deko3DDescriptorTable *m_pSamplerDescriptors;

	// Feeds m_SetupCmdBuf a fresh chunk of memory from m_pSetupCmdPool.
	// Must be called before every recording session on m_SetupCmdBuf (once
	// per BeginFrame()'s render-target bind, once per CreateTexture()'s
	// upload) - see the crash this fixes: m_SetupCmdBuf was never given any
	// memory at all, so its first real recording (a texture upload,
	// reached before the first real frame even renders, since startup
	// fonts/splash textures load first) tried to grow via a cbAddMem
	// callback that was never provided, and deko3d aborted the process.
	void RearmSetupCmdBuf();

	static const uint32_t NUM_DYNAMIC_SLICES = 2; // matches swapchain double-buffering
	static const uint32_t MAX_LIVE_TEXTURES = 4096; // see doc 10's descriptor-table sizing note

	// -- Shaders --
	dk::Shader m_VertexShader;
	dk::Shader m_FragmentShaders[NUM_SpriteShaderVariant];

	// -- Samplers: 4 fixed combinations (TextureWrapping x TextureFiltering), see doc 09 S4/S5 --
	int32_t m_iSamplerSlot[2][2]; // [bWrap][bFilter], -1 until first requested

	// -- Swapchain / framebuffers --
	dk::UniqueSwapchain m_Swapchain;
	static const int NUM_FRAMEBUFFERS = 2;
	dk::Image m_Framebuffers[NUM_FRAMEBUFFERS];
	Deko3DAlloc m_FramebufferMem[NUM_FRAMEBUFFERS];
	int m_iCurFramebufferSlot;
	int m_iWidth, m_iHeight;

	ActualVideoModeParams m_CurrentParams;

	// Diagnostic-only: counts BeginFrame()/EndFrame() calls separately so a
	// divergence between them (BeginFrame ahead of EndFrame by more than 1)
	// is directly visible in the log - added specifically to investigate a
	// crash inside dkQueueAcquireImage() that only manifests after several
	// successful frames, suggesting an acquire/present mismatch slowly
	// exhausting the swapchain's small (NUM_FRAMEBUFFERS) image pool rather
	// than a first-frame setup bug. Not meant to be permanent.
	uint64_t m_iBeginFrameCount;
	uint64_t m_iEndFrameCount;
};

#endif

/*
 * Copyright (c) 2026 the StepMania-nx contributors
 * Same license terms as the rest of stepmania/src (see LICENSE).
 */
