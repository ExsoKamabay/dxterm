#pragma once
#include "xterm/Types.h"
#include "xterm/Config.h"
#include <vector>
#include <deque>
#include <string>
#include <cstdint>

namespace xterm {

// Extra attribute bit set only in snapshot output (never stored) so the
// renderer can highlight the current selection.
enum SnapAttr : uint16_t { SNAP_SELECTED = 1u << 12 };

// The visible grid, a bounded scrollback history, a scrollback viewport, an
// alternate screen, tab stops, selection and search. 0-based coordinates.
// Not thread-safe on its own; Terminal owns the mutex.
class ScreenBuffer {
public:
    ScreenBuffer(int cols, int rows);

    void resize(int cols, int rows);
    void reset();                 // RIS: full reset

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cursorRow() const { return cy_; }
    int cursorCol() const { return cx_; }
    bool cursorVisible() const { return cursorVisible_; }
    bool onAltScreen() const { return inAlt_; }

    // ---- modes ----
    void setOriginMode(bool v);
    void setAutoWrap(bool v)      { autoWrap_ = v; }
    void setInsertMode(bool v)    { insertMode_ = v; }   // IRM (mode 4)
    void setReverseScreen(bool v) { if (reverseScreen_ != v) { reverseScreen_ = v; markAll(); } }
    void setCursorVisible(bool v) { cursorVisible_ = v; }
    void enterAltScreen(bool clear);
    void leaveAltScreen(bool restore);

    // ---- cursor ----
    void setCursor(int row, int col);   // honours origin mode
    void moveCursor(int dRow, int dCol);
    void setCursorRow(int row);
    void setCursorCol(int col);
    void carriageReturn();
    void lineFeed();
    void reverseLineFeed();
    void backspace();
    void saveCursor();
    void restoreCursor();
    // DECSTR: the DECSC slot returns to the home position with a plain pen. The LIVE cursor
    // is deliberately left where it is, which is what the VT510 table and xterm both do.
    void resetSavedCursor();

    // ---- tab stops ----
    void tab();                  // forward to next stop
    void tabBack(int n);         // CBT
    void tabForward(int n);      // CHT
    void setTabStop();           // HTS at cursor col
    void clearTabStop();         // TBC 0
    void clearAllTabStops();     // TBC 3

    // ---- writing ----
    void put(uint32_t cp);       // honours autowrap + wide chars

    // ---- erasing / editing ----
    void eraseInLine(int mode);      // 0 to end, 1 to start, 2 all
    void eraseInDisplay(int mode);   // 0/1/2/3 (3 clears scrollback)
    void eraseChars(int n);          // ECH
    void deleteChars(int n);         // DCH
    void insertChars(int n);         // ICH
    void insertLines(int n);         // IL
    void deleteLines(int n);         // DL

    // ---- scrolling ----
    void scrollUp(int n);
    void scrollDown(int n);
    void setScrollRegion(int top, int bottom);

    // ---- rendition ----
    void applySGR(const std::vector<int>& params);

    // ---- configuration / theme ----
    void applyConfig(const Config& c);
    const Config& config() const { return cfg_; }

    // Full grapheme (base + combining marks) at a viewport cell, UTF-8.
    std::string cellGrapheme(int viewRow, int viewCol) const;

    // ---- scrollback viewport ----
    // +up into history, -down toward live. INERT while the alternate screen is active: the
    // history belongs to the main screen, and letting the viewport walk into it there rendered
    // old shell output on top of the editor. The UI routes the gesture to the program instead
    // (arrow keys / wheel reports). See TerminalView.applyScrollGesture().
    void scrollView(int deltaLines);
    void scrollToBottom();
    int  viewOffset() const { return viewOffset_; }
    int  scrollbackSize() const { return (int)scroll_.size(); }

    // ---- damage tracking ----
    // True when the VISIBLE state changed since the previous call: either grid damage
    // (markRow/markAll) OR a change in the cursor's rendered state (position + visibility).
    // The cursor is painted from the snapshot, so a cursor-only move (CR, a LF that does
    // NOT scroll, CUP, CUU/CUD/CUF/CUB, CHA/VPA, HT/CBT, BS without erase, DECSC/DECRC)
    // or a DECTCEM show/hide is a visible change even though no cell was touched.
    // Sequences with no visible effect (DSR/CPR, pure SGR) still return false.
    bool takeDirty();
    void markAll();

    // ---- selection (absolute line coordinates: 0 = oldest scrollback) ----
    void selectStart(int viewRow, int viewCol);       // begin at a viewport cell
    void selectExtend(int viewRow, int viewCol);       // drag
    void selectWord(int viewRow, int viewCol);         // double-tap word
    void clearSelection();
    std::string selectionText() const;                 // UTF-8, trailing-space trimmed

    // ---- search over history + screen; returns absolute row or -1 ----
    int  searchNext(const std::string& q, int fromAbsRow, bool forward) const;
    void revealAbsRow(int absRow);                     // set viewport to show a row

    // Read the current viewport into flat buffers (length >= cols*rows).
    // Returns cursor index (row*cols+col) or -1 when the cursor is scrolled off.
    int snapshot(uint32_t* glyphs, uint32_t* fg, uint32_t* bg, uint32_t* attr) const;

private:
    using Line = std::vector<Cell>;

