#include "screen/ScreenBuffer.h"
#include "charset/Unicode.h"
#include <algorithm>
#include <cstring>

namespace xterm {

// Append a code point to a UTF-8 string.
static void appendUtf8(std::string& d, uint32_t cp) {
    if (cp < 0x80) d.push_back((char)cp);
    else if (cp < 0x800) { d.push_back((char)(0xC0|(cp>>6))); d.push_back((char)(0x80|(cp&0x3F))); }
    else if (cp < 0x10000) { d.push_back((char)(0xE0|(cp>>12))); d.push_back((char)(0x80|((cp>>6)&0x3F))); d.push_back((char)(0x80|(cp&0x3F))); }
    else { d.push_back((char)(0xF0|(cp>>18))); d.push_back((char)(0x80|((cp>>12)&0x3F))); d.push_back((char)(0x80|((cp>>6)&0x3F))); d.push_back((char)(0x80|(cp&0x3F))); }
}
// Append a cell's grapheme (base + up to two combining marks).
static void appendCellUtf8(std::string& d, const Cell& c) {
    appendUtf8(d, c.cp);
    if (c.comb0) appendUtf8(d, c.comb0);
    if (c.comb1) appendUtf8(d, c.comb1);
}

ScreenBuffer::ScreenBuffer(int cols, int rows)
    : cols_(std::max(1, cols)), rows_(std::max(1, rows)) {
    penFg_ = savedFg_ = cfg_.defaultFg;
    penBg_ = savedBg_ = cfg_.defaultBg;
    grid_.assign((size_t)cols_ * rows_, blank());
    rowDirty_.assign(rows_, 1);
    regionTop_ = 0; regionBottom_ = rows_ - 1;
    resetTabs();
}

void ScreenBuffer::applyConfig(const Config& c) {
    uint32_t oldFg = cfg_.defaultFg, oldBg = cfg_.defaultBg;
    cfg_ = c;
    if (cfg_.scrollback < 0) cfg_.scrollback = 0;
    if ((size_t)cfg_.scrollback > kHardMaxScrollback) cfg_.scrollback = (int)kHardMaxScrollback;
    while ((int)scroll_.size() > cfg_.scrollback) scroll_.pop_front();
    if (penFg_ == oldFg)   penFg_   = cfg_.defaultFg;   // keep default-coloured pen in sync
    if (penBg_ == oldBg)   penBg_   = cfg_.defaultBg;
    if (savedFg_ == oldFg) savedFg_ = cfg_.defaultFg;
    if (savedBg_ == oldBg) savedBg_ = cfg_.defaultBg;
    // Re-theme cells that use the *default* colour; explicitly SGR-coloured cells
    // keep their resolved colour. Runs once at session start (screen is empty).
    if (oldFg != cfg_.defaultFg || oldBg != cfg_.defaultBg) {
        auto remap = [&](Cell& cell) {
            if (cell.fg == oldFg) cell.fg = cfg_.defaultFg;
            if (cell.bg == oldBg) cell.bg = cfg_.defaultBg;
        };
        for (Cell& cell : grid_) remap(cell);
        for (Cell& cell : mainSaved_.grid) remap(cell);
        for (Line& ln : scroll_) for (Cell& cell : ln) remap(cell);
    }
    markAll();
}

void ScreenBuffer::resetTabs() {
    tabs_.assign(cols_, 0);
    for (int c = 0; c < cols_; c += 8) tabs_[c] = 1;
}

void ScreenBuffer::reset() {
    grid_.assign((size_t)cols_ * rows_, blank());
    scroll_.clear();
    inAlt_ = false; mainSaved_ = ScreenState{};
    cx_ = cy_ = savedCx_ = savedCy_ = 0;
    penFg_ = cfg_.defaultFg; penBg_ = cfg_.defaultBg; penAttr_ = ATTR_NONE;
    regionTop_ = 0; regionBottom_ = rows_ - 1;
    cursorVisible_ = true; wrapPending_ = false;
    autoWrap_ = true; originMode_ = false; reverseScreen_ = false; insertMode_ = false;
    viewOffset_ = 0; selActive_ = false;
    lastGlyphRow_ = lastGlyphCol_ = -1;
    resetTabs();
    markAll();
}

std::vector<Cell> ScreenBuffer::reflowMain(const std::vector<Cell>& src, int srcCols, int srcRows,
                                          int srcCy, int cols, int rows, int& newCy) {
    // Anchor on the CONTENT, not the physical bottom. A partially-filled terminal keeps its text
    // in the TOP rows with the cursor a few lines down and blank rows below; bottom-anchoring on a
    // shrink copied the blank band and dropped the text (the keyboard-open "blank screen" bug).
    // Anchor to the last meaningful row (max of the cursor row and the last non-blank row) instead,
    // and route any overflow through scrollback so an open/close cycle is reversible.
    const int copyCols = std::min(srcCols, cols);
    std::vector<Cell> ng((size_t)cols * rows, blank());
    auto copyRow = [&](int dr, int sr) {
        std::memcpy(&ng[(size_t)dr * cols], &src[(size_t)sr * srcCols], sizeof(Cell) * copyCols);
    };

    int lastUsed = std::max(0, std::min(srcCy, srcRows - 1));
    for (int r = srcRows - 1; r > lastUsed; --r) {
        const Cell* row = &src[(size_t)r * srcCols];
        bool blankRow = true;
        for (int c = 0; c < srcCols; ++c)
            if (row[c].cp != ' ' && row[c].cp != 0) { blankRow = false; break; }
        if (!blankRow) { lastUsed = r; break; }
    }
    const int content = lastUsed + 1;              // meaningful old rows [0..lastUsed]

    if (content <= rows) {
        // Everything fits. On grow, pull recent scrollback back onto the top so closing the
        // keyboard restores exactly what opening it pushed away.
        const int pull = std::min(rows - content, (int)scroll_.size());
        const int base = (int)scroll_.size() - pull;
        for (int i = 0; i < pull; ++i) {
            const Line& ln = scroll_[base + i];     // oldest of the block first
            Cell* d = &ng[(size_t)i * cols];
            const int n = std::min((int)ln.size(), cols);
            for (int c = 0; c < n; ++c) d[c] = ln[c];
        }
        for (int i = 0; i < pull; ++i) scroll_.pop_back();
        for (int r = 0; r < content; ++r) copyRow(pull + r, r);
        newCy = srcCy + pull;
    } else {
        // Shrunk below the content height: the top overflow scrolls into history; the bottom
        // `rows` meaningful lines stay on screen.
        const int off = content - rows;
        for (int r = 0; r < off; ++r) {
            Line ln(src.begin() + (size_t)r * srcCols,
                    src.begin() + (size_t)r * srcCols + srcCols);
            pushScrollback(ln);
        }
        for (int r = 0; r < rows; ++r) copyRow(r, off + r);
        newCy = srcCy - off;
    }
    return ng;
}

void ScreenBuffer::resize(int cols, int rows) {
    cols = std::max(1, cols); rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) return;

