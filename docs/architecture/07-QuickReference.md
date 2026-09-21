# Quick Reference Guide (Verified)

Every snippet below uses only method signatures confirmed to exist in the cited
file:line. Where the previous version's example used a fabricated method, the
correction is called out explicitly so the mistake isn't silently lost.

## File location index (confirmed real base classes)

| Concern | File | Real class | Base |
|---|---|---|---|
| Graphics API | `RageDisplay.h/cpp` | `RageDisplay` | — |
| OpenGL backend | `RageDisplay_OGL.h/cpp` | **`RageDisplay_Legacy`** (not `RageDisplay_OGL`) | `RageDisplay` |
| D3D backend | `RageDisplay_D3D.h/cpp` | `RageDisplay_D3D` | `RageDisplay` |
| GLES2 backend | `RageDisplay_GLES2.h/cpp` | `RageDisplay_GLES2` | `RageDisplay` |
| Null backend | `RageDisplay_Null.h/cpp` | `RageDisplay_Null` | `RageDisplay` |
| Base actor | `Actor.h/cpp` | `Actor` | `MessageSubscriber` |
| Container | `ActorFrame.h/cpp` | `ActorFrame` | `Actor` |
| Textured 2D | `Sprite.h/cpp` | `Sprite` | `Actor` |
| Solid-color quad | `Quad.h/cpp` | `Quad` | **`Sprite`** (not `Actor`) |
| 3D mesh | `Model.h/cpp` | `Model` | `Actor` |
| Text | `BitmapText.h/cpp` | `BitmapText` | `Actor` |
| Texture handle | `RageTexture.h/cpp` | `RageTexture` | — |
| Texture cache | `RageTextureManager.h/cpp` | `RageTextureManager`, global `TEXTUREMAN` | — |
| Font | `Font.h/cpp` | `Font`, `FontPage` | — |
| Font cache | `FontManager.h/cpp` | `FontManager`, global **`FONT`** (not `FONTMAN`) | — |
| Movie texture | `arch/MovieTexture/MovieTexture.h` | `RageMovieTexture` (not `MovieTexture`) | `RageTexture` |
| Math | `RageMath.h/cpp` | free functions (no class) | — |

## Common tasks — corrected

### Creating and grouping actors

```cpp
// ActorFrame::AddChild — ActorFrame.h:22
ActorFrame *pFrame = new ActorFrame();
Sprite *pSprite = new Sprite();               // Sprite.h:11
pSprite->SetName("MySprite");                 // Actor.h:300
pSprite->SetXY(100, 100);                     // Actor.h:340 — SetXYZ() does not exist
pSprite->SetZ(0);                             // Actor.h:339

pFrame->AddChild(pSprite);                    // ActorFrame.h:22
```

### Loading a texture

```cpp
// RageTextureManager::LoadTexture takes a RageTextureID by value —
// RageTextureID.h:63 makes the single-RString constructor non-explicit,
// so a plain path string still works via implicit conversion.
RageTexture *pTexture = TEXTUREMAN->LoadTexture("path/to/image.png"); // RageTextureManager.h:55

// ... use it ...
sprite->SetTexture(pTexture);                 // Sprite.h:48

// Release when done — there is no "Flush()"/"UnloadAll()"; you unload
// what you loaded:
TEXTUREMAN->UnloadTexture(pTexture);          // RageTextureManager.h:60
```

### Loading a font and measuring text — corrected: global is `FONT`, not `FONTMAN`

```cpp
// FontManager::LoadFont takes a path AND an optional character-restriction
// string — FontManager.h:15
Font *pFont = FONT->LoadFont("Common Normal");

// There is no GetStringWidth(RString). The real method takes a decoded
// wide string and reports SOURCE pixels (pre-zoom) — Font.h:155
int width = pFont->GetLineWidthInSourcePixels(L"Hello");
int height = pFont->GetHeight();              // Font.h:179

FONT->UnloadFont(pFont);                      // FontManager.h:17
```

In practice, text is almost never driven through `Font` directly — themes use
`BitmapText`, which owns its own `Font*` internally:

```cpp
BitmapText *pText = new BitmapText();          // BitmapText.h:11
pText->LoadFromFont("Common Normal");           // BitmapText.h:57
pText->SetText("Hello");                        // BitmapText.h:59
pText->SetXY(SCREEN_CENTER_X, SCREEN_CENTER_Y); // Actor.h:340
```

### Setting actor position, zoom, and color — corrected names

