# deko3d `RageDisplay` Backend — Feasibility Research

Scope of this pass: `stepmania/src` (RageDisplay abstraction + backend headers),
`stepmania/src/CMakeLists.txt`, `build.sh`, and `nxshim/` (the existing Switch
GL shim layer). No deko3d code was written — this is a research/scoping pass
only, per request. All claims below cite a file:line or are explicitly marked
as external/domain knowledge not verifiable from this repo.

## Bottom line

**Feasible, and worth doing — but it is not "just write a driver."** It's a
from-scratch, explicit-API renderer (closer to writing a Vulkan backend than
adapting an existing GL backend), because `RageDisplay`'s method surface is
modeled on GL/D3D9-style *global mutable state* (`SetBlendMode`, `SetTexture`,
`SetMaterial`, ad-hoc draw calls), while deko3d has no fixed-function pipeline,
no implicit state, and no runtime shader compilation. The `RageDisplay`
abstraction boundary itself is the right place to add it — that part is
genuinely clean — but the amount of *new* code needed is roughly on par with
`RageDisplay_GLES2.cpp` (1004 lines) plus an entire hand-written shader set
that doesn't currently exist anywhere in this codebase to port from.

## 1. Confirmed root cause of the current performance problem

`stepmania/src/CMakeLists.txt:650-669` links the Switch build against:

```cmake
if(SWITCH_LIBNX)
  list(APPEND SMDATA_LINK_LIB
    "${CMAKE_BINARY_DIR}/nxshim/libglad.a"
    "${CMAKE_BINARY_DIR}/nxshim/libnxshim.a"
    "SDL2" "SDL2_ttf" "freetype" "harfbuzz" "bz2" "z" "opus" "vpx"
    "swresample" "png"
    "EGL" "glapi" "drm_nouveau" "nx"
  )
endif()
```

`drm_nouveau` + `EGL` + `glapi` is devkitPro's **Mesa/nouveau** OpenGL stack —
an open-source, reverse-engineered driver for the Tegra X1's GPU, running
through a full GL state-tracker/translation layer. `build.sh:26,44` confirms
the renderer selected is desktop-style GL, not GLES2 (`-DHAVE_EGL`,
`-DWITH_GLES2=0`), so `StepMania.cpp:34-35`'s default (`SUPPORT_OPENGL` when
neither `SUPPORT_OPENGL` nor `SUPPORT_D3D` is predefined) selects
`RageDisplay_Legacy` (`StepMania.cpp:821`, real class name per
`01-RageDisplay.md`).

This is a well-documented bottleneck in the Switch homebrew scene: nouveau on
Switch does not have access to the same firmware-validated fast paths the
proprietary NVN driver (and deko3d, which is built to the same low-level
submission model) gets, has incomplete/emulated feature and texture-format
support, and pays full GL state-tracking + shader-recompilation overhead on
top. This part of the "why is performance poor" question is domain knowledge
about the devkitPro/nouveau stack, not something verifiable from this repo,
but it is the standard, widely-corroborated explanation and lines up exactly
with what's actually linked here.

deko3d, by contrast, talks to the same underlying command-submission interface
as the system's own NVN driver, with none of the GL translation overhead —
it's the correct target to chase for a real performance fix, not a lateral move.

## 2. How backend selection actually works (verified — this part is easy)

Confirmed in `01-RageDisplay.md`/`INDEX.md` and re-checked here:
`StepMania.cpp:790-844` is a flat, one-time `#if defined(SUPPORT_X)` /
string-compare chain, **not** a plugin registry (unlike the `RageDriver`
factory pattern used by `arch/MovieTexture/`, per `INDEX.md`'s movie-texture
section):

