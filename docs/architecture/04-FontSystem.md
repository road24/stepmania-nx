# Font & Text Rendering System (Verified)

`Font.h` (259 lines), `FontManager.h` (49 lines), `FontCharmaps.h`,
`FontCharAliases.h`, and `BitmapText.h` (177 lines) were all read in full.

## The single biggest correction: there is no FreeType, no live rasterization, no kerning

A codebase-wide search (`tgrep -i 'freetype|FT_Init|FT_Library'` across all of
`stepmania/src`) returns **zero matches**. The previous version of this document
described a TrueType-rasterization pipeline with a live glyph texture atlas and
kerning-pair lookups — none of that exists here. StepMania's runtime font system
loads **pre-made bitmap font sprite sheets** (texture + `.ini` describing glyph
placement), full stop. Live TTF rasterization would need to happen offline (the
`Texture Font Generator/` tool under `stepmania/src` is a separate Windows GUI
utility for *producing* such bitmap fonts, not a runtime component).

## Files (confirmed)

| File | Contents | Verified at |
|------|----------|-------------|
| Font.h/cpp | `Font`, `FontPage`, `FontPageTextures`, `FontPageSettings`, `glyph` | `Font.h:16,39,67,111,144` |
| FontManager.h/cpp | `FontManager`, global **`FONT`** (not `FONTMAN`) | `FontManager.h:9,21` |
| FontCharmaps.h/cpp | namespace, not class: `FontCharmaps::get_char_map(RString)` | `FontCharmaps.h:4-8` |
| FontCharAliases.h/cpp | namespace, not class: `ReplaceMarkers()`, `GetChar()` | `FontCharAliases.h:5-9` |
| BitmapText.h/cpp | `BitmapText : public Actor` — the actual text-drawing actor | `BitmapText.h:11` |

## Real architecture

```
BitmapText (Actor subclass — the thing themes actually put on screen)
  ├─ owns a Font*                                    (BitmapText.h:113)
  ├─ SetText() splits the string into m_wTextLines     (BitmapText.h:59,116)
  ├─ BuildChars() walks each wchar_t, looks up glyphs,
  │   fills m_aVertices : vector<RageSpriteVertex>      (BitmapText.h:129,138)
  └─ DrawPrimitives() → DrawChars(bUseStrokeTexture)      (BitmapText.h:69,139)

Font
  ├─ owns vector<FontPage*> m_apPages                    (Font.h:191)
  ├─ m_pDefault : FontPage*  (metrics source)              (Font.h:198)
  └─ GetGlyph(wchar_t) → const glyph&                       (Font.h:153)

FontPage
  ├─ FontPageTextures m_FontPageTextures  (pre-made bitmap)  (Font.h:129, :16-36)
  ├─ vector<glyph> m_aGlyphs                                  (Font.h:135)
  └─ map<wchar_t,int> m_iCharToGlyphNo                         (Font.h:137)

FontManager
  └─ LoadFont(path, sChars) → Font*                             (FontManager.h:15)
```

## `glyph` struct (lowercase; Font.h:39-64) — real fields

```cpp
struct glyph
{
    FontPage *m_pPage;                          // :42
    FontPageTextures m_FontPageTextures;          // :44
    int   m_iHadvance;   // horizontal advance      // :48
    float m_fWidth;      // rendered width            // :51
    float m_fHeight;     // rendered height             // :53
    float m_fHshift;     // horizontal render offset     // :56
    RectF m_TexRect;     // texture coordinate rect        // :59
};
```

Compare to what was previously claimed (`width, height, xoffset, yoffset,
xadvance, pTexture, fX1/fY1/fX2/fY2`) — wrong field names, wrong shape, and no
per-glyph texture pointer (the texture lives on the shared `FontPage`, since one
bitmap sheet serves many glyphs).

**There is no kerning.** No `GetKernAmount()` method, no kerning table, anywhere
in `Font.h`. Character spacing is purely `m_iHadvance` per glyph plus
`SetVertSpacing()`/line-wrap logic on `BitmapText` — no left/right character-pair
adjustment exists in this codebase.

## `Font` class (Font.h:144-225) — real API

