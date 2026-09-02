// Host-side regression tests for the terminal engine's damage tracking.
//
// These compile the PRODUCTION sources unmodified (ScreenBuffer / AnsiParser / Unicode /
// Terminal) for the host toolchain. Nothing here is part of the APK: Pty, Session and the
// JNI layer are excluded because they need a real PTY and Android.
//
// What is under test is the single gate that decides whether a frame is presented at all:
//   AnsiParser -> ScreenBuffer -> takeDirty() -> Session bumps generation_ -> TerminalView
//   re-snapshots and repaints.
// A sequence that changes what the user sees but returns false from takeDirty() is a bug;
// a sequence that changes nothing visible but returns true is a wasted repaint.
//
// Build & run:  native-tests/run-tests.sh

#include "parser/AnsiParser.h"
#include "screen/ScreenBuffer.h"
#include "parser/ParserHost.h"
#include "terminal/Terminal.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xterm;

// ---------------------------------------------------------------- tiny test harness ----
static int g_pass = 0, g_fail = 0;
static const char* g_group = "";

static void group(const char* name) { g_group = name; std::printf("\n-- %s\n", name); }

static void expect(bool cond, const char* what) {
    if (cond) { g_pass++; std::printf("  PASS  %s\n", what); }
    else      { g_fail++; std::printf("  FAIL  %s   [%s]\n", what, g_group); }
}

static void expectEq(int got, int want, const char* what) {
    if (got == want) { g_pass++; std::printf("  PASS  %s (=%d)\n", what, got); }
    else { g_fail++; std::printf("  FAIL  %s: got %d, want %d   [%s]\n", what, got, want, g_group); }
}

// ------------------------------------------------------------------------- fixtures ----
struct RecordingHost : ParserHost {
    std::string responses;
    void respond(const std::string& b) override { responses += b; }
};

// ScreenBuffer + parser, with the warm-up damage already consumed -- i.e. the state the
// reader thread is in after it has presented a frame and is waiting for the next read.
struct Screen {
    ScreenBuffer scr;
    RecordingHost host;
    AnsiParser parser;

    explicit Screen(const std::string& warmup = "hello world", int cols = 80, int rows = 24)
        : scr(cols, rows), parser(scr, host) {
        feed(warmup);
        scr.takeDirty();          // drain warm-up damage AND latch the cursor baseline
    }
    void feed(const std::string& s) { parser.feed((const uint8_t*)s.data(), s.size()); }
    bool dirtyAfter(const std::string& s) { feed(s); return scr.takeDirty(); }
};

static const std::string ESC = "\x1b";
static const std::string CSI = "\x1b[";

// A sequence that MUST produce a repaint: it changes what the user sees.
static void visible(const char* name, const std::string& seq, const std::string& warmup = "hello world") {
    Screen s(warmup);
    bool d = s.dirtyAfter(seq);
    if (d) { g_pass++; std::printf("  PASS  %-38s takeDirty=true  cursor=(%d,%d)\n", name, s.scr.cursorRow(), s.scr.cursorCol()); }
    else   { g_fail++; std::printf("  FAIL  %-38s takeDirty=FALSE (expected true)  [%s]\n", name, g_group); }
}

// A sequence that MUST NOT produce a repaint: nothing visible changed.
static void invisible(const char* name, const std::string& seq, const std::string& warmup = "hello world") {
    Screen s(warmup);
    bool d = s.dirtyAfter(seq);
    if (!d) { g_pass++; std::printf("  PASS  %-38s takeDirty=false\n", name); }
    else    { g_fail++; std::printf("  FAIL  %-38s takeDirty=TRUE (expected false, wasted repaint)  [%s]\n", name, g_group); }
}


// ============================ alternate-screen fixtures ====================================
// Byte streams below are TRIMMED CAPTURES of what nano 7.2 and vim actually write to a pty
// (TERM=xterm-256color, 80x24). Only redundant repaint runs were removed; every control
// sequence that participates in the screen switch, the scroll region, the cursor and the
// mode changes is reproduced verbatim, in the original order.
//
//   nano enter : CSI ?2004h CSI ?1049h CSI 22;0;0t CSI 1;24r ... CSI H CSI 2J ...
//   nano exit  : CSI 22d CSI J CSI 24d CSI ?12l CSI ?25h CSI 24;1H CSI ?1049l
//                CSI 23;0;0t CR CSI ?1l ESC > CSI ?2004l
//   vim  enter : CSI ?1049h CSI 22;0;0t CSI >4;2m CSI ?1h ESC = CSI ?2004h CSI ?1004h CSI 1;24r ...
//   vim  exit  : ... CSI ?1004l CSI ?2004l CSI ?1l ESC > CSI ?1049l CSI 23;0;0t CSI ?25h CSI >4;m
//
// Note what is NOT there: neither editor emits DECSC/DECRC, and neither resets DECSTBM on
// the way out. Tests must not assume otherwise.
static const std::string NANO_ENTER =
    "\x1b[?2004h\x1b[?1049h\x1b[22;0;0t\x1b[1;24r\x1b(B\x1b[m\x1b[4l\x1b[?7h\x1b[39;49m"
    "\x1b[?1h\x1b=\x1b[?25l\x1b[H\x1b[2J"
    "\x1b[H\x1b(B\x1b[0;7m  GNU nano 7.2   drac.py\x1b(B\x1b[m"
    "\r\x1b[2dprint(\"hello\")\r\x1b[3dprint(\"world\")"
    "\r\x1b[23d^G Help   ^O Write Out\r\x1b[24d^X Exit   ^R Read File";
static const std::string NANO_EXIT =
    "\x1b[22d\x1b[J\x1b[24d\x1b[?12l\x1b[?25h\x1b[24;1H\x1b[?1049l\x1b[23;0;0t\r"
    "\x1b[?1l\x1b>\x1b[?2004l";
// What nano repaints after SIGWINCH: clear, re-draw the title bar, re-arm the scroll region.
static const std::string NANO_REDRAW =
    "\x1b[H\x1b[2J\x1b[H\x1b(B\x1b[0;7m  GNU nano 7.2   drac.py\x1b(B\x1b[m"
    "\r\x1b[2dprint(\"hello\")";
static const std::string VIM_ENTER =
    "\x1b[?1049h\x1b[22;0;0t\x1b[>4;2m\x1b[?1h\x1b=\x1b[?2004h\x1b[?1004h\x1b[1;24r"
    "\x1b[?12h\x1b[?12l\x1b[22;2t\x1b[22;1t\x1b[27m\x1b[23m\x1b[29m\x1b[m\x1b[H\x1b[2J\x1b[?25l"
    "\x1b[24;1H\"drac.py\" 2L, 30B\x1b[1;1Hprint(\"hello\")\r\nprint(\"world\")"
    "\x1b[3;1H~\x1b[4;1H~\x1b[23;1H~\x1b[24;63H1,1\x1b[1;1H\x1b[?25h";
