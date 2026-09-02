#pragma once
#include <cstdint>

namespace xterm {

// Per-cell attribute bit flags.
enum Attr : uint16_t {
    ATTR_NONE      = 0,
    ATTR_BOLD      = 1u << 0,
    ATTR_UNDERLINE = 1u << 1,
    ATTR_INVERSE   = 1u << 2,
    ATTR_ITALIC    = 1u << 3,
    ATTR_DIM       = 1u << 4,
    ATTR_STRIKE    = 1u << 5,
    ATTR_BLINK     = 1u << 6,
    ATTR_INVISIBLE = 1u << 7,
    ATTR_WIDE      = 1u << 8,   // left cell of a double-width glyph
    ATTR_WIDE_TAIL = 1u << 9,   // right (continuation) cell of a wide glyph
    ATTR_COMBINING = 1u << 10,  // base cell carries >=1 combining mark (comb0/comb1)
};

// A single character cell. Colours are pre-resolved to ARGB so the renderer does
// not need the SGR palette. comb0/comb1 hold up to two combining marks composed
// onto the base code point (0 = none); further stacking marks are dropped.
struct Cell {
    uint32_t cp;      // base Unicode code point (' ' for blank)
    uint32_t fg;      // ARGB
    uint32_t bg;      // ARGB
    uint16_t attr;    // Attr bitmask
    uint32_t comb0;   // first combining mark or 0
    uint32_t comb1;   // second combining mark or 0

    bool operator==(const Cell& o) const {
        return cp == o.cp && fg == o.fg && bg == o.bg && attr == o.attr &&
               comb0 == o.comb0 && comb1 == o.comb1;
    }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

} // namespace xterm
