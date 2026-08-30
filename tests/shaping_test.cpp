// Devanagari shaping: the part that cannot be faked with a substitution table.
//
// Arabic joining is a lookup -- each letter has four forms and the neighbours
// decide which, so src/i18n/Arabic.cpp can do it with a table and no library.
// Devanagari is not like that. Two things happen that a table cannot express:
//
//   REORDERING. The vowel sign ि (U+093F) is TYPED after the consonant it
//   belongs to and DRAWN before it. "हिन्दी" is typed ह ि न ् द ी and the
//   first thing on the line is the ि.
//
//   CONJUNCTS. न + ् + द is not three letters with a mark between them. It is
//   one glyph, produced by a GSUB lookup in the font. Drawing the codepoints
//   leaves the halant (्) visible, which is how a Hindi reader can tell at a
//   glance that software was not up to the job.
//
// So this asserts the two properties that distinguish real shaping from
// drawing the string: the output is SHORTER than the input (glyphs were
// fused), and the reordered vowel comes first. Both are checked against the
// font that ships, because a shaper is only as good as the tables it is given.

#include <hb.h>

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    printf("  FAIL  %s\n", what.c_str());
}

static void section(const char* name) { printf("\n== %s ==\n", name); }

int main(int argc, char** argv) {
    const std::string fontPath =
        (argc > 1) ? std::string(argv[1])
                   : std::string("data/fonts/NotoSansDevanagari-Regular.ttf");

    hb_blob_t* blob = hb_blob_create_from_file(fontPath.c_str());
    if (!blob || hb_blob_get_length(blob) == 0) {
        printf("cannot read %s\n", fontPath.c_str());
        return 1;
    }
    hb_face_t* face = hb_face_create(blob, 0);
    hb_font_t* font = hb_font_create(face);
    hb_font_set_scale(font, 64 * 64, 64 * 64);

    // हिन्दी -- ha, i-matra, na, virama, da, ii-matra.
    const std::vector<uint32_t> typed = {0x0939, 0x093F, 0x0928, 0x094D, 0x0926, 0x0940};

    hb_buffer_t* buf = hb_buffer_create();
    // The cluster is the INDEX of the character, which is how the reordering
    // is observed afterwards: a glyph carries the cluster of the character it
    // came from, so an i-matra that moved shows up as a low cluster in a high
    // position. Passing 0 for every character, as this did at first, throws
    // that information away and makes the assertions below vacuous.
    for (size_t i = 0; i < typed.size(); ++i)
        hb_buffer_add(buf, typed[i], (unsigned)i);
    // ONE CLUSTER PER CHARACTER. At the default level HarfBuzz merges the
    // clusters of characters that combine, so ह+ि both come back as cluster 0
    // and the reordering becomes invisible -- which is what made the first
    // version of this test report a failure against correct shaping. This
    // level keeps the indices distinct so the movement can be seen.
    hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_DEVANAGARI);
    hb_buffer_set_language(buf, hb_language_from_string("hi", -1));
    hb_shape(font, buf, nullptr, 0);

    unsigned n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);

    section("HarfBuzz shapes Devanagari against the shipped font");
    printf("  typed %zu codepoints -> %u glyphs:", typed.size(), n);
    for (unsigned i = 0; i < n; ++i) printf(" %u@%u", info[i].codepoint, info[i].cluster);
    printf("\n");

    ok(n > 0, "the font shaped something");

    // FUSION. Six codepoints in; fewer glyphs out means the halant was consumed
    // into a conjunct rather than drawn as a mark of its own.
    ok(n < typed.size(), "glyphs were fused -- the conjunct formed");

    // REORDERING, asserted by GLYPH IDENTITY rather than by cluster.
    //
    // Cluster cannot show this. Moving the vowel in front of its consonant
    // forces HarfBuzz to merge their clusters to keep the sequence monotonic,
    // so both come back as cluster 0 whatever the cluster level -- which had
    // this test reporting a failure against output that was entirely correct.
    // What CAN be asserted is that the consonant is no longer first: something
    // was placed in front of ह, and the only thing available is the vowel.
    hb_codepoint_t haGlyph = 0;
    hb_font_get_nominal_glyph(font, 0x0939, &haGlyph);
    ok(haGlyph != 0, "the font maps ha");
    ok(n > 1 && info[0].codepoint != haGlyph && info[1].codepoint == haGlyph,
       "the i-matra was moved in front of its consonant");

    // The halant is gone as a glyph of its own -- consumed into the half form
    // of the consonant before it, which is what a conjunct IS.
    bool halantSurvived = false;
    for (unsigned i = 0; i < n; ++i)
        if (info[i].cluster == 3) halantSurvived = true;
    ok(!halantSurvived, "the halant was consumed rather than drawn");

    // And the tail of the word is still there.
    bool sawLast = false;
    for (unsigned i = 0; i < n; ++i)
        if (info[i].cluster == typed.size() - 1) sawLast = true;
    ok(sawLast, "the final vowel sign survived shaping");

    // A glyph id of 0 is .notdef -- the font could not map something.
    bool anyNotdef = false;
    for (unsigned i = 0; i < n; ++i) anyNotdef = anyNotdef || (info[i].codepoint == 0);
    ok(!anyNotdef, "no .notdef glyphs -- the font covers every character");

    hb_buffer_destroy(buf);
    hb_font_destroy(font);
    hb_face_destroy(face);
    hb_blob_destroy(blob);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
