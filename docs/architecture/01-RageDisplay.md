# RageDisplay - Graphics API Abstraction Layer (Verified)

All line numbers below refer to `stepmania/src/RageDisplay.h` unless another file
is named. This replaces a previous version of this document that invented most
method signatures; every signature here was copied from the actual header.

## Files

- `stepmania/src/RageDisplay.h` (492 lines) — interface, read in full
- `stepmania/src/RageDisplay_OGL.h` — OpenGL backend; **the class inside is named
  `RageDisplay_Legacy`**, not `RageDisplay_OGL` (`RageDisplay_OGL.h:1,32`). The
  file-level comment literally reads `/* RageDisplay_Legacy: OpenGL renderer. */`.
- `stepmania/src/RageDisplay_D3D.h` — `class RageDisplay_D3D: public RageDisplay` (`:6`)
- `stepmania/src/RageDisplay_GLES2.h` — `class RageDisplay_GLES2: public RageDisplay` (`:4`)
- `stepmania/src/RageDisplay_Null.h` — `class RageDisplay_Null: public RageDisplay` (`:6`)

## Class Hierarchy (verified)

```
RageDisplay (abstract, RageDisplay.h:215)
├─ RageDisplay_Legacy   (OpenGL — RageDisplay_OGL.h:32)
├─ RageDisplay_D3D       (RageDisplay_D3D.h:6)
├─ RageDisplay_GLES2     (RageDisplay_GLES2.h:4)
└─ RageDisplay_Null      (RageDisplay_Null.h:6)
```

## RageCompiledGeometry (lines 26-52)

Abstract vertex-buffer interface, declared *inside* RageDisplay.h (not its own file):

```cpp
class RageCompiledGeometry
{
public:
    virtual ~RageCompiledGeometry();
    void Set( const vector<msMesh> &vMeshes, bool bNeedsNormals );      // :31
    virtual void Allocate( const vector<msMesh> &vMeshes ) = 0;         // :33
    virtual void Change( const vector<msMesh> &vMeshes ) = 0;           // :34
    virtual void Draw( int iMeshIndex ) const = 0;                      // :35
protected:
    struct MeshInfo { int iVertexStart, iVertexCount, iTriangleStart, iTriangleCount;
                       bool m_bNeedsTextureMatrixScale; };               // :41-48
};
```

## RageDisplay public interface — grouped by what actually exists

### Initialization & video mode (lines 226-247)

```cpp
virtual const RagePixelFormatDesc *GetPixelFormatDesc( RagePixelFormat pf ) const = 0;  // :226
virtual RString Init( const VideoModeParams &p, bool bAllowUnacceleratedRenderer ) = 0; // :231
virtual RString GetApiDescription() const = 0;                                          // :233
virtual void GetDisplaySpecs(DisplaySpecs &out) const = 0;                              // :234
RString SetVideoMode( VideoModeParams p, bool &bNeedReloadTextures );   // concrete, :239
virtual void ResolutionChanged();                                       // :242
virtual bool BeginFrame();                                              // :244 — concrete, not pure
virtual void EndFrame();                                                // :245
virtual ActualVideoModeParams GetActualVideoModeParams() const = 0;     // :246
bool IsWindowed() const;                                                // :247
```

`BeginFrame()`/`EndFrame()` are declared `virtual` with base implementations in
`RageDisplay.cpp` (not pure virtual) — backends may call the base or fully override.

### Texture object lifecycle (lines 264-286)

```cpp
virtual uintptr_t CreateTexture(RagePixelFormat, RageSurface*, bool bGenerateMipMaps) = 0; // :264
virtual void UpdateTexture(uintptr_t iTexHandle, RageSurface*, int x, int y, int w, int h) = 0; // :269
virtual void DeleteTexture( uintptr_t iTexHandle ) = 0;                 // :274
virtual RageTextureLock *CreateTextureLock() { return nullptr; }        // :277
virtual void ClearAllTextures() = 0;                                    // :278
virtual int GetNumTextureUnits() = 0;                                   // :279
virtual void SetTexture( TextureUnit, uintptr_t /* iTexture */ ) = 0;   // :280 — HANDLE, not RageTexture*
virtual void SetTextureMode( TextureUnit, TextureMode ) = 0;            // :281
virtual void SetTextureWrapping( TextureUnit, bool ) = 0;               // :282
virtual int GetMaxTextureSize() const = 0;                              // :283
virtual void SetTextureFiltering( TextureUnit, bool ) = 0;              // :284
```

Note the texture handle is `uintptr_t` everywhere — "unsigned in OpenGL, texture
pointer in D3D" per the comment at `RageDisplay.h:262-263`. `RageDisplay` never
takes a `RageTexture*` parameter directly in its drawing calls; callers resolve a
texture object to its handle (via `RageTexture::GetTexHandle()`,
`RageTexture.h:18`) before calling `SetTexture()`.

### Render targets (lines 288-306)

