# Architecture Diagrams (Verified)

Every diagram below is built from a specific traced call chain or a directly-read
struct/enum — cited inline. Diagrams that were purely generic textbook filler in
the previous version (e.g. a made-up "texture filtering" pipeline with no
matching StepMania API) have been dropped rather than kept as decoration.

## System overview (class layers, verified)

```
┌──────────────────────────────────────────────────────────────────┐
│ Screen / ScreenManager                                            │
│   Screen : public ActorFrame : public Actor      (Screen.h:41)   │
│   global SCREENMAN : ScreenManager                                │
└───────────────────────┬────────────────────────────────────────────┘
                        │ ScreenManager::Draw() — ScreenManager.cpp:488
                        ▼
┌──────────────────────────────────────────────────────────────────┐
│ Actor / ActorFrame scene graph                                    │
│   Actor (Actor.h:105)                                              │
│    ├─ ActorFrame (ActorFrame.h:7)                                    │
│    ├─ Sprite (Sprite.h:11) ── Quad (Quad.h:7, extends Sprite!)          │
│    ├─ Model (Model.h:15)                                                 │
│    ├─ ActorMultiVertex / ActorMultiTexture (Actor.h subclasses)             │
│    └─ BitmapText (BitmapText.h:11)                                            │
└───────────────────────┬────────────────────────────────────────────────────┘
                        │ Actor::DrawPrimitives() (virtual, plural — Actor.h:275)
                        │ calls into RageDisplay's concrete draw methods
                        ▼
┌──────────────────────────────────────────────────────────────────┐
│ RageDisplay (RageDisplay.h:215) — concrete, non-virtual entry points │
│   DrawQuads/DrawQuadStrip/DrawFan/DrawStrip/DrawTriangles/            │
│   DrawLineStrip/DrawSymmetricQuadStrip/DrawCircle (:342-350)            │
│   each forwards to a protected pure-virtual …Internal() (:372-380)       │
└───────────────────────┬──────────────────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────────────┐
│ Backend implementation                                             │
│   RageDisplay_Legacy  (OpenGL — file RageDisplay_OGL.h:32)            │
│   RageDisplay_D3D     (RageDisplay_D3D.h:6)                              │
│   RageDisplay_GLES2   (RageDisplay_GLES2.h:4)                              │
│   RageDisplay_Null    (RageDisplay_Null.h:6)                                 │
└──────────────────────────────────────────────────────────────────────────┘
```

## Verified per-frame sequence — traced from `ScreenManager::Draw()`

Source: `ScreenManager.cpp:488-513`.

```
ScreenManager::Draw()
│
├─ if top-of-stack screen->IsFirstUpdate() → return (skip render entirely)   (:494)
│
├─ if( !DISPLAY->BeginFrame() ) return                                       (:497)
│
├─ DISPLAY->CameraPushMatrix()                                                (:500)
├─ DISPLAY->LoadMenuPerspective(0, SCREEN_WIDTH, SCREEN_HEIGHT,
│                                SCREEN_CENTER_X, SCREEN_CENTER_Y)              (:501)
├─ g_pSharedBGA->Draw()          // shared background animation actor            (:502)
├─ DISPLAY->CameraPopMatrix()                                                     (:503)
│
├─ for i in g_ScreenStack (bottom→top): g_ScreenStack[i].m_pScreen->Draw()          (:505-506)
├─ for each overlay screen:              g_OverlayScreens[i]->Draw()                  (:508-509)
│
└─ DISPLAY->EndFrame()                                                                  (:512)
```

Note the early-return on `IsFirstUpdate()` — the comment at `:490-495`
explains this exists so a screen's very-first update (which can include the
whole time spent loading) never gets rendered with `BeginFrame`/`EndFrame`
bracketing it, avoiding a spurious vsync wait on a frame that's about to be
discarded anyway.

## Verified `Actor::Draw()` sequence — traced from `Actor.cpp:376-487`

Since `Screen`, `ActorFrame`, `Sprite`, `Model`, etc. are all `Actor`
subclasses, every `pScreen->Draw()` call above runs through this exact logic:

```
Actor::Draw()                                                    Actor.cpp:376
│
├─ if( !m_bVisible || m_fHibernateSecondsLeft > 0 || EarlyAbortDraw() )
│      return                                                     (:378-383)
│
├─ [FakeParent / WrapperStates handling — lets one actor render      (:384-449)
│   wrapped inside another's diffuse/glow state without becoming
│   its real parent; see Actor.h:629-631 for the mechanism's purpose]
│
├─ this->PreDraw()                                                  (:458)
├─ if( PartiallyOpaque() ) {
│      this->BeginDraw()                                               (:462)
│      this->DrawPrimitives()    ← virtual, subclass-specific draw       (:463)
│      this->EndDraw()                                                     (:464)
│  }
└─ this->PostDraw()     ← ALWAYS runs, resets internal diffuse/glow           (:466, :489-493)
```

