// Session: validated config + lifecycle state machine + engine instance.
#pragma once

#include "core/context.hpp"
#include "core/engine.hpp"
#include "core/lifecycle.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace vhdp::core {

class Session {
public:
    Session(Context& ctx, std::shared_ptr<const ValidatedConfig> cfg);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    std::uint64_t id() const noexcept { return id_; }
    Status start();
    // Returns true when the session is in a terminal state (out/state filled).
    bool wait(std::chrono::milliseconds timeout, ExitResult& out, State& state);
    State state() const { return sm_.get(); }
    EngineKind engine_kind() const noexcept { return kind_.load(); }
    Status signal(int signo);
    Status resize(std::uint16_t rows, std::uint16_t cols);
    Status write_input(const std::uint8_t* data, std::size_t len, std::size_t& written);
    Status cancel();
    int pty_master() const;

private:
    void emit(EventKind kind, Severity sev, std::string name, std::string message,
              std::string detail = "{}");
    void fail_start(const Status& s, ExitResult r);
    void waiter_main() noexcept;
    void watchdog_main() noexcept;

    Context& ctx_;
    std::shared_ptr<const ValidatedConfig> cfg_;
    std::uint64_t id_;
    StateMachine sm_;
    std::atomic<EngineKind> kind_{EngineKind::auto_select};
    std::atomic<bool> cancel_requested_{false};

    std::mutex start_mu_;
    mutable std::mutex mu_;
    std::unique_ptr<EngineInstance> inst_;
    ExitResult result_;

    std::thread waiter_;
    std::thread watchdog_;
    std::mutex wd_mu_;
    std::condition_variable wd_cv_;
    bool wd_stop_ = false;
};

} // namespace vhdp::core