```cpp
virtual bool SupportsRenderToTexture() const { return false; }
virtual uintptr_t CreateRenderTarget( const RenderTargetParam &, int &iTextureWidthOut, int &iTextureHeightOut ) { return 0; } // :296
virtual uintptr_t GetRenderTarget() { return 0; }                       // :298
virtual void SetRenderTarget( uintptr_t iHandle, bool bPreserveTexture = true ) { } // :306
```

There is **no** `DeleteRenderTarget` — the comment at `:292-294` says to delete
a render target the same way as any texture, via `DeleteTexture()`.
`CreateRenderTarget` returns the handle directly and takes **two** output params
(width and height), not a single "index" out-param.

### Z-buffer, culling, materials, lighting (lines 308-337)

```cpp
virtual bool IsZTestEnabled() const = 0;
virtual bool IsZWriteEnabled() const = 0;
virtual void SetZWrite( bool ) = 0;
virtual void SetZTestMode( ZTestMode ) = 0;
virtual void SetZBias( float ) = 0;
virtual void ClearZBuffer() = 0;
virtual void SetCullMode( CullMode mode ) = 0;
virtual void SetAlphaTest( bool b ) = 0;
virtual void SetMaterial( const RageColor &emissive, const RageColor &ambient,
                           const RageColor &diffuse, const RageColor &specular,
                           float shininess ) = 0;                        // :319
virtual void SetLighting( bool b ) = 0;
virtual void SetLightOff( int index ) = 0;
virtual void SetLightDirectional( int index, const RageColor &ambient,
    const RageColor &diffuse, const RageColor &specular, const RageVector3 &dir ) = 0; // :329
virtual void SetSphereEnvironmentMapping( TextureUnit tu, bool b ) = 0;
virtual void SetCelShaded( int stage ) = 0;
```

### Drawing (lines 339-356) — these are the real primitive calls

```cpp
virtual RageCompiledGeometry* CreateCompiledGeometry() = 0;
virtual void DeleteCompiledGeometry( RageCompiledGeometry* p ) = 0;

void DrawQuads( const RageSpriteVertex v[], int iNumVerts );                         // :342 — concrete
void DrawQuadStrip( const RageSpriteVertex v[], int iNumVerts );                     // :343
void DrawFan( const RageSpriteVertex v[], int iNumVerts );                           // :344
void DrawStrip( const RageSpriteVertex v[], int iNumVerts );                         // :345
void DrawTriangles( const RageSpriteVertex v[], int iNumVerts );                     // :346
void DrawCompiledGeometry( const RageCompiledGeometry *p, int iMeshIndex,
                            const vector<msMesh> &vMeshes );                         // :347
void DrawLineStrip( const RageSpriteVertex v[], int iNumVerts, float LineWidth );    // :348
void DrawSymmetricQuadStrip( const RageSpriteVertex v[], int iNumVerts );            // :349
void DrawCircle( const RageSpriteVertex &v, float radius );                         // :350
void DrawQuad( const RageSpriteVertex v[] ) { DrawQuads(v,4); }                      // :352 — alias
```

All of these are **concrete, non-virtual** public methods that take a single array
of `RageSpriteVertex` (position + normal + color + texcoord packed together, see
`RageTypes.h:358-365`), not separate color/position arrays. Each one forwards to a
matching `protected`, pure-virtual `...Internal()` method (lines 372-380) that the
backend actually implements — e.g. `DrawQuads()` calls `DrawQuadsInternal()`. This
is a template-method pattern for shared bookkeeping (stats, degenerate-case
handling) around each backend's real draw call.

There is no `SetBlendAlpha`, `SetCorrectedBlendAlpha`, `SetTextureRenderingParams`,
`SetViewport`, or `SetScissor` method anywhere in this header.

### Blend mode (line 249)

```cpp
virtual void SetBlendMode( BlendMode mode ) = 0;
```

That's the entire blend-mode surface on `RageDisplay`. (Per-actor alpha is applied
via vertex color in the `RageSpriteVertex` array, not a separate display-level call.)

### Matrix stack (lines 402-433) — all concrete, none virtual

```cpp
void PushMatrix();
void PopMatrix();
void Translate( float x, float y, float z );
void TranslateWorld( float x, float y, float z );
void Scale( float x, float y, float z );
void RotateX( float deg );
void RotateY( float deg );
void RotateZ( float deg );
void SkewX( float fAmount );
void SkewY( float fAmount );
void MultMatrix( const RageMatrix &f ) { this->PostMultMatrix(f); }  // alias, :413
void PostMultMatrix( const RageMatrix &f );
void PreMultMatrix( const RageMatrix &f );
void LoadIdentity();

// Texture matrix
void TexturePushMatrix();
void TexturePopMatrix();
void TextureTranslate( float x, float y );

// Projection/view stack
void CameraPushMatrix();
void CameraPopMatrix();
void LoadMenuPerspective( float fFOVDegrees, float fWidth, float fHeight,
                           float fVanishPointX, float fVanishPointY );      // :427
void LoadLookAt( float fov, const RageVector3 &Eye, const RageVector3 &At,
                  const RageVector3 &Up );

// Centering matrix (screen letterbox/pillarbox adjustment)
void CenteringPushMatrix();
void CenteringPopMatrix();
void ChangeCentering( int trans_x, int trans_y, int add_width, int add_height );
```

