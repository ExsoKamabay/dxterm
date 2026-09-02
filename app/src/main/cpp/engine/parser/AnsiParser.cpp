#include "parser/AnsiParser.h"
#include <cstdio>
#include <cstdlib>

namespace xterm {

// Upper bound for a single OSC string payload. Legitimate OSC (title, base64 clipboard, the private
// DRACX app-control command) is far smaller; this only stops a malformed/hostile OSC that never sends
// its ST terminator from growing oscBuf_ without bound (memory-exhaustion hardening). On overflow we
// stop appending but keep scanning for the terminator, so the parser still recovers to Ground.
static constexpr size_t kMaxOscBytes = 1u << 20;   // 1 MiB

// Ceiling on one CSI numeric parameter, and on how many parameters a single sequence may carry.
// Both are attacker-supplied: a parameter is whatever a program in the shell chose to print.
// Without the value ceiling `curParam_ * 10 + digit` overflows a signed int, which is undefined
// behaviour, and the result then drives per-line loops in ScreenBuffer. Without the count ceiling
// a run of semicolons grows params_ by 4 bytes each until the process is out of memory.
// 65535 is xterm's own parameter ceiling; 256 parameters is an order of magnitude more than any
// real sequence (the longest in practice is an SGR run of about twenty).
static constexpr int    kMaxParamValue = 65535;
static constexpr size_t kMaxParams     = 256;

AnsiParser::AnsiParser(ScreenBuffer& screen, ParserHost& host)
    : scr_(screen), host_(host) {}

void AnsiParser::reset() {
    state_ = State::Ground;
    params_.clear(); curParam_ = 0; haveParam_ = false;
    interm_.clear(); privateMarker_ = false; oscBuf_.clear();
    resetInterpretation();
}

// The parser's own view of how the next byte is to be read: charset selection, a UTF-8
// sequence still in flight, and the glyph CSI b would repeat. Shared by RIS and DECSTR,
// which are the two sequences `reset` sends and both of which are defined to clear it.
void AnsiParser::resetInterpretation() {
    g0Graphics_ = g1Graphics_ = usingG1_ = false;
    uAcc_ = 0; uRemain_ = 0; uMin_ = 0;
    lastGlyph_ = 0;
}

int AnsiParser::param(size_t i, int def) const {
    if (i >= params_.size()) return def;
    return params_[i] == 0 ? def : params_[i];
}

// DEC Special Graphics: map ASCII 0x5F..0x7E to box-drawing/line code points.
uint32_t AnsiParser::mapCharset(uint32_t cp) const {
    bool gfx = usingG1_ ? g1Graphics_ : g0Graphics_;
    if (!gfx || cp < 0x5F || cp > 0x7E) return cp;
    static const uint32_t tbl[] = {
        /*5F _*/0x00A0,0x25C6,0x2592,0x2409,0x240C,0x240D,0x240A,0x00B0,
        /*67 g*/0x00B1,0x2424,0x240B,0x2518,0x2510,0x250C,0x2514,0x253C,
        /*6F o*/0x23BA,0x23BB,0x2500,0x23BC,0x23BD,0x251C,0x2524,0x2534,
        /*77 w*/0x252C,0x2502,0x2264,0x2265,0x03C0,0x2260,0x00A3,0x00B7,
    };
    return tbl[cp - 0x5F];
}

void AnsiParser::feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        switch (state_) {
            case State::Ground:     ground(b); break;
            case State::Esc:        esc(b); break;
            case State::EscInterm:  escInterm(b); break;
            case State::CsiParam:   csi(b); break;
            case State::OscString:  osc(b); break;
            case State::DcsString:
            case State::DcsPass:    dcs(b); break;
        }
    }
}

void AnsiParser::execC0(uint8_t b) {
    switch (b) {
        case 0x07: host_.bell(); break;                 // BEL
        case 0x08: scr_.backspace(); break;             // BS
        case 0x09: scr_.tab(); break;                   // HT
        case 0x0A: case 0x0B: case 0x0C:                // LF/VT/FF
            scr_.lineFeed(); break;
        case 0x0D: scr_.carriageReturn(); break;        // CR
        case 0x0E: usingG1_ = true; break;              // SO -> G1
        case 0x0F: usingG1_ = false; break;             // SI -> G0
        default: break;
    }
}