static const std::string VIM_EXIT =
    "\x1b[?25l\x1b[24;1H\x1b[K\x1b[24;1H:q\r\x1b[?2004l\x1b[>4;m\x1b[23;2t\x1b[23;1t"
    "\x1b[24;1H\x1b[K\x1b[24;1H\x1b[?1004l\x1b[?2004l\x1b[?1l\x1b>\x1b[?1049l\x1b[23;0;0t"
    "\x1b[?25h\x1b[>4;m";

// The visible grid as text, one row per line, trailing blanks trimmed. Comparing two of
// these is how "the screen came back exactly as it was" is asserted.
static std::string dumpScreen(const ScreenBuffer& s) {
    const int R = s.rows(), C = s.cols();
    std::vector<uint32_t> g(R * C), f(R * C), b(R * C), a(R * C);
    const_cast<ScreenBuffer&>(s).snapshot(g.data(), f.data(), b.data(), a.data());
    std::string out;
    for (int r = 0; r < R; ++r) {
        std::string row;
        for (int c = 0; c < C; ++c) {
            uint32_t cp = g[(size_t)r * C + c];
            row += (cp >= 32 && cp < 127) ? (char)cp : '.';
        }
        while (!row.empty() && row.back() == ' ') row.pop_back();
        out += row;
        out += '\n';
    }
    return out;
}

// Base code point of one visible cell, as the renderer would receive it.
static uint32_t glyphAt(const ScreenBuffer& s, int row, int col) {
    const int R = s.rows(), C = s.cols();
    std::vector<uint32_t> g(R * C), f(R * C), b(R * C), a(R * C);
    const_cast<ScreenBuffer&>(s).snapshot(g.data(), f.data(), b.data(), a.data());
    return g[(size_t)row * C + col];
}

// True when cp is a Unicode scalar value, i.e. something Character.toChars() on the Android
// side accepts. Anything else reaches the renderer as an invalid code point.
static bool isScalarValue(uint32_t cp) {
    return cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu);
}

// Attribute bits of one visible cell, as the renderer would receive them.
static uint16_t attrAt(const ScreenBuffer& s, int row, int col) {
    const int R = s.rows(), C = s.cols();
    std::vector<uint32_t> g(R * C), f(R * C), b(R * C), a(R * C);
    const_cast<ScreenBuffer&>(s).snapshot(g.data(), f.data(), b.data(), a.data());
    return (uint16_t)a[(size_t)row * C + col];
}

// True when ANY visible cell carries the selection highlight.
static bool anySelected(const ScreenBuffer& s) {
    const int R = s.rows(), C = s.cols();
    std::vector<uint32_t> g(R * C), f(R * C), b(R * C), a(R * C);
    const_cast<ScreenBuffer&>(s).snapshot(g.data(), f.data(), b.data(), a.data());
    for (int i = 0; i < R * C; ++i) if (a[i] & SNAP_SELECTED) return true;
    return false;
}

// A shell screen with real scrollback behind it: 40 lines that have already scrolled off,
// then a run of prompts, ending on "nano drac.py" -- the state the bug reports start from.
static void shellSession(Screen& s) {
    s.feed("\x1b[H\x1b[2J");
    for (int i = 1; i <= 40; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "OLD-SHELL-LINE-%02d\r\n", i);
        s.feed(buf);
    }
    for (int i = 0; i < 8; ++i) s.feed("[dracOS@Xterm]-[~]\r\n$ \r\n");
    s.feed("[dracOS@Xterm]-[~]\r\n$ nano drac.py\r\n");
    s.scr.takeDirty();
}

// Same idea but every row is filled out to column 78, so a resize that truncates columns
// is detectable instead of silently passing on narrow test data.
static void wideSession(Screen& s) {
    s.feed("\x1b[H\x1b[2J");
    for (int i = 1; i <= 30; ++i) {
        std::string line = "ROW";
        char n[8]; std::snprintf(n, sizeof n, "%02d", i);
        line += n;
        while ((int)line.size() < 78) line += (char)('a' + (i + (int)line.size()) % 26);
        s.feed(line + "\r\n");
    }
    s.scr.takeDirty();
}