    const int oldCols = cols_;
    const int oldRows = rows_;
    int newCy = cy_;
    std::vector<Cell> ng;

    if (!inAlt_) {
        ng = reflowMain(grid_, oldCols, oldRows, cy_, cols, rows, newCy);
    } else {
        // ---- alternate screen ----
        // Full-screen apps repaint on SIGWINCH, so a top-anchored copy of the ACTIVE grid is
        // enough here. The parked MAIN screen in mainSaved_ is deliberately NOT touched: nothing
        // will ever repaint it, so a truncating copy would destroy its bottom rows and its right
        // columns permanently. That was the "the shell screen does not come back after quitting
        // nano/vim" bug. It keeps its own geometry and is reflowed once, on the way out, by
        // leaveAltScreen() using the same reversible path the main screen uses.
        const int copyCols = std::min(oldCols, cols);
        const int cr = std::min(oldRows, rows);
        ng.assign((size_t)cols * rows, blank());
        for (int r = 0; r < cr; ++r)
            std::memcpy(&ng[(size_t)r * cols], &grid_[(size_t)r * oldCols],
                        sizeof(Cell) * copyCols);
    }

    grid_.swap(ng);
    cols_ = cols; rows_ = rows;
    rowDirty_.assign(rows_, 1);
    regionTop_ = 0; regionBottom_ = rows_ - 1;
    cx_ = std::min(cx_, cols_ - 1);
    cy_ = std::max(0, std::min(newCy, rows_ - 1));
    wrapPending_ = false;
    lastGlyphRow_ = lastGlyphCol_ = -1;
    resetTabs();
    viewOffset_ = std::min(viewOffset_, (int)scroll_.size());
    markAll();
}

