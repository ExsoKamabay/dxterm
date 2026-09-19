#include "core/lifecycle.hpp"

namespace vhdp::core {

const char* state_name(State s) noexcept {
    switch (s) {
        case State::created:
            return "created";
        case State::starting:
            return "starting";
        case State::running:
            return "running";
        case State::exited:
            return "exited";
        case State::failed:
            return "failed";
        case State::cancelled:
            return "cancelled";
    }
    return "unknown";
}

bool is_terminal(State s) noexcept {
    return s == State::exited || s == State::failed || s == State::cancelled;
}

bool can_transition(State from, State to) noexcept {
    switch (from) {
        case State::created:
            return to == State::starting || to == State::cancelled;
        case State::starting:
            return to == State::running || to == State::failed || to == State::cancelled;
        case State::running:
            return to == State::exited || to == State::cancelled || to == State::failed;
        case State::exited:
        case State::failed:
        case State::cancelled:
            return false;
    }
    return false;
}

State StateMachine::get() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

Status StateMachine::transition(State to) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!can_transition(state_, to)) {
            return {Code::invalid_state, std::string("invalid session state transition ") +
                                             state_name(state_) + " -> " + state_name(to)};
        }
        state_ = to;
    }
    cv_.notify_all();
    return Status::ok();
}

bool StateMachine::wait_terminal(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mu_);
    auto pred = [this] { return is_terminal(state_); };
    if (timeout.count() < 0) {
        cv_.wait(lock, pred);
        return true;
    }
    return cv_.wait_for(lock, timeout, pred);
}

} // namespace vhdp::core
