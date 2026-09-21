# StepMania Graphics & Rendering Pipeline Architecture

## Verification note

This documentation set was rewritten after an audit found the original pass contained
extensive fabricated APIs (invented method signatures, wrong base classes, wrong
singleton names, an entire imagined FreeType/glyph-atlas pipeline that does not exist
in this codebase). Every claim below was checked directly against source in
`stepmania/src` using `tgrep` and full-file reads. Each section cites `file:line`.
Where a detail could not be confirmed from the header alone (e.g. `.cpp`-only
behavior), it is stated as "implementation detail, not verified" rather than asserted.

Scope: `stepmania/src` only. No external libraries, themes, or build tooling.

## Real vs. previously-claimed corrections (highlights)

| Previously claimed | Actually is | Evidence |
|---|---|---|
| OpenGL backend class is `RageDisplay_OGL` | Class is `RageDisplay_Legacy` (file is named `RageDisplay_OGL.h`, class inside is not) | `stepmania/src/RageDisplay_OGL.h:32` |
| `RageDisplay::SetTexture(TextureUnit, const RageTexture*)` | `virtual void SetTexture( TextureUnit, uintptr_t )` — takes a raw handle, not a texture object | `stepmania/src/RageDisplay.h:280` |
| `RageDisplay::DrawQuads()` is pure virtual, takes color/position arrays | `void DrawQuads( const RageSpriteVertex v[], int iNumVerts )` — concrete, non-virtual, one vertex-struct array | `stepmania/src/RageDisplay.h:342` |
| Matrix stack (`PushMatrix`, `Translate`, etc.) is `virtual` | All concrete, non-virtual methods on `RageDisplay` | `stepmania/src/RageDisplay.h:403-416` |
| `RageDisplay::SetBlendAlpha`, `SetViewport`, `SetScissor`, `DeleteRenderTarget` exist | None of these exist anywhere in `RageDisplay.h` | n/a — absent from `stepmania/src/RageDisplay.h` |
| `Quad` extends `Actor` directly, has `SetWidth/SetColor/DrawPrimitive` | `Quad` extends **`Sprite`** and declares almost nothing of its own | `stepmania/src/Quad.h:7` |
| `Actor::SetXYZ()`, `GetScaleX/Y/Z()`, `SetAnchorX/Y()`, `SetAlpha()`, `SetEffectMode()`, `SetClipRect()` | None of these exist. Real names: `SetXY()`, `SetZ()`; `GetZoomX/Y/Z()`/`SetZoomX/Y/Z()`; no anchor concept; `SetDiffuseAlpha()`; per-effect setters like `SetEffectBob()`; crop/fade rect setters, no "ClipRect" | `stepmania/src/Actor.h:337-419, 457-473, 547-559` |
| `Actor::DrawPrimitive()` (singular) | `virtual void DrawPrimitives()` (plural) | `stepmania/src/Actor.h:275` |
| Font system rasterizes TrueType via FreeType, packs a live glyph texture atlas, supports kerning | No FreeType anywhere in `stepmania/src`. Fonts are pre-made **bitmap sprite sheets** described by `.ini` (`FontPageSettings`), loaded per `FontPage`. No kerning API exists | grep confirms zero FreeType references; `stepmania/src/Font.h:39-143` |
| Font singleton is `FONTMAN` | Global is `FONT` | `stepmania/src/FontManager.h:21` |
| `RageTextureManager::PreloadTextures()`, `Flush()`, `GetNumTextures()` | None exist. Real methods: `LoadTexture(RageTextureID)`, `UnloadTexture()`, `DeleteCachedTextures()`, `ReloadAll()`, `InvalidateTextures()` | `stepmania/src/RageTextureManager.h:55-79` |
| `RageTexturePreloader` loads asynchronously on a background thread | It is synchronous — `Load()` just calls `TEXTUREMAN->LoadTexture()` and holds a ref in a vector to keep it cached; no threading in the header | `stepmania/src/RageTexturePreloader.h:15-20` |
| `Model::LoadModel()`, `SetMaterial()`, `SetTexture(mesh, tex)` | Real: `Model::Load(const RString&)`, materials come from parsed Milkshape files (`vector<msMaterial>`), no public per-mesh texture setter | `stepmania/src/Model.h:23,63` |
| Movie backend base class is `MovieTexture` | Base class is `RageMovieTexture : public RageTexture`, using a `RageMovieTextureDriver` factory/registration pattern | `stepmania/src/arch/MovieTexture/MovieTexture.h:10,30` |
| `RageMath` classes have OOP methods (`RageVector3::Dot()`, `RageMatrix::operator*`, `MatrixIdentity()`) | All free C-style functions taking output pointers: `RageMatrixIdentity(RageMatrix*)`, `RageVec3Cross(...)`, etc. | `stepmania/src/RageMath.h:17-54` |