This entire block is **not virtual** — one implementation lives in
`RageDisplay.cpp` and maintains the matrix stacks generically; backends read the
current top-of-stack matrix via the protected `GetWorldTop()`/`GetProjectionTop()`/
`GetViewTop()`/`GetTextureTop()`/`GetCentering()` accessors (lines 452-457) when
they actually issue a draw call.

### Statistics (lines 393-400)

```cpp
int GetFPS() const;
int GetVPF() const;           // vertices per frame
int GetCumFPS() const;        // average FPS since last reset
virtual void ResetStats();
virtual void ProcessStatsOnFlip();
virtual RString GetStats() const;
void StatsAddVerts( int iNumVertsRendered );
```

### Screenshots (lines 358-369)

```cpp
enum GraphicsFileFormat { SAVE_LOSSLESS, SAVE_LOSSLESS_SENSIBLE, SAVE_LOSSY_LOW_QUAL, SAVE_LOSSY_HIGH_QUAL };
bool SaveScreenshot( RString sPath, GraphicsFileFormat format );
virtual RageSurface* CreateScreenshot() = 0;
virtual RageSurface *GetTexture( uintptr_t /* iTexture */ ) { return nullptr; }
```

## Key Data Structures (verified)

### VideoModeParams (lines 75-153)

Real fields, verbatim from the header:
`windowed`, `sDisplayId`, `width`, `height`, `bpp`, `rate`, `vsync`, `interlaced`,
`bSmoothLines`, `bTrilinearFiltering`, `bAnisotropicFiltering`,
`bWindowIsFullscreenBorderless`, `sWindowTitle`, `sIconFile`, `PAL`,
`fDisplayAspectRatio`. Constructed with a 16-argument constructor (`:80-97`) —
there is no builder/default-then-set pattern in the header.

### ActualVideoModeParams (lines 160-181)

`class ActualVideoModeParams: public VideoModeParams` adding `windowWidth`,
`windowHeight`, `renderOffscreen` — the *actual* window size chosen by the
low-level window backend, which can differ from the requested `width`/`height`
when `bWindowIsFullscreenBorderless` is true (comment at `:175-177`).

### RenderTargetParam (lines 183-200)

```cpp
struct RenderTargetParam {
    int iWidth, iHeight;
    bool bWithDepthBuffer;
    bool bWithAlpha;
    bool bFloat;
};
```

### RageTextureLock (lines 202-213)

```cpp
struct RageTextureLock {
    virtual void Lock( uintptr_t iTexHandle, RageSurface *pSurface ) = 0;
    virtual void Unlock( RageSurface *pSurface, bool bChanged = true ) = 0;
};
```
Used for streaming texture updates (e.g. video frames) without a full
`CreateTexture`/`DeleteTexture` cycle.

## Verified frame-driving call site

`ScreenManager::Draw()` (`ScreenManager.cpp:488`) is the actual per-frame driver:

```cpp
if( !DISPLAY->BeginFrame() ) return;
DISPLAY->CameraPushMatrix();
DISPLAY->LoadMenuPerspective( 0, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_CENTER_X, SCREEN_CENTER_Y );
g_pSharedBGA->Draw();
DISPLAY->CameraPopMatrix();
for each screen in g_ScreenStack: pScreen->Draw();
for each overlay screen: pScreen->Draw();
DISPLAY->EndFrame();
```
(`ScreenManager.cpp:497-512`)

## API-specific notes

Confirmed from class declarations only (bodies of the `.cpp` files were not read
in this pass, so implementation-level claims below are marked accordingly):

- **RageDisplay_Legacy** (`RageDisplay_OGL.h`) — includes `RageTextureRenderTarget.h`
  and `Sprite.h` directly; defines `FlushGLErrors()`/`AssertNoGLError()` debug
  macros around `glGetError()` (`:17-22`). Actual draw-call implementation not
  verified in this pass.
- **RageDisplay_D3D**, **RageDisplay_GLES2**, **RageDisplay_Null** — inheritance
  confirmed (`RageDisplay_D3D.h:6`, `RageDisplay_GLES2.h:4`, `RageDisplay_Null.h:6`);
  internals not read in this pass.

## Related Classes (verified)

- `RageTexture` (`RageTexture.h:10`) — `friend class RageTexture;` declared inside
  `RageDisplay` (`RageDisplay.h:217`), giving `RageTexture` access to private
  `RageDisplay` internals.
- `RageCompiledGeometry` (`RageDisplay.h:26`) — consumed by `DrawCompiledGeometry()`.
- `RageSurface` — forward-declared only in this header (`RageDisplay.h:14`); full
  definition lives in `RageSurface.h` (not read in this pass).

---

**Scope**: `stepmania/src/RageDisplay*.h` — `RageDisplay.h` read in full (492 lines);
backend headers read for class declarations only.