// ===========================================================================================
int main() {
    // -------------------------------------------------------------------------------------
    group("Cursor-only motion must raise damage (the reported bug)");
    // None of these touch a cell. Before the fix every one returned false, so the frame was
    // never re-presented and the cursor stayed painted at its previous position until the
    // user typed a character (which calls put() -> markRow()).
    visible("CR LF   (Enter: cursor to next line)", "\r\n");
    visible("LF      (no scroll, row 0 -> row 1)",  "\n");
    visible("CR      (column reset)",               "\r");
    visible("CUP     CSI 5;10 H",                   CSI + "5;10H");
    visible("CUD     CSI B (arrow down)",           CSI + "B");
    visible("CUU     CSI A (arrow up)",             CSI + "2B" + CSI + "A");
    visible("CUF     CSI C (arrow right)",          CSI + "C");
    visible("CUB     CSI D (arrow left)",           CSI + "D");
    visible("CHA     CSI 1 G (Home / Ctrl-A)",      CSI + "1G");
    visible("HPA     CSI 20 `",                     CSI + "20`");
    visible("VPA     CSI 10 d",                     CSI + "10d");
    visible("CNL     CSI E",                        CSI + "E");
    visible("CPL     CSI F",                        CSI + "2B" + CSI + "F");
    visible("HT      tab",                          "\t");
    visible("CHT     CSI 2 I",                      CSI + "2I");
    visible("CBT     CSI Z (Shift-Tab)",            "\t\t" + CSI + "Z");
    visible("BS      0x08 (no erase)",              "\b");
    visible("IND     ESC D",                        ESC + "D");
    visible("NEL     ESC E",                        ESC + "E");
    visible("DECTCEM hide  CSI ?25 l",              CSI + "?25l");
    visible("DECOM   CSI ?6 h (homes the cursor)",  CSI + "?6h");

    // DECTCEM show has to start from a hidden cursor to be a change at all.
    {
        Screen s;
        s.feed(CSI + "?25l");
        s.scr.takeDirty();                       // present the hidden state
        expect(s.dirtyAfter(CSI + "?25h"), "DECTCEM show CSI ?25 h after hide");
        expect(s.scr.cursorVisible(), "cursor is visible again after DECTCEM show");
    }

    // DECSC/DECRC: the restore must repaint when it lands somewhere else than the frame
    // that was last presented. Split across two takeDirty() calls, because that is what the
    // reader thread does -- one call per PTY read.
    {
        Screen s;                                        // cursor at (0,11)
        s.feed(ESC + "7");                               // DECSC saves (0,11)
        s.feed(CSI + "9;9H");                            // move away
        expect(s.scr.takeDirty(), "move away from the saved cell repaints");
        expectEq(s.scr.cursorRow(), 8, "cursor moved to row 8");
        expect(s.dirtyAfter(ESC + "8"), "DECRC ESC 8 back to the saved cell repaints");
        expectEq(s.scr.cursorCol(), 11, "DECRC restored the saved column");
    }
    {
        Screen s;
        s.feed(CSI + "s");                               // SCOSC
        s.feed(CSI + "9;9H");
        expect(s.scr.takeDirty(), "move away after CSI s repaints");
        expect(s.dirtyAfter(CSI + "u"), "DECRC CSI u repaints");
        expectEq(s.scr.cursorCol(), 11, "CSI u restored the saved column");
    }

    // -------------------------------------------------------------------------------------
    group("Sequences with no visible effect must NOT repaint (optimisation preserved)");
    // This is the guard on the fix: the damage-tracking optimisation documented in
    // ARCHITECTURE.md ("escape queries that produce no visible change do not trigger
    // repaints") must survive. These move nothing and touch nothing.
    invisible("DSR/CPR CSI 6 n (cursor report)",    CSI + "6n");
    invisible("DSR     CSI 5 n (status report)",    CSI + "5n");
    invisible("DA      CSI c (device attributes)",  CSI + "c");
    invisible("SGR     CSI 31 m (colour only)",     CSI + "31m");
    invisible("SGR     CSI 0 m (reset only)",       CSI + "0m");
    invisible("SGR     CSI 1;4;7 m",                CSI + "1;4;7m");
    invisible("DECAWM  CSI ?7 l (autowrap off)",    CSI + "?7l");
    invisible("IRM     CSI 4 h (insert mode)",      CSI + "4h");
    invisible("HTS     ESC H (set tab stop)",       ESC + "H");
    invisible("empty feed",                         "");
    invisible("DECTCEM show when already visible",  CSI + "?25h");
    // Clamped motion that cannot actually move the cursor is not visible either.
    // Warm up to "x" then CR, so the PRESENTED baseline already has the cursor at column 0.
    invisible("CUB at column 0 (clamped, no move)", CSI + "D", "x\r");
    invisible("BS at column 0 (clamped, no move)",  "\b",       "x\r");
    invisible("CUU at row 0 (clamped, no move)",    CSI + "A");
    invisible("CUP to the cell already occupied",   CSI + "1;12H");

    // Damage is evaluated per takeDirty(), i.e. per PTY read -- not per escape sequence.
    // A round trip that ends where it started inside ONE read is deliberately NOT damage:
    // the frame that would be presented is pixel-identical to the last one. This is the
    // same coalescing that makes a 32 KiB burst cost one repaint instead of thousands.
    invisible("DECSC + move + DECRC in one read",   ESC + "7" + CSI + "9;9H" + ESC + "8");
    invisible("move away and back in one read",     CSI + "9;9H" + CSI + "1;12H");

    // DSR must still ANSWER even though it does not repaint.
    {
        Screen s;
        s.feed(CSI + "6n");
        expect(s.host.responses.find("\x1b[") == 0, "DSR still writes a CPR response");
        expect(!s.scr.takeDirty(), "DSR response did not raise damage");
    }

    // -------------------------------------------------------------------------------------
    group("Idempotency: a second takeDirty() with no new input is false");
    {
        Screen s;
        expect(s.dirtyAfter("\r\n"), "1st takeDirty after Enter is true");
        expect(!s.scr.takeDirty(),   "2nd takeDirty with no input is false");
        expect(!s.scr.takeDirty(),   "3rd takeDirty with no input is false");
    }
    {
        Screen s;
        expect(s.dirtyAfter("abc"),  "1st takeDirty after printing is true");
        expect(!s.scr.takeDirty(),   "2nd takeDirty after printing is false");
    }
    {
        // Repeating the SAME cursor-only sequence must not keep repainting.
        Screen s;
        expect(s.dirtyAfter(CSI + "5;10H"), "CUP to a new cell is true");
        expect(!s.dirtyAfter(CSI + "5;10H"), "CUP to the SAME cell is false");
    }

    // -------------------------------------------------------------------------------------
    group("Grid damage still works (no regression in the existing path)");
    visible("print a glyph",                        "x");
    visible("EL      CSI K",                        CSI + "K");
    visible("ED      CSI 2 J",                      CSI + "2J");
    visible("ECH     CSI 5 X",                      CSI + "5X");
    visible("DCH     CSI 2 P",                      CSI + "2P");
    visible("ICH     CSI 2 @",                      CSI + "2@");
    visible("IL      CSI 2 L",                      CSI + "2L");
    visible("DL      CSI 2 M",                      CSI + "2M");
    visible("SU      CSI 2 S",                      CSI + "2S");
    visible("SD      CSI 2 T",                      CSI + "2T");
    visible("alt screen in  CSI ?1049 h",           CSI + "?1049h");
    visible("RIS     ESC c",                        ESC + "c");
    visible("DECSCNM CSI ?5 h (reverse video)",     CSI + "?5h");

    // -------------------------------------------------------------------------------------
    group("LF / RI raise damage via scrolling as well as via cursor motion");
    // Correction to an earlier claim: a LF at the bottom of the scroll region calls
    // scrollUp() -> markAll(), so it was ALREADY damage before the fix. The bug only ever
    // applied to the cursor-only case (a LF that just increments the row).
    {
        Screen s("hello world");
        s.feed(CSI + "24;1H");                    // last row of a 24-row screen
        s.scr.takeDirty();
        bool d = s.dirtyAfter("\n");
        expect(d, "LF at the bottom row scrolls -> damage");
        expectEq(s.scr.cursorRow(), 23, "cursor stays on the last row after the scroll");
    }
    {
        Screen s("hello world");
        s.feed(CSI + "1;1H");
        s.scr.takeDirty();
        bool d = s.dirtyAfter(ESC + "M");         // RI at the top -> scrollDown -> markAll
        expect(d, "RI at the top row scrolls -> damage");
        expectEq(s.scr.cursorRow(), 0, "cursor stays on the top row after the reverse scroll");
    }
    {
        Screen s("hello world");
        s.feed(CSI + "5;1H");
        s.scr.takeDirty();
        bool d = s.dirtyAfter("\n");              // mid-screen LF: cursor-only, no scroll
        expect(d, "LF mid-screen (cursor-only, no scroll) -> damage");
        expectEq(s.scr.cursorRow(), 5, "cursor advanced one row");
    }

    // -------------------------------------------------------------------------------------
    group("The reported scenario end to end");
    {
        Screen s("line one");
        bool afterEnter = s.dirtyAfter("\r\n");
        expect(afterEnter, "Enter alone repaints (cursor visible on the new line)");
        expectEq(s.scr.cursorRow(), 1, "cursor row after Enter");
        expectEq(s.scr.cursorCol(), 0, "cursor col after Enter");
        bool afterType = s.dirtyAfter("a");
        expect(afterType, "typing after Enter still repaints");
        expectEq(s.scr.cursorCol(), 1, "cursor col after typing one character");
    }

    // -------------------------------------------------------------------------------------
    group("Scrolled into history: cursor motion must not force pointless repaints");
    {
        // snapshot() reports cursor = -1 while the viewport is scrolled off the live bottom,
        // so the cursor is not painted and moving it changes nothing on screen.
        Screen s("x", 80, 3);
        for (int i = 0; i < 10; ++i) s.feed("line\r\n");   // build scrollback
        s.scr.scrollView(5);                                // scroll up into history
        s.scr.takeDirty();                                  // present the scrolled view
        expectEq(s.scr.viewOffset(), 5, "viewport is scrolled into history");
        expect(!s.dirtyAfter(CSI + "1;1H"), "cursor-only motion while scrolled -> no repaint");
        expect(!s.dirtyAfter(CSI + "2;5H"), "further cursor-only motion while scrolled -> no repaint");
        // Output that touches a cell still repaints, and scrollToBottom() still marks.
        expect(s.dirtyAfter("z"), "printing while scrolled still repaints");
    }
    {
        Screen s("x", 80, 3);
        for (int i = 0; i < 10; ++i) s.feed("line\r\n");
        s.scr.scrollView(5);
        s.scr.takeDirty();
        s.scr.scrollToBottom();
        expect(s.scr.takeDirty(), "returning to the live bottom repaints");
        expectEq(s.scr.viewOffset(), 0, "viewport is back at the live bottom");
    }

    // -------------------------------------------------------------------------------------
    group("Terminal: UI-consumed input modes must reach the UI without grid damage");
    // TerminalView caches appCursor / mouseMode / focusReporting out of meta[] inside
    // refreshSnapshot(), which only runs when generation_ moves, which only moves when
    // Terminal::takeDirty() is true. A mode change in a read that damages no cell therefore
    // has to be reported as damage or the UI keeps encoding input against the old modes.
    auto termFeed = [](Terminal& t, const std::string& s) {
        t.feed((const uint8_t*)s.data(), s.size());
    };
    auto metaOf = [](Terminal& t, int idx) {
        std::vector<uint32_t> g(80 * 24), f(80 * 24), b(80 * 24), a(80 * 24);
        int meta[Terminal::M_COUNT] = {0};
        t.snapshot(g.data(), f.data(), b.data(), a.data(), 80 * 24, meta);
        return meta[idx];
    };
    {
        Terminal t(80, 24);
        termFeed(t, "hello");
        t.takeDirty();                                   // drain warm-up
        expect(t.takeDirty() == false, "Terminal baseline is quiet");

        termFeed(t, CSI + "?1h");                        // DECCKM on, no cell touched
        expect(t.takeDirty(), "DECCKM on -> takeDirty true");
        expectEq(metaOf(t, Terminal::M_APP_CURSOR), 1, "meta M_APP_CURSOR after DECCKM on");

        termFeed(t, CSI + "?1h");                        // redundant re-assert
        expect(!t.takeDirty(), "DECCKM re-asserted to the same value -> no repaint");

        termFeed(t, CSI + "?1l");
        expect(t.takeDirty(), "DECCKM off -> takeDirty true");
        expectEq(metaOf(t, Terminal::M_APP_CURSOR), 0, "meta M_APP_CURSOR after DECCKM off");
    }
    {
        Terminal t(80, 24);
        termFeed(t, "hello"); t.takeDirty();
        termFeed(t, CSI + "?1000h");
        expect(t.takeDirty(), "mouse tracking 1000 on -> takeDirty true");
        expectEq(metaOf(t, Terminal::M_MOUSE_MODE), 1000, "meta M_MOUSE_MODE = 1000");
        termFeed(t, CSI + "?1002h");
        expect(t.takeDirty(), "mouse tracking 1000 -> 1002 -> takeDirty true");
        expectEq(metaOf(t, Terminal::M_MOUSE_MODE), 1002, "meta M_MOUSE_MODE = 1002");
        termFeed(t, CSI + "?1002h");
        expect(!t.takeDirty(), "mouse tracking re-asserted to the same mode -> no repaint");
        termFeed(t, CSI + "?1002l");
        expect(t.takeDirty(), "mouse tracking off -> takeDirty true");
        expectEq(metaOf(t, Terminal::M_MOUSE_MODE), 0, "meta M_MOUSE_MODE = 0");
    }
    {
        Terminal t(80, 24);
        termFeed(t, "hello"); t.takeDirty();
        termFeed(t, CSI + "?1004h");
        expect(t.takeDirty(), "focus reporting on -> takeDirty true");
        expectEq(metaOf(t, Terminal::M_FOCUS), 1, "meta M_FOCUS after enable");
        termFeed(t, CSI + "?1004h");
        expect(!t.takeDirty(), "focus reporting re-asserted -> no repaint");
        termFeed(t, CSI + "?1004l");
        expect(t.takeDirty(), "focus reporting off -> takeDirty true");
    }
    {
        // Modes with NO Kotlin consumer must not cost a repaint. mouseSgr_ is read inside
        // Terminal::mouseEvent() and bracketedPaste_ inside Terminal::wrapPaste(), both under
        // the engine mutex at call time, so neither can be stale.
        Terminal t(80, 24);
        termFeed(t, "hello"); t.takeDirty();
        termFeed(t, CSI + "?1006h");
        expect(!t.takeDirty(), "mouse SGR (1006) alone -> no repaint (no UI consumer)");
        termFeed(t, CSI + "?2004h");
        expect(!t.takeDirty(), "bracketed paste (2004) alone -> no repaint (no UI consumer)");
        // ...but bracketed paste must still take effect where it IS consumed.
        std::string wrapped = t.wrapPaste("abc");
        expect(wrapped == "\x1b[200~abc\x1b[201~", "wrapPaste still honours bracketed paste");
    }
    {
        // Cursor motion must reach the UI through Terminal too, not just ScreenBuffer.
        Terminal t(80, 24);
        termFeed(t, "hello"); t.takeDirty();
        termFeed(t, "\r\n");
        expect(t.takeDirty(), "Terminal::takeDirty true for a cursor-only Enter");
        expect(!t.takeDirty(), "Terminal::takeDirty idempotent");
    }
    {
        // Grid damage and a mode change arriving together must both be consumed, leaving
        // nothing latched for a later call.
        Terminal t(80, 24);
        termFeed(t, "hello"); t.takeDirty();
        termFeed(t, "x" + CSI + "?1h");
        expect(t.takeDirty(), "grid damage + mode change in one read -> true");
        expect(!t.takeDirty(), "nothing left latched afterwards");
    }

    // =====================================================================================
    // Alternate screen x viewport x resize -- the nano/vim desync regressions (T1..T15).
    //
    // Every editor byte string below was CAPTURED FROM A REAL PROGRAM on a Linux pty
    // (nano 7.2 and vim, TERM=xterm-256color, 80x24) with:
    //     printf '\x18' | script -q -c "stty rows 24 cols 80; nano drac.py" nano.raw
    // so these tests exercise what the editors actually send, not what we assume they send.
    // Verified from those captures: neither nano nor vim emits DECSC/DECRC (ESC 7 / ESC 8),
    // and both leave DECSTBM set on exit.
    // =====================================================================================
    group("Alternate screen: viewport, scrollback and resize (nano/vim desync)");

    // -- T1 --------------------------------------------------------------------------
    // BUG: a finger swipe inside nano moved viewOffset_ into the MAIN screen's scrollback,
    // so rows of old shell history were rendered on top of the editor's screen.
    // EXPECTED: while the alternate screen owns the display the main-screen scrollback is
    // locked -- scrollView() is inert and the viewport keeps showing the editor.
    {
        group("T1 alt_screen_locks_main_scrollback__swipe_must_not_move_viewport");
        Screen s("", 80, 24);
        shellSession(s);
        expect(s.scr.scrollbackSize() > 8, "fixture really has main-screen scrollback");
        s.feed(NANO_ENTER);
        std::string inEditor = dumpScreen(s.scr);
        s.scr.scrollView(+8);                       // one finger drag, ~8 lines
        expectEq(s.scr.viewOffset(), 0, "T1 viewOffset stays 0 while the alt screen is active");
        expect(dumpScreen(s.scr) == inEditor,
               "T1 viewport still shows the editor, no main-screen history bleeds in");
        s.scr.scrollView(-8);
        expectEq(s.scr.viewOffset(), 0, "T1 scrolling the other way is inert too");
    }

    // -- T2 --------------------------------------------------------------------------
    // BUG: leaveAltScreen() restored grid_ but left viewOffset_ untouched, so quitting the
    // editor dropped the user N lines deep into history until the shell happened to print.
    // EXPECTED: leaving the alternate screen returns the viewport to the live bottom.
    {
        group("T2 exiting_editor_must_reset_viewport_to_live_bottom");
        Screen s("", 80, 24);
        shellSession(s);
        std::string before = dumpScreen(s.scr);
        s.feed(NANO_ENTER);
        s.scr.scrollView(+8);                       // user tried to scroll inside nano
        s.feed(NANO_EXIT);
        expectEq(s.scr.viewOffset(), 0, "T2 viewOffset reset to the live bottom after CSI ?1049 l");
        expect(dumpScreen(s.scr) == before, "T2 the pre-editor screen is what is displayed");
    }

    // -- T3 --------------------------------------------------------------------------
    // BUG: ScreenBuffer::resize() reflowed the SAVED main grid (altGrid_) with a top-anchored
    // memcpy, so opening the keyboard inside nano destroyed the bottom rows of the shell
    // screen for good -- they were neither kept nor pushed to scrollback.
    // EXPECTED: the saved main screen survives an IME open/close cycle byte for byte.
    {
        group("T3 ime_resize_inside_nano_must_not_destroy_saved_main_screen_rows");
        Screen s("", 80, 24);
        shellSession(s);
        std::string before = dumpScreen(s.scr);
        const int beforeCy = s.scr.cursorRow(), beforeCx = s.scr.cursorCol();
        s.feed(NANO_ENTER);
        s.scr.resize(80, 13);                        // keyboard opens
        s.feed(NANO_REDRAW);                         // nano repaints on SIGWINCH
        s.scr.resize(80, 24);                        // keyboard closes
        s.feed(NANO_EXIT);
        expect(dumpScreen(s.scr) == before, "T3 main screen restored intact after IME resize inside nano");
        expectEq(s.scr.cursorRow(), beforeCy, "T3 cursor row restored");
        expectEq(s.scr.cursorCol(), beforeCx, "T3 cursor col restored");
        expectEq(s.scr.viewOffset(), 0, "T3 viewport at the live bottom");
    }

    // -- T4 --------------------------------------------------------------------------
    // BUG: the same memcpy truncated the saved main grid to min(oldCols, newCols) columns,
    // so a rotation inside the editor silently erased every cell to the right of the
    // narrower width. The fixture below is deliberately 78 columns wide.
    // EXPECTED: a column change inside the editor does not touch the saved main screen.
    {
        group("T4 rotation_resize_inside_nano_must_not_destroy_saved_main_screen_columns");
        Screen s("", 80, 24);
        wideSession(s);                              // rows filled out to column 78
        std::string before = dumpScreen(s.scr);
        s.feed(NANO_ENTER);
        s.scr.resize(50, 24);                        // rotate to portrait / narrower
        s.feed(NANO_REDRAW);
        s.scr.resize(80, 24);                        // rotate back
        s.feed(NANO_EXIT);
        expect(dumpScreen(s.scr) == before, "T4 wide main-screen content survives a column change inside nano");
    }

    // -- T5 --------------------------------------------------------------------------
    // Hardest real-world shape: the user quits the editor WHILE the keyboard is still open,
    // then closes it. The saved main screen must be reflowed content-anchored through
    // scrollback (the reversible path the primary screen already uses), not truncated.
    {
        group("T5 exiting_editor_at_a_different_size_reflows_main_screen_reversibly");
        Screen s("", 80, 24);
        shellSession(s);
        std::string before = dumpScreen(s.scr);
        s.feed(NANO_ENTER);
        s.scr.resize(80, 13);                        // keyboard opens
        s.feed(NANO_REDRAW);
        s.feed(NANO_EXIT);                           // quit nano while still shrunk
        expectEq(s.scr.viewOffset(), 0, "T5 viewport at the live bottom right after the exit");
        s.scr.resize(80, 24);                        // keyboard closes
        expect(dumpScreen(s.scr) == before, "T5 main screen is byte-identical again once the keyboard closes");
    }

    // -- T6 --------------------------------------------------------------------------
    // Non-regression guard: the simple case already worked before the fix and must stay green.
    {
        group("T6 vim_enter_exit_without_resize_is_lossless");
        Screen s("", 80, 24);
        shellSession(s);
        std::string before = dumpScreen(s.scr);
        const int beforeCy = s.scr.cursorRow(), beforeCx = s.scr.cursorCol();
        s.feed(VIM_ENTER);
        s.feed(VIM_EXIT);
        expect(dumpScreen(s.scr) == before, "T6 main screen restored after a plain vim session");
        expectEq(s.scr.cursorRow(), beforeCy, "T6 cursor row restored");
        expectEq(s.scr.cursorCol(), beforeCx, "T6 cursor col restored");
        expect(!s.scr.onAltScreen(), "T6 back on the main screen");
    }

    // -- T7 --------------------------------------------------------------------------
    // BUG: CSI > 4 ; 2 m is XTMODKEYS (modifyOtherKeys), which vim sends on entry. The
    // parser ignored the '>' private marker and ran it through applySGR(), latching
    // UNDERLINE|DIM onto the pen (attr 0x12).
    {
        group("T7 csi_gt_4_2_m_is_xtmodkeys_not_sgr");
        Screen s("", 80, 24);
        s.feed(CSI + ">4;2m");
        s.feed("X");
        expectEq((int)attrAt(s.scr, 0, 0), 0, "T7 CSI > 4 ; 2 m leaves the pen attributes untouched");
    }

    // -- T8 --------------------------------------------------------------------------
    // Same bug without the trailing ';': params = [4] alone, so no SGR 0 masks it and the
    // underline would stay latched for the rest of the session.
    {
        group("T8 csi_gt_4_m_without_semicolon_is_xtmodkeys_not_sgr");
        Screen s("", 80, 24);
        s.feed(CSI + ">4m");
        s.feed("X");
        expectEq((int)attrAt(s.scr, 0, 0), 0, "T8 CSI > 4 m leaves the pen attributes untouched");
    }

    // -- T9 --------------------------------------------------------------------------
    // BUG: CSI > c (Secondary DA, sent by vim at startup) was answered with the PRIMARY DA
    // reply CSI ? 62 ; 22 c. A DA2 reply must start with CSI >.
    {
        group("T9 csi_gt_c_answers_secondary_da_not_primary");
        Screen s("", 80, 24);
        s.host.responses.clear();
        s.feed(CSI + ">c");
        expect(s.host.responses.rfind("\x1b[>", 0) == 0,
               "T9 CSI > c is answered with a DA2 reply (CSI > ...)");
        s.host.responses.clear();
        s.feed(CSI + "c");
        expect(s.host.responses == "\x1b[?62;22c", "T9 plain CSI c still answers DA1 unchanged");
    }

    // -- T10 -------------------------------------------------------------------------
    // BUG: one shared saved-cursor slot served both DECSC/DECRC and CSI ?1049. A DECSC
    // issued INSIDE the alternate screen overwrote the cursor that ?1049h had parked for
    // the main screen, so ?1049l put the shell prompt at the editor's cursor position and
    // the shell then overwrote the restored screen from there.
    // EXPECTED: xterm/Termux semantics -- one saved-cursor slot per screen.
    {
        group("T10 decsc_inside_alt_screen_must_not_clobber_1049_saved_cursor");
        Screen s("", 80, 24);
        s.feed(CSI + "6;4H");                        // main-screen cursor at row 5, col 3
        expectEq(s.scr.cursorRow(), 5, "T10 fixture cursor row");
        s.feed(CSI + "?1049h");                      // save main cursor, go to alt
        s.feed(CSI + "10;20H");
        s.feed(ESC + "7");                           // DECSC *inside* the alt screen
        s.feed(CSI + "1;1H");
        s.feed(ESC + "8");                           // DECRC inside the alt screen
        expectEq(s.scr.cursorRow(), 9, "T10 DECRC still works inside the alt screen (row)");
        expectEq(s.scr.cursorCol(), 19, "T10 DECRC still works inside the alt screen (col)");
        s.feed(CSI + "?1049l");
        expectEq(s.scr.cursorRow(), 5, "T10 main-screen cursor row restored, not the editor's");
        expectEq(s.scr.cursorCol(), 3, "T10 main-screen cursor col restored, not the editor's");
    }

    // -- T11 -------------------------------------------------------------------------
    // Guard on an invariant that is currently correct and must not regress while the
    // buffers are being split: the alternate screen never feeds the scrollback.
    {
        group("T11 alt_screen_scrollup_must_not_grow_main_scrollback");
        Screen s("", 80, 24);
        shellSession(s);
        const int sb = s.scr.scrollbackSize();
        s.feed(NANO_ENTER);
        for (int i = 0; i < 50; ++i) s.feed("\x1b[24;1Hscrolling line\r\n");
        expectEq(s.scr.scrollbackSize(), sb, "T11 scrollback unchanged by 50 alt-screen scrolls");
        s.feed(NANO_EXIT);
        expectEq(s.scr.scrollbackSize(), sb, "T11 scrollback still unchanged after leaving the editor");
    }

    // -- T12 -------------------------------------------------------------------------
    // Selection is stored in ABSOLUTE line coordinates, so it is meaningless the moment the
    // screen buffer underneath it is swapped. Leaving it active leaked an inverted-colour
    // band from the shell screen onto the editor's screen and back again.
    {
        group("T12 selection_must_not_survive_alt_screen_switch");
        Screen s("", 80, 24);
        shellSession(s);
        s.scr.selectStart(2, 0);
        s.scr.selectExtend(2, 10);
        expect(!s.scr.selectionText().empty(), "T12 fixture really has a selection");
        s.feed(NANO_ENTER);
        expect(s.scr.selectionText().empty(), "T12 selection cleared when entering the alt screen");
        expect(!anySelected(s.scr), "T12 no SNAP_SELECTED cell painted on the editor's screen");
        s.scr.selectStart(1, 0);
        s.scr.selectExtend(1, 5);
        s.feed(NANO_EXIT);
        expect(s.scr.selectionText().empty(), "T12 selection cleared when leaving the alt screen");
        expect(!anySelected(s.scr), "T12 no SNAP_SELECTED cell painted on the restored main screen");
    }

    // -- T13 -------------------------------------------------------------------------
    // Rapid PTY output interleaved with resizes across the screen switch. Meant to be run
    // under -fsanitize=address,undefined; the assertions here only pin the invariants that
    // must hold no matter how the two buffers were reallocated underneath.
    {
        group("T13 rapid_output_interleaved_with_resize_is_memory_safe");
        Screen s("", 80, 24);
        shellSession(s);
        const int sizes[] = { 13, 24, 9, 31, 20, 24 };
        for (int round = 0; round < 6; ++round) {
            s.feed(NANO_ENTER);
            for (int i = 0; i < 400; ++i)
                s.feed("\x1b[1;1H\x1b[32mburst line with SGR \x1b[0mand tabs\t\tend\r\n");
            s.scr.resize(40 + (round * 7) % 41, sizes[round]);
            for (int i = 0; i < 400; ++i)
                s.feed("more output \xe4\xb8\xad\xe6\x96\x87 wide glyphs \r\n");
            s.feed(NANO_EXIT);
            s.scr.resize(80, 24);
            for (int i = 0; i < 200; ++i) s.feed("main screen output\r\n");
        }
        expect(!s.scr.onAltScreen(), "T13 ends on the main screen");
        expectEq(s.scr.viewOffset(), 0, "T13 viewport at the live bottom");
        expect(s.scr.cursorRow() >= 0 && s.scr.cursorRow() < s.scr.rows(), "T13 cursor row in range");
        expect(s.scr.cursorCol() >= 0 && s.scr.cursorCol() < s.scr.cols(), "T13 cursor col in range");
        expect(s.scr.scrollbackSize() <= 5000, "T13 scrollback stayed bounded");
    }

    // -- T14 -------------------------------------------------------------------------
    // The Kotlin gesture router (TerminalView.onTouchEvent) decides between "send Arrow
    // Up/Down to the PTY" and "move the scrollback viewport" purely from meta[M_ON_ALT].
    // If the engine ever stopped publishing that flag the swipe mapping would silently
    // revert to the buggy behaviour, so the contract is pinned here.
    {
        group("T14 terminal_meta_exposes_alt_screen_flag_for_gesture_routing");
        Terminal t(80, 24);
        for (int i = 0; i < 60; ++i) termFeed(t, "main screen history line\r\n");
        t.takeDirty();
        expect(metaOf(t, Terminal::M_SCROLLBACK) > 8, "T14 fixture really has scrollback to scroll into");
        t.scrollView(+5);
        expectEq(metaOf(t, Terminal::M_VIEW_OFFSET), 5, "T14 scrollView works normally on the main screen");
        t.scrollToBottom();
        expectEq(metaOf(t, Terminal::M_ON_ALT), 0, "T14 M_ON_ALT is 0 on the main screen");
        termFeed(t, CSI + "?1049h");
        expectEq(metaOf(t, Terminal::M_ON_ALT), 1, "T14 M_ON_ALT is 1 while the editor owns the screen");
        expectEq(metaOf(t, Terminal::M_VIEW_OFFSET), 0, "T14 M_VIEW_OFFSET pinned to 0 on the alt screen");
        t.scrollView(+5);
        expectEq(metaOf(t, Terminal::M_VIEW_OFFSET), 0, "T14 Terminal::scrollView is inert on the alt screen");
        termFeed(t, CSI + "?1049l");
        expectEq(metaOf(t, Terminal::M_ON_ALT), 0, "T14 M_ON_ALT back to 0 so normal scrollback gestures resume");
    }

    // -- T15 -------------------------------------------------------------------------
    // Entering and leaving the editor repeatedly must be idempotent: no drift in the
    // restored screen, the cursor, the scrollback depth or the viewport.
    {
        group("T15 repeated_editor_enter_exit_is_stable");
        Screen s("", 80, 24);
        shellSession(s);
        const std::string before = dumpScreen(s.scr);
        const int beforeCy = s.scr.cursorRow(), beforeCx = s.scr.cursorCol();
        const int beforeSb = s.scr.scrollbackSize();
        for (int i = 0; i < 10; ++i) {
            s.feed(NANO_ENTER);
            s.scr.scrollView(+4);                    // user swipes inside the editor each time
            s.scr.resize(80, 13);
            s.feed(NANO_REDRAW);
            s.scr.resize(80, 24);
            s.feed(NANO_EXIT);
        }
        expect(dumpScreen(s.scr) == before, "T15 main screen identical after 10 enter/exit cycles");
        expectEq(s.scr.cursorRow(), beforeCy, "T15 cursor row identical after 10 cycles");
        expectEq(s.scr.cursorCol(), beforeCx, "T15 cursor col identical after 10 cycles");
        expectEq(s.scr.scrollbackSize(), beforeSb, "T15 scrollback depth identical after 10 cycles");
        expectEq(s.scr.viewOffset(), 0, "T15 viewport at the live bottom after 10 cycles");
    }

    // -- T16 -------------------------------------------------------------------------
    // Malformed UTF-8 must never put a non-scalar code point in the grid. TerminalView
    // renders a cell with String(Character.toChars(cp)), which throws IllegalArgumentException
    // above U+10FFFF, so a byte sequence like F7 BF BF BF took the whole app down from inside
    // onDraw. `cat` on any binary file produces those bytes.
    {
        group("T16 malformed_utf8_never_yields_a_non_scalar_code_point");
        // Every one of these must land as U+FFFD. The last four are the overlong forms, which
        // decode to a real character (E0 80 AF is '/') and are the classic way to smuggle a
        // byte past a filter that only inspects the shortest encoding.
        struct Case { const char* name; const char* bytes; size_t len; };
        const Case cases[] = {
            { "F7 BF BF BF (above U+10FFFF)",  "\xf7\xbf\xbf\xbf", 4 },
            { "F5 80 80 80 (above U+10FFFF)",  "\xf5\x80\x80\x80", 4 },
            { "ED A0 80 (UTF-16 surrogate)",   "\xed\xa0\x80",     3 },
            { "ED BF BF (UTF-16 surrogate)",   "\xed\xbf\xbf",     3 },
            { "C0 80 (overlong NUL)",          "\xc0\x80",         2 },
            { "C1 BF (overlong DEL)",          "\xc1\xbf",         2 },
            { "E0 80 AF (overlong slash)",     "\xe0\x80\xaf",     3 },
            { "F0 80 80 AF (overlong slash)",  "\xf0\x80\x80\xaf", 4 },
        };
        for (const Case& c : cases) {
            Screen s("", 20, 3);
            s.feed(std::string(c.bytes, c.len));
            const uint32_t cp = glyphAt(s.scr, 0, 0);
            char what[160];
            std::snprintf(what, sizeof what, "T16 %s -> U+%X is a Unicode scalar value",
                          c.name, cp);
            expect(isScalarValue(cp), what);
            std::snprintf(what, sizeof what, "T16 %s -> replaced with U+FFFD", c.name);
            expectEq((int)cp, 0xFFFD, what);
        }
        // Well-formed input still decodes exactly as before.
        { Screen s("", 20, 3); s.feed("\xc3\xa9");
          expectEq((int)glyphAt(s.scr, 0, 0), 0xE9, "T16 C3 A9 still decodes to U+00E9"); }
        { Screen s("", 20, 3); s.feed("\xe4\xb8\xad");
          expectEq((int)glyphAt(s.scr, 0, 0), 0x4E2D, "T16 E4 B8 AD still decodes to U+4E2D"); }
        { Screen s("", 20, 3); s.feed("\xf0\x9f\x9a\x80");
          expectEq((int)glyphAt(s.scr, 0, 0), 0x1F680, "T16 F0 9F 9A 80 still decodes to U+1F680"); }
    }

    // -- T17 -------------------------------------------------------------------------
    // A CSI parameter is attacker-supplied: it is whatever a program in the shell printed.
    // Accumulating it without a ceiling overflows a signed int (undefined behaviour), and the
    // value then drives per-line loops, so `printf '\033[2000000000S'` scrolled for the better
    // part of an hour with the UI thread waiting on it. Clamped, each of these is instant, and
    // the resulting screen is the same one an unbounded loop would eventually have produced.
    {
        group("T17 huge_csi_parameters_are_bounded");
        const auto t0 = std::chrono::steady_clock::now();

        // CSI S: scrolling the full region further than screen+scrollback cannot change anything.
        { Screen s("keep me", 80, 24);
          s.feed(CSI + "2000000000S");
          expect(dumpScreen(s.scr).find("keep me") == std::string::npos,
                 "T17 CSI 2000000000 S scrolls the screen clear"); }

        // CSI T, CSI I, CSI Z, CSI X, CSI b: the same parameter, other loops.
        { Screen s("hello", 80, 24); s.feed(CSI + "2000000000T");
          expect(dumpScreen(s.scr).find("hello") == std::string::npos,
                 "T17 CSI 2000000000 T scrolls the screen clear"); }
        { Screen s("", 80, 24); s.feed(CSI + "2000000000I");
          expectEq(s.scr.cursorCol(), 79, "T17 CSI 2000000000 I stops at the last column"); }
        { Screen s("", 80, 24); s.feed(CSI + "40G" + CSI + "2000000000Z");
          expectEq(s.scr.cursorCol(), 0, "T17 CSI 2000000000 Z stops at the first column"); }
        { Screen s("hello", 80, 24); s.feed(CSI + "1G" + CSI + "2000000000X");
          expect(dumpScreen(s.scr).find("hello") == std::string::npos,
                 "T17 CSI 2000000000 X erases to the end of the line"); }
        { Screen s("", 80, 24); s.feed("x" + CSI + "2000000000b");
          expect(true, "T17 CSI 2000000000 b (REP) terminates"); }

        // A parameter with more digits than an int can hold must not overflow on the way in.
        { Screen s("", 80, 24); s.feed(CSI + "99999999999999999999C");
          expectEq(s.scr.cursorCol(), 79, "T17 a 20-digit parameter still clamps to the last column"); }

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        char what[128];
        std::snprintf(what, sizeof what,
                      "T17 the whole group finished in %lld ms (budget 2000 ms)", (long long)ms);
        expect(ms < 2000, what);
    }

    // -- T18 -------------------------------------------------------------------------
    // RIS (ESC c) and DECSTR (CSI ! p) are the two "put this terminal back to a known state"
    // sequences, and `reset` / `tput reset` send them. Both reset the SCREEN already; neither
    // reset the state that decides how the NEXT byte is interpreted, so a `reset` issued to
    // repair a confused terminal left it confused: insert mode still shifting the line right,
    // the pen still bold-red, ESC ( 0 still turning letters into box drawing.
    {
        group("T18 ris_and_decstr_reset_the_interpretation_state");

        // RIS clears the DEC line-drawing charset. 'q' maps to U+2500 while it is selected.
        { Screen s("", 20, 3);
          s.feed(ESC + "(0"); s.feed("q");
          expectEq((int)glyphAt(s.scr, 0, 0), 0x2500, "T18 ESC ( 0 selects line drawing");
          s.feed(ESC + "c"); s.feed("q");
          expectEq((int)glyphAt(s.scr, 0, 0), 'q', "T18 RIS clears the G0 line-drawing charset"); }

        // RIS clears the shift-out state (SO selects G1).
        { Screen s("", 20, 3);
          s.feed(ESC + ")0"); s.feed("\x0e"); s.feed("q");
          expectEq((int)glyphAt(s.scr, 0, 0), 0x2500, "T18 SO selects the G1 line-drawing charset");
          s.feed(ESC + "c"); s.feed("q");
          expectEq((int)glyphAt(s.scr, 0, 0), 'q', "T18 RIS clears the shift-out state"); }

        // RIS abandons a half-decoded UTF-8 sequence instead of composing the next byte into it.
        { Screen s("", 20, 3);
          s.feed("\xe4\xb8");            // two thirds of U+4E2D
          s.feed(ESC + "c"); s.feed("A");
          expectEq((int)glyphAt(s.scr, 0, 0), 'A', "T18 RIS drops a partial UTF-8 sequence"); }

        // DECSTR clears IRM. With it latched, every later line is shifted right as it prints.
        { Screen s("", 20, 3);
          s.feed(CSI + "4h");            // SM 4 = IRM on
          s.feed(CSI + "!p");            // DECSTR
          s.feed("AB" + CSI + "1G" + "C");
          expectEq((int)glyphAt(s.scr, 0, 1), 'B', "T18 DECSTR clears insert mode"); }

        // DECSTR resets the pen, so text printed afterwards is not still wearing the old SGR.
        { Screen s("", 20, 3);
          s.feed(CSI + "1;4m");          // bold + underline
          s.feed(CSI + "!p");
          s.feed("A");
          expectEq((int)attrAt(s.scr, 0, 0), (int)ATTR_NONE, "T18 DECSTR resets the pen to plain"); }

        // DECSTR homes the saved-cursor slot, so a later DECRC cannot restore a stale position.
        // The LIVE cursor must NOT move: neither the VT510 DECSTR table nor xterm moves it, and
        // a program that soft-resets mid-screen would otherwise find its output at the top left.
        { Screen s("", 20, 3);
          s.feed(CSI + "3;10H"); s.feed(ESC + "7");   // DECSC at (2,9)
          s.feed(CSI + "!p");
          expectEq(s.scr.cursorRow(), 2, "T18 DECSTR leaves the live cursor row alone");
          expectEq(s.scr.cursorCol(), 9, "T18 DECSTR leaves the live cursor col alone");
          s.feed(ESC + "8");                          // DECRC
          expectEq(s.scr.cursorRow(), 0, "T18 DECSTR homes the saved cursor row");
          expectEq(s.scr.cursorCol(), 0, "T18 DECSTR homes the saved cursor col"); }

        // DECSTR clears the line-drawing charset too (VT510 DECSTR resets G0..G3 to ASCII).
        { Screen s("", 20, 3);
          s.feed(ESC + "(0"); s.feed(CSI + "!p"); s.feed("q");
          expectEq((int)glyphAt(s.scr, 0, 0), 'q', "T18 DECSTR clears the line-drawing charset"); }
    }

    // -------------------------------------------------------------------------------------
    std::printf("\n================================================\n");
    std::printf("  passed: %d   failed: %d\n", g_pass, g_fail);
    std::printf("================================================\n");
    return g_fail == 0 ? 0 : 1;
}
