# Actor System - Scene Graph & Transformations (Verified)

`Actor.h` (784 lines) and `ActorFrame.h` (164 lines) were read in full;
`Actor.cpp` was read at the `Draw()`/`Update()` implementations. Line numbers
below cite those files unless stated otherwise.

## Files (confirmed to exist and their real base classes)

| File | Class | Base | Verified at |
|------|-------|------|-------------|
| Actor.h/cpp | `Actor` | `MessageSubscriber` | `Actor.h:105` |
| ActorFrame.h/cpp | `ActorFrame` | `Actor` | `ActorFrame.h:7` |
| Sprite.h/cpp | `Sprite` | `Actor` | `Sprite.h:11` |
| Model.h/cpp | `Model` | `Actor` | `Model.h:15` |
| Quad.h/cpp | `Quad` | **`Sprite`** | `Quad.h:7` |
| ActorScroller.h/cpp | `ActorScroller` | `ActorFrame` | `ActorScroller.h:9` |
| ActorMultiVertex.h/cpp | `ActorMultiVertex` | `Actor` | `ActorMultiVertex.h:28` |
| ActorMultiTexture.h/cpp | `ActorMultiTexture` | `Actor` | `ActorMultiTexture.h:11` |
| ActorFrameTexture.h/cpp | `ActorFrameTexture` | `ActorFrame` | `ActorFrameTexture.h:7` |
| BitmapText.h/cpp | `BitmapText` | `Actor` | `BitmapText.h:11` |

`Quad extends Sprite`, not `Actor` — a solid-color quad in StepMania is
implemented as a `Sprite` with no texture bound, reusing all of Sprite's
draw/animation machinery. `Quad.h` itself declares only a constructor,
`LoadFromNode()`, and `Copy()` (`Quad.h:9-19`) — nothing else, because it
inherits everything from `Sprite`.

`ActorProxy.h/cpp`, `AutoActor.h/cpp`, `DynamicActorScroller.h/cpp` exist in the
tree but were **not opened** in this pass; no claims are made about them here.

## Actor::Draw() — real call sequence (Actor.cpp:376-487)

The header comment (`Actor.h:239-246`) undersells the real sequence — it names
4 steps, but the implementation has more. Traced directly from `Actor::Draw()`:

```cpp
void Actor::Draw()                                    // Actor.cpp:376
{
    if( !m_bVisible || m_fHibernateSecondsLeft > 0 || this->EarlyAbortDraw() )
        return;                                        // early-out, :378-383
    // ... FakeParent / WrapperStates handling (wrapper actor support, :384-449) ...
    this->PreDraw();                                    // :458
    if( PartiallyOpaque() )                             // :460
    {
        this->BeginDraw();                              // :462
        this->DrawPrimitives();                          // :463 — plural, virtual, subclass hook
        this->EndDraw();                                // :464
    }
    this->PostDraw();                                    // :466 — always runs, even if not opaque
}
```

So the real order is **PreDraw → [BeginDraw → DrawPrimitives → EndDraw only if
`PartiallyOpaque()`] → PostDraw**, wrapped in visibility/hibernate/wrapper-state
handling. `PostDraw()` (`Actor.cpp:489-493`) resets `m_internalDiffuse` to white
and `m_internalGlow.a` to 0 — it exists specifically to undo per-draw diffuse/glow
overrides applied by "wrapper" actors (`m_WrapperStates`, a mechanism for
rendering one actor's state inside another without changing its parent —
comment at `Actor.h:629-631`).

The subclass override point is `DrawPrimitives()` (plural — `Actor.h:275`,
default empty body `{}`), **not** `DrawPrimitive()` singular.

## Position, size and "zoom" (there is no "scale" API)

`Actor.h:325-419`. Position setters operate on `DestTweenState()`, i.e. they
set where the actor tweens *to*, not necessarily its current rendered position:

```cpp
float GetX() const  { return m_current.pos.x; }        // :325 — current (post-tween) position
float GetDestX() const { return DestTweenState().pos.x; } // :334 — tween target
void  SetX( float x ) { DestTweenState().pos.x = x; }   // :337
void  SetXY( float x, float y );                        // :340 — no SetXYZ() exists
void  AddX( float x ) { SetX( GetDestX()+x ); }          // :344
```

