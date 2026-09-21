# Rendering Primitives & Geometry (Verified)

`RageModelGeometry.h` (full), `RageMath.h` (full, 127 lines), the vertex struct
definitions in `RageTypes.h`, and the mesh/material structs in `ModelTypes.h`
were read directly. `RageDisplay.h`'s `RageCompiledGeometry` was covered in
`01-RageDisplay.md` and is only summarized here.

## RageCompiledGeometry — see 01-RageDisplay.md

Declared inside `RageDisplay.h:26-52`, not its own file. Key correction already
noted there: `Set()` is concrete, `Allocate()`/`Change()`/`Draw(int)` are the
pure-virtual backend hooks.

## Real vertex types (RageTypes.h:358-385) — two distinct structs, not one

```cpp
struct RageSpriteVertex   // "has color" — comment at :358
{
    RageVector3 p;   // position
    RageVector3 n;   // normal
    RageVColor  c;   // diffuse color — packed color type, not a generic RageColor
    RageVector2 t;   // texture coordinates
};

struct RageModelVertex    // "doesn't have color. Relies on material color" — comment at :370
{
    RageVector3 p;                    // position
    RageVector3 n;                    // normal
    RageVector2 t;                    // texture coordinates
    int8_t      bone;                 // bone index for skinning
    RageVector2 TextureMatrixScale;   // usually (1,1)
};
```

This split is deliberate and load-bearing: 2D/UI drawing (`RageDisplay::DrawQuads`
etc., `01-RageDisplay.md`) uses `RageSpriteVertex` because sprites need
per-vertex color for diffuse gradients (`Actor`'s 4-corner diffuse,
`02-ActorSystem.md`). 3D models use `RageModelVertex` because their color comes
from the mesh's `msMaterial` instead (`ModelTypes.h:80-95`), and they need a
`bone` index for skeletal animation that sprites never use. The previous
documentation collapsed both into one invented `RageVertex` struct with fields
that don't exist in either real struct (no single struct has both `color` and
`bone`).

## Real mesh types (ModelTypes.h:8-95)

```cpp
struct msTriangle { uint16_t nVertexIndices[3]; };            // :8-11

struct msMesh
{
    RString sName;                                                // :16
    char nMaterialIndex;                                            // :17
    vector<RageModelVertex> Vertices;                                 // :19
    char m_iBoneIndex;   // -1 = no bone (whole-mesh rigid transform)   // :24
    vector<msTriangle> Triangles;                                         // :26
};

struct msMaterial
{
    int nFlags;  RString sName;                                            // :82-83
    RageColor Ambient, Diffuse, Specular, Emissive;                           // :84-87
    float fShininess, fTransparency;                                           // :88-89
    AnimatedTexture diffuse;   // :91 — texture can itself be animated/multi-frame
    AnimatedTexture alpha;     // :92 — separate animated alpha-mask texture
};
```

Triangles are stored as an array of 3-index structs (`msTriangle`), not a flat
`vector<uint16_t>` index buffer as previously claimed. Materials carry **two
independently-animated textures** (`diffuse` and `alpha`, both of type
`AnimatedTexture`, `ModelTypes.h:32-78`) rather than the single flat
`Material{ambient,diffuse,specular,emissive,shininess}` struct with one texture
that was previously documented — there is no separate simplified "Material"
type; `msMaterial` is the only material struct in this codebase.

## RageModelGeometry (RageModelGeometry.h, full, 59 lines) — public data, not getters

```cpp
class RageModelGeometry
{
public:
    void LoadMilkshapeAscii( const RString& sMilkshapeAsciiFile, bool bNeedsNormals ); // :18 — not "LoadMilkshapeModel"
    void OptimizeBones();                                                                // :19
    void MergeMeshes( int iFromIndex, int iToIndex );                                      // :20
    bool HasAnyPerVertexBones() const;                                                       // :21

    int m_iRefCount;                                    // :23 — public, same pattern as RageTexture/Font
    vector<msMesh> m_Meshes;                              // :25 — public, direct access, no GetMesh(i)/GetNumMeshes()
    RageCompiledGeometry* m_pCompiledGeometry;              // :26 — GPU-side copy, shared by all meshes
    RageVector3 m_vMins, m_vMaxs;                             // :28 — bounds are public fields, no GetBounds() method
};
```

Every previously-documented accessor (`GetNumMeshes()`, `GetMesh(int)`,
`GetVertices()`, `GetIndices()`, `GetTotalVertices()`, `GetTotalTriangles()`,
`GetBounds(mins,maxs)`) is fabricated — callers read `m_Meshes`, `m_vMins`,
`m_vMaxs` directly as public fields. The only file format supported per this
header is Milkshape ASCII (`LoadMilkshapeAscii`, `:18`) — there's no evidence
of a binary Milkshape (`.ms3d`) loader in this header.

## RageMath — free functions with output pointers, not an OOP vector/matrix API

Full contents of `RageMath.h` (127 lines):