The rest of this document, and the numbered docs it links to, reflect only verified structure.

## Core Files & Classes (verified)

### Display API Layer

| File | Class | Verified at |
|------|-------|-------------|
| RageDisplay.h/cpp | `RageDisplay` (abstract) | `RageDisplay.h:215` |
| RageDisplay_OGL.h/cpp | `RageDisplay_Legacy` (**not** `RageDisplay_OGL`) | `RageDisplay_OGL.h:32` |
| RageDisplay_D3D.h/cpp | `RageDisplay_D3D : public RageDisplay` | `RageDisplay_D3D.h:6` |
| RageDisplay_GLES2.h/cpp | `RageDisplay_GLES2 : public RageDisplay` | `RageDisplay_GLES2.h:4` |
| RageDisplay_Null.h/cpp | `RageDisplay_Null : public RageDisplay` | `RageDisplay_Null.h:6` |
| RageDisplay_OGL_Helpers.h/cpp | `RageDisplay_Legacy_Helpers` namespace (referenced from `RageDisplay_OGL.h:21`) | not independently opened |

### Vertex & Geometry Processing

| File | Class | Verified at |
|------|-------|-------------|
| RageDisplay.h | `RageCompiledGeometry` (abstract vertex buffer) | `RageDisplay.h:26` |
| RageModelGeometry.h/cpp | `RageModelGeometry` — public data members `m_Meshes`, `m_pCompiledGeometry`, `m_vMins`/`m_vMaxs` | `RageModelGeometry.h:12-29` |
| ActorMultiVertex.h/cpp | `ActorMultiVertex : public Actor` | `ActorMultiVertex.h:28` |
| Quad.h/cpp | `Quad : public Sprite` | `Quad.h:7` |
| ModelTypes.h | `msMesh`, `msMaterial`, `msTriangle`, `AnimatedTexture` | `ModelTypes.h:8,14,32,80` |
| RageTypes.h | `RageSpriteVertex` (p,n,c,t), `RageModelVertex` (p,n,t,bone,TextureMatrixScale), `RageMatrix` | `RageTypes.h:358,370,394` |

### Actor System (Scene graph)

| File | Class | Verified at |
|------|-------|-------------|
| Actor.h/cpp | `Actor : public MessageSubscriber` | `Actor.h:105` |
| ActorFrame.h/cpp | `ActorFrame : public Actor` | `ActorFrame.h:7` |
| Sprite.h/cpp | `Sprite : public Actor` | `Sprite.h:11` |
| Model.h/cpp | `Model : public Actor` | `Model.h:15` |
| Quad.h/cpp | `Quad : public Sprite` | `Quad.h:7` |
| ActorScroller.h/cpp | `ActorScroller : public ActorFrame` | `ActorScroller.h:9` |
| ActorFrameTexture.h/cpp | `ActorFrameTexture : public ActorFrame` | `ActorFrameTexture.h:7` |
| ActorMultiTexture.h/cpp | `ActorMultiTexture : public Actor` | `ActorMultiTexture.h:11` |
| ActorMultiVertex.h/cpp | `ActorMultiVertex : public Actor` | `ActorMultiVertex.h:28` |
| BitmapText.h/cpp | `BitmapText : public Actor` | `BitmapText.h:11` |
| Screen.h/cpp | `Screen : public ActorFrame` | `Screen.h:41` |