void AnsiParser::ground(uint8_t b) {
    if (b == 0x1B) { state_ = State::Esc; return; }
    if (b < 0x20) { execC0(b); return; }
    if (b == 0x7F) return;                              // DEL ignored

    // UTF-8 decode.
    if (uRemain_ > 0) {
        if ((b & 0xC0) == 0x80) {
            uAcc_ = (uAcc_ << 6) | (b & 0x3F);
            if (--uRemain_ == 0) {
                // Only a Unicode SCALAR VALUE may reach the grid. The three rejects below all
                // decode to something a naive accumulator accepts: an overlong form (E0 80 AF
                // is '/', the classic filter-evasion encoding), a UTF-16 surrogate, or, from a
                // 0xF5..0xF7 lead, a value above U+10FFFF. The Android renderer draws a cell
                // with String(Character.toChars(cp)), which throws IllegalArgumentException on
                // the last of those, so `cat` on a binary file used to take the app down from
                // inside onDraw.
                const bool overlong  = uAcc_ < uMin_;
                const bool surrogate = uAcc_ >= 0xD800u && uAcc_ <= 0xDFFFu;
                const bool tooLarge  = uAcc_ > 0x10FFFFu;
                printGlyph((overlong || surrogate || tooLarge) ? 0xFFFDu : uAcc_);
            }
            return;
        }
        uRemain_ = 0; // malformed; fall through to treat b fresh
    }
    if (b < 0x80) { printGlyph(b); return; }
    // uMin_ is the smallest value the sequence length is allowed to encode; see the overlong
    // check above.
    if ((b & 0xE0) == 0xC0) { uAcc_ = b & 0x1F; uRemain_ = 1; uMin_ = 0x80u;    return; }
    if ((b & 0xF0) == 0xE0) { uAcc_ = b & 0x0F; uRemain_ = 2; uMin_ = 0x800u;   return; }
    if ((b & 0xF8) == 0xF0) { uAcc_ = b & 0x07; uRemain_ = 3; uMin_ = 0x10000u; return; }
    printGlyph(0xFFFD); // invalid lead
}

void AnsiParser::printGlyph(uint32_t cp) { uint32_t g = mapCharset(cp); lastGlyph_ = g; scr_.put(g); }

void AnsiParser::esc(uint8_t b) {
    switch (b) {
        case '[': params_.clear(); curParam_ = 0; haveParam_ = false;
                  interm_.clear(); privateMarker_ = false; state_ = State::CsiParam; return;
        case ']': oscBuf_.clear(); state_ = State::OscString; return;
        case 'P': oscBuf_.clear(); state_ = State::DcsString; return;
        case 'M': scr_.reverseLineFeed(); state_ = State::Ground; return;   // RI
        case 'D': scr_.lineFeed(); state_ = State::Ground; return;          // IND
        case 'E': scr_.carriageReturn(); scr_.lineFeed(); state_ = State::Ground; return; // NEL
        case 'H': scr_.setTabStop(); state_ = State::Ground; return;        // HTS
        case '7': scr_.saveCursor(); state_ = State::Ground; return;
        case '8': scr_.restoreCursor(); state_ = State::Ground; return;
        case 'c':                                                           // RIS
            // A full reset, so the state that decides how the NEXT byte is read goes too:
            // the selected charset, a half-decoded UTF-8 sequence, and the input modes the
            // Android side caches out of meta[]. Resetting only the screen left `reset` unable
            // to repair the very confusion it is typed to repair (ESC ( 0 still in force turns
            // every letter into box drawing).
            scr_.reset();
            resetInterpretation();
            host_.setAppCursorKeys(false);
            host_.setAppKeypad(false);
            host_.setMouseTracking(0);
            host_.setMouseSGR(false);
            host_.setBracketedPaste(false);
            host_.setFocusReporting(false);
            state_ = State::Ground;
            return;
        case '=': host_.setAppKeypad(true); state_ = State::Ground; return;
        case '>': host_.setAppKeypad(false); state_ = State::Ground; return;
        case '(': case ')': case '#': case '*': case '+':
            interm_.clear(); interm_.push_back((char)b); state_ = State::EscInterm; return;
        case '\\': state_ = State::Ground; return;                         // ST
        default: state_ = State::Ground; return;
    }
}

void AnsiParser::escInterm(uint8_t b) {
    char lead = interm_.empty() ? 0 : interm_[0];
    if (lead == '(' || lead == ')') {
        bool gfx = (b == '0');
        if (lead == '(') g0Graphics_ = gfx; else g1Graphics_ = gfx;
    } else if (lead == '#' && b == '8') {
        // DECALN: fill screen with 'E'
        for (int r = 0; r < scr_.rows(); ++r) { scr_.setCursor(r, 0);
            for (int c = 0; c < scr_.cols(); ++c) scr_.put('E'); }
        scr_.setCursor(0, 0);
    }
    state_ = State::Ground;
}