```cpp
class Font
{
public:
    int m_iRefCount;                                        // :147 — public field, same refcount pattern as RageTexture
    RString path;                                             // :148

    const glyph &GetGlyph( wchar_t c ) const;                   // :153
    int GetLineWidthInSourcePixels( const wstring &szLine ) const; // :155 — NOT "GetStringWidth(RString)"
    int GetLineHeightInSourcePixels( const wstring &szLine ) const; // :156
    int GetGlyphsThatFit(const wstring& line, int* width) const;      // :157
    bool FontCompleteForString( const wstring &str ) const;             // :159

    void AddPage(FontPage *fp);                                          // :165
    void Load(const RString &sFontOrTextureFilePath, RString sChars);     // :172
    void CapsOnly();                                                        // :177

    int GetHeight() const  { return m_pDefault->m_iHeight; }                 // :179
    int GetCenter() const  { return m_pDefault->GetCenter(); }                 // :180
    int GetLineSpacing() const { return m_pDefault->m_iLineSpacing; }            // :181

    bool IsRightToLeft() const { return m_bRightToLeft; }                          // :185
    bool IsDistanceField() const { return m_bDistanceField; }                        // :186
private:
    vector<FontPage *> m_apPages;                                                     // :191
    FontPage *m_pDefault;                                                               // :198
    map<wchar_t,glyph*> m_iCharToGlyph;                                                   // :201
    glyph *m_iCharToGlyphCache[128];   // fast path for ASCII                              // :203
    bool m_bRightToLeft;   // true for Hebrew, Arabic, Urdu — changes glyph render order      // :210
};
```

Note the ASCII fast-path cache (`m_iCharToGlyphCache[128]`, `:203`) alongside the
general `map<wchar_t,glyph*>` — a small, real optimization detail that was
absent from the previous (fabricated) "texture atlas allocator" description.

There's no `GetStringWidth(const RString&)` — the real method takes a
`const wstring&` (already-decoded wide string) and is named
`GetLineWidthInSourcePixels()` (`:155`), operating in **source pixels** (i.e.
before any theme-level zoom is applied), consistent with `Actor`'s
unzoomed/zoomed distinction (`02-ActorSystem.md`).

## `FontPage` / `FontPageSettings` (Font.h:66-142) — this is the bitmap-sheet config

```cpp
struct FontPageSettings                                  // :67
{
    RString m_sTexturePath;                                 // :69
    int m_iDrawExtraPixelsLeft, m_iDrawExtraPixelsRight,       // :71-72
        m_iAddToAllWidths, m_iLineSpacing, m_iTop, m_iBaseline, // :73-76
        m_iDefaultWidth, m_iAdvanceExtraPixels;                 // :77-78
    float m_fScaleAllWidthsBy;                                   // :79
    map<wchar_t,int> CharToGlyphNo;                                // :82
    map<int,int> m_mapGlyphWidths;                                  // :84 — per-glyph width override

    RString MapRange( RString sMapping, int iMapOffset,             // :108
                        int iGlyphOffset, int iCount );
};

class FontPage
{
    void Load( const FontPageSettings &cfg );                        // :117
    int m_iHeight;  int m_iLineSpacing;  float m_fVshift;               // :120-122
    FontPageTextures m_FontPageTextures;                                  // :129 — the actual bitmap
    vector<glyph> m_aGlyphs;                                                // :135
};
```

`FontPageSettings::MapRange()` (`:108`) is how a named character-map range
(resolved via `FontCharmaps::get_char_map()`, below) gets assigned to a
contiguous run of glyph cells read off the sprite sheet — this is the real
mechanism that stands in for "Unicode range support," not a general Unicode
charmap system.

## `FontPageTextures` (Font.h:16-36)

```cpp
struct FontPageTextures
{
    RageTexture *m_pTextureMain;      // primary glyph bitmap        // :19
    RageTexture *m_pTextureStroke;    // optional outline/stroke layer // :23
};
```

Two textures per page — a main glyph bitmap plus an optional stroke/outline
texture "to help achieve complicated layer styles" (comment, `:20-22`). This
lines up with `BitmapText::DrawChars(bool bUseStrokeTexture)`
(`BitmapText.h:139`) and `SetStrokeColor()`/`GetStrokeColor()`
(`BitmapText.h:81-84`) — StepMania renders stroked text by drawing the glyph
quad twice, once per texture layer, not via a shader outline effect.

## `FontManager` (FontManager.h, full — 49 lines)

```cpp
class FontManager
{
    Font* LoadFont( const RString &sFontOrTextureFilePath, RString sChars = "" ); // :15
    Font *CopyFont( Font *pFont );                                                  // :16
    void UnloadFont( Font *fp );                                                      // :17
};
extern FontManager* FONT;                                                                // :21
```

