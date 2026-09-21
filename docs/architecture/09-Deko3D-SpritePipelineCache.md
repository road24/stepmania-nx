# deko3d Sprite Draw-State Design — Phase 1 (Corrected)

Scope: this is the Phase-1 spike from `08-Deko3D-Feasibility.md` §5.1 — get
`Sprite`/`Quad` drawing working under a deko3d `RageDisplay` backend.

**This document replaces an earlier version titled "pipeline-cache design."**
That version assumed deko3d bakes shader + blend + cull + depth state into one
immutable pipeline object (the way core Vulkan 1.0 does), and designed a
hash-mapped pipeline cache around that assumption. That assumption has since
been checked against the real deko3d SDK — found already installed locally at
`/opt/devkitpro/libnx/include/deko3d.h`/`.hpp` (devkitPro package `deko3d
0.5.0-1`, confirmed via `dkp-pacman -Q`; no GitHub clone was needed) — **and
it's wrong. deko3d has no pipeline object at all.** This materially simplifies
the design, laid out below.

## 1. Ground truth: what a single `Sprite` draw actually does (unchanged)

Traced directly from `Sprite::DrawTexture()` (`Sprite.cpp:527-663`):

```
Actor::SetGlobalRenderStates()                                    Sprite.cpp:529
  DISPLAY->SetBlendMode( m_BlendMode )                              Actor.cpp:765
  DISPLAY->SetZWrite( m_bZWrite )                                    Actor.cpp:766
  DISPLAY->SetZTestMode( m_ZTestMode )                                Actor.cpp:767
  DISPLAY->SetZBias( ... )                                             Actor.cpp:772/774
  DISPLAY->SetCullMode( m_CullMode )                                    Actor.cpp:778

[build 4 RageSpriteVertex positions from the cropped quad]              Sprite.cpp:563-575

DISPLAY->ClearAllTextures()                                              Sprite.cpp:577
DISPLAY->SetTexture( TextureUnit_1, texHandle )                           Sprite.cpp:578

Actor::SetTextureRenderStates()                                            Sprite.cpp:582
  DISPLAY->SetTextureWrapping( TextureUnit_1, m_bTextureWrapping )          Actor.cpp:783
  DISPLAY->SetTextureFiltering( TextureUnit_1, m_bTextureFiltering )         Actor.cpp:784

DISPLAY->SetEffectMode( m_EffectMode )                                        Sprite.cpp:583

[fill texture coords into v[], handling cropping]                             Sprite.cpp:587-625

if any corner's diffuse alpha > 0:                                             Sprite.cpp:628
    DISPLAY->SetTextureMode( TextureUnit_1, TextureMode_Modulate )              Sprite.cpp:633
    if shadow enabled:  v[].c = flat shadow color;  DISPLAY->DrawQuad(v)          Sprite.cpp:636-645
    v[].c = per-corner diffuse color;  DISPLAY->DrawQuad(v)   ← "diffuse pass"      Sprite.cpp:648-652

if glow.a > 0.0001:                                                               Sprite.cpp:656
    DISPLAY->SetTextureMode( TextureUnit_1, TextureMode_Glow )                      Sprite.cpp:658
    v[].c = glow color (flat);  DISPLAY->DrawQuad(v)          ← "glow pass"           Sprite.cpp:660

DISPLAY->SetEffectMode( EffectMode_Normal )     ← reset for the next actor              Sprite.cpp:662
```

`DrawQuad(v)` is `DrawQuads(v,4)` (`RageDisplay.h:352`); the base-class
`RageDisplay::DrawQuads()` (`RageDisplay.cpp:832-842`) just validates the
count and calls `DrawQuadsInternal()`. **`RageDisplay_Deko3D::DrawQuadsInternal()`
is the single choke point every sprite draw ends up at.**

## 2. What deko3d's real API looks like (verified against `deko3d.h`)

The full opaque-handle list in the SDK (`deko3d.h:74-85`) is: `Device,
MemBlock, Fence, Variable, CmdBuf, Queue, Shader, ImageLayout, Image,
ImageDescriptor, SamplerDescriptor, Swapchain`. **There is no `Pipeline`
type.** Instead, every fixed-function state category is its own small POD
struct, bound independently and directly onto the command buffer
(`deko3d.h:1260-1322`):

```c
void dkCmdBufBindShaders(DkCmdBuf, uint32_t stageMask, DkShader const* const[], uint32_t numShaders);
void dkCmdBufBindRasterizerState(DkCmdBuf, DkRasterizerState const*);       // deko3d.h:794-806 — includes cull mode
void dkCmdBufBindColorState(DkCmdBuf, DkColorState const*);                 // deko3d.h:885-891 — per-target blend enable
void dkCmdBufBindBlendStates(DkCmdBuf, uint32_t firstId, DkBlendState const[], uint32_t); // deko3d.h:967-976
void dkCmdBufBindDepthStencilState(DkCmdBuf, DkDepthStencilState const*);   // deko3d.h:1015-1032 — z-test, z-write, stencil
void dkCmdBufBindMultisampleState(DkCmdBuf, DkMultisampleState const*);     // deko3d.h:830-842
void dkCmdBufBindVtxAttribState(DkCmdBuf, DkVtxAttribState const[], uint32_t); // deko3d.h:1099-1109 — vertex layout, ALSO separate
void dkCmdBufBindVtxBufferState(DkCmdBuf, DkVtxBufferState const[], uint32_t); // deko3d.h:1111-1115
void dkCmdBufBindTextures(DkCmdBuf, DkStage, uint32_t firstId, DkResHandle const[], uint32_t);
```

