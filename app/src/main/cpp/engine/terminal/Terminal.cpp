#include "terminal/Terminal.h"
#include <cstdio>

namespace xterm {

Terminal::Terminal(int cols, int rows)
    : screen_(cols, rows), parser_(screen_, *this) {}

void Terminal::setResponder(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> lk(m_);
    responder_ = std::move(fn);
}

void Terminal::feed(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(m_);
    parser_.feed(data, len);
}

void Terminal::resize(int cols, int rows) {
    std::lock_guard<std::mutex> lk(m_);
    screen_.resize(cols, rows);
}

int Terminal::snapshot(uint32_t* glyphs, uint32_t* fg, uint32_t* bg, uint32_t* attr,
                       int capacity, int* meta) {
    std::lock_guard<std::mutex> lk(m_);
    int c = screen_.cols(), r = screen_.rows();
    meta[M_COLS] = c;
    meta[M_ROWS] = r;
    meta[M_CURSOR_VIS] = screen_.cursorVisible() ? 1 : 0;
    meta[M_APP_CURSOR] = appCursor_ ? 1 : 0;
    meta[M_MOUSE_MODE] = mouseMode_;
    meta[M_BRACKETED]  = bracketedPaste_ ? 1 : 0;
    meta[M_VIEW_OFFSET] = screen_.viewOffset();
    meta[M_SCROLLBACK]  = screen_.scrollbackSize();
    meta[M_BELL] = bellCount_;
    meta[M_TITLE_REV] = titleRev_;
    meta[M_CLIP_REV] = clipRev_;
    meta[M_ON_ALT] = screen_.onAltScreen() ? 1 : 0;
    meta[M_FOCUS] = focusReporting_ ? 1 : 0;
    meta[M_APPCTRL_REV] = appCtrlRev_;
    if (capacity < c * r) return -2;
    return screen_.snapshot(glyphs, fg, bg, attr);
}

void Terminal::dims(int& cols, int& rows) {
    std::lock_guard<std::mutex> lk(m_);
    cols = screen_.cols(); rows = screen_.rows();
}

bool Terminal::takeDirty() {
    std::lock_guard<std::mutex> lk(m_);
    // Screen damage is taken UNCONDITIONALLY (never short-circuited) so its flag is always
    // consumed and can never stay latched into a later call.
    bool d = screen_.takeDirty();
    // A UI-consumed input mode (DECCKM / mouse tracking / focus reporting) can change in a
    // read that damages no cell. generation_ is the only signal that makes TerminalView call
    // refreshSnapshot() and re-read meta[], so report it as damage; otherwise the UI keeps
    // encoding keys and touches against the previous modes.
    if (uiModeRev_ != uiModeRevSeen_) { uiModeRevSeen_ = uiModeRev_; d = true; }
    return d;
}

void Terminal::configure(const Config& c) {
    std::lock_guard<std::mutex> lk(m_);
    screen_.applyConfig(c);
}
uint32_t Terminal::cursorColor() {
    std::lock_guard<std::mutex> lk(m_);
    return screen_.config().cursorColor;
}
std::string Terminal::cellGrapheme(int viewRow, int viewCol) {
    std::lock_guard<std::mutex> lk(m_);
    return screen_.cellGrapheme(viewRow, viewCol);
}


std::string Terminal::wrapPaste(const std::string& text) {
    std::lock_guard<std::mutex> lk(m_);
    if (!bracketedPaste_) return text;
    return "\x1b[200~" + text + "\x1b[201~";
}

void Terminal::mouseEvent(int button, int col, int row, int type) {
    std::lock_guard<std::mutex> lk(m_);
    if (mouseMode_ == 0) return;
    if (type == 2) { // motion
        if (mouseMode_ != 1002 && mouseMode_ != 1003) return;
        if (mouseMode_ == 1002 && button < 0) return; // button-drag only
    }
    int cb = (button < 0 ? 3 : button);       // 3 = release in legacy
    if (type == 2) cb += 32;                   // motion flag
    std::string out;
    char buf[48];
    if (mouseSgr_) {
        int b = (button < 0 ? 0 : button) + (type == 2 ? 32 : 0);
        std::snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c", b, col + 1, row + 1,
                      type == 1 ? 'm' : 'M');
        out = buf;
    } else {
        out.push_back(0x1b); out.push_back('[');  out.push_back('M');
        out.push_back((char)(32 + cb));
        out.push_back((char)(32 + col + 1));
        out.push_back((char)(32 + row + 1));
    }
    if (responder_) responder_(out);
}