There is no `SetXYZ()`. To set Z, call `SetZ()` separately (`:339`).

Sizing uses "Zoom", not "Scale":

```cpp
float GetUnzoomedWidth() const  { return m_size.x; }    // :355
float GetZoomedWidth() const    { return m_size.x * m_baseScale.x * DestTweenState().scale.x; } // :357
void  SetZoom( float zoom );                             // :399 — sets x,y,z scale.* uniformly
void  SetZoomX/Y/Z( float zoom );                        // :408-416
void  ZoomTo( float fX, float fY );                      // :417 — via ZoomToWidth/ZoomToHeight
```

Internally the tween state does have a field named `scale` (`RageVector3 scale;`,
`Actor.h:213`), and there is a separate `m_baseScale` (`:656`) set via
`SetBaseZoomX/Y/Z()` (`:363-368`) — a second, non-tweened scale factor multiplied
in on top. There is **no** `SetAnchorX/Y()` or `GetAnchorX/Y()` anywhere in this
header — StepMania has no actor "anchor point" concept distinct from position.

## Color

```cpp
virtual void SetDiffuse( RageColor c );                  // :457 — sets all 4 corners
virtual void SetDiffuseAlpha( float f );                  // :458 — NOT "SetAlpha"
float GetCurrentDiffuseAlpha() const;                     // :459
void SetDiffuses( int i, RageColor c );                   // :461 — per-corner, by index
void SetDiffuseUpperLeft/UpperRight/LowerLeft/LowerRight( RageColor c ); // :462-465
void SetDiffuseTopEdge/RightEdge/BottomEdge/LeftEdge( RageColor c );     // :466-469
RageColor GetDiffuse() const;                             // :470 — returns corner 0 only
RageColor GetDiffuses( int i ) const;                     // :471 — indexed getter, not array pointer
void SetGlow( RageColor c );                              // :473
RageColor GetGlow() const;                                // :474
```

`NUM_DIFFUSE_COLORS` is `4` (`Actor.h:73`) — one per corner, for gradient fills.
There is no `SetAlpha()` and no `GetDiffuses()` overload returning a `const
RageColor*` array; `GetDiffuses(int i)` takes an index and returns one color.

## Effects — real API is per-effect setters, not a generic `SetEffectMode()`

```cpp
enum Effect { no_effect, diffuse_blink, diffuse_shift, diffuse_ramp,
              glow_blink, glow_shift, glow_ramp, rainbow,
              wag, bounce, bob, pulse, spin, vibrate };            // :129-133
enum EffectClock { CLOCK_TIMER, CLOCK_TIMER_GLOBAL, CLOCK_BGM_TIME,
                    CLOCK_BGM_BEAT, CLOCK_BGM_TIME_NO_OFFSET, CLOCK_BGM_BEAT_NO_OFFSET,
                    CLOCK_BGM_BEAT_PLAYER1, CLOCK_BGM_BEAT_PLAYER2,
                    CLOCK_LIGHT_1 = 1000, CLOCK_LIGHT_LAST = 1100, NUM_CLOCKS }; // :136-149

void StopEffect() { m_Effect = no_effect; }                        // :524
Effect GetEffect() const { return m_Effect; }                      // :525
void SetEffectPeriod( float fTime );                                // :534
void SetEffectMagnitude( RageVector3 vec );                          // :543 — a VECTOR, not a float
void SetEffectClock( EffectClock c );                                // :540

// Setting an effect means calling ONE of these — there is no generic setter:
void SetEffectDiffuseBlink( float fEffectPeriodSeconds, RageColor c1, RageColor c2 ); // :547
void SetEffectDiffuseShift/Ramp( ... );                              // :548-549
void SetEffectGlowBlink/Shift/Ramp( ... );                           // :550-552
void SetEffectRainbow( float fEffectPeriodSeconds );                 // :553
void SetEffectWag/Bounce/Bob( float fPeriod, RageVector3 vect );     // :554-556
void SetEffectPulse( float fPeriod, float fMinZoom, float fMaxZoom );// :557
void SetEffectSpin/Vibrate( RageVector3 vect );                      // :558-559
```