This is architecturally much closer to classic GL/D3D11 ("set a state object,
then issue a draw call") than to Vulkan/D3D12's monolithic Pipeline State
Object model. Every one of these bind calls is independent — changing blend
mode does not require touching the shader binding, the rasterizer state, or
anything else.

### The exact struct fields relevant to `Sprite` rendering

```c
// deko3d.h:967-976 — maps to Actor::SetBlendMode / BlendMode
typedef struct DkBlendState {
    DkBlendOp colorBlendOp : 3;           // deko3d.h:935-942: Add/Sub/RevSub/Min/Max
    DkBlendFactor srcColorBlendFactor : 6; // deko3d.h:944-965: Zero/One/SrcColor/SrcAlpha/DstAlpha/...
    DkBlendFactor dstColorBlendFactor : 6;
    DkBlendOp alphaBlendOp : 3;
    DkBlendFactor srcAlphaBlendFactor : 6;
    DkBlendFactor dstAlphaBlendFactor : 6;
} DkBlendState;

// deko3d.h:794-806 — maps to Actor::SetCullMode / CullMode
typedef struct DkRasterizerState {
    uint32_t rasterizerEnable : 1;
    uint32_t depthClampEnable : 1;
    uint32_t fillRectangleEnable : 1;
    DkPolygonMode polygonModeFront : 2;
    DkPolygonMode polygonModeBack : 2;
    DkFace cullMode : 2;              // None/Front/Back/FrontAndBack — deko3d.h:774-780
    DkFrontFace frontFace : 1;
    DkProvokingVertex provokingVertex : 1;
    uint32_t polygonSmoothEnableMask : 3;
    uint32_t depthBiasEnableMask : 3;
} DkRasterizerState;

// deko3d.h:1015-1032 — maps to Actor::SetZTestMode / SetZWrite
typedef struct DkDepthStencilState {
    uint32_t depthTestEnable : 1;
    uint32_t depthWriteEnable : 1;
    uint32_t stencilTestEnable : 1;
    DkCompareOp depthCompareOp : 4;      // Never/Less/Equal/Lequal/Greater/... — deko3d.h:636-646
    // ... stencil front/back ops, unused for Phase 1 ...
} DkDepthStencilState;

// deko3d.h:655-675 — maps to Actor::SetTextureWrapping / SetTextureFiltering
typedef struct DkSampler {
    DkFilter minFilter, magFilter;    // Nearest/Linear
    DkMipFilter mipFilter;
    DkWrapMode wrapMode[3];           // Repeat/MirroredRepeat/ClampToEdge/ClampToBorder/...
    float lodClampMin, lodClampMax, lodBias, lodSnap;
    bool compareEnable; DkCompareOp compareOp;
    // border color, anisotropy, reduction mode — unused for Phase 1
} DkSampler;
```

## 3. Corrected translation: `RageDisplay` setter → deko3d call

There is **no cache to design** for the four fixed-function axes — each is a
direct, cheap, POD-struct-fill-and-bind, no different in spirit from a GL
backend's own `glBlendFunc()`/`glCullFace()`/`glDepthFunc()` calls:

| `RageDisplay` call | Real values | deko3d translation |
|---|---|---|
| `SetBlendMode(BlendMode)` | 11 values, `RageTypes.h:8-23` | fill a `DkBlendState` (op + src/dst factors per value), `dkCmdBufBindBlendStates()` **and** toggle `DkColorState.blendEnableMask` via `dkColorStateSetBlendEnable()` (see correction below) |
| `SetCullMode(CullMode)` | `CULL_BACK`/`_FRONT`/`_NONE`, `RageTypes.h:60-68` | set `DkRasterizerState.cullMode` (`DkFace`), `dkCmdBufBindRasterizerState()` |
| `SetZTestMode`/`SetZWrite` | `RageTypes.h:70-78` | fill `DkDepthStencilState`, `dkCmdBufBindDepthStencilState()` |
| `SetTextureWrapping`/`SetTextureFiltering` | bool × bool | fill a `DkSampler`, `dkSamplerDescriptorInitialize()` once, reuse the resulting descriptor (§5) |

None of these require "does this combination already exist, and if not,
build it" logic — filling a bitfield struct and calling a bind function has
no meaningful cost to amortize. An optional, purely-CPU-side optimization is
to skip the bind call if the new state equals what's already bound (a simple
`==` check against the last-bound struct), exactly mirroring what a GL driver
already does internally — but this is a minor efficiency tweak, not a
correctness-required cache.

## 4. What actually still needs a lookup: shaders

`TextureMode` and `EffectMode` remain the one genuinely shader-selecting
axis (per the earlier discussion: `TextureMode_Glow`'s own doc comment,
`RageTypes.h:31-32`, describes real per-fragment math — "replace color with
white, keep alpha" — that has to be a different compiled program, not a
struct field). Confirmed from the SDK: `DkShader` (`deko3d.h:293-307,
1335-1337`) is a precompiled binary loaded from a `DkMemBlock`:

```c
typedef struct DkShaderMaker {
    DkMemBlock codeMem;   // GPU-visible memory holding the compiled shader
    const void* control;
    uint32_t codeOffset;
    uint32_t programId;
} DkShaderMaker;

void dkShaderInitialize(DkShader* obj, DkShaderMaker const* maker);
```

There is no runtime shader *compilation* call anywhere in the SDK — `uam`
already produced the compiled binary offline (per `08-Deko3D-Feasibility.md`).
So this isn't a lazily-populated cache with a build cost to avoid — it's:

1. At backend `Init()`, load the (small, fixed) set of precompiled shader
   binaries into one or more `DkMemBlock`s (flagged `DkMemBlockFlags_Code`,
   `deko3d.h:180`) and call `dkShaderInitialize()` once per shader, storing
   the resulting `DkShader` objects in a small fixed array or `switch`.
2. At draw time, pick the right `DkShader` for the current
   `(TextureMode, EffectMode)` pair and call `dkCmdBufBindShaders()`.

**Phase-1 shader inventory** (unchanged from the previous version of this
doc): one vertex shader (transform position, pass color/texcoord through),
plus fragment shaders for `TextureMode_Modulate` (`out = texture(tex,t) * c`)
and `TextureMode_Glow` (`out = vec4(1,1,1,texture(tex,t).a) * c`). Everything
else — `TextureMode_Add`, and all nine non-`Normal` `EffectMode` values
(including `EffectMode_DistanceField`, used by SDF font rendering per
`Font::IsDistanceField()`, `Font.h:186`) — is explicitly deferred, each being
its own shader-authoring task.

## 5. Samplers and textures — real descriptor mechanism (verified)

Confirmed from `deko3d.h:83-84,705-718,1348`:

```c
DK_DECL_OPAQUE(ImageDescriptor, 4, 32);      // 32-byte opaque descriptor
DK_DECL_OPAQUE(SamplerDescriptor, 4, 32);

void dkSamplerDescriptorInitialize(DkSamplerDescriptor* obj, DkSampler const* sampler);
void dkImageDescriptorInitialize(DkImageDescriptor* obj, DkImageView const* view, bool usesLoadOrStore, bool decayMS);

DkResHandle dkMakeImageHandle(uint32_t id);                 // pack an image-descriptor-table index
DkResHandle dkMakeSamplerHandle(uint32_t id);                // pack a sampler-descriptor-table index
DkResHandle dkMakeTextureHandle(uint32_t imageId, uint32_t samplerId); // combine both into one bindable handle
```

