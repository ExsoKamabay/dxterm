#pragma once
#include "screen/ScreenBuffer.h"
#include "parser/ParserHost.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

namespace xterm {

// Incremental ANSI/VT parser. Feeds bytes, drives a ScreenBuffer, and routes
// responses / OSC events through a ParserHost. Covers C0 controls, CSI (cursor,
// erase, edit, scroll, SGR, DEC private modes, DA/DSR), OSC (title, clipboard),
// DCS (consumed), a UTF-8 decoder, and G0/G1 charset selection.
class AnsiParser {
public:
    AnsiParser(ScreenBuffer& screen, ParserHost& host);

    void feed(const uint8_t* data, size_t len);
    void reset();

private:
    enum class State { Ground, Esc, EscInterm, CsiParam, OscString, DcsString, DcsPass };

    void ground(uint8_t b);
    void esc(uint8_t b);
    void escInterm(uint8_t b);
    void csi(uint8_t b);
    void dispatchCsi(uint8_t final);
    void osc(uint8_t b);
    void dispatchOsc();
    void dcs(uint8_t b);
    void printGlyph(uint32_t cp);
    void execC0(uint8_t b);

    void handleMode(bool priv, bool set);   // SM/RM and DECSET/DECRST
    void resetInterpretation();             // charset + UTF-8 + REP state (RIS and DECSTR)
    int  param(size_t i, int def) const;

    ScreenBuffer& scr_;
    ParserHost& host_;

    State state_ = State::Ground;
    std::vector<int> params_;
    int curParam_ = 0;
    bool haveParam_ = false;
    std::string interm_;          // intermediate / private-marker bytes for CSI
    bool privateMarker_ = false;  // '?' seen in CSI
    std::string oscBuf_;
    uint8_t oscTerm_ = 0;

    // UTF-8 decoder state. uMin_ is the smallest value the sequence in flight may legally
    // encode, which is what makes an overlong form detectable once it is complete.
    uint32_t uAcc_ = 0;
    int uRemain_ = 0;
    uint32_t uMin_ = 0;

    // Charset: true = G0/G1 is DEC special graphics (line-drawing).
    bool g0Graphics_ = false, g1Graphics_ = false;
    bool usingG1_ = false;

    uint32_t mapCharset(uint32_t cp) const;

    uint32_t lastGlyph_ = 0;   // for REP (CSI b)
};

} // namespace xterm