void AnsiParser::csi(uint8_t b) {
    if (b >= '0' && b <= '9') {
        // Saturate rather than wrap: see kMaxParamValue.
        if (curParam_ <= (kMaxParamValue - 9) / 10) curParam_ = curParam_ * 10 + (b - '0');
        else curParam_ = kMaxParamValue;
        haveParam_ = true;
        return;
    }
    if (b == ';') {
        if (params_.size() < kMaxParams) params_.push_back(haveParam_ ? curParam_ : 0);
        curParam_ = 0; haveParam_ = false;
        return;
    }
    if (b == '?' || b == '<' || b == '=' || b == '>') {
        privateMarker_ = (b == '?');
        if (interm_.size() < kMaxParams) interm_.push_back((char)b);   // bounded, as above
        return;
    }
    if (b >= 0x20 && b <= 0x2F) {
        if (interm_.size() < kMaxParams) interm_.push_back((char)b);   // intermediate
        return;
    }
    if (b < 0x20) { execC0(b); return; }
    if (params_.size() < kMaxParams) params_.push_back(haveParam_ ? curParam_ : 0);
    dispatchCsi(b);
    state_ = State::Ground;
}

void AnsiParser::dispatchCsi(uint8_t f) {
    // A CSI parameter-prefix byte other than '?' selects a DIFFERENT command space, so the final
    // byte does NOT mean what it means in a plain CSI. Two of these are sent by vim on every
    // start-up and shut-down and were previously mis-dispatched:
    //   CSI > 4 ; 2 m  XTMODKEYS (modifyOtherKeys): ran through applySGR() and latched
    //                  UNDERLINE|DIM onto the pen, so text drawn afterwards was corrupted.
    //   CSI > c        Secondary DA (DA2): was answered with the PRIMARY DA reply.
    // '?' keeps its existing route: csi() sets privateMarker_, which handleMode() reads.
    const bool gtMarker = interm_.find('>') != std::string::npos;
    const bool eqMarker = interm_.find('=') != std::string::npos;
    switch (f) {
        case 'A': scr_.moveCursor(-param(0,1), 0); break;
        case 'B': case 'e': scr_.moveCursor(param(0,1), 0); break;
        case 'C': scr_.moveCursor(0, param(0,1)); break;
        case 'D': scr_.moveCursor(0, -param(0,1)); break;
        case 'E': scr_.carriageReturn(); scr_.moveCursor(param(0,1), 0); break;
        case 'F': scr_.carriageReturn(); scr_.moveCursor(-param(0,1), 0); break;
        case 'G': case '`': scr_.setCursorCol(param(0,1) - 1); break;     // CHA/HPA
        case 'd': scr_.setCursorRow(param(0,1) - 1); break;               // VPA
        case 'H': case 'f': scr_.setCursor(param(0,1) - 1, param(1,1) - 1); break;
        case 'I': scr_.tabForward(param(0,1)); break;                     // CHT
        case 'Z': scr_.tabBack(param(0,1)); break;                        // CBT
        case 'g': if (param(0,0) == 3) scr_.clearAllTabStops(); else scr_.clearTabStop(); break; // TBC
        case 'J': scr_.eraseInDisplay(param(0,0)); break;
        case 'K': scr_.eraseInLine(param(0,0)); break;
        case 'L': scr_.insertLines(param(0,1)); break;
        case 'M': scr_.deleteLines(param(0,1)); break;
        case 'P': scr_.deleteChars(param(0,1)); break;
        case '@': scr_.insertChars(param(0,1)); break;
        case 'X': scr_.eraseChars(param(0,1)); break;                     // ECH
        case 'S': scr_.scrollUp(param(0,1)); break;
        case 'T': scr_.scrollDown(param(0,1)); break;
        case 'b': { int n = param(0,1); if (lastGlyph_) for (int k=0;k<n;++k) scr_.put(lastGlyph_); break; } // REP
        case 'm':
            if (gtMarker || eqMarker) break;                              // XTMODKEYS / XTQMODKEYS
            scr_.applySGR(params_); break;
        case 'r': scr_.setScrollRegion(param(0,1) - 1, param(1,0)); break;
        case 's': scr_.saveCursor(); break;
        case 'u': scr_.restoreCursor(); break;
        case 'h': handleMode(privateMarker_, true); break;
        case 'l': handleMode(privateMarker_, false); break;
        case 'c':
            if (gtMarker) { host_.respond("\x1b[>41;0;0c"); break; }       // DA2 -> VT420 family
            if (eqMarker) break;                                          // DA3 (DECRPTUI) unsupported
            host_.respond("\x1b[?62;22c"); break;                          // DA1 -> VT220
        case 'n': {                                                       // DSR
            int q = param(0,0);
            if (q == 5) host_.respond("\x1b[0n");
            else if (q == 6) { char buf[32]; std::snprintf(buf, sizeof buf,
                "\x1b[%d;%dR", scr_.cursorRow() + 1, scr_.cursorCol() + 1); host_.respond(buf); }
            break;
        }
        case 'p': /* DECSTR soft reset when interm has '!' */
            if (!interm_.empty() && interm_.back() == '!') {
                // Cursor position is NOT in the VT510 DECSTR table, but both calls below home it
                // as a side effect (DECSTBM and DECOM each do, for their own sequences), so a
                // soft reset used to teleport the caller's output to the top left.
                const int keepRow = scr_.cursorRow(), keepCol = scr_.cursorCol();
                scr_.setScrollRegion(0, scr_.rows());
                scr_.setOriginMode(false); scr_.setAutoWrap(true);
                scr_.setCursorVisible(true);
                // The rest of the VT510 DECSTR table. Without these a `tput reset` left IRM
                // latched (every later line shifted right as it printed), the pen still wearing
                // the last SGR, and DECRC able to restore a cursor saved before the reset.
                scr_.setInsertMode(false);
                scr_.applySGR(std::vector<int>{});      // SGR 0: plain pen, default colours
                scr_.resetSavedCursor();                // DECSC slot home; live cursor stays put
                resetInterpretation();
                host_.setAppCursorKeys(false);
                host_.setAppKeypad(false);
                scr_.setCursor(keepRow, keepCol);       // origin mode is absolute again by now
            }
            break;
        case 'q': /* DECSCUSR cursor style (space q): tracked as visible only */ break;
        default: break;
    }
}