`DrawPrimitives()` is where subclass behavior diverges:
- `Sprite::DrawPrimitives()` — `Sprite.h:36`
- `Model::DrawPrimitives()` — `Model.h:39`
- `ActorFrame::DrawPrimitives()` — `ActorFrame.h:69` (draws `m_SubActors`)
- `ActorMultiVertex::DrawPrimitives()` — `ActorMultiVertex.h:74`
- `BitmapText::DrawPrimitives()` — `BitmapText.h:69` (calls `DrawChars()`)

## Actor tree — real classes, real base relationships

```
Screen (ActorFrame)                              Screen.h:41
├─ m_SubActors : vector<Actor*>                   ActorFrame.h:102
│   ├─ Sprite (background image)                   Sprite.h:11
│   ├─ Model (3D element)                            Model.h:15
│   ├─ BitmapText (label)                              BitmapText.h:11
│   ├─ Quad (solid-color panel — IS a Sprite)            Quad.h:7
│   └─ ActorFrame (nested sub-group)                       ActorFrame.h:7
│        └─ ... more children (m_SubActors) ...
└─ DrawOrder controls ordering within a frame,
    applied by ActorFrame::SortByDrawOrder()                ActorFrame.h:44
    against the DRAW_ORDER_* constants                        Actor.h:19-31
```

There is no verified "wrapper.underlay/overlay/transitions" fixed slot
structure in the Actor/ActorFrame headers — draw order is a plain integer
(`m_iDrawOrder`, `Actor.h:729`) compared via `SortByDrawOrder()`; the
`DRAW_ORDER_*` `#define`s are just conventional integer values themes use, not
enforced named layers in the C++ structure.

## Verified resource-cache pipeline — `RageTextureManager`

Source: `RageTextureManager.h:48-99` (see `03-TextureSystem.md` for full detail).

```
RageTextureManager::LoadTexture( RageTextureID ID )              (:55)
│
├─ [cache lookup keyed by RageTextureID equality — RageTextureID.h:71-87,
│   which compares every field EXCEPT Policy]
│
├─ found  → (refcount handling — RageTexture::m_iRefCount, public field)
└─ not found → LoadTextureInternal(ID)  (private, :94)
                → constructs a RageTexture subclass
                  (RageBitmapTexture | RageTextureRenderTarget | RageMovieTexture)
                → caches it

Cache invalidation is NOT automatic garbage collection — it's two explicit hooks:
  DeleteCachedTextures() → GarbageCollect(screen_changed)   "call this between Screens"  (:72)
  DoDelayedDelete()      → GarbageCollect(delayed_delete)   "call this on switch theme"    (:75)
```

## Verified glyph pipeline — reconstructed from `BitmapText`/`Font` headers

See `04-FontSystem.md` for full detail; this is the condensed flow:

```
BitmapText::SetText("Hello")                                   BitmapText.h:59
  → m_wTextLines : vector<wstring>                                (:116)
  → BuildChars()                                                   (:138)
       for each wchar_t c:
         Font::GetGlyph(c) → const glyph&                            Font.h:153
           (m_iCharToGlyphCache[128] ASCII fast path,                Font.h:203
            else m_iCharToGlyph map, else FONT_DEFAULT_GLYPH)         Font.h:201,231
         glyph.m_TexRect / m_fWidth / m_fHeight / m_fHshift / m_iHadvance
           → appended into m_aVertices : vector<RageSpriteVertex>       BitmapText.h:129

Actor::Draw() → BitmapText::DrawPrimitives()                            BitmapText.h:69
  → DrawChars(bUseStrokeTexture)                                          BitmapText.h:139
       binds glyph.m_pPage->m_FontPageTextures.m_pTextureMain
       (and m_pTextureStroke if stroke enabled), submits m_aVertices        Font.h:19,23,129
```

No kerning step exists in this pipeline (`04-FontSystem.md`) — spacing is purely
cumulative `m_iHadvance` per glyph.

## Matrix stack — concrete, not virtual (RageDisplay.h:402-433)

