# StepMania Graphics & Rendering Architecture Documentation

Reference documentation for the rendering pipeline architecture in `stepmania/src`.

## ⚠️ Verification pass (read this first)

The original version of this documentation set was written without reading most
of the actual source files, and contained extensive fabricated APIs: invented
method signatures, wrong base classes, wrong singleton names, and an entire
imagined FreeType/glyph-atlas font pipeline that does not exist anywhere in this
codebase. Every document was subsequently rewritten against direct reads of the
real headers (and key `.cpp` call sites), with `file:line` citations for every
non-trivial claim. See **[INDEX.md](INDEX.md) → "Real vs. previously-claimed
corrections"** for a table of the most consequential fixes, for example:

- The OpenGL backend class is `RageDisplay_Legacy`, not `RageDisplay_OGL`
  (`RageDisplay_OGL.h:32`).
- `Quad` extends `Sprite`, not `Actor` (`Quad.h:7`).
- The font-manager global is `FONT`, not `FONTMAN` (`FontManager.h:21`).
- There is no FreeType/live glyph rasterization/kerning in this codebase —
  fonts are pre-made bitmap sprite sheets (`Font.h`, `FontCharmaps.h`).
- `RageDisplay::SetTexture()` takes a raw `uintptr_t` handle, not a
  `RageTexture*` (`RageDisplay.h:280`).

Any claim in these docs that could not be directly confirmed from a header read
in this pass is explicitly flagged as such in the relevant document rather than
stated as fact.

## Navigation Guide

1. **[INDEX.md](INDEX.md)** — Start here. Verified file/class map, real class
   hierarchy, and the corrections table referenced above.

2. **[01-RageDisplay.md](01-RageDisplay.md)** — `RageDisplay` interface, read in
   full (492 lines). Every method grouped by what it actually does, with the
   concrete-vs-pure-virtual distinction called out (most of the "drawing" and
   "matrix stack" API is concrete/non-virtual, contrary to the earlier draft).

3. **[02-ActorSystem.md](02-ActorSystem.md)** — `Actor`/`ActorFrame` read in
   full, plus `Sprite`, `Model`, `Quad`, `ActorMultiVertex`, `ActorScroller`,
   `ActorFrameTexture`, `ActorMultiTexture` read in full. Real `Draw()` call
   sequence traced from `Actor.cpp:376-487`.

4. **[03-TextureSystem.md](03-TextureSystem.md)** — `RageTexture`,
   `RageTextureID`, `RageTextureManager`, `RageBitmapTexture`,
   `RageTextureRenderTarget`, `RageTexturePreloader` all read in full. Corrects
   the previous doc's invented async-preloading design (`RageTexturePreloader`
   is synchronous).

5. **[04-FontSystem.md](04-FontSystem.md)** — `Font`, `FontManager`,
   `FontCharmaps`, `FontCharAliases`, `BitmapText` all read in full. Replaces
   the fabricated FreeType/atlas pipeline with the real bitmap-sheet system.

6. **[05-GeometryAndPrimitives.md](05-GeometryAndPrimitives.md)** —
   `RageModelGeometry` and `RageMath` read in full; vertex/mesh/material structs
   read from `RageTypes.h`/`ModelTypes.h`. Corrects the invented OOP math API —
   `RageMath.h` is entirely free functions with output pointers.

7. **[06-ArchitectureDiagrams.md](06-ArchitectureDiagrams.md)** — Every diagram
   now cites either a header line range or a traced `.cpp` call chain
   (`ScreenManager::Draw()`, `Actor::Draw()`). Generic, uncited diagrams from
   the previous version were dropped rather than kept as filler.

8. **[07-QuickReference.md](07-QuickReference.md)** — Every code snippet
   rewritten to use only confirmed real signatures; each corrected example
   states what the previous, non-compiling version got wrong.

