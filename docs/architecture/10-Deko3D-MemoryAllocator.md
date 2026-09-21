# deko3d GPU Memory Allocator — Design Scope

Identified as the actual blocking prerequisite for Phase 1: every piece of
`RageDisplay_Deko3D` (textures, vertex uploads, uniform buffers, shader code,
descriptors) needs `DkMemBlock` sub-allocation, and none of it exists yet.
This document scopes that allocator, grounded in `deko3d.h`'s memory
primitives (read in full, see `01-RageDisplay.md`/`09-Deko3D-SpritePipelineCache.md`)
and the official SDK's own allocator/upload code (`SampleFramework/CMemPool.*`,
`CExternalImage.cpp`, `CCmdMemRing.h`, `CDescriptorSet.h` — all read in full).

## The structural constraint that shapes everything: flags are per-block, not per-allocation

`DkMemBlockMaker` (`deko3d.h:185-199`) takes one `flags` value for the whole
block. A block flagged `DkMemBlockFlags_Code` cannot also hold texture image
data flagged `DkMemBlockFlags_Image`, and CPU-writable "plain" data can't
share a block with either. This means **the number of distinct pools is
dictated by the number of distinct flag combinations actually needed** — not
a convenience choice. Five combinations cover everything Phase 1 (and
Phase 2's `Model` geometry) needs:

| Pool | Flags | Confirmed by |
|---|---|---|
| **Code** | `CpuUncached\|GpuCached\|Code` | `deko3d.h:180`; `deko_basic/main.c:112-114` |
| **Image** | `GpuCached\|Image` | `deko3d.h:181`; `Example04_TexturedCube.cpp:160` |
| **Scratch/staging** | `CpuUncached\|GpuCached` (plain) | `CExternalImage.cpp:10` (`scratchPool`) |
| **Dynamic per-frame** | `CpuUncached\|GpuCached` (plain) | `Example04_TexturedCube.cpp:162`, `CCmdMemRing.h` |
| **Descriptors** | plain, needs `DK_IMAGE_DESCRIPTOR_ALIGNMENT`/`DK_SAMPLER_DESCRIPTOR_ALIGNMENT` (32B) | `CDescriptorSet.h:15-16` |

Descriptors and dynamic-per-frame data share the same flag combination as
"Scratch," but **cannot share the same pool instance** — see below, this is
about allocation *pattern*, not flags.

## Why one allocation strategy doesn't fit all five pools

The five pools split cleanly into three genuinely different allocation
*patterns*, confirmed by how the official examples actually use each one:

### Pattern A — bump-allocate once, never free (Code pool)

Shaders are loaded once at `Init()` from a small, fixed set of precompiled
`.dksh` files, and live for the app's entire lifetime. `deko_basic`'s own
`loadShader()` (`main.c:42-65`) uses a literal incrementing offset — its own
comment admits "this isn't a general purpose allocation algorithm"
(`:52-53`) — and that's *correct* for this case, not a shortcut: there's
nothing to free, nothing to fragment, and the shader count is small and
known upfront (Phase 1: one vertex shader + 2-3 fragment shader variants,
per `09-Deko3D-SpritePipelineCache.md` §4/§6). **A simple bump allocator is
the right tool here, not a missing feature.**

### Pattern B — free-list with coalescing (Image pool, and Phase 2's `Model` geometry)

Textures load and unload continuously and unpredictably throughout a
session — `RageTextureManager`'s cache churns via `DeleteCachedTextures()`
between screens and `DoDelayedDelete()` on theme switch
(`03-TextureSystem.md`). A bump allocator here would fragment/leak within
minutes of normal use. This is the one pool that genuinely needs
`SampleFramework/CMemPool`'s actual design (read in full,
`09-Deko3D-SpritePipelineCache.md` §9's "General memory-management scale"):
one or more large (default 8 MiB) `DkMemBlock`s, sub-allocated via a
best-fit free list built on an intrusive tree, with adjacent-free-slice
coalescing (`CMemPool::Slice::canCoalesce()`, `CMemPool.h:54`) to avoid
fragmentation over a long session. Phase 2's `Model` vertex/index buffers
(`RageCompiledGeometry`'s backing store) have the identical load/unload
lifecycle and should use this same allocator shape — plausibly the same
pool instance, since neither needs the `Image` flag's texture-specific
behavior... except `RageCompiledGeometry` data doesn't need `Image` at all
(it's vertex/index data, not a sampled image), so **Model geometry is
actually a sixth, `Image`-flag-free pool that also needs Pattern B** — see
open item below.

### Pattern C — synchronous transient reuse (Scratch pool)

Confirmed from `CExternalImage::load()` (`CExternalImage.cpp:8-37`, read in
full): loading one texture means (1) reading raw pixel bytes into a scratch
allocation, (2) allocating the final image in the Image pool, (3) recording
a `copyBufferToImage()` command, submitting it, and **synchronously
`waitIdle()`-ing**, then (4) destroying the scratch allocation immediately
(`:34-35`). This matches how `RageTextureManager::LoadTexture()` already
works today — called synchronously during scene loading, not a per-frame hot
path (`03-TextureSystem.md`). Because uploads are synchronous and one at a
time, the scratch pool never needs more than one in-flight allocation —
**a single reused transient buffer, sized for the largest expected raw
pixel payload, is sufficient.** No free list, no ring, no fragmentation
concern: allocate, upload, wait, free, repeat.

### Pattern D — ring-buffered, N slices, fence-guarded (Dynamic pool)

Already fully resolved for command-buffer memory via `CCmdMemRing`
(`09-Deko3D-SpritePipelineCache.md` §9): N memory slices (2-3, matching
swapchain buffering) with a `dk::Fence` per slice, so the CPU never
overwrites a slice the GPU might still be reading from a prior frame. The
**same pattern applies to per-frame vertex data and per-frame uniform data**
— `Sprite.cpp` rebuilds `RageSpriteVertex v[4]` fresh every draw call
(`Sprite.cpp:563`, per `09-Deko3D-SpritePipelineCache.md` §1), and matrix
uniforms update every frame (`Example04_TexturedCube.cpp:334-337`
`pushConstants` pattern). **These need their own `CCmdMemRing`-shaped ring,
separate from the command-memory ring** — a command list and the vertex/
uniform data it references are logically distinct allocations with the same
lifetime shape, not one buffer serving both roles.

### Pattern E — long-lived, small, fixed-slot (Descriptor pool)

Confirmed from `CDescriptorSet<NumDescriptors>` (`CDescriptorSet.h`, read in
full): one allocation of `NumDescriptors * 32 bytes`, individual slots
written via `pushData()` at `baseAddr + id*DescriptorSize`
(`CDescriptorSet.h:47-52`). This must **not** live in the Pattern-D ring
(descriptors persist as long as the texture/sampler is alive, which
outlives any single frame) — it needs a long-lived allocation, sized once.

## The concrete design question this raises: `NumDescriptors` is compile-time in every official example

Every official example instantiates `CDescriptorSet<MaxImages>` with a
small, fixed, compile-time constant (`Example04_TexturedCube.cpp:109-110`:
`MaxImages = 1`, `MaxSamplers = 1`). StepMania's actual texture count is
**not** fixed and not small — `RageTextureManager` caches however many
distinct `RageTextureID`s a theme's screens load, plausibly dozens to a few
hundred concurrently. This is a real design decision the examples don't
answer, because none of them have this problem:

- **Recommended for Phase 1**: pick a generously large, fixed maximum (e.g.
  2048-4096 image-descriptor slots) rather than build a resizable
  descriptor table. At 32 bytes/slot (`DK_IMAGE_DESCRIPTOR_ALIGNMENT`,
  `deko3d.h:154`), even 4096 slots is 128 KiB — cheap. A resizable table is
  the wrong complexity to take on here: existing bound `DkResHandle`s
  already baked into in-flight command lists reference old slot indices,
  so growing the table mid-session would need index-stability guarantees a
  simple resize can't give for free.
- **Real requirement this creates**: `RageDisplay_Deko3D::CreateTexture()`
  needs an explicit, checked failure path for "descriptor table full" —
  silently wrapping or corrupting a neighboring slot on overflow would be a
  much worse failure mode than a clear, loud error. Not designed in this
  pass; needs to happen alongside actually writing `CreateTexture()`.
- Sampler count stays fixed at 4 regardless (`TextureWrapping` ×
  `TextureFiltering`, both bool — `09-Deko3D-SpritePipelineCache.md` §4/§5),
  no scaling concern there.

## Sizing the pools — genuinely not answerable from documentation alone

The official examples size their pools per-demo (`Example04`: 16 MiB images
/ 128 KiB code / 1 MiB data; `Example07`: 64 MiB images, larger for its
higher-resolution mesh/framebuffer needs). **There is no principled number
to put here without profiling real StepMania content** — a theme's texture
footprint, typical live-texture count during gameplay, and peak per-frame
vertex/uniform volume are all things that vary by theme and haven't been
measured against this project's actual assets in this pass. The honest
scope here is: start with generous-but-arbitrary defaults (e.g. Image pool
sized similarly to `Example07`'s 64 MiB as a starting point, since StepMania
themes commonly include full-screen banners/backgrounds at least as large
as that example's assets), instrument actual usage once Phase 1 renders
something, and tune from real numbers rather than guessing further in a
document. This also interacts with the Switch's overall per-application
memory budget (a real constraint), which is Tegra/OS-level domain knowledge
not verified in this pass — flagged as such rather than asserted.

## Fit against StepMania's actual engine — not just "port vs. reimplement"

The right question isn't "is `CMemPool` legally reusable" (it is — see
license note below) but "does its *shape* and its *implementation* both fit
what this specific engine has and needs." Checked both sides directly:

### What StepMania's engine actually has, that this design must sit alongside

- **No existing GPU-memory-suballocation code anywhere in this codebase**
  (searched for any `Pool`/`Allocator`/`Arena`/`FreeList` class — none
  exist). This isn't surprising: `RageDisplay_Legacy`/`RageDisplay_D3D`/
  `RageDisplay_GLES2` never needed one, because GL and D3D9 manage texture/
  buffer memory for you. The deko3d backend is introducing a category of
  responsibility that is genuinely new to this 20+-year-old engine, not
  replacing something that already exists.
- **A consistent existing idiom for GPU-adjacent resource lifetime**: plain
  public `int m_iRefCount` fields with explicit `Unload()`/`Destroy()` calls
  — confirmed identically on `RageTexture` (`RageTexture.h:58`), `Font`
  (`Font.h:147`), and `RageModelGeometry` (`RageModelGeometry.h:23`). This
  engine's convention is explicit, manual, C-with-classes resource
  management, not RAII handles with destructor-triggered auto-free.
- **Confirmed single-threaded texture loading**: no `RageThread`/`pthread`
  usage anywhere in `RageTextureManager.cpp`/`RageTexturePreloader.cpp`, and
  `RageDisplay::SupportsThreadedRendering()` defaults `false`
  (`RageDisplay.h:252`) with nothing overriding it. `RageThread`/`RageMutex`
  do exist in this engine (`RageThreads.h:7,90`) for other subsystems, but
  there's no evidence texture loading needs them — so, matching `CMemPool`'s
  own no-locking assumption, **no thread-safety requirement exists here**.
  This was worth checking rather than assuming; it came back clean.
- **A hard, confirmed build constraint that rules out literal porting**:
  `StepmaniaCore.cmake:23-25` sets `SM_CPP_STANDARD` to **`gnu++11`**
  specifically for `SWITCH_LIBNX` (`if(CMAKE_SYSTEM_NAME MATCHES "Linux" OR
  SWITCH_LIBNX) ... set(SM_CPP_STANDARD "gnu++11")`) — applied project-wide
  to the single `SM_EXE_NAME` target (`src/CMakeLists.txt:424`). The
  SampleFramework code uses `std::optional` (C++17), extensive multi-
  statement `constexpr` (needs C++14's relaxed rules at minimum), and
  template `Handle` wrapper classes with implicit-bool/destructor-driven
  RAII throughout `CMemPool`/`CCmdMemRing`/`CDescriptorSet`/`CShader`. **None
  of this compiles under `-std=gnu++11` as shipped.**

### What this actually settles

Bumping the whole project's C++ standard just to reuse ~600 lines of
example-license'd utility code would be a disproportionate, wide-blast-radius
change — `SM_CPP_STANDARD` is one global setting applied to every `.cpp` in
the `StepMania` target, not scoped to new files, so it risks interacting with
two decades of existing code and this specific devkitA64/libnx toolchain's
own C++17 maturity, for a payoff of avoiding writing a few hundred lines of
straightforward bookkeeping code. That's not a good trade.

So the fit-based answer, not just a license-based one: **take the
*architecture* (the three real patterns — bump, free-list+coalescing,
N-slice ring — each matched to a pool's actual lifecycle, per the sections
above) and write it as fresh, C++11-conformant code in this engine's own
idiom** — plain structs and explicit `Allocate()`/`Free()` methods
matching the `m_iRefCount`/`Unload()` convention already used everywhere
else in this codebase, a `vector`-based free list rather than
`CIntrusiveList`/`CIntrusiveTree` templates, no `std::optional`/`Handle`-
with-destructor wrapper. Not a literal port (blocked by the C++11
constraint), and not a from-scratch reinvention ignoring a proven shape
either (the free-list-with-coalescing design for the Image pool is
independently justified below, not just copied because the example did it).

### Why free-list-with-coalescing specifically earns its keep for the Image pool

Worth confirming this against StepMania's *real* texture-cache behavior
rather than assuming the example's sophistication is deco3d-specific ritual.
`RageTextureManager` caches by `RageTextureID` with reference counting
(`03-TextureSystem.md`) — critically, this means **long-lived and
short-lived textures coexist in memory at overlapping times**: a
`ScreenSystemLayer`-style UI chrome texture used on nearly every screen
stays loaded (refcount never hits zero) across many scene transitions,
while a song-select-specific banner loads and unloads every time that one
screen is entered/left. Because these interleave in time and in the
underlying memory blocks, a simpler "reset the whole arena at each
`DeleteCachedTextures()` scene-transition flush point" scheme (the
naturally simpler alternative to a real free list) **would not work** — it
would free memory still in use by the long-lived textures. This confirms
free-list-with-coalescing is the right complexity for this specific pool,
independent of what the demo examples happened to choose — it's demanded
by this engine's actual cache semantics.

`SampleFramework/LICENSE` (read in full) is a permissive zlib-style license
regardless, for the record — reuse would have been legally fine; the C++11
constraint is the actual reason to write fresh code instead, not a
licensing concern.

## Proposed pool inventory for Phase 1 (+ the Phase 2 addition it implies)

```
CodePool        (Pattern A: bump)              — shaders, Init()-time, never freed
ImagePool       (Pattern B: free-list+coalesce) — RageTexture backing storage
ScratchPool     (Pattern C: reused transient)   — texture upload staging, one-at-a-time
DynamicCmdPool  (Pattern D: N-slice ring)       — CCmdMemRing-style, command-buffer memory
DynamicDataPool (Pattern D: N-slice ring)       — separate ring, per-frame vertex + uniform data
DescriptorPool  (Pattern E: long-lived fixed)   — image + sampler descriptor tables, sized generously upfront

(Phase 2 addition, not Phase 1:)
GeometryPool    (Pattern B: free-list+coalesce) — Model vertex/index buffers, same shape as ImagePool
                                                   but without the Image flag; plausibly a second
                                                   CMemPool instance rather than shared with ImagePool,
                                                   since the flag combination genuinely differs
```

## Open items this document does not resolve

1. ~~Port vs. reimplement `CMemPool`~~ — **resolved**: write fresh,
   C++11-conformant code matching this engine's existing idiom, for the
   reasons above (StepMania's Switch build is confirmed `gnu++11`, the
   SampleFramework code uses C++17 features that won't compile under it).
   `CMemPool.cpp`'s actual free-list/coalescing *algorithm* is still worth
   reading as a reference for the logic shape (best-fit search,
   adjacent-slice coalescing) before writing the fresh version — that's an
   implementation-detail read, not a design-decision blocker anymore.
2. **Decide the descriptor-table-full failure path** for `CreateTexture()` —
   needs to happen alongside writing that method, not in the abstract.
3. **Pick real pool sizes** — needs actual StepMania theme/gameplay content
   to profile against; the numbers in this document are explicitly
   placeholders, not recommendations to ship with.
4. **Confirm Switch's per-application memory budget** against the sum of
   these pools plus everything else StepMania already allocates (audio,
   song data, existing CPU-side texture/font caches) — external
   platform knowledge, not checked in this pass.
5. **Confirm whether a distinct "transfer queue" is needed** for texture
   uploads, or whether reusing the main render queue (as
   `CExternalImage::load()`'s `transferQueue` parameter appears to do in
   every example call site) is correct — `deko3d.h`'s `DkQueueFlags` enum
   (`:259-269`) has no distinct transfer-queue flag, suggesting the answer
   is "reuse the main queue," but this wasn't independently confirmed
   against a call site passing a genuinely different queue instance.

---

**Scope**: `deko3d.h` memory-related sections (re-cited, previously read in
full per `09-Deko3D-SpritePipelineCache.md`), `SampleFramework/CMemPool.h`
(class shape only — `.cpp` not read), `CExternalImage.h`/`.cpp` (read in
full, newly read for this document), `CCmdMemRing.h`/`CDescriptorSet.h`
(previously read in full), `SampleFramework/LICENSE` (read in full).
`03-TextureSystem.md` for `RageTextureManager`'s real cache-churn behavior.
Also newly checked for this document: `stepmania/src/*.h` searched for
existing pool/allocator classes (none found), `RageThreads.h` (confirms
`RageThread`/`RageMutex` exist as this engine's threading primitives),
`RageTextureManager.cpp`/`RageTexturePreloader.cpp` searched for thread
usage (none found — confirms synchronous, single-threaded texture loading),
and `StepmaniaCore.cmake`/`src/CMakeLists.txt` (confirms the Switch build's
C++ standard is `gnu++11`).