```cpp
RageDisplay *pRet = nullptr;
for (each entry in PREFSMAN->m_sVideoRenderers, comma-separated) {
    if (sRenderer == "opengl")  { #if SUPPORT_OPENGL  pRet = new RageDisplay_Legacy; #endif }
    else if (sRenderer == "gles2") { #if SUPPORT_GLES2 pRet = new RageDisplay_GLES2; #endif }
    else if (sRenderer == "d3d")   { #if SUPPORT_D3D   pRet = new RageDisplay_D3D;   #endif }
    else if (sRenderer == "null")  { return new RageDisplay_Null; }
    else RageException::Throw(...);
}
```
(`StepMania.cpp:813-844`)

Adding deko3d here is mechanical: a new `else if (sRenderer.CompareNoCase("deko3d")==0) { #if defined(SUPPORT_DEKO3D) pRet = new RageDisplay_Deko3D; #endif }`
branch, a new `#include "RageDisplay_Deko3D.h"` guarded by `#if
defined(SUPPORT_DEKO3D)` (mirroring `StepMania.cpp:469-475`), a new
`SUPPORT_DEKO3D` compile definition wired in for the Switch target only (in
`build.sh`/`CMakeLists.txt`, mirroring how `WITH_GLES2`/`SWITCH_LIBNX` are
threaded through today), and a new source-file entry + new link libraries
(`deko3d`, likely `stb`/whatever `uam`-compiled-shader loader is used) in
`stepmania/src/CMakeLists.txt`'s existing `if(SWITCH_LIBNX)` block
(`:650-669`). None of this is architecturally risky — it's the same shape as
the three backends already there.

**LowLevelWindow does not need to be touched or implemented.**
`arch/LowLevelWindow/LowLevelWindow.h:12` says outright: *"Handle low-level
operations that OGL 1.x doesn't give us"* — its interface (`GetProcAddress`,
`SwapBuffers`, `TryVideoMode`) is GL-context-and-swap-chain shaped, and both
`RageDisplay_Legacy` and `RageDisplay_GLES2` depend on it for exactly that
reason (`RageDisplay_GLES2.cpp:16` includes it directly). deko3d manages its
own swapchain (`DkSwapchain`) against `nwindowGetDefault()` from libnx —
completely outside this abstraction. A deko3d backend would do what
`RageDisplay_D3D` **already confirms** is a safe, precedented pattern in
this exact codebase (D3D9 also has its own device/swapchain model) and
manage device/queue/swapchain setup entirely inside its own `Init()`/
`TryVideoMode()`, never touching `LowLevelWindow`.

**Resolved — no SDL2/deko3d conflict risk.** This was flagged as an open
integration question and has since been checked directly. `arch/arch_default.h:24-26`
confirms Switch exclusively uses `LowLevelWindow_SDL` (`#elif
defined(__SWITCH__) ... #include "LowLevelWindow/LowLevelWindow_SDL.h"`),
whose `TryVideoMode()` calls `SDL_CreateWindow(..., SDL_WINDOW_OPENGL)` +
`SDL_GL_CreateContext()` (`LowLevelWindow_SDL.cpp:106,113`). But searching
every call site that actually constructs a `LowLevelWindow`
(`grep -rn 'LowLevelWindow::Create'`) finds exactly two: inside
`RageDisplay_Legacy::Init()` (`RageDisplay_OGL.cpp:476`) and
`RageDisplay_GLES2::Init()` (`RageDisplay_GLES2.cpp:229`) — nowhere else.
Confirmed zero references to `LowLevelWindow` or `SDL_CreateWindow` anywhere
in `RageDisplay_D3D.cpp` either. Since exactly one `RageDisplay` subclass is
ever constructed per session (`StepMania.cpp:813-865`), if `"deko3d"` wins
that selection, `RageDisplay_Legacy`/`RageDisplay_GLES2::Init()` never run,
so `SDL_CreateWindow`'s `SDL_WINDOW_OPENGL` path never executes — there is
nothing for a deko3d-owned `nwindowGetDefault()`/`DkSwapchain` to conflict
with. SDL2 remains available (and presumably still used) for input handling
independent of this — that's a separate subsystem from `LowLevelWindow`,
unaffected by which `RageDisplay` backend is active.