```cpp
// Position — SetXYZ() does not exist
actor->SetXY(x, y);              // Actor.h:340
actor->SetZ(z);                  // Actor.h:339

// Rotation — this part of the previous doc was correct
actor->SetRotationZ(angle_degrees); // Actor.h:426

// Size is "Zoom", not "Scale" — GetScaleX/SetScaleX do not exist
actor->SetZoom(1.5f);             // Actor.h:399 — uniform x/y/z
actor->SetZoomX(1.5f);            // Actor.h:408
actor->SetZoomY(1.5f);            // Actor.h:412

// Color — SetAlpha() does not exist; it's SetDiffuseAlpha()
actor->SetDiffuse(RageColor(1, 1, 1, 1));  // Actor.h:457
actor->SetDiffuseAlpha(0.8f);               // Actor.h:458 — NOT SetAlpha()
actor->SetGlow(RageColor(0.5f, 0.5f, 0.5f, 1)); // Actor.h:473

// Blend mode — real enum values (RageTypes.h:8-23). BLEND_SCREEN and
// BLEND_MULTIPLY do NOT exist.
actor->SetBlendMode(BLEND_ADD);   // Actor.h:582
```

### Effects — there is no generic `SetEffectMode()`

The previous doc's `actor->SetEffectMode(Actor::bob); actor->SetEffectMagnitude(10.0f);`
does not compile: `SetEffectMagnitude()` takes a `RageVector3`, not a `float`,
and setting an effect means calling its specific setter, which both selects and
configures it in one call:

```cpp
// SetEffectBob(period, magnitude-as-a-vector) — Actor.h:556
actor->SetEffectBob(2.0f, RageVector3(0, 10, 0));   // bob 10px vertically, 2s period

// Or, e.g., a diffuse color blink between two colors:
actor->SetEffectDiffuseBlink(0.5f, RageColor(1,1,1,1), RageColor(1,0,0,1)); // Actor.h:547

actor->StopEffect();               // Actor.h:524 — clears back to no_effect
```

### Animation & tweening — corrected `TweenType` values

```cpp
// TweenType enum (Tween.h:10-19): TWEEN_LINEAR, TWEEN_ACCELERATE,
// TWEEN_DECELERATE, TWEEN_SPRING, TWEEN_BEZIER. There is no TWEEN_EASE_OUT —
// the closest real value is TWEEN_DECELERATE.
actor->BeginTweening(1.0f, TWEEN_LINEAR);        // Actor.h:480
actor->SetDiffuse(RageColor(1, 0, 0, 1));

actor->BeginTweening(1.0f, TWEEN_DECELERATE);     // "ease out"-like
actor->SetDiffuseAlpha(0.0f);                      // fade out — not SetAlpha()
```

### Grouping actors (ActorFrame) — corrected accessor names

```cpp
ActorFrame *pGroup = new ActorFrame();
pGroup->SetName("MyGroup");
pGroup->SetXY(200, 200);
pGroup->SetRotationZ(45);           // rotates all children as a group

Sprite *pChild1 = new Sprite();
pChild1->SetXY(10, 0);
pGroup->AddChild(pChild1);          // ActorFrame.h:22

// Real accessors — there is no GetChildAtIndex() or recursive FindActor()
int n = pGroup->GetNumChildren();         // ActorFrame.h:30 — returns int
Actor *found = pGroup->GetChild("Name");  // ActorFrame.h:28 — exact-name lookup, this frame only
vector<Actor*> kids = pGroup->GetChildren(); // ActorFrame.h:29 — returns a COPY
```

### Render-to-texture — corrected: no public `Create(param)`/`SetRenderTarget()`

```cpp
// ActorFrameTexture's real API configures flags then calls Create() —
// ActorFrameTexture.h:23-37
ActorFrameTexture *pRT = new ActorFrameTexture();
pRT->SetTextureName("MyRenderTarget");
pRT->EnableAlphaBuffer(true);
pRT->Create();

pRT->AddChild(someActor);   // inherited from ActorFrame — draws into the render target

// To use the result as a texture elsewhere:
sprite->SetTexture(pRT->GetTexture());  // ActorFrameTexture.h:28 — returns RageTextureRenderTarget*
```

### Message handling — corrected: no `AddMessageSubscriber`/`RemoveMessageSubscriber`

```cpp
// Actor's real message entry point (Actor.h:612):
class MyActor : public Actor
{
    virtual void HandleMessage( const Message &msg )
    {
        if( msg.GetName() == "ScreenChanged" )
        {
            // handle it
        }
    }
};

// Subscribing comes from MessageSubscriber, which Actor inherits
// (MessageManager.h:176-179) — there is no AddMessageSubscriber() on Actor:
actor->SubscribeToMessage("ScreenChanged");

// PlayCommand is a convenience that just routes through HandleMessage:
actor->PlayCommand("MyCommand");   // Actor.h:601
```