void Terminal::scrollView(int d) { std::lock_guard<std::mutex> lk(m_); screen_.scrollView(d); }
void Terminal::scrollToBottom() { std::lock_guard<std::mutex> lk(m_); screen_.scrollToBottom(); }

void Terminal::selectStart(int row, int col) { std::lock_guard<std::mutex> lk(m_); screen_.selectStart(row, col); }
void Terminal::selectExtend(int row, int col){ std::lock_guard<std::mutex> lk(m_); screen_.selectExtend(row, col); }
void Terminal::selectWord(int row, int col)  { std::lock_guard<std::mutex> lk(m_); screen_.selectWord(row, col); }
void Terminal::clearSelection()              { std::lock_guard<std::mutex> lk(m_); screen_.clearSelection(); }
std::string Terminal::selectionText()        { std::lock_guard<std::mutex> lk(m_); return screen_.selectionText(); }

int Terminal::search(const std::string& q, int fromAbsRow, bool forward) {
    std::lock_guard<std::mutex> lk(m_);
    int hit = screen_.searchNext(q, fromAbsRow, forward);
    if (hit >= 0) screen_.revealAbsRow(hit);
    return hit;
}

std::string Terminal::takeTitle() { std::lock_guard<std::mutex> lk(m_); return title_; }
std::string Terminal::takeClipboard() {
    std::lock_guard<std::mutex> lk(m_);
    std::string s = clipboard_; clipboard_.clear(); return s;
}

// ---- ParserHost ----
void Terminal::respond(const std::string& bytes) { if (responder_) responder_(bytes); }
void Terminal::setTitle(const std::string& t) { title_ = t; titleRev_++; }

// Private app-control channel (OSC 5391). Latches the payload and bumps its own revision so the
// UI polls it independently of title/clipboard traffic (no race with a distro PROMPT_COMMAND that
// sets the window title). markAll() forces a damage bump this reader-loop iteration so the event
// surfaces on the very next frame even when the payload produced no visible screen change.
void Terminal::appControl(const std::string& payload) {
    appCtrl_ = payload;
    appCtrlRev_++;
    screen_.markAll();
}
std::string Terminal::takeAppControl() {
    std::lock_guard<std::mutex> lk(m_);
    std::string s; s.swap(appCtrl_); return s;
}
void Terminal::copyToClipboard(const std::string& c) { clipboard_ = c; clipRev_++; }
void Terminal::bell() { bellCount_++; }
// The three modes below are cached on the Android side out of meta[], so a change must
// reach the UI through takeDirty()/generation_ even when the same read damages no cell.
// Bump only on an ACTUAL change: programs re-assert these modes redundantly (vim sends
// DECCKM on every :redraw), and bumping on every set would repaint for nothing.
void Terminal::setAppCursorKeys(bool v) { if (appCursor_ != v)     { appCursor_ = v;     uiModeRev_++; } }
void Terminal::setMouseTracking(int mode) { if (mouseMode_ != mode) { mouseMode_ = mode;  uiModeRev_++; } }
void Terminal::setFocusReporting(bool v) { if (focusReporting_ != v) { focusReporting_ = v; uiModeRev_++; } }
// No uiModeRev_ bump: these two have no Kotlin consumer. mouseSgr_ is read only by
// Terminal::mouseEvent() and bracketedPaste_ only by Terminal::wrapPaste(), both under m_
// at call time, so neither can be stale and neither needs a repaint.
void Terminal::setMouseSGR(bool v) { mouseSgr_ = v; }
void Terminal::setBracketedPaste(bool v) { bracketedPaste_ = v; }

} // namespace xterm