```
RageDisplay maintains 4 independent stacks, each with push/pop:
  World     — PushMatrix()/PopMatrix(), Translate/Scale/RotateX/Y/Z/Skew   (:403-416)
  Texture   — TexturePushMatrix()/TexturePopMatrix()/TextureTranslate       (:419-422)
  Camera    — CameraPushMatrix()/CameraPopMatrix()/LoadMenuPerspective/
              LoadLookAt                                                    (:425-428)
  Centering — CenteringPushMatrix()/CenteringPopMatrix()/ChangeCentering      (:431-433)

Backends read the current top via protected accessors, not by re-deriving state:
  GetWorldTop() / GetProjectionTop() / GetViewTop() / GetTextureTop() / GetCentering()  (:452-457)
```

All push/pop/transform methods here are ordinary (non-virtual) member functions
implemented once in `RageDisplay.cpp` — backends don't override matrix-stack
behavior, only the eventual draw call that consumes the resulting matrix.

The actual matrix math is done via free functions in `RageMath.h`
(`RageMatrixIdentity`, `RageMatrixMultiply`, `RageMatrixTranslation`, etc. —
see `05-GeometryAndPrimitives.md`), each writing through an output pointer
rather than returning by value or being a class method.

## Verified enums for render state (RageTypes.h:8-78)

```cpp
enum BlendMode {                                    // :8-23
    BLEND_NORMAL, BLEND_ADD, BLEND_SUBTRACT, BLEND_MODULATE,
    BLEND_COPY_SRC, BLEND_ALPHA_MASK, BLEND_ALPHA_KNOCK_OUT,
    BLEND_ALPHA_MULTIPLY, BLEND_WEIGHTED_MULTIPLY, BLEND_INVERT_DEST,
    BLEND_NO_EFFECT
};
enum TextureMode { TextureMode_Modulate, TextureMode_Glow, TextureMode_Add }; // :26-40
enum EffectMode {                                      // :43-58
    EffectMode_Normal, EffectMode_Unpremultiply, EffectMode_ColorBurn,
    EffectMode_ColorDodge, EffectMode_VividLight, EffectMode_HardMix,
    EffectMode_Overlay, EffectMode_Screen, EffectMode_YUYV422,
    EffectMode_DistanceField
};
enum CullMode { CULL_BACK, CULL_FRONT, CULL_NONE };       // :60-68
enum ZTestMode { ZTEST_OFF, ZTEST_WRITE_ON_PASS, ZTEST_WRITE_ON_FAIL }; // :70-78
```

`BLEND_SCREEN` and `BLEND_MULTIPLY`, previously listed as blend modes, do not
exist — `EffectMode_Screen` exists but it's a *shader effect mode*
(`SetEffectMode`/`SetCelShaded` territory, `RageDisplay.h:285-286,337`), a
separate render-state axis from `BlendMode` entirely. `TextureMode_Glow`'s
comment (`RageTypes.h:31-32`) explicitly documents the "replace color with
white, keep alpha, combine with `BLEND_ADD`" technique StepMania uses for glow
— a concrete, verified example of how these two enums combine in practice.

## Class dependency graph (verified relationships only)

```
Actor (MessageSubscriber)
  ├─ owns TweenState m_current / m_start, vector<TweenStateAndInfo*> m_Tweens   Actor.h:662-670
  ├─ calls RageDisplay's concrete Draw*/matrix methods from DrawPrimitives()     (backend-agnostic)
  └─ subclasses: ActorFrame, Sprite, Model, ActorMultiVertex,
                 ActorMultiTexture, BitmapText                                   (per-class headers)

Sprite
  ├─ holds RageTexture* m_pTexture                                              Sprite.h:110
  └─ Quad extends Sprite, adds nothing                                          Quad.h:7-19

Model
  ├─ holds RageModelGeometry* m_pGeometry                                        Model.h:60
  ├─ holds vector<msMaterial> m_Materials                                          Model.h:63
  └─ RageModelGeometry holds RageCompiledGeometry* m_pCompiledGeometry              RageModelGeometry.h:26

BitmapText
  ├─ holds Font* m_pFont                                                             BitmapText.h:113
  └─ Font holds vector<FontPage*>, each FontPage holds FontPageTextures                Font.h:129,191

RageTextureManager
  └─ produces/caches RageTexture subclasses, keyed by RageTextureID                      RageTextureManager.h:55

RageDisplay
  └─ friend class RageTexture  (grants RageTexture access to RageDisplay internals)        RageDisplay.h:217
```

---

**Scope**: Every diagram cites a specific header line range or a traced `.cpp`
call chain (`Actor.cpp`, `ScreenManager.cpp`). No diagram in this file
represents generic graphics-textbook content that isn't backed by a citation
above it.
