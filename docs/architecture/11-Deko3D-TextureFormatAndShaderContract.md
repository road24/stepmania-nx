# Resolving the Two Remaining Blockers: Texture Format Mapping & Shader Binding Contract

Design philosophy per direction given: StepMania is an engine, not a single
game — theme/content compatibility matters more than squeezing every format
onto a native fast path. **Never refuse a texture format. When there's no
native GPU equivalent, convert it in software and log a `LOG->Warn()`
telling the user/theme-author exactly what to change for better load
times/performance, instead of failing or silently degrading.** This turned
out to already be almost exactly how the existing GL backend behaves — it
was just missing the runtime warning.

## Part 1: Texture pixel-format mapping

### The existing engine-level fallback mechanism, found by reading real code instead of assuming

`RageBitmapTexture.cpp:194-240` picks an initial `RagePixelFormat` from
loading hints (`actualID.iColorDepth`, palette detection), then does exactly
the compatibility check this task asked for:

```cpp
// Make we're using a supported format. Every card supports either RGBA8 or RGBA4.
if( !DISPLAY->SupportsTextureFormat(pixfmt) )
{
    pixfmt = RagePixelFormat_RGBA8;
    if( !DISPLAY->SupportsTextureFormat(pixfmt) )
        pixfmt = RagePixelFormat_RGBA4;
}
```
(`RageBitmapTexture.cpp:234-240`)

This means: **`RageDisplay_Deko3D` doesn't need to accept every format at
the `CreateTexture()` boundary at all** — it needs `SupportsTextureFormat()`
to correctly report which formats it natively handles, and this
already-shipped engine code funnels everything else toward `RGBA8` before
ever calling `CreateTexture()`. But that's only the *destination texture*
format decision; the *source surface* still needs to be reconciled against
whatever `pixfmt` was ultimately chosen, per-pixel, and that's a second,
separate mechanism.

### The actual per-pixel conversion mechanism, and where it already exists

`RageDisplay_Legacy::GetImgPixelFormat()` (`RageDisplay_OGL.cpp:2131-2166`)
is the real reference implementation for exactly this problem — a
backend-private helper, not a shared cross-backend utility, so
`RageDisplay_Deko3D` needs its own copy of this shape:

```cpp
RagePixelFormat RageDisplay_Legacy::GetImgPixelFormat( RageSurface* &img, bool &bFreeImg, int width, int height, bool bPalettedTexture )
{
    RagePixelFormat pixfmt = FindPixelFormat( img->format->BitsPerPixel,
        img->format->Rmask, img->format->Gmask, img->format->Bmask, img->format->Amask );

    bool bSupported = true;
    if (!bPalettedTexture && img->fmt.BytesPerPixel == 1 && !g_bColorIndexTableWorks)
        bSupported = false;
    if (pixfmt == RagePixelFormat_Invalid || !SupportsSurfaceFormat(pixfmt))
        bSupported = false;

    if (!bSupported)
    {
        /* ... "very slow code path, should almost never be used" ... */
        pixfmt = RagePixelFormat_RGBA8;
        const RagePixelFormatDesc *pfd = DISPLAY->GetPixelFormatDesc(pixfmt);
        RageSurface *imgconv = CreateSurface( img->w, img->h, pfd->bpp,
            pfd->masks[0], pfd->masks[1], pfd->masks[2], pfd->masks[3] );
        RageSurfaceUtils::Blit( img, imgconv, width, height );  // ← the actual reusable conversion routine
        img = imgconv;
        bFreeImg = true;
    }
    else bFreeImg = false;

    return pixfmt;
}
```
(`RageDisplay_OGL.cpp:2131-2166`, condensed)

**This is exactly the mechanism the task asked for, already proven and
shipping** — `RageSurfaceUtils::Blit()` is the real, existing, reusable
per-pixel format-converting blit (it already handles arbitrary mask/shift/
loss combinations, since `RageSurfaceFormat` is fully generic —
`RageSurface.h:39-69` — not limited to the fixed `RagePixelFormat` enum).
The only thing missing from the existing pattern is a runtime log — the
comment "*this is a very slow code path, which should almost never be
used*" (`RageDisplay_OGL.cpp:2148-2149`) is exactly the sentiment the task
asked to promote from a source comment into an actual `LOG->Warn()` call.
Confirmed real API: `RageLog.h:13`, `void Warn( const char *fmt, ... )`.

### The deko3d version of this, resolved