## Key constants — verified

### Draw order layers (Actor.h:19-31) — unchanged from before, confirmed correct

```cpp
DRAW_ORDER_BEFORE_EVERYTHING = -200
DRAW_ORDER_UNDERLAY          = -100
DRAW_ORDER_DECORATIONS       =    0
DRAW_ORDER_OVERLAY           = +100
DRAW_ORDER_TRANSITIONS       = +200
DRAW_ORDER_AFTER_EVERYTHING  = +300
```

### Blend modes (RageTypes.h:8-23) — corrected list

```cpp
BLEND_NORMAL, BLEND_ADD, BLEND_SUBTRACT, BLEND_MODULATE, BLEND_COPY_SRC,
BLEND_ALPHA_MASK, BLEND_ALPHA_KNOCK_OUT, BLEND_ALPHA_MULTIPLY,
BLEND_WEIGHTED_MULTIPLY, BLEND_INVERT_DEST, BLEND_NO_EFFECT
```
`BLEND_SCREEN` and `BLEND_MULTIPLY` do **not** exist.

### Effect types (Actor.h:129-133) — confirmed correct as previously listed

```cpp
no_effect, diffuse_blink, diffuse_shift, diffuse_ramp,
glow_blink, glow_shift, glow_ramp, rainbow,
wag, bounce, bob, pulse, spin, vibrate
```
But there is no generic setter — see "Effects" above.

### Effect clock sources (Actor.h:136-149) — confirmed, with two previously-omitted values

```cpp
CLOCK_TIMER, CLOCK_TIMER_GLOBAL, CLOCK_BGM_TIME, CLOCK_BGM_BEAT,
CLOCK_BGM_TIME_NO_OFFSET, CLOCK_BGM_BEAT_NO_OFFSET,
CLOCK_BGM_BEAT_PLAYER1, CLOCK_BGM_BEAT_PLAYER2,
CLOCK_LIGHT_1 = 1000, CLOCK_LIGHT_LAST = 1100
```

### Tween types (Tween.h:10-19) — corrected

```cpp
TWEEN_LINEAR, TWEEN_ACCELERATE, TWEEN_DECELERATE, TWEEN_SPRING, TWEEN_BEZIER
```
No `TWEEN_EASE_OUT`, `TWEEN_EASE_IN`, etc.

### Texture units (RageDisplay.h:15-22) — confirmed correct

```cpp
TextureUnit_1, TextureUnit_2, TextureUnit_3, TextureUnit_4
```

### Alignment constants (Actor.h:34-66) — confirmed correct

```cpp
HorizAlign_Left, HorizAlign_Center, HorizAlign_Right
VertAlign_Top, VertAlign_Middle, VertAlign_Bottom
align_left=0.0f, align_center=0.5f, align_right=1.0f
align_top=0.0f, align_middle=0.5f, align_bottom=1.0f
```

## Singleton globals — verified names

```cpp
DISPLAY      // RageDisplay*        — RageDisplay.h:466
TEXTUREMAN   // RageTextureManager* — RageTextureManager.h:101
FONT         // FontManager*        — FontManager.h:21 — NOT "FONTMAN"
MESSAGEMAN   // MessageManager*     — MessageManager.h:209
GAMESTATE    // GameState*          — GameState.h:459
FILEMAN      // RageFileManager*    — RageFileManager.h:84
SCREENMAN    // ScreenManager*      — ScreenManager.h:125
```

## Architecture layer quick lookup (verified base classes)

| Layer | Key classes | Base confirmed at |
|-------|------------|---|
| Application | `Screen` subclasses | `Screen : public ActorFrame`, `Screen.h:41` |
| Scene graph | `Actor`, `ActorFrame`, `Sprite`, `Model`, `BitmapText` | `Actor.h:105`, `ActorFrame.h:7`, `Sprite.h:11`, `Model.h:15`, `BitmapText.h:11` |
| Rendering | `RageDisplay` | `RageDisplay.h:215` |
| Graphics API | `RageDisplay_Legacy` (OGL), `RageDisplay_D3D`, `RageDisplay_GLES2`, `RageDisplay_Null` | see backend header citations above |
| Resources | `RageTexture`, `Font`, `RageModelGeometry` | `RageTexture.h:10`, `Font.h:144`, `RageModelGeometry.h:12` |
| Management | `RageTextureManager`, `FontManager` | `RageTextureManager.h:48`, `FontManager.h:9` |

---

**Scope**: Every example cross-checked against a specific header line. No claim
in this file is speculative; anything not directly confirmed from a header was
omitted rather than guessed.