void ScreenBuffer::markAll() {
    std::fill(rowDirty_.begin(), rowDirty_.end(), 1);
    dirty_ = true;
}
bool ScreenBuffer::takeDirty() {
    bool d = dirty_;
    // The cursor is part of the painted frame, so a change in its rendered state is damage
    // even when no cell was touched. Checked here once, centrally, rather than in each
    // cursor mutator: a single site cannot be left incomplete, and put() pays nothing.
    // Skipped while the viewport is scrolled into history, where snapshot() reports the
    // cursor as -1 and it is not drawn (scrollView/scrollToBottom already markAll()).
    if (viewOffset_ == 0) {
        if (cx_ != lastCursorCx_ || cy_ != lastCursorCy_ ||
            cursorVisible_ != lastCursorVisible_) {
            d = true;
            lastCursorCx_ = cx_;
            lastCursorCy_ = cy_;
            lastCursorVisible_ = cursorVisible_;
        }
    }
    dirty_ = false;
    std::fill(rowDirty_.begin(), rowDirty_.end(), 0);
    return d;
}

// ---- modes ----
void ScreenBuffer::setOriginMode(bool v) { originMode_ = v; setCursor(0, 0); }

void ScreenBuffer::enterAltScreen(bool clear) {
    if (inAlt_) { if (clear) { std::fill(grid_.begin(), grid_.end(), pen(' ')); markAll(); } return; }
    // Park the ENTIRE main screen: grid, geometry, cursor, its own DECSC slot and its margins.
    // Nothing may mutate it until we switch back (see ScreenState).
    mainSaved_.grid         = grid_;
    mainSaved_.cols         = cols_;
    mainSaved_.rows         = rows_;
    mainSaved_.cx           = cx_;
    mainSaved_.cy           = cy_;
    mainSaved_.savedCx      = savedCx_;
    mainSaved_.savedCy      = savedCy_;
    mainSaved_.savedAttr    = savedAttr_;
    mainSaved_.savedFg      = savedFg_;
    mainSaved_.savedBg      = savedBg_;
    mainSaved_.regionTop    = regionTop_;
    mainSaved_.regionBottom = regionBottom_;
    mainSaved_.valid        = true;

    inAlt_ = true;
    scrollToBottom();   // the main screen's history is locked away for the duration
    clearSelection();   // selection is in ABSOLUTE line coords -> meaningless across the swap
    // The alternate screen gets its OWN empty saved-cursor slot. Sharing one slot let a DECSC
    // issued by the editor overwrite the cursor CSI ?1049h had parked for the main screen, so
    // quitting dropped the shell prompt at the editor's cursor and it overwrote the restored
    // screen from there. xterm and Termux both keep one slot per screen.
    savedCx_ = savedCy_ = 0;
    savedAttr_ = ATTR_NONE;
    savedFg_ = cfg_.defaultFg; savedBg_ = cfg_.defaultBg;
    if (clear) std::fill(grid_.begin(), grid_.end(), pen(' '));
    markAll();
}

void ScreenBuffer::leaveAltScreen(bool /*restore*/) {
    if (!inAlt_) return;
    inAlt_ = false;   // FIRST: pushScrollback() is a no-op while the alternate screen is active,
                      // and the reflow below needs to be able to spill into scrollback.
    if (mainSaved_.valid) {
        int newCy = mainSaved_.cy;
        if (mainSaved_.cols != cols_ || mainSaved_.rows != rows_) {
            // The terminal was resized (IME, rotation) while the editor owned the display.
            // Reflow the main screen ONCE, here, content-anchored through scrollback, the same
            // reversible path resize() uses, instead of having truncated it on every resize.
            grid_ = reflowMain(mainSaved_.grid, mainSaved_.cols, mainSaved_.rows,
                               mainSaved_.cy, cols_, rows_, newCy);
        } else {
            grid_ = mainSaved_.grid;
        }
        // The reflow may have shifted every row; the parked saved-cursor slot has to move with it,
        // because the parser calls restoreCursor() straight after this for CSI ?1049 l.
        const int shift = newCy - mainSaved_.cy;
        cx_        = mainSaved_.cx;
        cy_        = newCy;
        savedCx_   = mainSaved_.savedCx;
        savedCy_   = mainSaved_.savedCy + shift;
        savedAttr_ = mainSaved_.savedAttr;
        savedFg_   = mainSaved_.savedFg;
        savedBg_   = mainSaved_.savedBg;
        regionTop_    = mainSaved_.regionTop;
        regionBottom_ = mainSaved_.regionBottom;
        mainSaved_ = ScreenState{};
    }
    // Clamp everything the reflow could have pushed out of range.
    savedCx_ = std::max(0, std::min(savedCx_, cols_ - 1));
    savedCy_ = std::max(0, std::min(savedCy_, rows_ - 1));
    regionTop_    = std::max(0, std::min(regionTop_, rows_ - 1));
    regionBottom_ = std::max(regionTop_, std::min(regionBottom_, rows_ - 1));
    clampCursor();
    viewOffset_ = 0;     // symmetric with enterAltScreen(): the viewport is back at the live bottom
    wrapPending_ = false;
    lastGlyphRow_ = lastGlyphCol_ = -1;
    clearSelection();
    markAll();
}