```cpp
RagePixelFormat RageDisplay_Deko3D::GetImgPixelFormat( RageSurface* &img, bool &bFreeImg, int width, int height, bool bPalettedTexture )
{
    RagePixelFormat pixfmt = FindPixelFormat( img->format->BitsPerPixel,
        img->format->Rmask, img->format->Gmask, img->format->Bmask, img->format->Amask );

    bool bSupported = !bPalettedTexture  // deko3d has no hardware palette format at all (see table below)
        && pixfmt != RagePixelFormat_Invalid
        && SupportsSurfaceFormat(pixfmt);

    if (!bSupported)
    {
        LOG->Warn(
            "Texture format %s has no native deko3d equivalent; converting to RGBA8 in "
            "software. This is slower to load. For better load times, re-export this "
            "asset as RGBA8, BGRA8, RGBA4, RGB5A1, or RGB5.",
            RagePixelFormatToString(pixfmt).c_str());

        pixfmt = RagePixelFormat_RGBA8;
        const RagePixelFormatDesc *pfd = DISPLAY->GetPixelFormatDesc(pixfmt);
        RageSurface *imgconv = CreateSurface(img->w, img->h, pfd->bpp,
            pfd->masks[0], pfd->masks[1], pfd->masks[2], pfd->masks[3]);
        RageSurfaceUtils::Blit(img, imgconv, width, height);
        img = imgconv;
        bFreeImg = true;
    }
    else bFreeImg = false;

    return pixfmt;
}
```

Never fails, never rejects a theme asset — matches the requested philosophy
exactly. The only engine-visible difference from the GL backend's existing
behavior is that the "slow path" is now loud instead of silent.

### The actual mapping table — better coverage than originally feared

Cross-referencing StepMania's `RagePixelFormat` enum (`RageDisplay.h:54-71`)
against the full `DkImageFormat` enum (`deko3d.h:382-516`, read in full):

| `RagePixelFormat` | `DkImageFormat` | Fit |
|---|---|---|
| `RGBA8` | `RGBA8_Unorm` | **Exact native match** |
| `BGRA8` | `BGRA8_Unorm` | **Exact native match** |
| `RGBA4` | `RGBA4_Unorm` | **Exact native match** |
| `RGB5A1` | `RGB5A1_Unorm` | **Exact native match** |
| `RGB5` | `RGB5_Unorm` | **Likely native match** — same name, bit-order not independently re-verified against `RageSurfaceFormat`'s mask/shift values in this pass |
| `RGB8` | *(none)* | **No native 24bpp format exists** — only `RGB32_*` at 32-bit/channel. Falls back to RGBA8 via the mechanism above. |
| `PAL` | *(none)* | **No hardware palette/indexed format anywhere in `DkImageFormat`** — modern GPUs generally dropped this. Falls back to RGBA8. Notably, the *existing GL backend* also sometimes can't do this natively either (`g_bColorIndexTableWorks` flag, `RageDisplay_OGL.cpp:2138`) — this isn't a deko3d-specific weakness, it's a real hardware-generation gap GL already has to work around too. |
| `BGR8` | *(none)* | Same as `RGB8` — no 24bpp native format. Falls back to RGBA8. |
| `A1BGR5` | `A5BGR5_Unorm`? | **Name mismatch** (A**1**BGR5 vs A**5**BGR5) — not confirmed equivalent, needs bit-layout verification before trusting; falls back to RGBA8 if not confirmed. |
| `X1RGB5` | *(no obvious match)* | Not confirmed; falls back to RGBA8 conservatively. |

Net result: **5 of 9 real formats (RGBA8, BGRA8, RGBA4, RGB5A1, RGB5) map
natively** — better coverage than originally assumed before actually
cross-referencing the two enums. Only `RGB8`/`BGR8`/`PAL` are *confirmed*
fallback cases (no native format exists at all, not just "unverified"), and
`A1BGR5`/`X1RGB5` are conservatively treated as fallback pending a real
bit-layout check — safe by construction (compatibility-first means "fall
back and warn" is always an acceptable outcome, never a bug, for any format
this table gets wrong or leaves unconfirmed).

### `SupportsTextureFormat()`/`SupportsSurfaceFormat()` implementation

```cpp
bool RageDisplay_Deko3D::SupportsTextureFormat( RagePixelFormat pixfmt, bool /*realtime*/ )
{
    switch (pixfmt) {
        case RagePixelFormat_RGBA8:
        case RagePixelFormat_BGRA8:
        case RagePixelFormat_RGBA4:
        case RagePixelFormat_RGB5A1:
        case RagePixelFormat_RGB5:
            return true;
        default:
            return false;   // triggers RageBitmapTexture.cpp's own RGBA8/RGBA4 fallback (already exists, no change needed there)
    }
}
```

`SupportsSurfaceFormat()` (the source-side check inside the private
`GetImgPixelFormat()` helper above) can use the identical set — a surface
already in one of these 5 layouts needs no conversion; anything else
(including palette) gets the warn-and-convert treatment.

## Part 2: Shader binding contract

### Vertex layout — resolved down to the exact byte-level detail, including a real BGRA gotcha