```cpp
// Vectors
void RageVec2Normalize( RageVector2* pOut, const RageVector2* pV );          // :20
void RageVec3Normalize( RageVector3* pOut, const RageVector3* pV );          // :21
void RageVec3Cross(RageVector3* ret, RageVector3 const* a, RageVector3 const* b); // :23
void RageVec3TransformCoord( RageVector3* pOut, const RageVector3* pV, const RageMatrix* pM );  // :24
void RageVec3TransformNormal( RageVector3* pOut, const RageVector3* pV, const RageMatrix* pM );  // :25
void RageVec3ClearBounds( RageVector3 &mins, RageVector3 &maxs );              // :17
void RageVec3AddToBounds( const RageVector3 &p, RageVector3 &mins, RageVector3 &maxs ); // :18

// Matrices
void RageMatrixIdentity( RageMatrix* pOut );                                     // :27
void RageMatrixMultiply( RageMatrix* pOut, const RageMatrix* pA, const RageMatrix* pB ); // :29 — pOut = pB * pA
void RageMatrixTranslation( RageMatrix* pOut, float x, float y, float z );          // :30
void RageMatrixScaling( RageMatrix* pOut, float x, float y, float z );               // :31
void RageMatrixSkewX/SkewY( RageMatrix* pOut, float fAmount );                          // :32-33
void RageMatrixRotationX/Y/Z( RageMatrix* pOut, float fTheta );                            // :36-38
void RageMatrixRotationXYZ( RageMatrix* pOut, float rX, float rY, float rZ );                // :39
void RageMatrixTranspose( RageMatrix* pOut, const RageMatrix* pIn );                           // :54
RageMatrix RageLookAt( float eyex,eyey,eyez, centerx,centery,centerz, upx,upy,upz );              // :49-52 — returns by value

// Quaternions (used for bone rotation in Model animation)
void RageQuatFromHPR/PRH(RageVector4* pOut, RageVector3 hpr);                                        // :41-42
void RageMatrixFromQuat( RageMatrix* pOut, const RageVector4 q );                                       // :43
void RageQuatSlerp(RageVector4 *pOut, const RageVector4 &from, const RageVector4 &to, float t);           // :44
void RageQuatMultiply( RageVector4* pOut, const RageVector4 &pA, const RageVector4 &pB );                    // :48

// Fast trig approximations
float RageFastSin/Cos/Tan/Csc( float x ) CONST_FUNCTION;                                                       // :56-59
```

None of these are member functions. There is no `RageVector3::Dot()`,
`RageVector3::Cross()`, `RageVector3::Normalize()`, `RageMatrix::operator*`, no
free-standing `MatrixIdentity()`/`MatrixTranslation()`/`MatrixLookAt()`/
`MatrixPerspective()` — every real function is prefixed `Rage...` and writes
its result through an output pointer (`pOut`) rather than returning by value
(the sole verified exception being `RageLookAt`, `:49`, which returns a
`RageMatrix` by value). Perspective/frustum matrix construction is **not** in
`RageMath.h` at all — it's `RageDisplay::GetPerspectiveMatrix()` and the
protected `GetOrthoMatrix()`/`GetFrustumMatrix()` (`RageDisplay.h:442,445-446`),
i.e. display-backend-specific, not a generic math utility (the header comment
at `RageDisplay.h:444` even notes "Different for D3D and OpenGL... not sure
why they're not compatible").

`RageMath.h` also defines two Bezier/spline helper classes used for tween
easing curves, not geometry:

```cpp
class RageQuadratic  { float Evaluate( float fT ) const; ... };     // :64-82
class RageBezier2D   { void Evaluate( float fT, float *pX, float *pY ) const; ... }; // :84-99
```

## ActorMultiVertex — see 02-ActorSystem.md for full verified API

Already covered there. Key fact repeated for this doc's context: it stores
`vector<RageSpriteVertex> _Vertices` (`ActorMultiVertex.h:156`) — the 2D sprite
vertex type, not `RageModelVertex` — and exposes a `DrawMode` enum
(`Quads, QuadStrip, Fan, Strip, Triangles, LineStrip, SymmetricQuadStrip`,
`ActorMultiVertex.h:9-19`) that maps directly onto `RageDisplay`'s
`DrawQuads`/`DrawQuadStrip`/`DrawFan`/`DrawStrip`/`DrawTriangles`/
`DrawLineStrip`/`DrawSymmetricQuadStrip` methods (`RageDisplay.h:342-349`) —
this is the one place in the codebase confirmed to expose that full primitive
set directly to theme scripting.

## Quad — not a primitive class at all

Previously described as its own `Actor`-derived primitive with `SetWidth/
SetHeight/SetColor`. Verified reality (`Quad.h:7`, see `02-ActorSystem.md`):
`Quad : public Sprite`, declaring nothing but a constructor, `LoadFromNode()`,
and `Copy()`. Its "geometry" is whatever `Sprite::DrawPrimitives()`
(`Sprite.h:36`) already builds for a texture-less sprite — there is no
separate quad-specific vertex or draw path.

---

**Scope**: `RageModelGeometry.h` (full), `RageMath.h` (full), `RageTypes.h`
(vertex/matrix struct section), `ModelTypes.h` (mesh/material struct section).
`.cpp` implementations were not read in this pass.