// ---- cursor ----
void ScreenBuffer::clampCursor() {
    cx_ = std::max(0, std::min(cx_, cols_ - 1));
    cy_ = std::max(0, std::min(cy_, rows_ - 1));
}
void ScreenBuffer::setCursor(int row, int col) {
    if (originMode_) {
        row += regionTop_;
        row = std::min(row, regionBottom_);
        row = std::max(row, regionTop_);
    }
    cy_ = std::max(0, std::min(row, rows_ - 1));
    cx_ = std::max(0, std::min(col, cols_ - 1));
    wrapPending_ = false;
}
void ScreenBuffer::setCursorRow(int row) { setCursor(originMode_ ? row - regionTop_ : row, cx_); }
void ScreenBuffer::setCursorCol(int col) { cx_ = std::max(0, std::min(col, cols_ - 1)); wrapPending_ = false; }
void ScreenBuffer::moveCursor(int dRow, int dCol) {
    cy_ += dRow; cx_ += dCol; clampCursor(); wrapPending_ = false;
}
void ScreenBuffer::carriageReturn() { cx_ = 0; wrapPending_ = false; }
void ScreenBuffer::backspace() { if (cx_ > 0) cx_--; wrapPending_ = false; }

void ScreenBuffer::lineFeed() {
    if (cy_ == regionBottom_) scrollUp(1);
    else if (cy_ < rows_ - 1) cy_++;
    wrapPending_ = false;
}
void ScreenBuffer::reverseLineFeed() {
    if (cy_ == regionTop_) scrollDown(1);
    else if (cy_ > 0) cy_--;
    wrapPending_ = false;
}
void ScreenBuffer::saveCursor() {
    savedCx_ = cx_; savedCy_ = cy_;
    savedAttr_ = penAttr_; savedFg_ = penFg_; savedBg_ = penBg_;
}
void ScreenBuffer::resetSavedCursor() {
    savedCx_ = savedCy_ = 0;
    savedAttr_ = ATTR_NONE;
    savedFg_ = cfg_.defaultFg; savedBg_ = cfg_.defaultBg;
}
void ScreenBuffer::restoreCursor() {
    cx_ = savedCx_; cy_ = savedCy_;
    penAttr_ = savedAttr_; penFg_ = savedFg_; penBg_ = savedBg_;
    clampCursor(); wrapPending_ = false;
}

// ---- tab stops ----
void ScreenBuffer::tab() { tabForward(1); }
void ScreenBuffer::tabForward(int n) {
    // Past cols_ stops the cursor is pinned at the last column and further iterations do
    // nothing, so a CSI I with a huge parameter must not spin cols_ times for each of them.
    n = std::min(n, cols_);
    for (int k = 0; k < n; ++k) {
        int c = cx_ + 1;
        while (c < cols_ && !tabs_[c]) c++;
        cx_ = std::min(c, cols_ - 1);
    }
    wrapPending_ = false;
}
void ScreenBuffer::tabBack(int n) {
    n = std::min(n, cols_);       // pinned at column 0 after that; see tabForward
    for (int k = 0; k < n; ++k) {
        int c = cx_ - 1;
        while (c > 0 && !tabs_[c]) c--;
        cx_ = std::max(c, 0);
    }
    wrapPending_ = false;
}
void ScreenBuffer::setTabStop() { if (cx_ >= 0 && cx_ < cols_) tabs_[cx_] = 1; }
void ScreenBuffer::clearTabStop() { if (cx_ >= 0 && cx_ < cols_) tabs_[cx_] = 0; }
void ScreenBuffer::clearAllTabStops() { std::fill(tabs_.begin(), tabs_.end(), 0); }

// ---- writing ----
void ScreenBuffer::newLineIfWrap() {
    if (wrapPending_) {
        cx_ = 0;
        if (cy_ == regionBottom_) scrollUp(1);
        else if (cy_ < rows_ - 1) cy_++;
        wrapPending_ = false;
    }
}
void ScreenBuffer::attachCombining(uint32_t cp) {
    if (lastGlyphRow_ < 0 || lastGlyphRow_ >= rows_ ||
        lastGlyphCol_ < 0 || lastGlyphCol_ >= cols_) return;
    Cell& base = at(lastGlyphRow_, lastGlyphCol_);
    if (base.attr & ATTR_WIDE_TAIL) return;
    if (base.comb0 == 0)      base.comb0 = cp;
    else if (base.comb1 == 0) base.comb1 = cp;
    else return;                       // >2 stacking marks: dropped (documented)
    base.attr |= ATTR_COMBINING;
    markRow(lastGlyphRow_);
}

