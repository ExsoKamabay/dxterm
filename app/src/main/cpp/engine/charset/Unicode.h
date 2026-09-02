#pragma once
#include <cstdint>

namespace xterm {

// Column width of a Unicode code point: 0 (combining / zero-width),
// 1 (normal), or 2 (East-Asian wide / fullwidth). Modelled on the standard
// wcwidth interval tables. Control characters return 0 (handled elsewhere).
int charWidth(uint32_t cp);

} // namespace xterm