9. **[08-Deko3D-Feasibility.md](08-Deko3D-Feasibility.md)** — Feasibility
   research for a deko3d `RageDisplay` backend to fix poor Switch performance.
   Confirms the current Switch build renders through Mesa/nouveau (not native
   hardware access), scopes what a deko3d backend would actually require
   (no fixed-function pipeline, no existing shader set to port from, manual
   GPU memory/command-buffer management), and lays out the concrete
   integration points and a phased de-risking plan.

10. **[09-Deko3D-SpritePipelineCache.md](09-Deko3D-SpritePipelineCache.md)** —
    Phase-1 design for drawing `Sprite`/`Quad` under deko3d. **Corrected**
    after reading the real deko3d SDK headers (found already installed
    locally at `/opt/devkitpro/libnx/include/`, no GitHub clone needed): the
    original design assumed a monolithic Vulkan-1.0-style pipeline object;
    the real SDK has no `Pipeline` type at all — every fixed-function state
    category binds independently, GL/D3D11-style. The only real cache needed
    is a small, fixed shader lookup array; everything else is direct,
    cheap state translation. **§9 further validates the whole design against
    devkitPro's official example projects** (also installed locally) —
    resolving the remaining `uam`/per-frame-command-buffer open questions,
    fixing a missed blend-enable-bit requirement, softening an overclaim
    about vertex-layout state persistence, and surfacing two previously
    unscoped requirements (docked/handheld resolution switching, and an
    explicit texture-format decision). **§10 spot-checks Phase 2** (`Model`
    lighting) against `Example07_MeshLighting.cpp` — confirms Blinn-Phong is
    the right technique, but finds the example bakes Material×Light into one
    fixed response, while StepMania's `SetMaterial()`/`SetLightDirectional()`
    are independently-settable and need to be multiplied together in-shader.
    It also checks `Example08_DeferredShading.cpp` and correctly rules it
    out — deferred shading only pays off with many lights, and this codebase
    is confirmed to use exactly one, ever. **§8 covers every remaining
    example**, and surfaces a genuinely valuable find in `deko_console`:
    rendering many quads as one instanced draw call, updated by writing
    directly into GPU memory with no per-frame command re-recording — not
    part of Phase 1's baseline, but a concrete future optimization directly
    applicable to `BitmapText` glyph rendering.

11. **[10-Deko3D-MemoryAllocator.md](10-Deko3D-MemoryAllocator.md)** — Scopes
    the actual blocking prerequisite for Phase 1: GPU memory sub-allocation.
    Identifies five distinct pools (shader code, texture storage, upload
    staging, per-frame ring-buffered data, descriptor tables), each needing
    a different allocation strategy for reasons grounded in `deko3d.h`'s
    per-block flag constraint and the official SDK's own upload code
    (`CExternalImage.cpp`'s confirmed staging→copy→final-image pattern).
    Resolves port-vs-reimplement for the SDK's own allocator (`CMemPool`)
    against this engine's actual constraints, not just its license: no
    existing pool/allocator code anywhere in this codebase, confirmed
    single-threaded texture loading, and — decisively — a confirmed
    `gnu++11` C++ standard on the Switch build (`StepmaniaCore.cmake:23-25`)
    that the SDK's C++17-flavored example code won't compile under.
    Verdict: fresh C++11 code matching this engine's existing idiom, using
    the SDK examples' proven *architecture* (free-list+coalescing,
    N-slice ring) rather than their literal implementation. Flags real
    remaining open items (pool sizing, descriptor-table overflow handling)
    that need actual profiling or implementation-time decisions.