There is no `actor->SetEffectMode(Actor::bob)` call in the real API — calling
`SetEffectBob(period, magnitude_vector)` both selects the effect *and* configures
it in one call. `SetEffectMagnitude()` takes a `RageVector3`, not a `float`.

## Tweening

```cpp
virtual void BeginTweening( float time, ITween *pInterp );   // :479
void BeginTweening( float time, TweenType tt = TWEEN_LINEAR ); // :480
virtual void StopTweening();                                  // :481
virtual void FinishTweening();                                // :485
virtual void HurryTweening( float factor );                   // :486
```

`TweenType` (verified in `Tween.h:10-19`) is:
`TWEEN_LINEAR, TWEEN_ACCELERATE, TWEEN_DECELERATE, TWEEN_SPRING, TWEEN_BEZIER`.
There is no `TWEEN_EASE_OUT` — the closest real equivalent is `TWEEN_DECELERATE`.

`TweenState` (`Actor.h:202-234`) holds `pos`, `rotation`, `quat`, `scale`,
`fSkewX`/`fSkewY`, `crop` (a `RectF`), `fade` (a `RectF`), `diffuse[4]`, `glow`,
and `aux`. Crop/fade are the closest things to "clipping" — there is **no**
`SetClipRect()`/`GetClipRect()` anywhere in `Actor.h`:

```cpp
void SetCropLeft/Top/Right/Bottom( float percent );  // Actor.h:445-448
void SetFadeLeft/Top/Right/Bottom( float percent );  // Actor.h:450-453
```

## Blend mode & render state (Actor.h:582-591)

```cpp
void SetBlendMode( BlendMode mode ) { m_BlendMode = mode; }   // :582
void SetTextureWrapping( bool b );                              // :584
void SetTextureFiltering( bool b );                              // :585
virtual void SetZTestMode( ZTestMode mode );                     // :588
virtual void SetZWrite( bool b );                                // :589
virtual void SetCullMode( CullMode mode );                       // :591
```

## Messages

`Actor` inherits `MessageSubscriber` (`MessageManager.h:166-183`), whose real
API is:

```cpp
void SubscribeToMessage( MessageID message );        // MessageManager.h:176
void SubscribeToMessage( const RString &sMessageName ); // :177
void UnsubscribeAll();                                // :179
```

There is no `AddMessageSubscriber()`/`RemoveMessageSubscriber()` on `Actor`.
`Actor` also declares its own message entry point:

```cpp
virtual void HandleMessage( const Message &msg );     // Actor.h:612
void PlayCommand( const RString &sCommandName ) { HandleMessage( Message(sCommandName) ); } // :601
```

`PlayCommand()` is a convenience wrapper that constructs a `Message` from a
command name and routes it straight through `HandleMessage()` — it is not a
separate broadcast mechanism.

## ActorFrame — composite container (ActorFrame.h, read in full)

```cpp
virtual void AddChild( Actor *pActor );               // :22
virtual void RemoveChild( Actor *pActor );             // :26
void TransferChildren( ActorFrame *pTo );              // :27
Actor* GetChild( const RString &sName );               // :28 — not const-qualified
vector<Actor*> GetChildren() { return m_SubActors; }   // :29 — returns a COPY of the vector
int GetNumChildren() const { return m_SubActors.size(); } // :30 — int, not unsigned int
void RemoveAllChildren();                               // :33
void MoveToTail( Actor* pActor );                       // :38
void MoveToHead( Actor* pActor );                       // :43
void SortByDrawOrder();                                 // :44
```

There is no `GetChildAtIndex(unsigned int)` and no recursive `FindActor()` — to
walk a hierarchy you use `GetChildren()` and recurse manually, or `GetChild()`
by exact name at that frame's own level.

`ActorFrame` also overrides the draw/update virtuals and adds lighting-related
setters not present on `Actor`:

