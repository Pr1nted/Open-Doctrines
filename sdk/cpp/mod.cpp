// Hello Panel in C++. Mirrors sdk/examples/hello-panel/mod.c.
//
// Same header, same ABI — gearbox.h is extern "C", so the imports and exports
// are unaffected by name mangling. What C++ buys you here is types: a Str that
// carries its own length, so you cannot pass a pointer with the wrong size.
//
// Freestanding: no libc, no libc++, no exceptions, no RTTI, no allocator. The
// standard library is not available; anything you need, you write.

#include "gearbox.h"

namespace {

// A borrowed UTF-8 span. The ABI is (ptr, len) everywhere, and bundling them
// removes the single easiest mistake to make against this API.
struct Str {
    const char* p;
    uint32_t    n;

    template <uint32_t N>
    constexpr Str(const char (&lit)[N]) : p(lit), n(N - 1) {}
    constexpr Str(const char* q, uint32_t m) : p(q), n(m) {}
};

inline void log(gearbox_log_level lvl, Str s) { gearbox_log(lvl, s.p, s.n); }
inline void text(gearbox_panel pl, int32_t x, int32_t y, uint32_t c, Str s) {
    gearbox_draw_text(pl, x, y, c, s.p, s.n);
}
inline bool button(gearbox_panel pl, int32_t x, int32_t y, int32_t w, int32_t h, Str s) {
    return gearbox_button(pl, x, y, w, h, s.p, s.n) != 0;
}

// Fixed-capacity line builder. No allocator exists, so the buffer is the API.
template <uint32_t Cap>
class Line {
public:
    void clear() { m_n = 0; }
    Str str() const { return Str(m_buf, m_n); }

    Line& operator<<(Str s) {
        for (uint32_t i = 0; i < s.n && m_n < Cap; i++) m_buf[m_n++] = s.p[i];
        return *this;
    }
    Line& operator<<(uint64_t v) {
        char tmp[24];
        uint32_t k = 0;
        if (v == 0) tmp[k++] = '0';
        while (v > 0 && k < sizeof tmp) { tmp[k++] = char('0' + (v % 10)); v /= 10; }
        while (k > 0 && m_n < Cap) m_buf[m_n++] = tmp[--k];
        return *this;
    }
    Line& operator<<(int64_t v) {
        if (v < 0) { *this << Str("-"); return *this << uint64_t(-v); }
        return *this << uint64_t(v);
    }

private:
    char     m_buf[Cap];
    uint32_t m_n = 0;
};

gearbox_env_t g_env;
gearbox_panel g_panel;
uint32_t      g_cursor;

}  // namespace

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    if (g_env.is_headless) {
        log(GEARBOX_LOG_INFO, "hello-panel-cpp: headless, no UI");
        return 0;
    }
    g_panel = gearbox_panel_register("Hello Panel (C++)", 17, 280, 150);
    if (g_panel == 0)
        log(GEARBOX_LOG_WARN, "hello-panel-cpp: no panel, running quiet");
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) { g_cursor = 0; }

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    (void)h;
    gearbox_draw_rect(panel, 0, 0, int32_t(w), 1, 0x3C3C5AFFu);

    Line<96> l;
    l << Str("Turn ") << uint64_t(gearbox_turn_number());
    text(panel, 8, 8, 0xFFFFFFFFu, l.str());

    const uint32_t count = gearbox_country_count();
    l.clear();
    l << Str("Countries: ") << uint64_t(count);
    text(panel, 8, 28, 0xB4B4C8FFu, l.str());

    if (count == 0) {
        text(panel, 8, 52, 0x9696A0FFu, "No world loaded");
        return;
    }
    if (g_cursor >= count) g_cursor = 0;

    const gearbox_country c = gearbox_country_at(g_cursor);
    if (c != GEARBOX_INVALID) {
        char name[64];
        uint32_t got = gearbox_country_name(c, name, sizeof name);
        if (got > sizeof name) got = sizeof name;   // truncated, not an error
        text(panel, 8, 52, 0xFFFFFFFFu, Str(name, got));

        l.clear();
        l << Str("Provinces: ") << uint64_t(gearbox_country_province_count(c));
        text(panel, 8, 72, 0xB4B4C8FFu, l.str());

        l.clear();
        l << Str("Treasury: ") << int64_t(gearbox_country_treasury(c));
        text(panel, 8, 92, 0xB4B4C8FFu, l.str());
    }

    if (button(panel, 8, 116, 120, 24, "Next country")) g_cursor++;
}