## 3. What actually has to be implemented — the real scope

Every `RageDisplay_GLES2`-equivalent backend must implement the full pure/
overridable-virtual surface of `RageDisplay` (`01-RageDisplay.md`, ~30 public
methods + 8 protected `...Internal()` draw methods + `TryVideoMode` +
`CreateScreenshot` + `GetOrthoMatrix`). Confirmed against
`RageDisplay_GLES2.h` (112 lines, full method list) as the smallest complete
real-world reference: texture create/update/delete, all 7 draw-primitive
`...Internal()` methods, Z-test/Z-write/cull/alpha-test state, **and full
fixed-function material/lighting** (`SetMaterial`, `SetLighting`,
`SetLightDirectional`, `SetSphereEnvironmentMapping`, `SetCelShaded`).

That last group is the expensive part, and it is **not decorative** — it is
exercised by real gameplay code. Confirmed callers of
`SetMaterial`/`SetLighting`/`SetCelShaded`: `Model.cpp`, `DancingCharacters.cpp`,
`BeginnerHelper.cpp`, `ActorFrame.cpp` (search of `stepmania/src/*.cpp`). So a
deko3d backend can't just blit textured 2D sprites (NoteField, banners, UI) —
it also has to correctly light and cel-shade 3D character/`Model` geometry
(`Model.h`, `02-ActorSystem.md`), which under a real fixed-function-free API
means writing actual Blinn-Phong-equivalent vertex/fragment shaders, not
toggling `glEnable(GL_LIGHTING)`.

### There is no existing shader set to port from

This is the single biggest scope surprise from this pass. `RageDisplay_GLES2`
sounds like it should be the template — GLES2 has no fixed-function pipeline
either, so in principle it already had to solve "how do we do
`SetMaterial`/`SetLighting`/texture-combine without fixed function." But:

- `RageDisplay_GLES2.cpp` contains **zero** calls to
  `glCreateShader`/`glShaderSource`/`glCompileShader`/`glLinkProgram`
  (checked directly — no matches in the file).
- It `#include <GL/glew.h>` (`RageDisplay_GLES2.cpp:18`) — the **desktop** GL
  loader, not a GLES2 header — and its pixel-format table
  (`RageDisplay_GLES2.cpp:26-60`) is identical in shape to what a desktop GL
  backend would use.

In other words, this backend does not appear to actually be a working,
shader-based ES2 renderer as shipped — there's no fixed-function-emulation
shader code anywhere in the tree for a deko3d backend to adapt. **The entire
vertex/fragment shader set (textured-quad, vertex-color modulate, glow-add,
Blinn-Phong lighting for `Model`, cel-shade outline pass) has to be authored
from scratch**, in whatever shading language `uam` (deko3d's offline shader
compiler) accepts, then compiled to `.dksh` binaries as a **build-time step**,
then loaded via `DkShader` at runtime. This is new tooling in the build
pipeline (`build.sh`/`CMakeLists.txt`), not just new `.cpp` files.

### The state model — corrected after reading the real deko3d SDK headers

**Update**: the paragraph originally here assumed deko3d bakes blend/cull/
depth/shader together into one immutable pipeline object, the way core
Vulkan 1.0 does. That assumption has since been checked against the real
`deko3d.h`/`deko3d.hpp` (devkitPro `deko3d` package 0.5.0, installed locally
at `/opt/devkitpro/libnx/include/`) and **it's wrong — deko3d has no pipeline
object at all.** There is no `DkPipeline` type anywhere in the SDK (the full
opaque-type list is `Device, MemBlock, Fence, Variable, CmdBuf, Queue,
Shader, ImageLayout, Image, ImageDescriptor, SamplerDescriptor, Swapchain`,
`deko3d.h:74-85`). Instead, every fixed-function state category is bound
**independently and directly onto the command buffer**, confirmed from the
full `dkCmdBufBind*`/`dkCmdBufSet*` list (`deko3d.h:1260-1322`):

