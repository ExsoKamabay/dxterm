#pragma once
#include <string>

namespace xterm {

// Callbacks the parser needs from the surrounding terminal: writing responses
// back to the PTY, and surfacing out-of-band events (title, clipboard, bell)
// and input-affecting modes (app cursor keys, mouse, paste, focus).
struct ParserHost {
    virtual ~ParserHost() = default;
    virtual void respond(const std::string& bytes) = 0;   // -> PTY master
    virtual void setTitle(const std::string&) {}
    virtual void copyToClipboard(const std::string&) {}
    // Private application-control channel (OSC 5391 ; <payload>). Used by the in-terminal
    // settings dashboard (`xset`). Default no-op so any host that ignores it is unaffected.
    virtual void appControl(const std::string&) {}
    virtual void bell() {}
    virtual void setAppCursorKeys(bool) {}
    virtual void setAppKeypad(bool) {}
    virtual void setMouseTracking(int) {}                 // 0 off, 1000/1002/1003
    virtual void setMouseSGR(bool) {}                     // 1006
    virtual void setBracketedPaste(bool) {}
    virtual void setFocusReporting(bool) {}
};

} // namespace xterm
