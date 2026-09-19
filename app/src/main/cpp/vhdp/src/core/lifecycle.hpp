// Explicit session lifecycle state machine.
//
//   created --start--> starting --exec ok--> running --guest exit--> exited
//      |                  |                     |---timeout/cancel--> cancelled
//      |                  |--exec failed------> failed
//      |                  |--cancel-----------> cancelled
//      |--destroy/cancel before start--------> cancelled
//   running --supervisor failure--> failed
//
// Terminal states (exited, failed, cancelled) have no outgoing transitions.
#pragma once

#include "common/status.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace vhdp::core {

enum class State : std::uint32_t {
    created = 0,
    starting = 1,
    running = 2,
    exited = 3,
    failed = 4,
    cancelled = 5
};

const char* state_name(State s) noexcept;
bool is_terminal(State s) noexcept;
bool can_transition(State from, State to) noexcept;

class StateMachine {
public:
    State get() const;
    // Fails with invalid_state (and changes nothing) for a forbidden transition.
    Status transition(State to);
    // Waits until a terminal state is reached. timeout < 0 waits forever.
    // Returns true when terminal.
    bool wait_terminal(std::chrono::milliseconds timeout) const;

private:
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    State state_ = State::created;
};

} // namespace vhdp::core
