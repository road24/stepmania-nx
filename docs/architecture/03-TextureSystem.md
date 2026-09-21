# Texture Management System (Verified)

`RageTexture.h`, `RageTextureID.h`, `RageTextureManager.h`, `RageBitmapTexture.h`,
`RageTextureRenderTarget.h`, `RageTexturePreloader.h` were all read in full. Line
numbers cite those files.

## Architecture (revised)

```
Sprite / Model / ActorMultiTexture / BitmapText   (consumers, hold RageTexture*)
         ↓ RageTextureManager::LoadTexture(RageTextureID)
RageTextureManager (cache, keyed by RageTextureID, refcounts via RageTexture::m_iRefCount)
         ↓ constructs
RageTexture subclass (RageBitmapTexture | RageTextureRenderTarget | RageMovieTexture)
         ↓ RageTexture::GetTexHandle() → uintptr_t
RageDisplay::SetTexture(TextureUnit, uintptr_t)   (RageDisplay.h:280)
```

The manager never hands `RageDisplay` a `RageTexture*` — it resolves to a raw
`uintptr_t` handle first (`RageDisplay.h:262-263`, `RageTexture.h:18`).

## RageTextureID — a plain data struct, not a class with getters (RageTextureID.h, full)

```cpp
struct RageTextureID                                   // :10
{
    RString filename;                                   // :12 — public field, accessed directly
    int iMaxSize;                                        // :15
    bool bMipMaps;                                        // :18
    int iAlphaBits;                                        // :22
    int iGrayscaleBits;                                     // :32
    int iColorDepth;                                         // :36
    bool bDither;                                             // :39
    bool bStretch;                                             // :42
    bool bHotPinkColorKey;                                       // :46
    RString AdditionalTextureHints;                               // :49
    enum TexPolicy { TEX_VOLATILE, TEX_DEFAULT } Policy;           // :55

    RageTextureID();                                                // :59
    RageTextureID( const RString &fn );                              // :63 — implicit from a plain path string
};
```

There is no `GetPath()`/`GetMipMaps()` — code reads `.filename`/`.bMipMaps`
directly. Because the single-`RString` constructor (`:63`) is not `explicit`,
`RageTextureManager::LoadTexture("path/to/image.png")` compiles via implicit
conversion — this specific usage pattern in the previous docs happened to be
correct even though the accessor methods it implied were not. Equality/ordering
operators (`:71-123`) compare every field **except** `Policy`
(comment: "not considered for ordering/equality", `:52-54`) — so loading the
same file with `TEX_VOLATILE` vs. `TEX_DEFAULT` reuses the same cache entry with
a different policy, per the comment at `RageTextureManager.h:51-54`.

## RageTexture — abstract base (RageTexture.h, full, 110 lines)

```cpp
class RageTexture                                       // :10
{
public:
    RageTexture( RageTextureID file );                     // :13
    virtual ~RageTexture() = 0;                              // :14 — pure virtual destructor
    virtual void Update( float ) {}                            // :15
    virtual void Reload() {}                                     // :16
    virtual void Invalidate() {}   // only called by RageTextureManager::InvalidateTextures // :17
    virtual uintptr_t GetTexHandle() const = 0;    // accessed by RageDisplay // :18

    // movie texture hooks (default no-ops for static textures)
    virtual void SetPosition( float ) {}
    virtual void DecodeSeconds( float ) {}
    virtual void SetPlaybackRate( float ) {}
    virtual bool IsAMovie() const { return false; }
    virtual void SetLooping(bool) {}

    int GetSourceWidth() const;    int GetSourceHeight() const;   // :27-28 — original file dimensions
    int GetTextureWidth() const;   int GetTextureHeight() const;  // :29-30 — in-memory texture dimensions (may be padded)
    int GetImageWidth() const;     int GetImageHeight() const;    // :31-32 — image region within the texture

    int GetFramesWide() const;     int GetFramesHigh() const;     // :34-35 — sprite-sheet frame grid

    const RectF *GetTextureCoordRect( int frameNo ) const;         // :52
    int GetNumFrames() const { return m_iFramesWide*m_iFramesHigh; } // :53

    int m_iRefCount;                                                 // :58 — PUBLIC field
    bool m_bWasUsed;                                                  // :59 — PUBLIC field
    const RageTextureID &GetID() const { return m_ID; }                // :62
};
```

Three separate width/height concepts exist, not one `GetWidth()`/`GetHeight()`
pair as previously claimed:
- **Source** = the original file's pixel dimensions.
- **Texture** = the in-memory GPU texture's dimensions (often padded to a
  power-of-two).
- **Image** = the actual image data's footprint inside that (possibly padded)
  texture.

Conversion-ratio helpers exist for exactly this reason (`RageTexture.h:45-50`):
`GetSourceToImageCoordsRatioX/Y()`, `GetImageToTexCoordsRatioX/Y()`,
`GetSourceToTexCoordsRatioX/Y()`.

Reference counting is a **public plain `int` field** (`m_iRefCount`,
`RageTexture.h:58`), incremented/decremented directly by
`RageTextureManager` — there is no `IncrementRefCount()`/`Release()` method on
`RageTexture` itself. There is no `GetPixelFormat()`, `GetSourceImage()`,
`SourceText()/SourceImage()/SourceMovie()`, or `RageTextureRenderingParams`
class anywhere in this header — none of that API exists.

## RageTextureManager (RageTextureManager.h, full, 128 lines)