void AnsiParser::handleMode(bool priv, bool set) {
    for (int m : params_) {
        if (!priv) {
            if (m == 4) scr_.setInsertMode(set);   // IRM (insert/replace). 20 = LNM (ignored).
            continue;
        }
        switch (m) {
            case 1:  host_.setAppCursorKeys(set); break;
            case 5:  scr_.setReverseScreen(set); break;          // DECSCNM
            case 6:  scr_.setOriginMode(set); break;             // DECOM
            case 7:  scr_.setAutoWrap(set); break;               // DECAWM
            case 25: scr_.setCursorVisible(set); break;          // DECTCEM
            case 47: case 1047:
                if (set) scr_.enterAltScreen(true); else scr_.leaveAltScreen(true); break;
            case 1049:
                if (set) { scr_.saveCursor(); scr_.enterAltScreen(true); }
                else { scr_.leaveAltScreen(true); scr_.restoreCursor(); }
                break;
            case 1000: host_.setMouseTracking(set ? 1000 : 0); break;
            case 1002: host_.setMouseTracking(set ? 1002 : 0); break;
            case 1003: host_.setMouseTracking(set ? 1003 : 0); break;
            case 1006: host_.setMouseSGR(set); break;
            case 1004: host_.setFocusReporting(set); break;
            case 2004: host_.setBracketedPaste(set); break;
            default: break;
        }
    }
}

// ---- OSC ----
static bool base64Decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int v = val(c); if (v < 0) continue;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((char)((buf >> bits) & 0xFF)); }
    }
    return true;
}

void AnsiParser::osc(uint8_t b) {
    if (b == 0x07) { dispatchOsc(); state_ = State::Ground; return; }       // BEL terminator
    if (b == 0x1B) { oscTerm_ = 0x1B; return; }                             // maybe ST
    if (b == '\\' && oscTerm_ == 0x1B) { dispatchOsc(); oscTerm_ = 0; state_ = State::Ground; return; }
    oscTerm_ = 0;
    if (b >= 0x20 && oscBuf_.size() < kMaxOscBytes) oscBuf_.push_back((char)b);
}

void AnsiParser::dispatchOsc() {
    // Format: <code> ; <text>
    size_t sc = oscBuf_.find(';');
    if (sc == std::string::npos) return;
    int code = std::atoi(oscBuf_.substr(0, sc).c_str());
    std::string arg = oscBuf_.substr(sc + 1);
    if (code == 0 || code == 1 || code == 2) {
        host_.setTitle(arg);
    } else if (code == 52) {
        // 52 ; <selection> ; <base64>
        size_t sc2 = arg.find(';');
        if (sc2 != std::string::npos) {
            std::string data = arg.substr(sc2 + 1), decoded;
            if (data != "?" && base64Decode(data, decoded)) host_.copyToClipboard(decoded);
        }
    } else if (code == 5391) {
        // Private drac-Xterm application-control channel (payload = "DRACX;<verb>[;<data>]").
        // Emitted by the in-terminal `xset` command to open the settings dashboard.
        host_.appControl(arg);
    }
    // 4/10/11 palette queries acknowledged by ignoring (defaults already applied).
}

void AnsiParser::dcs(uint8_t b) {
    // Consume the DCS string until ST (ESC \) or BEL, discarding the payload.
    if (b == 0x07) { state_ = State::Ground; return; }
    if (b == 0x1B) { oscTerm_ = 0x1B; return; }
    if (b == '\\' && oscTerm_ == 0x1B) { oscTerm_ = 0; state_ = State::Ground; return; }
    oscTerm_ = 0;
}

} // namespace xterm