Descriptors live in a GPU-visible descriptor table (bound once via
`dkCmdBufBindImageDescriptorSet`/`dkCmdBufBindSamplerDescriptorSet`,
`deko3d.h:1265-1266`) and are referenced by table index, not by pointer. This
confirms the original design's instinct in two separate ways:

- **Samplers**: only 4 real combinations exist for Phase 1
  (`TextureWrapping` × `TextureFiltering`, both bool). Build all 4
  `DkSamplerDescriptor`s once at `Init()`, store them at 4 fixed slots in the
  sampler descriptor table, and pick a slot ID by a simple 2-bit index at
  bind time. No lazy cache needed — eager, fixed-size, exactly as the
  original design proposed, just confirmed against real descriptor-table
  mechanics rather than assumed.
- **Textures**: each loaded `RageTexture` needs exactly one `DkImage` +
  `DkImageDescriptor`, created once (in `RageDisplay_Deko3D::CreateTexture()`,
  matching where `RageTexture::GetTexHandle()`, `RageTexture.h:18`, expects a
  handle to come from) and destroyed once (in `DeleteTexture()`). This isn't
  a new cache to design — it maps directly onto the texture object lifecycle
  `RageTextureManager` (`03-TextureSystem.md`) already manages; the deko3d
  backend just needs to store the resulting descriptor-table index as the
  `uintptr_t` handle `RageTexture` carries around.

`SetTexture(TextureUnit, uintptr_t)` (`RageDisplay.h:280`) then becomes:
combine the stored image-descriptor index with the current sampler slot via
`dkMakeTextureHandle(imageId, samplerId)`, and pass the result to
`dkCmdBufBindTextures()`.

## 6. Vertex layout — also a separate bind, but see the correction below

`DkVtxAttribState`/`DkVtxBufferState` (`deko3d.h:1099-1115`) are bound
independently of everything else (`dkCmdBufBindVtxAttribState`/
`dkCmdBufBindVtxBufferState`, `deko3d.h:1274-1275`). For Phase 1, every
sprite draw uses the same `RageSpriteVertex` layout (`p`/`n`/`c`/`t`,
`RageTypes.h:358-365`).

**Correction, found while cross-checking `Example04_TexturedCube.cpp`
(devkitPro official examples, `SampleFramework`-based)**: the original
wording here said this bind happens "once at `Init()` and never touched
again." That's only true in the degenerate case where *nothing else on the
same queue ever binds a different vertex-attrib-state*. Confirmed from the
example: bound GPU state persists across separately-submitted command lists
on the same queue (its static `render_cmdlist`, containing the one-time
`bindVtxAttribState` call, remains in effect across per-frame submissions of
a *separate* dynamic command list that only does `pushConstants` — see §9
below) — so "bind once, forget" is real, but only as long as the bound value
doesn't need to change. Once Phase 2 adds `Model` rendering (`RageModelVertex`
layout, `RageTypes.h:370-385`) interleaved with `Sprite` draws in the same
frame — which `Actor`/`ActorFrame` freely allows, since both can be children
of the same `ActorFrame` (`02-ActorSystem.md`) — the backend needs a
last-bound-value check and rebind-on-change, exactly like the other state
axes in §3, not a true set-once-forever bind. This doesn't change Phase 1's
scope (only `RageSpriteVertex` is ever used), but the phrasing needs to not
overclaim permanence for a value that Phase 2 will need to change.

## 7. Sketch: the corrected `DrawQuadsInternal`

```cpp
void RageDisplay_Deko3D::SetBlendMode(BlendMode mode)   { m_pending.blendMode = mode; }
void RageDisplay_Deko3D::SetCullMode(CullMode mode)      { m_pending.cullMode = mode; }
void RageDisplay_Deko3D::SetZTestMode(ZTestMode mode)     { m_pending.zTestMode = mode; }
void RageDisplay_Deko3D::SetZWrite(bool b)                 { m_pending.zWrite = b; }
void RageDisplay_Deko3D::SetTextureMode(TextureUnit, TextureMode tm) { m_pending.textureMode = tm; }
void RageDisplay_Deko3D::SetEffectMode(EffectMode em)        { m_pending.effectMode = em; }
void RageDisplay_Deko3D::SetTexture(TextureUnit tu, uintptr_t h)      { m_pendingImageId[tu] = (uint32_t)h; }
void RageDisplay_Deko3D::SetTextureWrapping(TextureUnit tu, bool b)    { m_pendingSamplerId[tu] = SamplerSlot(b, m_pendingFilter[tu]); }
void RageDisplay_Deko3D::SetTextureFiltering(TextureUnit tu, bool b)    { m_pendingSamplerId[tu] = SamplerSlot(m_pendingWrap[tu], b); }

void RageDisplay_Deko3D::DrawQuadsInternal(const RageSpriteVertex v[], int n)
{
    // Direct, cheap state translation — no lookup/creation, just fill+bind:
    DkBlendState        bs = TranslateBlendMode(m_pending.blendMode);
    DkRasterizerState    rs = TranslateCullMode(m_pending.cullMode);
    DkDepthStencilState   ds = TranslateZState(m_pending.zTestMode, m_pending.zWrite);
    m_cmdbuf.bindBlendStates(0, bs);
    m_cmdbuf.bindRasterizerState(rs);
    m_cmdbuf.bindDepthStencilState(ds);

    // Small fixed lookup — this IS the one real "cache," and it's just an array index:
    DkShader const* shaders[2] = { &m_vertexShader, &m_fragmentShaders[ShaderIndex(m_pending.textureMode, m_pending.effectMode)] };
    m_cmdbuf.bindShaders(DkStageFlag_Vertex | DkStageFlag_Fragment, shaders);

    DkResHandle tex = dkMakeTextureHandle(m_pendingImageId[TextureUnit_1], m_pendingSamplerId[TextureUnit_1]);
    m_cmdbuf.bindTextures(DkStage_Fragment, 0, tex);

    // upload v[0..n) into this frame's vertex sub-allocation, then:
    m_cmdbuf.draw(DkPrimitive_Quads, n, 1, 0, 0);
}
```

This keeps every existing call site in `Sprite.cpp`/`Actor.cpp` completely
unmodified, and confirms the payoff noted in `08-Deko3D-Feasibility.md` §1:
`RageDisplay` being a clean abstraction boundary means all of this
translation is internal to the new backend alone.

## 8. A better pattern found in `deko_console` — not Phase 1 baseline, but a real future optimization