```c
void dkCmdBufBindShaders(DkCmdBuf, uint32_t stageMask, DkShader const* const[], uint32_t);
void dkCmdBufBindRasterizerState(DkCmdBuf, DkRasterizerState const*);      // cull mode, polygon mode
void dkCmdBufBindColorState(DkCmdBuf, DkColorState const*);
void dkCmdBufBindBlendStates(DkCmdBuf, uint32_t firstId, DkBlendState const[], uint32_t);
void dkCmdBufBindDepthStencilState(DkCmdBuf, DkDepthStencilState const*);   // z-test, z-write
void dkCmdBufBindVtxAttribState(DkCmdBuf, DkVtxAttribState const[], uint32_t);
void dkCmdBufBindTextures(DkCmdBuf, DkStage, uint32_t firstId, DkResHandle const[], uint32_t);
```

This is architecturally much closer to classic GL/D3D11 "set a state object,
then draw" than to Vulkan/D3D12's monolithic Pipeline State Object model. The
practical consequence is a significant **downward** revision of scope:

- `RageDisplay::SetBlendMode(BlendMode)` → fill a `DkBlendState` struct
  (`deko3d.h:967-976`, plain POD: `colorBlendOp`, `srcColorBlendFactor`,
  `dstColorBlendFactor`, `alphaBlendOp`, `srcAlphaBlendFactor`,
  `dstAlphaBlendFactor`) and call `dkCmdBufBindBlendStates()`. No object
  creation, no "pipeline" lookup — it's a cheap translate-and-bind, the same
  shape as the GL backend's own `glBlendFunc()` call.
- `SetCullMode` → one field (`DkFace cullMode`) inside `DkRasterizerState`
  (`deko3d.h:794-806`), bound via `dkCmdBufBindRasterizerState()`. Same story.
- `SetZTestMode`/`SetZWrite` → `DkDepthStencilState` (`deko3d.h:1015-1032`),
  bound via `dkCmdBufBindDepthStencilState()`. Same story.
- **The one thing that genuinely needs a real cache is shaders**
  (`TextureMode`/`EffectMode` — see `09-Deko3D-SpritePipelineCache.md`), since
  those are actual precompiled GPU programs (`DkShader`, `deko3d.h:293-307`,
  loaded from a `DkMemBlock`), not translatable POD structs. But there's no
  runtime shader *compilation* to cache against either — `uam` already did
  that offline — so this is "hold a small, fixed array of already-loaded
  `DkShader` objects and pick the right one," not a lazily-growing cache with
  an eviction question.

So **there is no "pipeline cache" data structure to design** in the sense
originally scoped. What remains genuinely new work:

- **No implicit "current texture unit."** Textures are combined with a
  sampler into one `DkResHandle` via `dkMakeTextureHandle(imageId,
  samplerId)` (`deko3d.h:715-718`) and bound via `dkCmdBufBindTextures()`.
  `SetTexture(TextureUnit, uintptr_t)` (`RageDisplay.h:280`) has to resolve
  to building/reusing that handle, not an immediate GL-style bind — real
  work, but small and mechanical, and it maps directly onto the existing
  `RageTexture`/`RageTextureManager` object lifecycle (create the image
  descriptor once, in `CreateTexture()`).
- **All GPU memory is manually managed.** `DkMemBlock` allocation/
  sub-allocation for vertex buffers, index buffers, uniform buffers, and
  texture/shader-code storage has no driver-provided convenience layer. This
  is real, unavoidable work — confirmed by the `dkMemBlockCreate`/
  `dkCmdBufAddMemory`/manual-alignment constants (`DK_MEMBLOCK_ALIGNMENT`,
  `DK_UNIFORM_BUF_ALIGNMENT`, etc., `deko3d.h:140-160`) — considerably more
  manual than GL's `glBufferData` or D3D9's `IDirect3DVertexBuffer9`.
