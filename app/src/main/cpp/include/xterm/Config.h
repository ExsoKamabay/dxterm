#pragma once
#include "xterm/Types.h"
#include <cstdint>

namespace xterm {

// Runtime configuration + theme: the single source of truth for colours and
// scrollback depth. Defaults mirror res/values/colors.xml; the Android layer
// overrides them at session creation via nativeConfigure so colors.xml drives
// the engine. Colours are resolved here (index -> ARGB) at parse time.
struct Config {
    uint32_t defaultFg   = 0xFFE6E6E6u;
    uint32_t defaultBg   = 0xFF0A0A0Cu;
    uint32_t cursorColor = 0xFF8A5CF6u;
    int      scrollback  = 5000;

    // 16-colour ANSI system palette (0-7 normal, 8-15 bright).
    uint32_t palette[16] = {
        0xFF000000, 0xFFCC3333, 0xFF33CC33, 0xFFCCCC33,
        0xFF3366CC, 0xFFCC33CC, 0xFF33CCCC, 0xFFCCCCCC,
        0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
        0xFF5599FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF,
    };

    // xterm 256-colour index -> ARGB (0-15 palette, 16-231 cube, 232-255 gray).
    uint32_t color(int idx) const {
        if (idx < 0) return defaultFg;
        if (idx < 16) return palette[idx];
        if (idx < 232) {
            int n = idx - 16, r = n / 36, g = (n / 6) % 6, b = n % 6;
            auto ch = [](int v) -> uint32_t { return v == 0 ? 0 : (uint32_t)(55 + v * 40); };
            return 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
        }
        if (idx < 256) {
            uint32_t v = (uint32_t)(8 + (idx - 232) * 10);
            return 0xFF000000u | (v << 16) | (v << 8) | v;
        }
        return defaultFg;
    }

    // Standard 16-colour ANSI selector (SGR 30-37 / 90-97).
    uint32_t ansi(int idx, bool bright) const {
        if (idx < 0 || idx > 7) return defaultFg;
        return color(bright ? idx + 8 : idx);
    }

    Cell blank() const { return Cell{ (uint32_t)' ', defaultFg, defaultBg, ATTR_NONE, 0, 0 }; }
};

} // namespace xterm