Not independently verified in this pass (present as files, contents not read): `ActorProxy.h/cpp`, `AutoActor.h/cpp`, `DynamicActorScroller.h/cpp`, `ActorSound.h/cpp`, `ActorUtil.h/cpp`.

### Texture Management

| File | Class | Verified at |
|------|-------|-------------|
| RageTexture.h/cpp | `RageTexture` (abstract; public field `int m_iRefCount`) | `RageTexture.h:10,58` |
| RageTextureID.h/cpp | `struct RageTextureID` — plain data struct (`filename`, `bMipMaps`, etc.), not a class with getters | `RageTextureID.h:10-18` |
| RageTextureManager.h/cpp | `RageTextureManager`, global `TEXTUREMAN` | `RageTextureManager.h:48,101` |
| RageBitmapTexture.h/cpp | `RageBitmapTexture : public RageTexture` | `RageBitmapTexture.h:8` |
| RageTextureRenderTarget.h/cpp | `RageTextureRenderTarget : public RageTexture` | `RageTextureRenderTarget.h:10` |
| RageTexturePreloader.h/cpp | `RageTexturePreloader` — synchronous, holds refs in `vector<RageTexture*>` | `RageTexturePreloader.h:7-21` |
| ActorFrameTexture.h/cpp | `ActorFrameTexture : public ActorFrame` (render-to-texture actor) | `ActorFrameTexture.h:7` |
| ActorMultiTexture.h/cpp | `ActorMultiTexture : public Actor` (composites multiple `RageTexture*` units) | `ActorMultiTexture.h:11,36-42` |

### Font & Text Rendering

| File | Contents | Verified at |
|------|----------|-------------|
| Font.h/cpp | `Font`, `FontPage`, `FontPageTextures`, `FontPageSettings`, `glyph` (lowercase struct) | `Font.h:16,39,67,111,144` |
| FontManager.h/cpp | `FontManager`, global **`FONT`** | `FontManager.h:9,21` |
| FontCharmaps.h/cpp | namespace `FontCharmaps`, function `get_char_map(RString)` | `FontCharmaps.h:4-7` |
| FontCharAliases.h/cpp | namespace `FontCharAliases`, functions `ReplaceMarkers()`, `GetChar()` | `FontCharAliases.h:5-8` |
| BitmapText.h/cpp | `BitmapText : public Actor` — the actual text-rendering actor | `BitmapText.h:11` |

### Video / Movie Textures

| File | Class | Verified at |
|------|-------|-------------|
| arch/MovieTexture/MovieTexture.h | `RageMovieTexture : public RageTexture` (abstract); `RageMovieTextureDriver` (factory base) | `MovieTexture.h:10,30` |
| arch/MovieTexture/MovieTexture_Generic.h/cpp | `MovieTexture_Generic : public RageMovieTexture` | `MovieTexture_Generic.h:80` |
| arch/MovieTexture/MovieTexture_FFMpeg.h/cpp | `MovieTexture_FFMpeg : public MovieTexture_Generic`; `RageMovieTextureDriver_FFMpeg : public RageMovieTextureDriver` | `MovieTexture_FFMpeg.h:37,46` |
| arch/MovieTexture/MovieTexture_DShow.h/cpp | `MovieTexture_DShow : public RageMovieTexture`; `RageMovieTextureDriver_DShow` | `MovieTexture_DShow.h:32,74` |
| arch/MovieTexture/MovieTexture_Null.h/cpp | `RageMovieTextureDriver_Null : public RageMovieTextureDriver` (no separate texture class found in header) | `MovieTexture_Null.h:6` |

### Math