- **Command buffers are explicit and must be sized/recorded/submitted by
  hand** (`dkCmdBufCreate`/`dkCmdBufFinishList`/`dkQueueSubmitCommands`,
  `deko3d.h:1246-1334`), with manual fence synchronization
  (`dkQueueSignalFence`/`dkFenceWait`) — there is no driver thread doing this
  implicitly the way GL's driver does.

Net effect on sizing: the *state-translation* work is smaller than originally
estimated (it's mostly 1:1 mechanical translation, not a caching subsystem).
The *memory-management and command-buffer* work is exactly as large as
originally estimated and is the real remaining cost, alongside the
from-scratch shader authoring (§"There is no existing shader set to port
from", above). A more honest sizing is still similar order of magnitude to
`RageDisplay_D3D.cpp` (1505 lines) for the plumbing, **plus** the shader
authoring + offline-compilation pipeline — just for a different reason than
originally stated (manual memory/command-buffer bookkeeping and shader
authoring, not pipeline-permutation management).

## 4. Concrete integration points once the backend exists (low risk, verified)

These are the "wiring" changes, all small and precedented:

1. **`stepmania/src/RageDisplay_Deko3D.h`/`.cpp`** — new files, `class
   RageDisplay_Deko3D : public RageDisplay`, implementing the full surface
   from `RageDisplay.h` (see `01-RageDisplay.md`).
2. **`StepMania.cpp:469-475`** — add `#if defined(SUPPORT_DEKO3D) #include
   "RageDisplay_Deko3D.h" #endif`.
3. **`StepMania.cpp:813-844`** — add the `"deko3d"` string branch.
4. **`stepmania/src/CMakeLists.txt:650-669`** — add `RageDisplay_Deko3D.cpp`
   to the Switch source list and link `deko3d` (plus whatever `uam`-shader
   loader support library is needed) instead of/alongside
   `EGL`/`glapi`/`drm_nouveau`.
5. **`build.sh`** — add a `SUPPORT_DEKO3D` define for the Switch target
   (mirroring `-DHAVE_EGL`/`-DWITH_GLES2=0` at `build.sh:26,44`), and a
   pre-build step invoking `uam` to compile shader sources to `.dksh`,
   packaged into the NRO's RomFS for runtime loading.
6. **Startup renderer probe order** — `CreateDisplay()` (`StepMania.cpp:771-865`,
   called once at boot) already loops over `PREFSMAN->m_sVideoRenderers`
   (comma-separated, split at `:808`), constructs the named backend, calls its
   `Init()`, and only `break`s once `Init()` returns success (`:849-858`); if
   `Init()` returns a non-empty error it deletes that backend and tries the
   *next name in the list* (`:850-856`). Setting the Switch-build default to
   `"deko3d,opengl"` uses this existing, unmodified mechanism to try deko3d
   first and fall back to the whole `RageDisplay_Legacy` backend if `Init()`
   reports failure. No new preference-system code is needed — but see §5.3 for
   exactly what this does and does not protect against.

## 5. Recommended de-risking approach

Given the shader-authoring and state-model work is the real cost, not the
`RageDisplay` plumbing:

1. **Spike the state-translation/descriptor/command-buffer plumbing first**,
   targeting only `Sprite`/`Quad` rendering (the `TextureMode_Modulate` /
   `TextureMode_Glow` / `TextureMode_Add` cases, `RageTypes.h:26-40`) — this
   covers the overwhelming majority of screen time (UI, NoteField, banners)
   without needing the lighting/material shader work at all. See
   `09-Deko3D-SpritePipelineCache.md` for the detailed design, now corrected
   against the real deko3d SDK headers.
2. **Add `Model` lighting/cel-shading second**, once the state-translation and
   descriptor-binding approach from step 1 is proven — this is where the
   Blinn-Phong-equivalent shaders and `SetCelShaded` outline pass are needed.
3. **Set `PREFSMAN->m_sVideoRenderers = "deko3d,opengl"` during development**
   — but be precise about what this buys you, because it is easy to
   over-read. `CreateDisplay()` (`StepMania.cpp:771-865`) picks **one whole
   backend for the entire session**, once, at startup. It is a probe order
   over complete drivers, not a runtime or per-feature safety net. Concretely:
   - It **only** helps if `RageDisplay_Deko3D::Init()` itself detects a
     problem and returns a descriptive error string (`:849-856`) — e.g.
     "this GPU/firmware doesn't support X" or "required shader failed to
     load." In that case the loop cleanly deletes the deko3d backend and
     tries `"opengl"` next, and the user never sees a failure.
   - It does **not** help if `Init()` itself crashes (segfault, unhandled
     exception) instead of returning — nothing after a crash executes, so the
     loop never reaches the next entry. This is a real risk specific to
     deko3d, precisely because GPU memory and command buffers are manually
     managed (§3) — a mistake there is more likely to hard-crash than to
     produce a clean, catchable error the way a GL driver call would.
   - It does **not** help with anything that goes wrong *after* deko3d's
     `Init()` already succeeded and the loop `break`s (`:858`) — e.g. a
     `Model`-lighting code path that isn't implemented yet and crashes or
     misrenders three songs into a gameplay session. `CreateDisplay()` has
     already returned by then; there is no mechanism here that re-evaluates
     the renderer choice mid-session or swaps backends per feature.
   - OpenGL is not "covering the gap" for anything deko3d doesn't implement —
     it is a completely separate, complete backend that only ever runs
     instead of deko3d, never alongside or underneath it. There is no
     mixed/hybrid rendering path in this codebase (`RageDisplay` is selected
     once as a single global pointer, `StepMania.cpp:813`).
4. **Treat the `uam` shader-compilation step as a first-class build
   dependency** from day one — retrofitting a shader build pipeline after
   writing the backend `.cpp` around string-literal-GLSL assumptions (the
   trap the in-tree `RageDisplay_GLES2` seems to have fallen into, per §3)
   would mean redoing the renderer's internals a second time.

## 6. What was verified since the first pass, and what's still open

**Now verified**: deko3d's actual API surface was checked directly against
the real SDK headers, found already installed locally at
`/opt/devkitpro/libnx/include/deko3d.h` (1429 lines) and `.hpp` (1186 lines),
package version `deko3d 0.5.0-1` (confirmed via `dkp-pacman -Q`) — no GitHub
clone was needed. This resolved the state-model question from §3 (no
monolithic pipeline object exists) and confirmed the state/shader/sampler/
memory/command-buffer primitives cited throughout this document and
`09-Deko3D-SpritePipelineCache.md`. There is still no deko3d *code* in this
repo (the `feature/deko3d-rendering` branch still only differs from `develop`
by `.gitignore`/`build.sh` housekeeping) — only the SDK headers were read.

**Also now verified**: devkitPro's official deko3d example projects, shipped
alongside the SDK at `/opt/devkitpro/examples/switch/graphics/deko3d/`, were
read in full for the relevant files (see
`09-Deko3D-SpritePipelineCache.md` §9 for the complete breakdown). This
resolved `uam`'s shader-language requirement (plain GLSL, `#version 460`,
compiled via `uam -s <stage> -o out.dksh in.glsl`) and the per-frame
command-buffer/uniform-update pattern, and surfaced two genuine scope items
this document didn't previously cover:
- **Docked/handheld resolution switching** is a real, required integration
  point (`onOperationMode()` libnx applet callback → rebuild framebuffers/
  swapchain), mapping onto `RageDisplay::ResolutionChanged()`/`TryVideoMode()`
  (`RageDisplay.h:242,385`) — not mentioned anywhere in this document's
  original integration-points list (§4).
- **Texture format is an explicit choice, not a given.** The official
  examples default to GPU-native compressed formats (e.g. BC1) built offline;
  this project's existing texture pipeline decodes to raw RGBA8 at runtime
  (`RageBitmapTexture`/`RageSurface`, `03-TextureSystem.md`). Phase 1 should
  explicitly target uncompressed `DkImageFormat_RGBA8_Unorm` and defer
  GPU-native compression as a separate, later optimization — see
  `09-Deko3D-SpritePipelineCache.md` §9 for the reasoning.

**Still not verified in this pass:**
- `RageDisplay_OGL.cpp` (2860 lines) and `RageDisplay_D3D.cpp` (1505 lines)
  were sized (`wc -l`) but not read in full — the size comparison in this
  document is structural (method-surface-driven), not a line-by-line audit.
- Tegra X1 hardware-specific capabilities/limits beyond what's in the SDK
  headers and examples (e.g. real-world performance characteristics of
  specific `DkImageFormat`/compression choices) are external domain
  knowledge.
- `Example07_MeshLighting.cpp` has since been read and cross-checked against
  Phase 2's `Model`-lighting assumptions (see
  `09-Deko3D-SpritePipelineCache.md` §10) — it confirmed Blinn-Phong is the
  right technique, but also found a genuine gap: the example bakes a fixed
  Material×Light result into one uniform block, while StepMania's real
  `SetMaterial()`/`SetLightDirectional()` (`Model.cpp:344,419,446`;
  `ActorFrame.cpp:216`) are independently-settable and must be multiplied
  together in-shader — a real, previously-unscoped addition to Phase 2's
  shader-authoring work.