void ScreenBuffer::put(uint32_t cp) {
    int w = charWidth(cp);
    if (w == 0) { attachCombining(cp); return; }   // compose onto the preceding base
    scrollToBottom();
    newLineIfWrap();
    if (w == 2 && cx_ == cols_ - 1) {
        // no room for a wide glyph on this line; wrap first
        at(cy_, cx_) = pen(' ');
        cx_ = 0;
        if (cy_ == regionBottom_) scrollUp(1); else if (cy_ < rows_ - 1) cy_++;
    }
    if (insertMode_) insertChars(w);               // IRM: shift the line right first
    Cell c = pen(cp);
    if (w == 2) c.attr |= ATTR_WIDE;
    at(cy_, cx_) = c;
    lastGlyphRow_ = cy_; lastGlyphCol_ = cx_;      // base cell for any combining marks
    if (w == 2 && cx_ + 1 < cols_) {
        Cell tail = pen(' '); tail.attr |= ATTR_WIDE_TAIL;
        at(cy_, cx_ + 1) = tail;
    }
    markRow(cy_);
    cx_ += w;
    if (cx_ >= cols_) { cx_ = cols_ - 1; wrapPending_ = autoWrap_; }
}

// ---- erasing / editing ----
void ScreenBuffer::eraseInLine(int mode) {
    int r = cy_;
    int from = 0, to = cols_ - 1;
    if (mode == 0) from = cx_;
    else if (mode == 1) to = cx_;
    for (int c = from; c <= to; ++c) at(r, c) = pen(' ');
    markRow(r); wrapPending_ = false; lastGlyphRow_ = lastGlyphCol_ = -1;
}
void ScreenBuffer::eraseInDisplay(int mode) {
    if (mode == 0) {
        eraseInLine(0);
        for (int r = cy_ + 1; r < rows_; ++r) { for (int c = 0; c < cols_; ++c) at(r, c) = pen(' '); markRow(r); }
    } else if (mode == 1) {
        eraseInLine(1);
        for (int r = 0; r < cy_; ++r) { for (int c = 0; c < cols_; ++c) at(r, c) = pen(' '); markRow(r); }
    } else if (mode == 2 || mode == 3) {
        for (int i = 0; i < (int)grid_.size(); ++i) grid_[i] = pen(' ');
        if (mode == 3) scroll_.clear();
        markAll();
    }
    wrapPending_ = false;
}
void ScreenBuffer::eraseChars(int n) {
    // Clamped BEFORE the addition: cx_ + n overflows a signed int for a large ECH parameter,
    // which is undefined behaviour, and the loop bound is then anyone's guess.
    n = std::max(1, std::min(n, cols_ - cx_));
    for (int c = cx_; c < cx_ + n; ++c) at(cy_, c) = pen(' ');
    markRow(cy_);
}
void ScreenBuffer::deleteChars(int n) {
    n = std::max(1, std::min(n, cols_ - cx_));
    for (int c = cx_; c < cols_; ++c)
        at(cy_, c) = (c + n < cols_) ? at(cy_, c + n) : pen(' ');
    markRow(cy_);
}
void ScreenBuffer::insertChars(int n) {
    n = std::max(1, std::min(n, cols_ - cx_));
    for (int c = cols_ - 1; c >= cx_; --c)
        at(cy_, c) = (c - n >= cx_) ? at(cy_, c - n) : pen(' ');
    markRow(cy_);
}
void ScreenBuffer::insertLines(int n) {
    if (cy_ < regionTop_ || cy_ > regionBottom_) return;
    n = std::max(1, std::min(n, regionBottom_ - cy_ + 1));
    for (int r = regionBottom_; r >= cy_ + n; --r)
        for (int c = 0; c < cols_; ++c) at(r, c) = at(r - n, c);
    for (int r = cy_; r < cy_ + n; ++r)
        for (int c = 0; c < cols_; ++c) at(r, c) = pen(' ');
    for (int r = cy_; r <= regionBottom_; ++r) markRow(r);
}
void ScreenBuffer::deleteLines(int n) {
    if (cy_ < regionTop_ || cy_ > regionBottom_) return;
    n = std::max(1, std::min(n, regionBottom_ - cy_ + 1));
    for (int r = cy_; r <= regionBottom_ - n; ++r)
        for (int c = 0; c < cols_; ++c) at(r, c) = at(r + n, c);
    for (int r = regionBottom_ - n + 1; r <= regionBottom_; ++r)
        for (int c = 0; c < cols_; ++c) at(r, c) = pen(' ');
    for (int r = cy_; r <= regionBottom_; ++r) markRow(r);
}