| File | Style | Verified at |
|------|-------|-------------|
| RageMath.h/cpp | Free functions with output pointers (`RageMatrixIdentity(RageMatrix*)`, `RageVec3Cross(...)`), plus `RageQuadratic`/`RageBezier2D` classes for tween curves | `RageMath.h:17-54,64-99` |

## Real Class Hierarchy (verified)

```
MessageSubscriber (MessageManager.h:166)
  └─ Actor (Actor.h:105)
       ├─ ActorFrame (ActorFrame.h:7)
       │    ├─ Screen (Screen.h:41)
       │    ├─ ActorFrameTexture (ActorFrameTexture.h:7)
       │    └─ ActorScroller (ActorScroller.h:9)
       ├─ Sprite (Sprite.h:11)
       │    └─ Quad (Quad.h:7)          ← NOT a direct Actor subclass
       ├─ Model (Model.h:15)
       ├─ ActorMultiVertex (ActorMultiVertex.h:28)
       ├─ ActorMultiTexture (ActorMultiTexture.h:11)
       └─ BitmapText (BitmapText.h:11)

RageTexture (RageTexture.h:10)
  ├─ RageBitmapTexture (RageBitmapTexture.h:8)
  ├─ RageTextureRenderTarget (RageTextureRenderTarget.h:10)
  └─ RageMovieTexture (arch/MovieTexture/MovieTexture.h:10)
       ├─ MovieTexture_DShow (MovieTexture_DShow.h:32)
       └─ MovieTexture_Generic (MovieTexture_Generic.h:80)
            └─ MovieTexture_FFMpeg (MovieTexture_FFMpeg.h:37)

RageDisplay (RageDisplay.h:215)
  ├─ RageDisplay_Legacy   (RageDisplay_OGL.h:32   — OpenGL, misleadingly-named file)
  ├─ RageDisplay_D3D      (RageDisplay_D3D.h:6)
  ├─ RageDisplay_GLES2    (RageDisplay_GLES2.h:4)
  └─ RageDisplay_Null     (RageDisplay_Null.h:6)
```

## Verified Per-Frame Rendering Sequence

Traced directly from `ScreenManager::Draw()`:

```
ScreenManager::Draw()                          ScreenManager.cpp:488
  if top screen's IsFirstUpdate() → return early              (:494)
  if !DISPLAY->BeginFrame() → return                          (:497)
  DISPLAY->CameraPushMatrix()                                 (:500)
  DISPLAY->LoadMenuPerspective(0, SCREEN_WIDTH, SCREEN_HEIGHT,
                                SCREEN_CENTER_X, SCREEN_CENTER_Y)  (:501)
  g_pSharedBGA->Draw()              // shared background anim (:502)
  DISPLAY->CameraPopMatrix()                                  (:503)
  for each screen in g_ScreenStack (bottom→top): pScreen->Draw() (:505-506)
  for each overlay screen: pScreen->Draw()                    (:508-509)
  DISPLAY->EndFrame()                                         (:512)
```

Since `Screen : public ActorFrame : public Actor`, `pScreen->Draw()` runs the base
`Actor::Draw()` sequence (see `01-RageDisplay.md`/`02-ActorSystem.md` for that call
chain, traced from `Actor.cpp:376`).

## See Also

- [01-RageDisplay.md](01-RageDisplay.md) — Verified `RageDisplay` interface
- [02-ActorSystem.md](02-ActorSystem.md) — Verified `Actor`/`ActorFrame` API
- [03-TextureSystem.md](03-TextureSystem.md) — Verified texture management
- [04-FontSystem.md](04-FontSystem.md) — Verified bitmap-font system (no FreeType)
- [05-GeometryAndPrimitives.md](05-GeometryAndPrimitives.md) — Verified geometry/math
- [06-ArchitectureDiagrams.md](06-ArchitectureDiagrams.md) — Diagrams rebuilt from cited code
- [07-QuickReference.md](07-QuickReference.md) — Corrected code examples that match real signatures
