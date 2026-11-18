// Font atlas test: glyphs keep their size when ImGui repacks its bakes
// $ cmake --build build && ./build/prospero_fonttest
//
// ImGui's FreeType loader keeps one face per font, shared by its sizes.
// After a zoom ImGui discards the sizes no longer drawn and packs its
// pool of bakes, moving each live one down over the gaps. The loader
// switched the face to a bake's size only when the bake's address
// differed from the last it used, so a bake moved to that address had
// its new glyphs drawn at the other size: scattered letters too large
// in small headers (issue #77), or empty. FetchGui.cmake patches the
// loader; this runs the sequence against it.
//
// The unit tests have neither ImGui nor FreeType, so this is a program
// of its own, built from ImGui's sources with its stock configuration.
//
// This is free and unencumbered software released into the public domain.
#include <stdio.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/freetype/imgui_freetype.h"

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static unsigned char const monottf[] = {
    #embed "../assets/JetBrainsMono-Regular.ttf"
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

static int fails;

static void expect(char const *name, bool ok)
{
    if (!ok) {
        fprintf(stderr, "fonttest: FAIL %s\n", name);
        fails++;
    }
}

static float glyphheight(ImFontBaked *b, ImWchar c)
{
    ImFontGlyph *g = b->FindGlyph(c);
    return g ? g->Y1 - g->Y0 : -1;
}

int main()
{
    ImFontAtlas atlas;
    atlas.FontLoader = ImGuiFreeType::GetFontLoader();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    ImFont *font = atlas.AddFontFromMemoryTTF((void *)monottf, (int)sizeof(monottf), 15, &cfg);
    ImFontAtlasUpdateNewFrame(&atlas, 1, true);

    // In the pool: the size before a zoom, a big one, a small one; the
    // big one is the last the loader rasterized at
    ImFontBaked *old   = font->GetFontBaked(17);
    ImFontBaked *big   = font->GetFontBaked(26);
    ImFontBaked *small = font->GetFontBaked(13);
    glyphheight(old, 'x');
    float want  = glyphheight(small, 'U');
    float tall  = glyphheight(big, 'U');
    void *bigat = big;

    // The old size discarded, the next frame packs the pool
    ImFontAtlasBakedDiscard(&atlas, font, old);
    ImFontAtlasUpdateNewFrame(&atlas, 2, true);
    small = font->GetFontBaked(13);
    expect("moved", (void *)small == bigat);
    expect("sizes", want>0 && tall>want);
    expect("small", glyphheight(small, 'R') == want);

    // And the big one, now first loaded after the small one
    big = font->GetFontBaked(26);
    expect("big", glyphheight(big, 'R') == tall);

    if (!fails) {
        printf("fonttest: ok\n");
    }
    return fails != 0;
}