```cpp
class RageTextureManager
{
    void Update( float fDeltaTime );                                  // :53
    RageTexture* LoadTexture( RageTextureID ID );                       // :55 — the real load entry point
    RageTexture* CopyTexture( RageTexture *pCopy ); // ref to same texture, not a deep copy // :56
    bool IsTextureRegistered( RageTextureID ID ) const;                   // :57
    void UnloadTexture( RageTexture *t );                                 // :60
    void ReloadAll();                                                      // :61
    bool SetPrefs( RageTextureManagerPrefs prefs );                         // :65
    void DeleteCachedTextures() { GarbageCollect( screen_changed ); }        // :72 — "call this between Screens"
    void DoDelayedDelete()      { GarbageCollect( delayed_delete ); }         // :75 — "call this on switch theme"
    void InvalidateTextures();                                                 // :77
};
extern RageTextureManager* TEXTUREMAN;                                          // :101
```

None of the following exist: `PreloadTextures(folder)`, `Flush()`,
`UnloadAll()`, `GetNumTextures()`, `GetTexture(int i)`, `DisablePreload(bool)`.
Cache lifecycle is instead driven by two explicit GC hooks —
`DeleteCachedTextures()` for screen transitions and `DoDelayedDelete()` for
theme switches — both routed through the private `GarbageCollect(GCType)`
(`:92-93`, `GCType` is `screen_changed` or `delayed_delete`).

`RageTextureManagerPrefs` (`:9-46`) holds global policy, not per-load options:
`m_iTextureColorDepth`, `m_iMovieColorDepth`, `m_bDelayedDelete`,
`m_iMaxTextureResolution`, `m_bHighResolutionTextures`, `m_bMipMaps`.

## RageBitmapTexture (RageBitmapTexture.h, full, 50 lines)

```cpp
class RageBitmapTexture : public RageTexture              // :8
{
    RageBitmapTexture( RageTextureID name );                 // :11
    virtual void Reload();                                     // :15
    virtual uintptr_t GetTexHandle() const { return m_uTexHandle; } // :16
private:
    void Create();   // called by constructor and Reload         // :19
    void Destroy();
    uintptr_t m_uTexHandle;  // unsigned in GL, IDirect3DTexture9* in D3D // :21
};
```

The header itself is minimal — it's a thin wrapper holding one handle plus
`Create()`/`Destroy()`. Format conversion, mip generation, and any
power-of-two padding logic live in `RageBitmapTexture.cpp`, which was **not**
read in this pass; no specific claim is made here about how that padding or
conversion is implemented beyond what `RageTexture`'s three-tier width/height
API (source/texture/image, above) implies must exist.

## RageTextureRenderTarget (RageTextureRenderTarget.h, full, 59 lines)

```cpp
class RageTextureRenderTarget: public RageTexture           // :10
{
    RageTextureRenderTarget( RageTextureID name, const RenderTargetParam &param ); // :13
    virtual void Reload();                                     // :16
    virtual uintptr_t GetTexHandle() const { return m_iTexHandle; } // :17
    void BeginRenderingTo( bool bPreserveTexture = true );        // :19
    void FinishRenderingTo();                                       // :20
private:
    void Create();  void Destroy();                                  // :27-28 — private, called from ctor
};
```

Construction takes both the `RageTextureID` and the `RenderTargetParam`
together — there's no public parameterless `Create(param)` call as previously
claimed; sizing/depth/alpha/float options are fixed at construction via
`RenderTargetParam` (`RageDisplay.h:183-200`: `iWidth`, `iHeight`,
`bWithDepthBuffer`, `bWithAlpha`, `bFloat`).

## RageTexturePreloader — synchronous, not async (RageTexturePreloader.h, full, 49 lines)

```cpp
class RageTexturePreloader
{
    void Load( const RageTextureID &ID );          // :15
    void UnloadAll();                                 // :16
    void Swap( RageTexturePreloader &rhs );             // :17
private:
    vector<RageTexture*> m_apTextures;                    // :20
};
```

This class does **not** do background/threaded loading. Based on the header
alone: `Load()` presumably calls `TEXTUREMAN->LoadTexture(ID)` synchronously and
stashes the resulting pointer in `m_apTextures`, simply to hold a reference
(keeping `m_iRefCount` above zero) so the texture survives whatever garbage
collection would otherwise reclaim it. There is no `TryGetTexture()`,
`Finish()`, or `Cancel()` — those were invented. (The `.cpp` was not read in
this pass, so the exact load timing is not independently confirmed, but nothing
in the header suggests a worker thread.)

## ActorFrameTexture and ActorMultiTexture

Covered in `02-ActorSystem.md` — both are `Actor`/`ActorFrame` subclasses that
*consume* the texture system (one for render-to-texture, one for compositing
multiple `RageTexture*`), not part of the texture system's own class hierarchy.

## Movie textures are part of this hierarchy too

`RageMovieTexture` (`arch/MovieTexture/MovieTexture.h:10`) is
`: public RageTexture` — see `04-FontSystem.md`'s sibling doc, or the top-level
`INDEX.md`, for the verified movie-texture class tree. The previous docs used
the class name `MovieTexture` for the abstract base; the real name is
`RageMovieTexture`.

---

**Scope**: `RageTexture.h`, `RageTextureID.h`, `RageTextureManager.h`,
`RageBitmapTexture.h`, `RageTextureRenderTarget.h`, `RageTexturePreloader.h` —
all read in full. `.cpp` files for these classes were not read in this pass;
claims about them are explicitly flagged as unverified where made.