The global is **`FONT`**, not `FONTMAN`. `LoadFont()` takes **two** parameters —
a path and an optional `sChars` restricting which characters to load — not the
single-argument call previously documented. There is no `GetNumFonts()`,
`GetFont(int)`, `FindFont()`, `ClearAllTextures()`, or `ClearUnused()` anywhere
in this header.

## `FontCharmaps` — a namespace of one function (FontCharmaps.h, full)

```cpp
namespace FontCharmaps
{
    extern const wchar_t M_SKIP;
    const wchar_t *get_char_map(RString name);     // :7
}
```

Given a named range (e.g. an alphabet-frame layout name used in a font's
`.ini`), returns the sequence of `wchar_t` codepoints that the sprite sheet's
cells map to, in order. This is not a class, has no `AddCharset`/`GetCharset`
methods, and is not a general "character set support" system as previously
described.

## `FontCharAliases` — text-marker substitution, not glyph fallback (FontCharAliases.h, full)

```cpp
namespace FontCharAliases
{
    void ReplaceMarkers( RString &sText );              // :7
    bool GetChar( RString &codepoint, wchar_t &ch );      // :8
}
```

This is for substituting textual markers embedded in theme strings (e.g. a
`{...}` token) with a specific private-use-area glyph codepoint — it is
**not** a "missing character falls back to a similar one" mechanism as the
previous doc claimed. There is no `SetAlias(wchar_t, wchar_t)` or
`ResolveFallback()` method; `FontCharAliases` is a free-function namespace, not
a class with instance state.

`FONT_DEFAULT_GLYPH` (`Font.h:231`, value `0xF8FF`, "last private-use Unicode
character") is the actual fallback codepoint used when a character has no
glyph, per the comment at `Font.h:227-230`.

## `BitmapText` — the real text-rendering actor (BitmapText.h, full, 177 lines)

```cpp
class BitmapText : public Actor                                     // :11
{
    bool LoadFromFont( const RString& sFontName );                     // :57
    virtual void SetText( const RString& sText, const RString& sAlternateText = "",
                           int iWrapWidthPixels = -1 );                   // :59
    void SetVertSpacing( int iSpacing );                                    // :60
    void SetMaxWidth( float fMaxWidth );  void SetMaxHeight( float fMaxHeight ); // :61-62
    virtual void DrawPrimitives();                                            // :69
    void SetStrokeColor(RageColor c);                                          // :81
    void SetTextGlowMode( TextGlowMode tgm );                                    // :86
    void AddAttribute( size_t iPos, const Attribute &attr );                      // :106 — per-substring color override
protected:
    Font *m_pFont;                                                                 // :113
    vector<wstring> m_wTextLines;                                                    // :116
    vector<RageSpriteVertex> m_aVertices;                                              // :129 — the actual draw data
    vector<FontPageTextures*> m_vpFontPageTextures;                                      // :131
    void BuildChars();          // recomputes m_aVertices from m_wTextLines               // :138
    void DrawChars( bool bUseStrokeTexture );                                                // :139
};
```

`Attribute` (`:95-103`) lets a substring of the text get its own diffuse/glow
color — e.g. per-word colored text — layered on top of the base `Actor` diffuse
system via `AddAttribute(pos, attr)`.

## Verified rendering flow

```
Theme calls BitmapText::SetText("Hello")                    BitmapText.h:59
  → SetTextInternal() splits into m_wTextLines (vector<wstring>) BitmapText.h:116,143
  → BuildChars()                                                BitmapText.h:138
      for each wchar_t in each line:
        Font::GetGlyph(c) → const glyph&                          Font.h:153
          (looks up m_iCharToGlyphCache[128] fast path, or
           m_iCharToGlyph map, falling back to FONT_DEFAULT_GLYPH)   Font.h:203,201,231
        append quad(s) to m_aVertices using glyph.m_TexRect,
          glyph.m_fWidth/m_fHeight/m_fHshift, advance by m_iHadvance  BitmapText.h:129 / Font.h:48-59

Actor::Draw() → BitmapText::DrawPrimitives()                     BitmapText.h:69
  → DrawChars(bUseStrokeTexture)                                    BitmapText.h:139
      binds glyph.m_pPage->m_FontPageTextures (main [+ stroke]) and
      submits m_aVertices, per FontPage (grouped so each page's
      texture is bound once)                                          Font.h:44,129
```

---

**Scope**: `Font.h`, `FontManager.h`, `FontCharmaps.h`, `FontCharAliases.h`,
`BitmapText.h` — all read in full. `.cpp` implementation files were not read in
this pass; the flow above is reconstructed from header declarations, member
comments, and naming, and is marked as such rather than asserted as exact
runtime behavior.