The remaining example projects (`Example01`/`02`/`03`/`05`/`06`/`09`, and
`deko_console`) were scanned for anything else relevant to Phase 1. Most
confirmed things already found or turned out architecturally irrelevant
(noted briefly at the end of this section); one — `deko_console` — surfaced a
genuinely valuable technique worth recording even though it isn't part of
Phase 1's baseline design.

### The technique: one instanced draw call, updated by writing directly into GPU memory

`deko_console/source/gpu_console.c` implements libnx's debug text console
entirely on deko3d. Unlike every other example, it **never re-records its
draw command**, even though its content (the printed text) changes
constantly:

```c
// GpuRenderer_init(), gpu_console.c:392-393 — built ONCE, at startup:
dkCmdBufDraw(r->cmdbuf, DkPrimitive_Triangles, 3, totalConSize, 0, 0);
//                                              ^3 verts   ^one instance per character cell
r->cmdsRender = dkCmdBufFinishList(r->cmdbuf);

// GpuRenderer_drawChar(), gpu_console.c:408-434 — called every time a character is printed:
dkFenceWait(&r->lastRenderFence, UINT64_MAX);          // :428 — wait for the GPU to finish reading last frame's data
ConsoleChar* pos = &r->charBuf[y*con->consoleWidth+x];  // :430 — a raw CPU pointer into GPU-visible memory
pos->tileId = c; pos->frontPal = writingColor; pos->backPal = screenColor; // :431-433 — direct write, no command-buffer call at all

// GpuRenderer_flushAndSwap(), gpu_console.c:452-466 — called once per frame:
dkQueueSubmitCommands(r->queue, r->cmdsRender);        // :463 — re-submits the SAME unchanged command list
dkQueueSignalFence(r->queue, &r->lastRenderFence, false); // :466
```

The whole console grid renders as **one instanced draw call**
(`instanceCount = totalConSize`, one "tile" triangle per character cell,
positioned via `gl_InstanceID` in `console_vsh.glsl:19-25`). Per-character
data (`tileId`/`frontPal`/`backPal`) lives in a per-instance vertex buffer
(`DkVtxBufferState{ .divisor=1 }`, `gpu_console.c:86` — confirmed real field,
`deko3d.h:1111-1115`). Updating on-screen text is just a CPU memory write
into that buffer, fence-guarded against the GPU still reading the previous
frame's contents — no draw-command re-recording, no state rebinding, no
per-character draw call.

### Why this matters for StepMania, and why it's not Phase 1's baseline

This directly applies wherever many quads share the same texture, shader,
and blend/effect state and only differ in position/color/texcoord-offset —
which describes **`BitmapText` glyph rendering** almost exactly: all glyphs
drawn from one `FontPage` share one texture
(`FontPageTextures.m_pTextureMain`, `Font.h:19`, per `04-FontSystem.md`), and
`BitmapText::BuildChars()` already accumulates all of a string's glyph quads
into one `vector<RageSpriteVertex> m_aVertices` (`BitmapText.h:129`) before
`DrawChars()` submits them — i.e. **the CPU-side data is already batched**;
today it's just submitted as one `RageDisplay::DrawQuads()` call per
`BitmapText` actor rather than one instanced draw covering many actors'
worth of glyphs. The same could apply to note-skin arrows in gameplay, which
typically share one texture atlas per skin. Given the original ask that
started this whole investigation was "performance is poor," this is a
concrete, evidenced lever for reducing draw-call count beyond what deko3d
alone buys — worth a dedicated future design pass.

**It is not part of Phase 1's baseline**, for two reasons:
- Phase 1 draws heterogeneous, arbitrary sprites (different textures, blend
  modes, effect modes per `Actor`) — the general case this document already
  designed for (§9's "record once doesn't apply" finding still stands for
  that general case). Batching only helps the sub-case where consecutive
  draws happen to share identical state, which requires either grouping
  logic in the backend or theme/content-level texture-atlasing discipline —
  real additional design work, not something to retrofit into Phase 1's
  correctness-first pass.
- The example's own synchronization is naive: **one** buffer, **one** fence
  — `GpuRenderer_drawChar()` will genuinely block the CPU
  (`dkFenceWait(UINT64_MAX)`, `:428`) if called before the GPU finishes
  reading the previous frame's data. That's an acceptable trade-off for a
  debug console (updated occasionally, blocking briefly is fine) but would
  reintroduce a real per-frame CPU/GPU stall if naively applied to content
  that changes every single frame (e.g. `NoteField` during gameplay). Adopting
  this pattern for genuinely-every-frame-changing content would need the same
  N-slice ring-buffering `CCmdMemRing` already does for command memory
  (§9) applied to the *instance data* buffer too — a combination this
  example doesn't itself demonstrate, so it remains a design task, not a
  fully-proven reference.

### Blend-enable mechanism, now positively confirmed (not just inferred from absence)

`09-Deko3D-SpritePipelineCache.md`'s §3 correction (blend needs both a
`DkBlendState` and a separate `DkColorState` enable bit) was originally
inferred from `deko_basic` *never* enabling blending. `Example05_Tessellation`
(read for its blend-configuration bullet point) confirms it positively:

```cpp
// Example05_Tessellation.cpp:184-208
dk::ColorState colorState;
dk::BlendState blendState;
colorState.setBlendEnable(0, true);   // :194 — the separate enable bit, confirmed in a real "turn blending on" case
// ... rasterizerState.setPolygonMode(DkPolygonMode_Line) etc. (unrelated to blending) ...
cmdbuf.bindColorState(colorState);
cmdbuf.bindBlendStates(0, blendState);
```

### The rest: confirmed subsets or confirmed irrelevant

- **`Example01_SimpleSetup`/`Example02_Triangle`** — device/queue/memory/
  framebuffer/shader/vertex-buffer setup, all already covered more
  completely by `Example04_TexturedCube` (§9); no new information.