```cpp
virtual void UpdateInternal( float fDeltaTime );  // :67
virtual void BeginDraw();  virtual void DrawPrimitives();  virtual void EndDraw(); // :68-70
void SetCustomLighting( bool bCustomLighting );    // :83
void SetAmbientLightColor/DiffuseLightColor/SpecularLightColor( RageColor c ); // :84-86
void SetLightDirection( RageVector3 vec );          // :87
void SetFOV( float fFOV );  void SetVanishPoint( float fX, float fY ); // :80-81
```

`m_SubActors` (`ActorFrame.h:102`) is a `vector<Actor*>` — the children are held
as raw, non-owning-by-default pointers; `DeleteChildrenWhenDone(true)`
(`:52`) opts into ownership/auto-delete, used by the
`ActorFrameAutoDeleteChildren` subclass (`:128-134`).

## Draw order constants (Actor.h:19-31) — confirmed unchanged

```cpp
#define DRAW_ORDER_BEFORE_EVERYTHING  -200
#define DRAW_ORDER_UNDERLAY           -100
#define DRAW_ORDER_DECORATIONS           0
#define DRAW_ORDER_OVERLAY            +100
#define DRAW_ORDER_TRANSITIONS        +200
#define DRAW_ORDER_AFTER_EVERYTHING   +300
```
Set/read via `SetDrawOrder(int)`/`GetDrawOrder()` (`Actor.h:574-575`); actual
ordering is applied by `ActorFrame::SortByDrawOrder()` (`ActorFrame.h:44`).

## Sprite (Sprite.h, read in full — 175 lines)

```cpp
class Sprite: public Actor                              // Sprite.h:11
{
    virtual void DrawPrimitives();                        // :36
    virtual void Load( RageTextureID ID );                 // :47
    void SetTexture( RageTexture *pTexture );               // :48
    RageTexture* GetTexture() { return m_pTexture; }        // :51
    virtual int GetNumStates() const;                        // :55
    virtual void SetState( int iNewState );                  // :56
    int GetState() { return m_iCurState; }                    // :57
    void SetCustomTextureRect( const RectF &new_texcoord_frect ); // :67
    void SetCustomTextureCoords( float fTexCoords[8] );        // :68
    void ScaleToClipped( float fWidth, float fHeight );        // :90
    void CropTo( float fWidth, float fHeight );                 // :91
};
```
`Sprite::SetTexture(RageTexture*)` matches what was previously documented — this
one was correct. The custom-coordinate API uses flat `float[8]` arrays (4 x/y
pairs: top-left, bottom-left, bottom-right, top-right per the comment at
`Sprite.h:124-127`), not a `RageVector2[4]`.

## Model (Model.h, read in full — 118 lines)

```cpp
class Model : public Actor                                // Model.h:15
{
    void Load( const RString &sFile );                       // :23 — NOT "LoadModel"
    void LoadMilkshapeAscii( const RString &sFile );          // :26
    void PlayAnimation( const RString &sAniName, float fPlayRate = 1 ); // :32
    void SetRate( float fRate );  void SetLoop( bool b );     // :33-34
    void SetPosition( float fSeconds );                        // :35
    virtual void DrawPrimitives();                              // :39
    void SetCelShading( bool bShading );                        // :42
private:
    RageModelGeometry *m_pGeometry;                             // :60
    vector<msMaterial> m_Materials;                             // :63 — parsed from file, no public setter
    map<RString,msAnimation> m_mapNameToAnimation;              // :64
};
```
There is no public `LoadModel()`, `SetMaterial()`/`GetMaterial()`, or
`SetTexture(int meshIndex, ...)`. Materials (`msMaterial`, `ModelTypes.h:80-95`)
are populated by parsing a Milkshape file and are not set individually at
runtime through the `Model` interface documented here. `Model`'s public surface
is centered on **animation playback** (`PlayAnimation`, `SetRate`, `SetLoop`,
`SetPosition`, `SetSecondsIntoAnimation`) and cel-shading, not per-mesh material
authoring.

## ActorMultiVertex (ActorMultiVertex.h, read in full — 203 lines)