12. **[11-Deko3D-TextureFormatAndShaderContract.md](11-Deko3D-TextureFormatAndShaderContract.md)** —
    Resolves the two remaining Phase 1 blockers, compatibility-first per
    direction given: never refuse a texture format, convert in software and
    log a warning instead. Found that `RageBitmapTexture.cpp` and
    `RageDisplay_OGL.cpp`'s existing `GetImgPixelFormat()` **already
    implement almost exactly this pattern** (down to a comment saying "very
    slow path, should almost never be used") — it just needed the warning
    promoted from a source comment to a real `LOG->Warn()` call. Cross-
    referenced StepMania's `RagePixelFormat` enum against `DkImageFormat` and
    found 5 of 9 formats map natively (better than assumed). Also nails down
    the exact shader binding contract (attribute locations, uniform/texture
    binding numbers) and catches a real gotcha: `RageVColor` stores bytes as
    B,G,R,A, not R,G,B,A — missing this would have silently swapped red and
    blue on every sprite.

13. **[12-Deko3D-Phase1-Implementation-Status.md](12-Deko3D-Phase1-Implementation-Status.md)** —
    Real code, not design. `Deko3DMemPool.h/.cpp`, `RageDisplay_Deko3D.h/.cpp`,
    three `.glsl` shaders, and the full CMake/`StepMania.cpp` wiring were
    written and **actually compiled against this project's real Switch
    toolchain** (`aarch64-none-elf-g++`, devkitPro's `deko3d`/`libnx` SDKs) —
    every claim of "compiles clean" in that document was verified by running
    the build, not by inspection. Documents six real bugs (wrong method
    names, a CMake target-ordering error, and one genuine frame-lifecycle
    bug where the swapchain image was being bound after the frame's draw
    calls instead of before) that were only caught this way.

## Scope

All documentation covers files **under `stepmania/src` only**. Out of scope:
external libraries, platform build tooling, theme/resource content, and
gameplay logic. `.cpp` implementation files were read only where directly
needed to confirm a call sequence (`Actor.cpp`, `ScreenManager.cpp`); most
`.cpp` bodies were **not** read, and documents say so explicitly wherever a
claim would otherwise imply `.cpp`-level knowledge.

## Verified real class hierarchy (condensed — full version in INDEX.md)

```
Actor (Actor.h:105)
  ├─ ActorFrame (ActorFrame.h:7)
  │    ├─ Screen (Screen.h:41)
  │    ├─ ActorFrameTexture (ActorFrameTexture.h:7)
  │    └─ ActorScroller (ActorScroller.h:9)
  ├─ Sprite (Sprite.h:11)
  │    └─ Quad (Quad.h:7)                 ← corrected: not a direct Actor subclass
  ├─ Model (Model.h:15)
  ├─ ActorMultiVertex / ActorMultiTexture
  └─ BitmapText (BitmapText.h:11)

RageDisplay (RageDisplay.h:215)
  ├─ RageDisplay_Legacy  (OpenGL — misleadingly kept in file RageDisplay_OGL.h)
  ├─ RageDisplay_D3D
  ├─ RageDisplay_GLES2
  └─ RageDisplay_Null

RageTexture (RageTexture.h:10)
  ├─ RageBitmapTexture
  ├─ RageTextureRenderTarget
  └─ RageMovieTexture (arch/MovieTexture/MovieTexture.h:10)  ← corrected name
       ├─ MovieTexture_DShow
       └─ MovieTexture_Generic → MovieTexture_FFMpeg
```

## Key Concepts (verified)

### 1. Actor composite pattern
`Actor` is the component, `ActorFrame` is the composite holding
`vector<Actor*> m_SubActors` (`ActorFrame.h:102`). Confirmed correct.

### 2. Display strategy pattern
`RageDisplay` is the abstract interface; `RageDisplay_Legacy`, `RageDisplay_D3D`,
`RageDisplay_GLES2`, `RageDisplay_Null` are the strategies (all confirmed
`: public RageDisplay`). The naming was wrong (`RageDisplay_OGL` doesn't exist
as a class name) but the pattern itself is real.

### 3. Resource manager pattern
`RageTextureManager` and `FontManager` cache by ID and use a public
`int m_iRefCount` field on the resource object itself (`RageTexture.h:58`,
`Font.h:147`) rather than a private refcount with accessor methods.

### 4. Matrix stack management
Confirmed real, but the stack operations are concrete (non-virtual) methods on
`RageDisplay` itself (`RageDisplay.h:402-433`), not virtual hooks each backend
overrides.

### 5. Message passing
`Actor` inherits `MessageSubscriber` (`MessageManager.h:166`), whose real API is
`SubscribeToMessage()`/`UnsubscribeAll()` — not `AddMessageSubscriber()`.

## Common Tasks

### Finding something
- **Where is RageDisplay?** → [INDEX.md](INDEX.md) → Files table
- **How does actor animation work?** → [02-ActorSystem.md](02-ActorSystem.md) → "Tweening"
- **What's in a texture?** → [03-TextureSystem.md](03-TextureSystem.md) → "RageTexture — abstract base"
- **How do I render text?** → [04-FontSystem.md](04-FontSystem.md) → "Verified rendering flow"
- **Show me a diagram** → [06-ArchitectureDiagrams.md](06-ArchitectureDiagrams.md)
- **I need example code** → [07-QuickReference.md](07-QuickReference.md)

### Understanding a concept
- **What's the rendering pipeline?** → [01-RageDisplay.md](01-RageDisplay.md) + [06-ArchitectureDiagrams.md](06-ArchitectureDiagrams.md)
- **How does texturing work?** → [03-TextureSystem.md](03-TextureSystem.md)
- **What about 3D models?** → [05-GeometryAndPrimitives.md](05-GeometryAndPrimitives.md) + [02-ActorSystem.md](02-ActorSystem.md) ("Model" section)
- **How are actors organized?** → [02-ActorSystem.md](02-ActorSystem.md) + [06-ArchitectureDiagrams.md](06-ArchitectureDiagrams.md) ("Actor tree")

## File → Class mapping (corrected)

| Layer | Class | File | Base (verified) |
|-------|-------|------|------|
| Graphics API | `RageDisplay` | RageDisplay.h | — |
| Graphics API | `RageDisplay_Legacy` | RageDisplay_OGL.h | `RageDisplay` (RageDisplay_OGL.h:32) |
| Graphics API | `RageCompiledGeometry` | RageDisplay.h | — (nested in RageDisplay.h, not its own file) |
| Scene Graph | `Actor` | Actor.h | `MessageSubscriber` |
| Scene Graph | `ActorFrame` | ActorFrame.h | `Actor` |
| Scene Graph | `Sprite` | Sprite.h | `Actor` |
| Scene Graph | `Quad` | Quad.h | `Sprite` |
| Scene Graph | `Model` | Model.h | `Actor` |
| Scene Graph | `BitmapText` | BitmapText.h | `Actor` |
| Resources | `RageTexture` | RageTexture.h | — |
| Resources | `RageTextureID` | RageTextureID.h | — (plain struct) |
| Management | `RageTextureManager` | RageTextureManager.h | — |
| Management | `FontManager` (global `FONT`) | FontManager.h | — |
| Font | `Font` | Font.h | — |
| Geometry | `RageModelGeometry` | RageModelGeometry.h | — |
| Math | (free functions) | RageMath.h | — (no class) |

## Notes for developers

- Class names do **not** always match file names — `RageDisplay_OGL.h` holds
  `RageDisplay_Legacy`; `MovieTexture.h` holds `RageMovieTexture`. Always check
  the actual `class` declaration, not the filename.
- Headers define the interface; most `.cpp` bodies in this codebase were not
  read for this documentation pass — treat any `.cpp`-level behavioral claim in
  these docs as explicitly flagged, not assumed.
- Public data fields are common in this codebase (`RageTexture::m_iRefCount`,
  `RageModelGeometry::m_Meshes`, `Font::m_iRefCount`) — don't assume an
  encapsulated getter/setter exists just because one would be idiomatic
  elsewhere.

---

**Scope**: `stepmania/src` rendering architecture only.
**Verification method**: `tgrep` + full-file `Read` of every header cited;
select `.cpp` call chains traced where noted.
**Last updated**: 2026-09-20 (verification pass).