// ---- scrolling ----
void ScreenBuffer::pushScrollback(const Line& line) {
    if (inAlt_) return;   // alt screen has no scrollback
    scroll_.push_back(line);
    if ((int)scroll_.size() > cfg_.scrollback) scroll_.pop_front();
    if (viewOffset_ > 0) viewOffset_ = std::min(viewOffset_ + 1, (int)scroll_.size());
}
void ScreenBuffer::scrollUp(int n) {
    n = std::max(1, n);
    bool full = (regionTop_ == 0 && regionBottom_ == rows_ - 1);
    // Beyond this the state stops changing: the region is entirely blank, and when the whole
    // screen scrolls the history holds nothing but the blank lines this loop pushed into it.
    // The clamp is therefore exact, and it is what keeps `printf '\033[2000000000S'` from
    // scrolling for the better part of an hour with the UI thread waiting on the parser.
    n = std::min(n, (regionBottom_ - regionTop_ + 1) + (full && !inAlt_ ? cfg_.scrollback : 0));
    for (int k = 0; k < n; ++k) {
        if (full && !inAlt_) {
            Line top(grid_.begin(), grid_.begin() + cols_);
            pushScrollback(top);
        }
        for (int r = regionTop_; r < regionBottom_; ++r)
            for (int c = 0; c < cols_; ++c) at(r, c) = at(r + 1, c);
        for (int c = 0; c < cols_; ++c) at(regionBottom_, c) = pen(' ');
    }
    markAll();
}
void ScreenBuffer::scrollDown(int n) {
    // Nothing leaves the region downwards, so a full region height blanks it; see scrollUp.
    n = std::max(1, std::min(n, regionBottom_ - regionTop_ + 1));
    for (int k = 0; k < n; ++k) {
        for (int r = regionBottom_; r > regionTop_; --r)
            for (int c = 0; c < cols_; ++c) at(r, c) = at(r - 1, c);
        for (int c = 0; c < cols_; ++c) at(regionTop_, c) = pen(' ');
    }
    markAll();
}
void ScreenBuffer::setScrollRegion(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom <= 0 || bottom > rows_) bottom = rows_;
    if (top >= bottom) { top = 0; bottom = rows_; }
    regionTop_ = top; regionBottom_ = bottom - 1;
    setCursor(0, 0);
}

// ---- SGR ----
void ScreenBuffer::applySGR(const std::vector<int>& p) {
    auto n = p.size();
    if (n == 0) { penAttr_ = ATTR_NONE; penFg_ = cfg_.defaultFg; penBg_ = cfg_.defaultBg; return; }
    for (size_t i = 0; i < n; ++i) {
        int v = p[i];
        switch (v) {
            case 0:  penAttr_ = ATTR_NONE; penFg_ = cfg_.defaultFg; penBg_ = cfg_.defaultBg; break;
            case 1:  penAttr_ |= ATTR_BOLD; break;
            case 2:  penAttr_ |= ATTR_DIM; break;
            case 3:  penAttr_ |= ATTR_ITALIC; break;
            case 4:  penAttr_ |= ATTR_UNDERLINE; break;
            case 5: case 6: penAttr_ |= ATTR_BLINK; break;
            case 7:  penAttr_ |= ATTR_INVERSE; break;
            case 8:  penAttr_ |= ATTR_INVISIBLE; break;
            case 9:  penAttr_ |= ATTR_STRIKE; break;
            case 22: penAttr_ &= ~(ATTR_BOLD | ATTR_DIM); break;
            case 23: penAttr_ &= ~ATTR_ITALIC; break;
            case 24: penAttr_ &= ~ATTR_UNDERLINE; break;
            case 25: penAttr_ &= ~ATTR_BLINK; break;
            case 27: penAttr_ &= ~ATTR_INVERSE; break;
            case 28: penAttr_ &= ~ATTR_INVISIBLE; break;
            case 29: penAttr_ &= ~ATTR_STRIKE; break;
            case 39: penFg_ = cfg_.defaultFg; break;
            case 49: penBg_ = cfg_.defaultBg; break;
            case 38: case 48: {
                bool fg = (v == 38);
                if (i + 1 < n && p[i + 1] == 5 && i + 2 < n) {
                    uint32_t col = cfg_.color(p[i + 2]); i += 2;
                    if (fg) penFg_ = col; else penBg_ = col;
                } else if (i + 1 < n && p[i + 1] == 2 && i + 4 < n) {
                    uint32_t col = 0xFF000000u | ((p[i+2]&0xFF)<<16) | ((p[i+3]&0xFF)<<8) | (p[i+4]&0xFF);
                    i += 4;
                    if (fg) penFg_ = col; else penBg_ = col;
                }
                break;
            }
            default:
                if (v >= 30 && v <= 37) penFg_ = cfg_.ansi(v - 30, false);
                else if (v >= 40 && v <= 47) penBg_ = cfg_.ansi(v - 40, false);
                else if (v >= 90 && v <= 97) penFg_ = cfg_.ansi(v - 90, true);
                else if (v >= 100 && v <= 107) penBg_ = cfg_.ansi(v - 100, true);
                break;
        }
    }
}