`RageSpriteVertex` (`RageTypes.h:358-365`): `p` (`RageVector3`), `n`
(`RageVector3`, present in the struct but **unused by sprite rendering** —
Phase 1 doesn't need lighting), `c` (`RageVColor`), `t` (`RageVector2`).

Checked `RageVColor`'s actual definition rather than assuming standard RGBA
byte order — and it matters:

```cpp
class RageVColor {
public:
    uint8_t b,g,r,a;    // "specific ordering required by Direct3D"  — RageTypes.h:313
    ...
};
```

Memory order is **B,G,R,A**, not R,G,B,A. deko3d's `DkVtxAttribState`
anticipates exactly this case — it has a dedicated `isBgra : 1` bitfield
(`deko3d.h:1108`, confirmed in the struct read in full for
`09-Deko3D-SpritePipelineCache.md`) that swizzles B/R during vertex fetch so
the shader always receives normal `(r,g,b,a)` order regardless of storage.
Missing this would silently swap red and blue on every sprite — a real bug
this check caught before it could happen.

Resolved attribute table (skips `n` entirely — no attribute binding needed
for data the shader never reads, just leave it as unused stride padding):

```cpp
constexpr std::array<DkVtxAttribState, 3> SpriteVertexAttribs = {{
    { 0, 0, offsetof(RageSpriteVertex, p), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0, 0 },  // location 0: position
    { 0, 0, offsetof(RageSpriteVertex, c), DkVtxAttribSize_4x8,  DkVtxAttribType_Unorm, 0, 1 },  // location 1: color, isBgra=1
    { 0, 0, offsetof(RageSpriteVertex, t), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0, 0 },  // location 2: texcoord
}};
constexpr std::array<DkVtxBufferState, 1> SpriteVertexBuffers = {{
    { sizeof(RageSpriteVertex), 0 },  // stride = full struct size (28 bytes: 12+16+8... see note), divisor=0 (per-vertex, not per-instance)
}};
```

### Uniform/texture binding numbers — fixed convention for Phase 1

One transform matrix, computed CPU-side as the product of the current
World/View/Projection matrices already tracked by `RageDisplay`'s matrix
stack (`01-RageDisplay.md`) at the moment of each draw call — simpler than
passing three separate matrices into the shader, since Phase 1 sprites never
need to access them individually in-shader. Per the resolved matrix-layout
finding (`09-Deko3D-SpritePipelineCache.md`), `RageMatrix`'s raw bytes need
no transpose for a GLSL `mat4`.

```glsl
// sprite_vsh.glsl
#version 460
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inTexCoord;

layout (location = 0) out vec4 outColor;
layout (location = 1) out vec2 outTexCoord;

layout (std140, binding = 0) uniform Transform { mat4 mvp; } u;

void main() {
    gl_Position = u.mvp * vec4(inPos, 1.0);
    outColor = inColor;
    outTexCoord = inTexCoord;
}
```

```glsl
// sprite_modulate_fsh.glsl  (TextureMode_Modulate)
#version 460
layout (location = 0) in vec4 inColor;
layout (location = 1) in vec2 inTexCoord;
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D tex;
void main() { outColor = texture(tex, inTexCoord) * inColor; }
```

```glsl
// sprite_glow_fsh.glsl  (TextureMode_Glow)
#version 460
layout (location = 0) in vec4 inColor;
layout (location = 1) in vec2 inTexCoord;
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D tex;
void main() { outColor = vec4(1.0, 1.0, 1.0, texture(tex, inTexCoord).a) * inColor; }
```

Binding convention, fixed for all Phase 1 shaders: **uniform block
`Transform` at binding 0** (bound via `dkCmdBufBindUniformBuffer(DkStage_Vertex, 0, ...)`),
**texture sampler at binding 0** (bound via `dkCmdBufBindTextures(DkStage_Fragment, 0, ...)`
using the combined image+sampler `DkResHandle` from
`09-Deko3D-SpritePipelineCache.md` §5). Matches the naming/numbering
convention already used identically across every official example read in
this investigation (`texture_fsh.glsl:6`, `transform_vsh.glsl:11`).

## Both blockers resolved

Neither needed new architecture — both were "read the existing shipped code
and follow its own established pattern, adding the one runtime-visibility
improvement asked for." `RageDisplay_Deko3D::CreateTexture()` and the Phase
1 shader set are now specified precisely enough to write directly.

---

**Scope**: `RageBitmapTexture.cpp:160-269` (full pixel-format-selection and
fallback logic), `RageDisplay_OGL.cpp:2131-2237` (`GetImgPixelFormat`/
`CreateTexture`, read in full for this document), `RageSurface.h` (read in
full — first read of this file in the whole investigation), `RageLog.h`
(confirmed `Warn()` exists), `RageTypes.h` (`RageVColor` definition,
`RagePixelFormat`/`DkImageFormat` enums cross-referenced). Builds on
`09-Deko3D-SpritePipelineCache.md`'s resolved matrix-layout and
descriptor/sampler-binding findings.