```cpp
enum DrawMode { DrawMode_Quads, DrawMode_QuadStrip, DrawMode_Fan, DrawMode_Strip,
                 DrawMode_Triangles, DrawMode_LineStrip, DrawMode_SymmetricQuadStrip }; // :9-19
class ActorMultiVertex: public Actor
{
    void SetNumVertices( size_t n );                            // :90
    void AddVertex();  void AddVertices( int Add );              // :92-93
    void SetVertexPos( int index, float x, float y, float z );   // :109
    void SetVertexColor( int index, RageColor c );                // :110
    void SetVertexCoords( int index, float TexCoordX, float TexCoordY ); // :111
    void SetDrawState( DrawMode dm, int first, int num );          // :99
    void SetTexture( RageTexture *Texture );                       // :85
};
```
Vertices are stored as `vector<RageSpriteVertex>` (`:156`) and set one at a time
by index, not via a bulk `SetVertices(vector<RageVertex>)` call. Draw topology is
selected through the `DrawMode` enum (`:9-19`), not a raw OpenGL `GLenum`.

## ActorScroller (ActorScroller.h, read in full — 127 lines)

```cpp
class ActorScroller : public ActorFrame                       // :9
{
    void SetTransformFromExpression( const RString &sTransformFunction ); // :15
    void EnableMask( float fWidth, float fHeight );              // :21
    void SetLoop( bool bLoop );  void SetWrap( bool bWrap );      // :32-33
    void SetNumItemsToDraw( float fNumItemsToDraw );               // :34
    void SetSecondsPerItem( float fSeconds );                       // :43
protected:
    Quad m_quadMask;                                                // :85 — masking uses an actual Quad
    LuaExpressionTransform m_exprTransformFunction; // params: self,offset,itemIndex,numItems // :88
};
```
Item positioning is driven by a **Lua expression function**
(`LuaExpressionTransform`, `:88`) evaluated per item with `(self, offset,
itemIndex, numItems)`, not by fixed width/height viewport properties. There is
no `SetMaxWidth()`/`SetMaxHeight()`/`SetNumVisibleItems()`.

## ActorFrameTexture (ActorFrameTexture.h, read in full — 89 lines)

```cpp
class ActorFrameTexture: public ActorFrame
{
    void SetTextureName( const RString &sName );                 // :23
    RageTextureRenderTarget *GetTexture();                         // :28
    void EnableDepthBuffer/EnableAlphaBuffer/EnableFloat/EnablePreserveTexture( bool b ); // :30-33
    void Create();                                                  // :35
    virtual void DrawPrimitives();                                  // :37
private:
    RageTextureRenderTarget *m_pRenderTarget;                       // :43
};
```
There is no `SetRenderTarget(RageTextureRenderTarget*)` or `EnablePreRender(bool)`
— you configure the target's properties (depth/alpha/float/preserve) and call
`Create()`; the render target object itself is owned internally.

## ActorMultiTexture (ActorMultiTexture.h, read in full — 74 lines)

```cpp
class ActorMultiTexture: public Actor
{
    void ClearTextures();
    int AddTexture( RageTexture *pTexture );                       // :25
    void SetTextureMode( int iIndex, TextureMode tm );               // :26
    void SetSizeFromTexture( RageTexture *pTexture );                 // :28
    void SetTextureCoords( const RectF &r );                          // :29
private:
    vector<TextureUnitState> m_aTextureUnits;                          // :42
};
```
Textures are added one at a time via `AddTexture()` (which returns a slot
index), not set by fixed `SetTexture(slot, texture)`/`SetTextureCoords(slot,
coords[4])` calls as previously claimed.

---

**Scope**: `Actor.h` (full), `Actor.cpp` (Draw/Update sections), `ActorFrame.h`
(full), `Sprite.h`, `Model.h`, `Quad.h`, `ActorMultiVertex.h`, `ActorScroller.h`,
`ActorFrameTexture.h`, `ActorMultiTexture.h` (all full reads), `Tween.h` (enum
only), `MessageManager.h` (MessageSubscriber section only).