// ---- viewport ----
void ScreenBuffer::scrollView(int deltaLines) {
    // The scrollback belongs to the MAIN screen. While the alternate screen is up, walking the
    // viewport into it rendered rows of old shell output on top of the editor, and the editor's
    // next glyph (put() -> scrollToBottom()) snapped it back. That was the "scrolling in nano does
    // nothing but flicker" report. The gesture is routed to the program by the UI instead.
    if (inAlt_) return;
    int v = viewOffset_ + deltaLines;
    v = std::max(0, std::min(v, (int)scroll_.size()));
    if (v != viewOffset_) { viewOffset_ = v; markAll(); }
}
void ScreenBuffer::scrollToBottom() {
    if (viewOffset_ != 0) { viewOffset_ = 0; markAll(); }
}

// ---- absolute-line access ----
const ScreenBuffer::Line* ScreenBuffer::absLine(int abs, Line& tmp) const {
    int sb = (int)scroll_.size();
    if (abs < 0 || abs >= sb + rows_) return nullptr;
    if (abs < sb) return &scroll_[abs];
    int r = abs - sb;
    tmp.assign(grid_.begin() + (size_t)r * cols_, grid_.begin() + (size_t)(r + 1) * cols_);
    return &tmp;
}

// ---- selection ----
void ScreenBuffer::selectStart(int viewRow, int viewCol) {
    int top = totalLines() - rows_ - viewOffset_;
    selAnchorRow_ = selFocusRow_ = top + viewRow;
    selAnchorCol_ = selFocusCol_ = std::max(0, std::min(viewCol, cols_ - 1));
    selActive_ = true; markAll();
}
void ScreenBuffer::selectExtend(int viewRow, int viewCol) {
    if (!selActive_) return;
    int top = totalLines() - rows_ - viewOffset_;
    selFocusRow_ = top + viewRow;
    selFocusCol_ = std::max(0, std::min(viewCol, cols_ - 1));
    markAll();
}
void ScreenBuffer::selectWord(int viewRow, int viewCol) {
    int top = totalLines() - rows_ - viewOffset_;
    int absRow = top + viewRow;
    Line tmp; const Line* ln = absLine(absRow, tmp);
    if (!ln) return;
    auto isWord = [](uint32_t c){ return (c=='_' ) || (c>='0'&&c<='9') ||
        (c>='A'&&c<='Z') || (c>='a'&&c<='z') || c>=0x80; };
    int c = std::max(0, std::min(viewCol, cols_ - 1));
    if (c >= (int)ln->size() || !isWord((*ln)[c].cp)) { selectStart(viewRow, viewCol); return; }
    int lo = c, hi = c;
    while (lo > 0 && isWord((*ln)[lo - 1].cp)) lo--;
    while (hi + 1 < (int)ln->size() && isWord((*ln)[hi + 1].cp)) hi++;
    selActive_ = true;
    selAnchorRow_ = selFocusRow_ = absRow;
    selAnchorCol_ = lo; selFocusCol_ = hi;
    markAll();
}
void ScreenBuffer::clearSelection() { if (selActive_) { selActive_ = false; markAll(); } }