- `Example08_DeferredShading.cpp` has also since been checked (see
  `09-Deko3D-SpritePipelineCache.md` §10) and confirmed **correctly
  irrelevant** to Phase 2: it's a many-lights/many-objects optimization
  technique, and StepMania's codebase-wide light count is confirmed to be
  exactly one (`ActorFrame.cpp:216`), so deferred shading's G-buffer memory
  and extra composition pass would be pure overhead with no scaling benefit.
- All remaining examples (`Example01`/`02`/`03`/`05`/`06`/`09`, and
  `deko_console`) have since been checked too (see
  `09-Deko3D-SpritePipelineCache.md` §8). Most confirmed subsets of what
  `Example04`/`07`/`08` already covered, or architecturally unrelated
  (compute shaders, tessellation) — but `deko_console` surfaced a genuinely
  valuable finding: it renders its entire (constantly-updating) text grid as
  **one instanced draw call**, updating on-screen content by writing
  directly into a CPU-mapped, fence-guarded per-instance GPU buffer, with no
  per-frame command re-recording at all. This is a real, evidenced
  performance lever beyond "just get deko3d working" — directly applicable
  to `BitmapText` glyph rendering (whose glyph quads are already batched
  CPU-side into one vertex array per actor) and potentially note-skin
  rendering, both prime candidates for exactly this instancing technique.
  It's explicitly **not** part of Phase 1's baseline design (see
  `09-Deko3D-SpritePipelineCache.md` §8 for why), but is a concrete,
  worthwhile follow-on optimization once Phase 1 is working — directly
  relevant to the original "performance is poor" motivation for this whole
  investigation.

---

**Scope**: `RageDisplay.h`, `RageDisplay_GLES2.h/.cpp`, `StepMania.cpp`
(renderer-selection section), `stepmania/src/CMakeLists.txt`,
`arch/LowLevelWindow/LowLevelWindow.h`, `build.sh`, `nxshim/` directory
listing, `CMake/DefineOptions.cmake`. Builds on the verified `RageDisplay`
architecture in `01-RageDisplay.md` and `INDEX.md`.
