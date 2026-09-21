# Phase 1 Implementation Status

Real code, not a design sketch. Every claim in this document was validated
by actually compiling the corresponding file against this project's real
Switch build (`cmake`/`make` in `build/release`, configured for
`SWITCH_LIBNX=1` with the real `aarch64-none-elf-g++` toolchain and the
devkitPro `deko3d`/`libnx` SDKs installed at `/opt/devkitpro`) — not by
inspection alone. This follows the same discipline as the design docs: no
claim here is asserted without having been checked against something real.

## Files added

| File | Purpose | Compile status |
|---|---|---|
| `stepmania/src/Deko3DMemPool.h/.cpp` | The three allocation patterns from `10-Deko3D-MemoryAllocator.md` (bump, free-list+coalescing, N-slice ring), plus the fixed-size descriptor table | **Compiles clean**, zero errors/warnings from this file |
| `stepmania/src/RageDisplay_Deko3D.h/.cpp` | The `RageDisplay` backend itself | **Compiles clean**, zero errors/warnings from this file |
| `stepmania/src/deko3d_shaders/sprite_vsh.glsl` | Phase 1 vertex shader | **Compiles clean via `uam`**, produces a valid `.dksh` |
| `stepmania/src/deko3d_shaders/sprite_modulate_fsh.glsl` | `TextureMode_Modulate` fragment shader | **Compiles clean via `uam`** |
| `stepmania/src/deko3d_shaders/sprite_glow_fsh.glsl` | `TextureMode_Glow` fragment shader | **Compiles clean via `uam`** |

## Build wiring added

- `stepmania/src/CMakeData-rage.cmake` — `Deko3DMemPool.cpp`/`RageDisplay_Deko3D.cpp` added to the source list, gated `if(SWITCH_LIBNX)` (mirrors the existing `WITH_GLES2` pattern at the same location).
- `stepmania/src/CMakeLists.txt`:
  - `deko3d` added to the `SWITCH_LIBNX` link-library list.
  - `SUPPORT_DEKO3D` compile definition added — **had to be placed after
    `add_executable()`** rather than inside `CMakeData-rage.cmake` (included
    before the target exists); found this the hard way via a real
    `get_target_property() called with non-existent target "StepMania"`
    configure error, not by reading the macro first.
  - A `Deko3DShaders` custom target that runs `uam` to compile each `.glsl`
    into `Data/Shaders/Deko3D/*.dksh`, wired as a build dependency of the
    `StepMania` target. **Actually run and verified** to produce valid
    `.dksh` files in the right location.
- `stepmania/src/StepMania.cpp` — `#include "RageDisplay_Deko3D.h"` guarded
  by `SUPPORT_DEKO3D`, and a `"deko3d"` branch in `CreateDisplay()`'s
  renderer-selection loop, exactly matching the existing `"opengl"`/`"gles2"`/
  `"d3d"` pattern (`08-Deko3D-Feasibility.md` §4).

## A real, previously-unaddressed decision made during implementation

**Shaders don't use RomFS.** Every devkitPro deko3d example loads compiled
shaders from `romfs:/shaders/*.dksh`. This project has no RomFS anywhere —
checked, and confirmed: `RageFileManager.cpp`'s `ChangeToDirOfExecutable()`
resolves `__SWITCH__` paths via a plain `GetCwd()`, and the sibling
`stepmania/Data/Shaders/GLSL/` directory (existing theme-effect shaders,
e.g. `Cel.frag`, `Distance field.frag` — which map directly onto the
`EffectMode` enum values Phase 1 explicitly deferred) already establishes
the real convention: shaders ship as plain files under `Data/Shaders/`,
loaded via a CWD-relative path, like every other engine asset. The new
`.dksh` files follow that exact convention (`Data/Shaders/Deko3D/`) instead
of introducing RomFS packaging for three files.

## Bugs found only by actually compiling (not by review)

Six real, mechanical bugs were caught this way, none of which were visible
from reading the deko3d headers alone:

1. `dk::UniqueMemBlock` has no `.get()` method — needs an explicit/implicit
   cast (`(DkMemBlock)handle`) instead.
2. `dk::Swapchain::acquireImage()` takes `(int&, DkFence&)`; the simpler
   `int`-returning overload lives on `dk::Queue`, not `dk::Swapchain`.
3. `RageDisplay::SetEffectMode()`/`IsEffectModeSupported()` have default
   (non-pure-virtual) bodies in the base class — easy to forget to declare
   an override at all, and the compiler catches it as "no declaration
   matches" rather than a missing-override warning.
4. `DkVtxAttribState`'s aggregate initializer takes exactly its 6 *named*
   bitfields — the two anonymous padding bitfields in the struct are skipped
   for aggregate-init purposes, not counted. Passing 7 values (as if the
   padding needed a value too) is a compile error, not a silent
   misalignment — good, since a silent version of this bug would have been
   much worse.
5. `dk::CmdBuf` has no singular `bindRenderTarget()` convenience method in
   the C++ wrapper (unlike the C API's `dkCmdBufBindRenderTarget`) — only
   the plural `bindRenderTargets(ArrayProxy, depthTarget)`.
6. **A genuine structural bug, not just a compile error**: the first draft
   acquired the swapchain image and bound it as the render target inside
   `EndFrame()`, *after* every `DrawQuadsInternal()` call for the frame had
   already run — meaning no sprite draw would ever have had a render target
   bound at all. This was only caught while fixing an unrelated compile
   error in that same function and re-reading the surrounding logic; the
   frame lifecycle now correctly acquires+binds in `BeginFrame()` and only
   presents in `EndFrame()`.

## Confirmed not yet validated

- **No full link attempted successfully yet in this pass** — a full
  `make StepMania` was kicked off to validate the entire codebase links
  together (not just that the new files compile in isolation), but has not
  been confirmed complete as of this document being written. This document
  will be wrong if the link surfaces further issues; treat "compiles clean"
  above as verified, and full linking as in-progress.
- **Never run on real hardware or an emulator.** Nothing in this pass
  confirms the backend actually produces correct pixels — only that it
  compiles against the real SDK and toolchain, and that the individual
  API calls match verified signatures.
- **`UpdateTexture()`, `CreateScreenshot()`, and every `Draw*Internal()`
  except `DrawQuadsInternal()`** are stubbed with a `LOG->Warn()` and a
  no-op body, matching what Phase 1's scope always said would remain
  unimplemented (`09-Deko3D-SpritePipelineCache.md` §1: only
  `TextureMode_Modulate`/`_Glow` via `DrawQuads` are exercised by
  `Sprite::DrawTexture()`).
- **Docked/handheld resolution switching** (`onOperationMode()`) is still
  not hooked up — `TryVideoMode()` creates a swapchain once at `Init()` time
  and has no path to rebuild it if the console's display mode changes.
- Pool sizes in `RageDisplay_Deko3D.cpp` (`IMAGE_POOL_SIZE = 64 MiB`, etc.)
  are the same explicitly-flagged placeholders from
  `10-Deko3D-MemoryAllocator.md` — not tuned against real content.

---

**Scope**: This document describes only what was written and compiled in
this implementation pass. It is not a design document — see
`08`-`11-Deko3D-*.md` for the design rationale behind every decision
referenced here.
