#pragma once
#include "screen/ScreenBuffer.h"
#include "parser/AnsiParser.h"
#include "parser/ParserHost.h"
#include "xterm/Config.h"
#include <mutex>
#include <string>
#include <functional>
#include <cstdint>

namespace xterm {

// Thread-safe terminal: reader thread feeds bytes; UI thread snapshots and sends
// input. Implements ParserHost so the parser can write responses back to the PTY
// and surface title/clipboard/bell and input modes.
class Terminal : public ParserHost {
public:
    Terminal(int cols, int rows);

    // Set by Session so parser responses (DA/DSR) and mouse events reach the PTY.
    void setResponder(std::function<void(const std::string&)> fn);

    void feed(const uint8_t* data, size_t len);
    void resize(int cols, int rows);

    // meta layout (see indices below); returns cursor index or -2 if buffers too small.
    enum {
        M_COLS = 0, M_ROWS, M_CURSOR_VIS, M_APP_CURSOR, M_MOUSE_MODE,
        M_BRACKETED, M_VIEW_OFFSET, M_SCROLLBACK, M_BELL, M_TITLE_REV,
        M_CLIP_REV, M_ON_ALT, M_FOCUS, M_APPCTRL_REV, M_COUNT
    };
    int snapshot(uint32_t* glyphs, uint32_t* fg, uint32_t* bg, uint32_t* attr,
                 int capacity, int* meta /* size >= M_COUNT */);

    void dims(int& cols, int& rows);
    // True if the screen changed since the last call, or if a UI-consumed input mode
    // changed. The latter is needed because generation_ is the ONLY channel that makes
    // TerminalView re-read meta[]: a program that turns on DECCKM / mouse tracking /
    // focus reporting in a read that damages no cell would otherwise leave the UI
    // encoding input against the previous modes.
    bool takeDirty();

    // ---- configuration / theme ----
    void configure(const Config& c);
    uint32_t cursorColor();
    std::string cellGrapheme(int viewRow, int viewCol);  // base + combining marks (UTF-8)

    // ---- input helpers ----
    std::string wrapPaste(const std::string& text);   // adds bracketed-paste markers if enabled
    void mouseEvent(int button, int col, int row, int type); // 0 press,1 release,2 move

    // ---- viewport ----
    void scrollView(int deltaLines);
    void scrollToBottom();

    // ---- selection / search ----
    void selectStart(int row, int col);
    void selectExtend(int row, int col);
    void selectWord(int row, int col);
    void clearSelection();
    std::string selectionText();
    int  search(const std::string& q, int fromAbsRow, bool forward); // returns absRow or -1

    // ---- out-of-band state (polled by UI) ----
    std::string takeTitle();       // current title
    std::string takeClipboard();   // pending clipboard text, then cleared
    std::string takeAppControl();  // last app-control payload (OSC 5391), then cleared

    // ---- ParserHost ----
    void respond(const std::string& bytes) override;
    void setTitle(const std::string&) override;
    void copyToClipboard(const std::string&) override;
    void bell() override;
    void setAppCursorKeys(bool v) override;
    void setMouseTracking(int mode) override;
    void setMouseSGR(bool v) override;
    void setBracketedPaste(bool v) override;
    void setFocusReporting(bool v) override;
    void appControl(const std::string& payload) override;

private:
    std::mutex m_;
    ScreenBuffer screen_;
    AnsiParser parser_;
    std::function<void(const std::string&)> responder_;

    bool appCursor_ = false;
    int  mouseMode_ = 0;       // 0/1000/1002/1003
    bool mouseSgr_ = false;
    bool bracketedPaste_ = false;
    bool focusReporting_ = false;

    // Revision of the input modes the ANDROID side actually reads out of meta[] and caches:
    // appCursor_ (M_APP_CURSOR), mouseMode_ (M_MOUSE_MODE), focusReporting_ (M_FOCUS)
    // See TerminalView.refreshSnapshot(). Deliberately NOT bumped for mouseSgr_ or
    // bracketedPaste_: those have no Kotlin consumer. mouseSgr_ is read only inside
    // Terminal::mouseEvent() and bracketedPaste_ only inside Terminal::wrapPaste(), both
    // under this mutex at call time, so they are never stale and need no extra repaint.
    // Guarded by m_ (bumped from the parser inside feed(), read inside takeDirty()).
    // Unsigned so wraparound is defined behaviour rather than signed overflow UB: a program
    // is free to toggle these modes indefinitely, and only the != comparison matters.
    // Not exposed through meta[], so the width is a private implementation detail.
    unsigned uiModeRev_ = 0;
    unsigned uiModeRevSeen_ = 0;

    std::string title_;
    int titleRev_ = 0;
    std::string clipboard_;
    int clipRev_ = 0;
    int bellCount_ = 0;
    std::string appCtrl_;
    int appCtrlRev_ = 0;
};

} // namespace xterm