- **`Example03_Cube`** — depth buffer, uniform buffers, resolution-switching
  on `onOperationMode()`, depth-discard-after-barrier: all already found via
  `Example04`/`Example07` (§9, §10); no new information. (Its listed topics
  do *not* include blending — that was `Example05`'s, checked above.)
- **`Example06_Multisampling`** — confirms the same MSAA/resolve/discard
  pattern already found and ruled out of scope via `Example07` (§10); no new
  information.
- **`Example09_SimpleCompute`** — compute shaders, SSBOs, GPU-driven
  geometry. `RageDisplay.h`'s full interface exposes no compute-related
  method at all (confirmed in `01-RageDisplay.md`) — architecturally
  unrelated to anything `RageDisplay` needs, Phase 1 or Phase 2.

## 9. Validated against devkitPro's own official deko3d examples

The SDK install also ships example projects at
`/opt/devkitpro/examples/switch/graphics/deko3d/` (`deko_basic`,
`deko_console`, `deko_examples` — the latter containing 9 numbered examples
built on a shared `SampleFramework/`). These are real, working,
Nintendo-homebrew-ecosystem-standard reference implementations, not just API
declarations — reading them surfaced both confirmations and gaps.

### Confirmed correct (previously assumed, now directly evidenced)

- **Shaders are hand-written GLSL** (`#version 460`, e.g.
  `deko_basic/source/triangle_vsh.glsl`), resolving open item #2 below.
- **`uam`'s exact CLI**, from `deko_basic/Makefile:190-208`:
  `uam -s vert -o out.dksh in.glsl` (stage flag is one of `vert`/`tess_ctrl`/
  `tess_eval`/`geom`/`frag`/`comp`, inferred by convention from a
  `_vsh`/`_tcsh`/`_tesh`/`_gsh`/`_fsh`/plain suffix on the source filename).
  Compiled `.dksh` output is placed under `romfs/shaders/`, confirming
  `08-Deko3D-Feasibility.md`'s claim that shaders ship in the NRO's RomFS.
- **Shader loading mechanism** (`deko_basic/source/main.c:43-65`,
  `SampleFramework/CShader.cpp`): read the `.dksh` file, copy its bytes into a
  `DkMemBlock` flagged `DkMemBlockFlags_Code`, call `dkShaderMakerDefaults()`
  + `dkShaderInitialize()` — exactly the mechanism §4 above already described.
- **Descriptor-set mechanism for textures/samplers**
  (`SampleFramework/CDescriptorSet.h`): one fixed-size `DkMemBlock` region
  holds `N` descriptors; `bindForImages()`/`bindForSamplers()` bind the whole
  table once; individual slots are written via `cmdbuf.pushData()` at
  `baseAddr + id*DescriptorSize`. Matches §5's design exactly —
  `Example04_TexturedCube.cpp:194-208` shows the actual sequence: initialize
  a `DkSampler`, call `dkSamplerDescriptorInitialize()`, `update()` the
  descriptor-set slot, then `bindForImages()`/`bindForSamplers()`.
- **Uniform buffers carry per-frame transform data**
  (`Example04_TexturedCube.cpp:327,356-358`): bind a `DkGpuAddr` range once
  via `dkCmdBufBindUniformBuffer`, then update its contents per frame via
  `dyncmd.pushConstants(addr, size, offset, dataSize, &data)`. This resolves
  what was open item #4 below.

### Resolved: `RageMatrix`'s raw bytes need no transpose or repacking for a GLSL `mat4`

A real question this raised but didn't answer at the time: every example
vertex shader does `gl_Position = u.projMtx * worldPos` — a GLSL `mat4`
uniform expects genuine column-major float layout. `RageMatrix`'s own doc
comment (`RageTypes.h:388-393`) is confusingly worded ("elements are
specified in row-major order... consistent with... Direct3D") and doesn't
settle whether its *physical byte layout* matches what GLSL needs.

Resolved by checking what the working GL backend actually does, rather than
reasoning about the comment: `RageDisplay_OGL.cpp:1024,1030,1033,1113,1446`
all call `glLoadMatrixf((const float*)&mat)` — a **direct pointer cast, no
`GetTranspose()` call anywhere** (`RageMatrix::GetTranspose()` exists,
`RageTypes.h:413`, but is never invoked near any of these sites). Since
`glLoadMatrixf` is universally defined to expect column-major, column-vector
layout — the exact same convention GLSL's `mat4` uniforms use — and this is
the layout `RageDisplay_Legacy` has shipped correctly with all along,
**`RageMatrix`'s raw 16 floats can be `memcpy`'d directly into a GLSL `mat4`
uniform via `pushConstants`, with no transpose and no repacking.** The
confusing header comment describes the logical `operator()(row,col)`
indexing convention exposed to C++ callers, not the physical layout that
matters for GPU upload — those are different things, and only the physical
layout matters here.

### Genuine gap found: blend state needs a second, separate enable bit

`deko_basic/source/main.c:152-163` binds `DkRasterizerState`/`DkColorState`/
`DkColorWriteState` but **never binds a `DkBlendState` at all**, because
`dkColorStateDefaults()` sets `blendEnableMask = 0x00` (`deko3d.h:895`) —
blending is off by default, per render target, via a *separate* bit in
`DkColorState`, toggled with `dkColorStateSetBlendEnable(state, targetId,
bool)` (`deko3d.h:900-906`). The original design in §3 only mentioned filling
`DkBlendState` — it missed that `SetBlendMode()`'s translation also has to
flip the corresponding bit in `DkColorState` (already fixed in §3's table,
above). A `BLEND_NO_EFFECT`-style mode most likely maps to blending disabled
entirely (skip `DkBlendState` and clear the `DkColorState` bit), not to a
same-shaped `DkBlendState` with trivial factors.

### Genuine gap found: the "record once" pattern does not apply to StepMania's actor tree, and this needs to be explicit

Both `deko_basic/source/main.c` and `Example04_TexturedCube.cpp` **bake their
actual draw commands into one static `DkCmdList` built once**
(`main.c:156-165`; `Example04_TexturedCube.cpp:309-348`'s
`recordStaticCommands()`), and merely re-submit that same unchanged list every
frame, updating only a small uniform buffer's *contents* via a tiny separate
dynamic command list (`Example04_TexturedCube.cpp:352-361`, using
`CCmdMemRing`, see below). This works because both examples draw exactly the
same fixed geometry with the same shader/texture/state, forever.

**This pattern does not carry over to `Sprite`/`Actor` rendering.** Within a
single frame, StepMania draws a varying number of actors, each potentially
with a different bound texture, different `TextureMode`/`EffectMode` (hence
different shader), different blend/cull/z state, and freshly-recomputed
vertex data (`Sprite.cpp` rebuilds `v[4]` from scratch — cropping, tweened
position, per-corner diffuse color — on every single draw, per §1). None of
that is a fixed quantity the way `Example04`'s cube geometry and texture are.
So Phase 1's design correctly requires genuine per-frame command
*re-recording* (§7's sketch), not the bake-once-and-patch-uniforms
optimization the official examples showcase — this is worth stating
explicitly so a future contributor skimming the official examples doesn't
try to force StepMania's heterogeneous actor tree into the wrong pattern.

### Resolved: the per-frame dynamic command-buffer pattern (was open item #3)

`SampleFramework/CCmdMemRing.h` is exactly the reference implementation for
this. A ring of `NumSlices` memory regions (`Example04_TexturedCube.cpp` uses
`NumFramebuffers` = 2), each with its own `dk::Fence`:

```cpp
void begin(dk::CmdBuf cmdbuf) {
    cmdbuf.clear();                                    // reset recorded-command bookkeeping (not the memory bytes)
    m_fences[m_curSlice].wait();                        // wait until the GPU is done reading this slice's PREVIOUS contents
    cmdbuf.addMemory(m_mem.getMemBlock(), sliceOffset, sliceSize);
}
DkCmdList end(dk::CmdBuf cmdbuf) {
    cmdbuf.signalFence(m_fences[m_curSlice]);            // mark this slice done once the GPU finishes it
    m_curSlice = (m_curSlice + 1) % NumSlices;             // round-robin to the next slice
    return cmdbuf.finishList();
}
```
(`CCmdMemRing.h:28-61`, comments paraphrased)

This is the recommended way to implement Phase 1's per-frame sprite command
recording: allocate a small ring (2-3 slices is standard for double/triple
buffering with the swapchain), `begin()` at the start of each frame's sprite
pass, record every `DrawQuadsInternal()` call's state binds + draw into it,
`end()` and submit at frame end. Directly resolves what was open item #3.

### New gap, not previously considered: docked/handheld resolution switching

`Example04_TexturedCube.cpp:376-386` overrides `onOperationMode(mode)` —  a
libnx applet callback fired when the Switch changes between docked (up to
1080p) and handheld (720p) — and responds by tearing down and rebuilding the
entire framebuffer/swapchain/depth-buffer set
(`destroyFramebufferResources()`/`createFramebufferResources()`). None of the
prior deko3d documents in this project mentioned this at all. It maps
directly onto `RageDisplay::ResolutionChanged()` (`RageDisplay.h:242`) and
`TryVideoMode()` (`RageDisplay.h:385`, both covered in `01-RageDisplay.md`) —
`RageDisplay_Deko3D` needs to hook the same libnx callback and drive those
existing virtual methods from it. This is real, previously-unscoped work,
though small and well-precedented by the example.

### New gap, not previously considered: texture format/compression is an explicit choice, not a given

`Example04_TexturedCube.cpp:189` loads its texture as pre-compressed
`DkImageFormat_RGB_BC1` data shipped as a raw `.bc1` blob in RomFS — i.e. the
official examples' "best practice" is to convert textures to a GPU-native
compressed format **offline**, analogous to the shader-compilation step.
StepMania's existing texture pipeline (`RageBitmapTexture`/`RageSurface`,
`03-TextureSystem.md`) instead decodes PNG/JPG/BMP to raw RGBA8 pixel data
**at runtime**, in CPU memory. For Phase 1, the correct, explicitly-scoped
choice is to map that decoded RGBA8 data straight onto
**`DkImageFormat_RGBA8_Unorm`** (`deko3d.h:412`, uncompressed, no format
conversion needed) rather than adopt the examples' compressed-texture
pipeline — that keeps Phase 1's texture path a thin translation of the
existing `RageTextureManager` lifecycle (`03-TextureSystem.md`), deferring
GPU-native texture compression (real memory/bandwidth win, real added
build-pipeline complexity) as an explicit, separate future optimization
rather than an implicit Phase-1 requirement.

### New gap, not previously considered: depth-buffer discard as a tile-GPU efficiency pattern

`Example04_TexturedCube.cpp:340-344` calls `cmdbuf.barrier(DkBarrier_Fragments,
0)` followed by `cmdbuf.discardDepthStencil()` after its draw, to avoid the
GPU writing back depth data nothing will read again — a real Tegra/tile-based
efficiency pattern (`DkBarrier`'s "similar to Vulkan renderpasses" doc
comment, `deko3d.h:337`). Since `Sprite` rendering does exercise Z-test/
Z-write (`Actor::SetGlobalRenderStates()`, `Actor.cpp:766-767`), this is
relevant to Phase 1, not just Phase 2's `Model` depth-testing — worth
including in the eventual per-frame render pass, not discovered independently
in the earlier design pass.

### General memory-management scale, now evidenced rather than assumed

`SampleFramework/CMemPool.h` (confirmed by reading in full) is a genuine
free-list allocator with block coalescing, built on an intrusive
list/red-black-tree (`CIntrusiveList`/`CIntrusiveTree`), managing one or more
large (default 8 MiB) `DkMemBlock`s and carving out sub-allocation `Handle`s
on demand. This directly evidences (rather than just asserts) the "manual GPU
memory management is real, unavoidable work" claim in
`08-Deko3D-Feasibility.md` §3 — the official examples don't get away with a
handful of `dkMemBlockCreate()` calls either; they need this same class of
general-purpose allocator, which is a reasonable minimum-scope target for
whatever the deko3d `RageDisplay` backend builds for itself.

## 10. Phase 2 spot-check: `Example07_MeshLighting.cpp` against `Model` lighting

This document is scoped to Phase 1 (`Sprite`/`Quad`), but `Model` lighting
was mentioned throughout as Phase 2 (`08-Deko3D-Feasibility.md` §5.2, §9's
new-item list). Since devkitPro ships a mesh-lighting example
(`Example07_MeshLighting.cpp`, 407 lines, plus `transform_normal_vsh.glsl`
and `basic_lighting_fsh.glsl`, read in full), it's worth spot-checking Phase
2's assumptions against it now, before any Phase 2 design work starts.

### Confirmed: Blinn-Phong is the right technique, and here's the real shader

`basic_lighting_fsh.glsl:15-46` does exactly what `02-ActorSystem.md`'s
Phase-2 assumption named: per-fragment N·L diffuse plus a halfway-vector
specular term raised to a shininess exponent —

```glsl
vec3 lightDir = normalize(u.lightPos.xyz - inWorldPos);   // or -u.lightPos.xyz if directional (w==0)
float diffuse = max(0.0, dot(normal, lightDir));
vec3 halfwayDir = normalize(lightDir + normalize(-inWorldPos));
float specular = pow(max(0.0, dot(normal, halfwayDir)), u.specular.w);  // .w = shininess
vec3 color = u.ambient + u.diffuse*diffuse + u.specular.xyz*specular;
```

This confirms "write an actual Blinn-Phong-equivalent fragment shader" was
the right framing, not an overstatement.

### Genuine gap: the example bakes Material×Light together; StepMania keeps them separate and multiplies at runtime

This is the one real correction needed. `Example07`'s `Lighting` uniform
(`ambient`/`diffuse`/`specular`, `Example07_MeshLighting.cpp:56-62,151-154`)
is initialized with hardcoded values —
`{0.046227,0.028832,0.003302}` / `{0.564963,0.367818,0.051293}` /
`24*{0.394737,0.308916,0.134004}` — which are the classic fixed "polished
gold" material preset numbers. **This example has no separate light and
material concept at all; it bakes one fixed light-response for one fixed
"gold" mesh.**

StepMania's real API is not that. Confirmed from actual call sites:

```cpp
// ActorFrame.cpp:216 — the LIGHT, set once per frame/scene:
DISPLAY->SetLightDirectional( 0, m_ambientColor, m_diffuseColor, m_specularColor, m_lightDirection );

// Model.cpp:344,419,446 — the MATERIAL, set per mesh, from the loaded msMaterial:
DISPLAY->SetMaterial( Emissive, Ambient, Diffuse, mat.Specular, mat.fShininess );
```

These are two **independently-settable** things — `RageDisplay.h:319-334`
(`SetMaterial`/`SetLightDirectional`, both covered in `01-RageDisplay.md`) —
that classic GL fixed-function lighting multiplies together per-component at
draw time (`finalAmbient = matAmbient * lightAmbient`, `finalDiffuse =
matDiffuse * lightDiffuse * NdotL`, `finalSpecular = matSpecular *
lightSpecular * spec^shininess`, all summed with `matEmissive` added
unconditionally). None of that multiplication or the emissive term appears
anywhere in `basic_lighting_fsh.glsl`.

So Phase 2's actual fragment shader needs to be a genuine extension of this
example's technique, not a direct port of it: **two separate uniform blocks
(Material: emissive/ambient/diffuse/specular/shininess; Light: ambient/
diffuse/specular/direction), multiplied together in-shader, plus the
emissive term added unconditionally** — more shader-authoring work than the
reference example's single-baked-response version, though the core N·L /
halfway-vector math it demonstrates carries over directly.

### Confirmed: exactly one light in practice — codebase-wide

Searched `ActorFrame.cpp`/`Model.cpp` for `SetLightDirectional` call sites:
there is exactly **one**, always with `index=0` (`ActorFrame.cpp:216`).
`RageDisplay_Legacy::SetLightDirectional()`/`SetLightOff()`
(`RageDisplay_OGL.cpp:2034-2056`) map `index` straight onto GL's
`GL_LIGHT0+index`, so the abstraction technically supports multiple lights,
but nothing in this codebase actually drives more than one. Phase 2's Light
uniform block can safely be sized for a single light, matching
`Example07`'s single-`Lighting`-uniform shape — this part of the example's
structure *does* carry over directly, it's specifically the
material/light-separation that needs adding.

### Confirmed clean: vertex/index buffer upload path, no new problem to solve

`Example07` loads pre-baked binary vertex/index files
(`teapot-vtx.bin`/`teapot-idx.bin`) directly into `DkMemBlock`-backed buffers
and draws with `dkCmdBufBindIdxBuffer(DkIdxFormat_Uint16, ...)` +
`drawIndexed()`. StepMania already parses meshes on the CPU into
`vector<RageModelVertex>` + `vector<msTriangle>` (`ModelTypes.h:14-27`, via
`Model::LoadMilkshapeAscii()`, `Model.h:26`) — `msTriangle` is already
`uint16_t nVertexIndices[3]` (`ModelTypes.h:8-11`), an exact match for
`DkIdxFormat_Uint16`. So Phase 2 doesn't need any new mesh-loading logic —
just a `RageCompiledGeometry` subclass (the existing per-backend abstraction
point, `RageDisplay.h:26-52,339-340`, covered in `05-GeometryAndPrimitives.md`)
whose `Allocate()`/`Change()` upload already-parsed `RageModelVertex` data
into a `DkMemBlock`, exactly mirroring what this example's vertex/index
buffer setup already does by hand.

### Correction: vertex layout needs 4 attributes, not this example's 2

`Example07`'s `Vertex{position[3], normal[3]}` (`Example07_MeshLighting.cpp:33-37`)
has no texture coordinate and no bone data. StepMania's real
`RageModelVertex` (`RageTypes.h:370-385`, verified in `05-GeometryAndPrimitives.md`)
has `p, n, t (texcoord), bone, TextureMatrixScale` — a superset. Phase 2's
`DkVtxAttribState` array needs 4 (or 5, if `TextureMatrixScale` gets its own
slot) entries, not this example's 2 — mechanically the same
`DkVtxAttribState{bufferId, ..., offsetof(...), size, type, ...}` pattern
(`deko3d.h:1099-1109`), just a bigger array. Not a blocker, just a scope
correction against directly copying this example's attribute table.

### Correctly excluded from scope: MSAA and sRGB framebuffers

`Example07` renders to an offscreen 4x-multisampled, sRGB color buffer
(`DkMsMode_4x`, `DkImageFormat_RGBA8_Unorm_sRGB`,
`Example07_MeshLighting.cpp:75,183-188`) and explicitly resolves it into the
presentable framebuffer (`cmdbuf.resolveImage()`, `:230`) before discarding
the offscreen buffers (`:320-326`). **Nothing in `RageDisplay.h` exposes a
multisampling or sRGB-framebuffer control** (confirmed: the full 492-line
header has no such method) — this is purely this example's own
visual-quality embellishment for showing off deko3d's MSAA support, not
something `RageDisplay`'s fixed-function-equivalent API requires or Phase 2
needs to replicate. Worth stating explicitly so this doesn't get mistaken for
required scope: Phase 2 can render directly into the swapchain framebuffer,
single-sampled, exactly like Phase 1's sprite path, with no separate resolve/
discard step.

### Correctly excluded from scope: `Example08_DeferredShading.cpp`

Also checked, and this one is a clean "not relevant" — worth recording the
reasoning rather than silently skipping it, since deferred shading is a
plausible-sounding next thing to check for a lighting-related task.

The example's own header comment states its purpose precisely: "a multipass
rendering technique that goes well with the tiled cache"
(`Example08_DeferredShading.cpp:3`). It renders geometry once into a 3-target
G-buffer (`albedo`/`normal`/`viewDir`, each `DkImageFormat_RGBA16_Float`,
`:201-207`, bound via `cmdbuf.bindRenderTargets({&albedoTarget, &normalTarget,
&viewDirTarget}, &depthTarget)`, `:324`), explicitly enables Tegra's
tile-cache mode to keep that G-buffer resident in fast on-chip memory across
passes (`cmdbuf.setTileSize(64,64)` + `tiledCacheOp(DkTiledCacheOp_Enable)`,
`:185-186`), then runs a **second, fullscreen composition pass**
(`composition_fsh.glsl`) that samples the G-buffer via `texelFetch()` and
computes lighting once per screen pixel rather than once per (object ×
light) pair.

That decoupling is the entire point of deferred shading: it's a win when a
scene has many lights and/or many overlapping objects, because it makes
lighting cost proportional to screen pixels instead of to `objects × lights`.
Per §10 above, StepMania's actual usage is confirmed to be **exactly one
light, ever** (`ActorFrame.cpp:216`, the only `SetLightDirectional()` call
site in the codebase), applied to comparatively few `Model` draws per frame
(3D models are a minority of on-screen content next to `Sprite`/`BitmapText`,
per `02-ActorSystem.md`). With one light, deferred shading has no
scaling advantage to offer — it would add G-buffer memory (three
`RGBA16_Float` render targets, `:205`, considerably more memory and bandwidth
than a single `RGBA8_Unorm` framebuffer), an extra full-screen pass, and
tile-cache configuration overhead, for zero benefit over Phase 2's plan of a
single forward-shaded draw per mesh (§10's Material×Light-in-one-pass
approach). Correctly out of scope.

One secondary, minor data point worth keeping: `composition_fsh.glsl:43-46`
does `albedo.rgb * u.diffuse * diffuse` — i.e. it multiplies a per-pixel
albedo term into the light's diffuse channel, one step closer to
StepMania's real material×light separation than `Example07`'s fully-baked
version (§10), though it still only modulates the diffuse channel and uses a
single flat `albedo` value rather than a full material struct (no separate
ambient/specular/emissive material color). Doesn't change §10's conclusion —
Phase 2's shader still needs the fuller Material struct — but confirms the
"multiply material color into the light's diffuse term" shape is a real,
recurring pattern across the SDK's own examples, not something invented for
this project.

## 12. Open items before implementation starts (updated)

1. ~~Confirm deko3d's actual state-object granularity~~ — **resolved**: no
   monolithic pipeline; state is independently bindable (§2-3).
2. ~~Confirm `uam`'s shader-source-language requirements~~ — **resolved**:
   GLSL, compiled via `uam -s <stage> -o out.dksh in.glsl` (§9).
3. ~~Design the per-frame vertex/command ring-buffer~~ — **resolved**:
   `CCmdMemRing`'s slice+fence pattern is the reference implementation (§9).
4. ~~Decide how matrices reach the vertex shader~~ — **resolved**: a uniform
   buffer, bound once, updated per-frame via `pushConstants` (§9).
5. ~~Decide the texture pixel-format mapping~~ — **resolved**:
   `11-Deko3D-TextureFormatAndShaderContract.md` — 5 of 9 `RagePixelFormat`
   values map natively to `DkImageFormat`; the rest fall back to RGBA8 via
   the exact mechanism `RageDisplay_Legacy::GetImgPixelFormat()` already
   uses, with a `LOG->Warn()` added.
6. **Hook `onOperationMode()` for docked/handheld switching** — newly
   identified in §9; not designed in this pass.
7. **Decide the general-purpose GPU memory allocator's shape** — whether to
   port/adapt `CMemPool`-equivalent logic or write a simpler bump/pool
   allocator sized for this project's actual texture/vertex/shader-code
   volume — not designed in this pass.
8. **(Phase 2) Author the Material×Light fragment shader** — per §10, this is
   more elaborate than `Example07`'s reference shader: two separate uniform
   blocks multiplied together, plus an unconditional emissive term. Not
   designed in this pass.
9. **(Phase 2) Size the `Model` vertex-attribute table for 4 attributes**
   (position, normal, texcoord, bone/`TextureMatrixScale`), not `Example07`'s
   2 — per §10. Not designed in this pass.
10. **(Future optimization, not Phase 1) Design instanced/batched glyph and
    sprite rendering** — per §8's `deko_console` finding. Would need
    grouping logic (batch consecutive draws sharing texture/shader/blend
    state) plus ring-buffered instance data (extending `CCmdMemRing`'s
    pattern to per-instance vertex data, not just command memory, to avoid
    the naive single-buffer stall `deko_console` itself has). Most
    promising first target: `BitmapText`, whose glyph quads are already
    CPU-side batched into one `vector<RageSpriteVertex>` per actor
    (`BitmapText.h:129`). Not designed in this pass.

---

**Scope**: Builds on `Sprite.cpp`/`Actor.cpp`/`RageDisplay.cpp`/`Model.cpp`/
`ActorFrame.cpp`/`RageDisplay_OGL.cpp` (light-index mapping only) as before,
cross-checked against `/opt/devkitpro/libnx/include/deko3d.h` (1429 lines,
read in full), `deko3d.hpp` (C++ wrapper, spot-checked for `CmdBuf::bind*`
naming), and devkitPro's official example projects at
`/opt/devkitpro/examples/switch/graphics/deko3d/` (`deko_basic/source/main.c`
and `.glsl` files read in full; `deko_examples/source/Example04_TexturedCube.cpp`
and `Example07_MeshLighting.cpp` read in full, plus their `.glsl` shaders;
`Example08_DeferredShading.cpp` read partially — header comment, struct/
vertex-layout section, tiled-cache/render-target bind call sites, and its
`.glsl` shaders read in full, sufficient to confirm the technique doesn't
apply here; `SampleFramework/CCmdMemRing.h`, `CDescriptorSet.h`, `CMemPool.h`,
`CShader.h` read in full; `deko_console/source/gpu_console.c` and its
`.glsl` shaders read in full; `Example01_SimpleSetup.cpp`/
`Example02_Triangle.cpp`/`Example03_Cube.cpp`/`Example06_Multisampling.cpp`/
`Example09_SimpleCompute.cpp` header comments read plus targeted grep for
blend/instancing-related calls, sufficient to confirm each is either a
subset of an already-fully-read example or architecturally unrelated;
`Example05_Tessellation.cpp` read partially, targeted at its blend-state
setup only. Every numbered example and `deko_console` in the SDK's install
has now been checked at least at header-comment level; all `.cpp` files
have been either fully read or confirmed via targeted search to contain
nothing further relevant to `RageDisplay`'s scope.