std::string ScreenBuffer::selectionText() const {
    if (!selActive_) return {};
    int r0 = selAnchorRow_, c0 = selAnchorCol_, r1 = selFocusRow_, c1 = selFocusCol_;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { std::swap(r0, r1); std::swap(c0, c1); }
    std::string out;
    Line tmp;
    for (int r = r0; r <= r1; ++r) {
        const Line* ln = absLine(r, tmp);
        std::string row;
        if (ln) {
            int from = (r == r0) ? c0 : 0;
            int to   = (r == r1) ? c1 : (int)ln->size() - 1;
            from = std::max(0, from); to = std::min(to, (int)ln->size() - 1);
            for (int c = from; c <= to; ++c) {
                const Cell& cell = (*ln)[c];
                if (cell.attr & ATTR_WIDE_TAIL) continue;
                appendCellUtf8(row, cell);
            }
            size_t last = row.find_last_not_of(' ');
            if (last == std::string::npos) row.clear(); else row.erase(last + 1);
        }
        out += row;
        if (r != r1) out.push_back('\n');
    }
    return out;
}

// ---- search ----
int ScreenBuffer::searchNext(const std::string& q, int fromAbsRow, bool forward) const {
    if (q.empty()) return -1;
    int total = totalLines();
    Line tmp;
    auto rowText = [&](int abs) -> std::string {
        const Line* ln = absLine(abs, tmp);
        std::string s;
        if (!ln) return s;
        for (const Cell& cell : *ln) {
            if (cell.attr & ATTR_WIDE_TAIL) continue;
            appendCellUtf8(s, cell);
        }
        return s;
    };
    if (forward) {
        for (int r = fromAbsRow; r < total; ++r)
            if (rowText(r).find(q) != std::string::npos) return r;
    } else {
        for (int r = std::min(fromAbsRow, total - 1); r >= 0; --r)
            if (rowText(r).find(q) != std::string::npos) return r;
    }
    return -1;
}
void ScreenBuffer::revealAbsRow(int absRow) {
    int sb = (int)scroll_.size();
    int desiredTop = std::max(0, absRow - rows_ / 2);
    int off = (sb + rows_) - rows_ - desiredTop; // viewOffset that puts desiredTop at view top
    scrollView(off - viewOffset_);
}

// ---- grapheme (base + combining marks) at a viewport cell ----
std::string ScreenBuffer::cellGrapheme(int viewRow, int viewCol) const {
    int top = totalLines() - rows_ - viewOffset_;
    int abs = top + viewRow;
    Line tmp; const Line* ln = absLine(abs, tmp);
    std::string s;
    if (!ln || viewCol < 0 || viewCol >= (int)ln->size()) return s;
    appendCellUtf8(s, (*ln)[viewCol]);
    return s;
}

// ---- snapshot ----
int ScreenBuffer::snapshot(uint32_t* glyphs, uint32_t* fg, uint32_t* bg, uint32_t* attr) const {
    int sb = (int)scroll_.size();
    // Belt and braces: the alternate screen has no history of its own, so it is always rendered
    // from the live grid even if some future path managed to leave viewOffset_ non-zero.
    const int off = inAlt_ ? 0 : viewOffset_;
    int top = sb + rows_ - rows_ - off;          // absolute row shown at view row 0
    // selection bounds normalized
    int sr0 = selAnchorRow_, sc0 = selAnchorCol_, sr1 = selFocusRow_, sc1 = selFocusCol_;
    if (sr1 < sr0 || (sr1 == sr0 && sc1 < sc0)) { std::swap(sr0, sr1); std::swap(sc0, sc1); }

    Line tmp;
    for (int vr = 0; vr < rows_; ++vr) {
        int abs = top + vr;
        const Line* ln = absLine(abs, tmp);
        for (int c = 0; c < cols_; ++c) {
            int idx = vr * cols_ + c;
            Cell cell = (ln && c < (int)ln->size()) ? (*ln)[c] : blank();
            uint32_t f = cell.fg, b = cell.bg;
            uint16_t a = cell.attr;
            if (reverseScreen_) std::swap(f, b);
            if (a & ATTR_INVERSE) std::swap(f, b);
            if (a & ATTR_INVISIBLE) f = b;
            bool sel = selActive_ &&
                       (abs > sr0 || (abs == sr0 && c >= sc0)) &&
                       (abs < sr1 || (abs == sr1 && c <= sc1)) &&
                       (sr0 != sr1 || (c >= sc0 && c <= sc1));
            if (sel) { std::swap(f, b); a |= SNAP_SELECTED; }
            glyphs[idx] = cell.cp;
            fg[idx] = f; bg[idx] = b; attr[idx] = a;
        }
    }
    // cursor index only when the live screen bottom is in view
    if (off == 0) return cy_ * cols_ + cx_;
    return -1;
}

} // namespace xterm