    // A screen that is NOT currently on display. The main screen and the alternate screen are
    // independent buffers (as in xterm, and as in Termux's mMainBuffer/mAltBuffer): resize()
    // only ever reflows the ACTIVE one, and each keeps its own cursor, its own DECSC slot and
    // its own scroll margins. The inactive one also records the geometry it was captured at,
    // so leaveAltScreen() can reflow it once, correctly and content-anchored through scrollback,
    // if the terminal was resized while the editor owned the display. Reflowing it eagerly
    // inside resize() is what used to destroy the shell screen behind nano/vim: nothing repaints
    // an inactive buffer, so a truncating copy loses those rows and columns for good.
    struct ScreenState {
        std::vector<Cell> grid;
        int cols = 0, rows = 0;          // geometry `grid` was captured at
        int cx = 0, cy = 0;              // live cursor of that screen
        int savedCx = 0, savedCy = 0;    // that screen's own DECSC / CSI ?1049 slot
        uint16_t savedAttr = ATTR_NONE;
        uint32_t savedFg = 0, savedBg = 0;
        int regionTop = 0, regionBottom = 0;
        bool valid = false;
    };

    // Content-anchored reflow of a MAIN-screen grid into a new geometry. Overflow at the top is
    // routed into scrollback and, on growth, recent scrollback is pulled back onto the top, so an
    // IME open/close cycle is reversible. Shared by resize() (live main screen) and
    // leaveAltScreen() (main screen coming back at a different size). Must run with inAlt_ ==
    // false, because pushScrollback() is a no-op while the alternate screen is active.
    std::vector<Cell> reflowMain(const std::vector<Cell>& src, int srcCols, int srcRows,
                                 int srcCy, int newCols, int newRows, int& newCy);

    Cell& at(int row, int col) { return grid_[row * cols_ + col]; }
    const Cell& at(int row, int col) const { return grid_[row * cols_ + col]; }
    void clampCursor();
    void newLineIfWrap();
    void pushScrollback(const Line& line);
    Cell pen(uint32_t cp) const { return Cell{ cp, penFg_, penBg_, penAttr_, 0, 0 }; }
    Cell blank() const { return cfg_.blank(); }
    void attachCombining(uint32_t cp);
    void resetTabs();
    int  totalLines() const { return (int)scroll_.size() + rows_; }
    // Absolute-line access spanning scrollback (0..scroll_.size()-1) then screen.
    const Line* absLine(int abs, Line& tmp) const;
    void markRow(int r) { if (r >= 0 && r < rows_) rowDirty_[r] = 1; dirty_ = true; }

    Config cfg_;                       // theme + scrollback (single source of truth)
    int cols_, rows_;
    int cx_ = 0, cy_ = 0;
    int savedCx_ = 0, savedCy_ = 0;
    uint16_t savedAttr_ = ATTR_NONE;
    uint32_t savedFg_ = 0, savedBg_ = 0;    // set from cfg_ in the constructor
    int regionTop_ = 0, regionBottom_ = 0;
    bool cursorVisible_ = true;
    bool wrapPending_ = false;
    bool autoWrap_ = true;
    bool insertMode_ = false;
    bool originMode_ = false;
    bool reverseScreen_ = false;
    bool dirty_ = true;
    // Cursor state as observed by the last takeDirty(), i.e. the state the previously
    // reported frame was drawn from. Compared in takeDirty() so cursor-only motion raises
    // damage WITHOUT sprinkling markRow()/markAll() across ~20 cursor mutators (which would
    // be easy to leave incomplete) and without any cost in the put() hot path.
    // Only tracked while viewOffset_ == 0: with the viewport scrolled into history
    // snapshot() reports cursor = -1 and the cursor is not painted, so comparing there
    // would force repaints that change nothing on screen. Returning to the live bottom
    // goes through scrollView()/scrollToBottom(), which already markAll().
    // Sentinels are unreachable as a real cursor state, so the first call reports true.
    int  lastCursorCx_ = -1, lastCursorCy_ = -1;
    bool lastCursorVisible_ = false;

    uint32_t penFg_ = 0;                 // set from cfg_ in the constructor
    uint32_t penBg_ = 0;
    uint16_t penAttr_ = ATTR_NONE;
    int lastGlyphRow_ = -1, lastGlyphCol_ = -1;  // last base cell (for combining marks)

    std::vector<Cell> grid_;          // rows_ * cols_ (live screen)
    ScreenState mainSaved_;           // the parked MAIN screen while inAlt_ (see ScreenState)
    bool inAlt_ = false;
    std::deque<Line> scroll_;         // bounded ring history (front = oldest, O(1) ends)
    std::vector<uint8_t> tabs_;       // per-column tab stops
    std::vector<uint8_t> rowDirty_;   // per-row damage
    int viewOffset_ = 0;              // lines scrolled up from the live bottom

    // selection, absolute coordinates
    bool selActive_ = false;
    int selAnchorRow_ = 0, selAnchorCol_ = 0;
    int selFocusRow_ = 0, selFocusCol_ = 0;

    static constexpr size_t kHardMaxScrollback = 100000;  // safety ceiling
};

} // namespace xterm
