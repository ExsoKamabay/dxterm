#include "core/session.hpp"

#include "common/json.hpp"

#include <utility>

namespace vhdp::core {

namespace {

const char* reason_name(ExitReason r) {
    switch (r) {
        case ExitReason::none:
            return "none";
        case ExitReason::normal:
            return "exited";
        case ExitReason::signaled:
            return "signaled";
        case ExitReason::timeout:
            return "timeout";
        case ExitReason::cancelled:
            return "cancelled";
        case ExitReason::start_failed:
            return "start_failed";
    }
    return "unknown";
}

} // namespace

Session::Session(Context& ctx, std::shared_ptr<const ValidatedConfig> cfg)
    : ctx_(ctx), cfg_(std::move(cfg)), id_(ctx.next_session_id()) {
    ctx_.live_sessions().fetch_add(1);
}

Session::~Session() {
    State s = sm_.get();
    if (s == State::created) {
        (void)sm_.transition(State::cancelled);
    } else if (!is_terminal(s)) {
        (void)cancel();
    }
    if (waiter_.joinable()) {
        waiter_.join();
    }
    {
        std::lock_guard<std::mutex> lock(wd_mu_);
        wd_stop_ = true;
    }
    wd_cv_.notify_all();
    if (watchdog_.joinable()) {
        watchdog_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        inst_.reset();
    }
    ctx_.live_sessions().fetch_sub(1);
}

void Session::emit(EventKind kind, Severity sev, std::string name, std::string message,
                   std::string detail) {
    EventSink& sink = ctx_.sink();
    if (!sink.wants(sev)) {
        return;
    }
    Event ev;
    ev.kind = kind;
    ev.severity = sev;
    ev.session_id = id_;
    ev.timestamp_ns = monotonic_ns();
    ev.name = std::move(name);
    ev.message = std::move(message);
    ev.detail_json = std::move(detail);
    sink.emit(std::move(ev));
}

void Session::fail_start(const Status& s, ExitResult r) {
    r.reason = ExitReason::start_failed;
    r.status = s;
    {
        std::lock_guard<std::mutex> lock(mu_);
        result_ = std::move(r);
    }
    JsonWriter w;
    w.begin_object()
        .key("status")
        .value(code_name(s.code()))
        .key("exec_errno")
        .value(result_.exec_errno)
        .end_object();
    emit(EventKind::lifecycle, Severity::error, "session.failed", s.message(), w.str());
    (void)sm_.transition(cancel_requested_.load() ? State::cancelled : State::failed);
}

Status Session::start() {
    std::lock_guard<std::mutex> start_lock(start_mu_);
    if (Status s = sm_.transition(State::starting); !s.is_ok()) {
        return s;
    }
    emit(EventKind::lifecycle, Severity::debug, "session.starting", "session starting");

    auto choice = select_engine(cfg_->cfg.engine, ctx_.engines());
    if (!choice.is_ok()) {
        Status s = choice.status();
        fail_start(s, ExitResult{});
        return s;
    }
    kind_.store(choice.value().engine->kind());
    {
        JsonWriter w;
        w.begin_object()
            .key("engine")
            .value(engine_name(kind_.load()))
            .key("requested")
            .value(engine_name(cfg_->cfg.engine));
        w.end_object();
        emit(EventKind::lifecycle, Severity::info, "engine.selected", choice.value().reason,
             w.str());
    }

    auto inst = choice.value().engine->create_instance(cfg_, ctx_.sink(), id_);
    if (!inst.is_ok()) {
        Status s = inst.status();
        fail_start(s, ExitResult{});
        return s;
    }
    EngineInstance* raw = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        inst_ = std::move(inst.value());
        raw = inst_.get();
    }
    if (cancel_requested_.load()) {
        (void)raw->cancel(ExitReason::cancelled);
    }
    Status s = raw->start();
    if (!s.is_ok()) {
        ExitResult r = raw->wait();
        fail_start(s, std::move(r));
        return s;
    }
    if (Status t = sm_.transition(State::running); !t.is_ok()) {
        (void)raw->cancel(ExitReason::cancelled);
    }
    if (cancel_requested_.load()) {
        (void)raw->cancel(ExitReason::cancelled);
    }
    try {
        waiter_ = std::thread([this] { waiter_main(); });
        if (cfg_->cfg.timeout_ms > 0) {
            watchdog_ = std::thread([this] { watchdog_main(); });
        }
    } catch (...) {
        (void)raw->cancel(ExitReason::cancelled);
        if (!waiter_.joinable()) {
            // Without a waiter nobody can reach a terminal state: wait inline.
            ExitResult r = raw->wait();
            {
                std::lock_guard<std::mutex> lock(mu_);
                result_ = r;
            }
            (void)sm_.transition(State::failed);
        }
        return {Code::no_memory, "cannot create session threads"};
    }
    return Status::ok();
}

void Session::waiter_main() noexcept {
    EngineInstance* raw = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        raw = inst_.get();
    }
    ExitResult r;
    try {
        r = raw->wait();
    } catch (...) {
        r.reason = ExitReason::cancelled;
        r.status = {Code::internal, "exception while waiting for the guest"};
    }
    State terminal = State::exited;
    if (r.reason == ExitReason::timeout || r.reason == ExitReason::cancelled) {
        terminal = State::cancelled;
    } else if (r.reason == ExitReason::start_failed || r.reason == ExitReason::none) {
        terminal = State::failed;
    }
    try {
        JsonWriter w;
        w.begin_object();
        w.key("reason").value(reason_name(r.reason));
        w.key("exit_code").value(r.exit_code);
        w.key("signal").value(r.term_signal);
        w.key("shell_status").value(shell_status(r));
        w.end_object();
        std::string msg = std::string("session ended: ") + reason_name(r.reason);
        {
            std::lock_guard<std::mutex> lock(mu_);
            result_ = r;
        }
        emit(EventKind::lifecycle, Severity::info, "session.exited", msg, w.str());
    } catch (...) {
        std::lock_guard<std::mutex> lock(mu_);
        result_ = r;
    }
    (void)sm_.transition(terminal);
    {
        std::lock_guard<std::mutex> lock(wd_mu_);
        wd_stop_ = true;
    }
    wd_cv_.notify_all();
}

void Session::watchdog_main() noexcept {
    std::unique_lock<std::mutex> lock(wd_mu_);
    bool stopped = wd_cv_.wait_for(lock, std::chrono::milliseconds(cfg_->cfg.timeout_ms),
                                   [this] { return wd_stop_; });
    if (stopped) {
        return;
    }
    lock.unlock();
    try {
        emit(EventKind::lifecycle, Severity::warning, "session.timeout",
             "timeout of " + std::to_string(cfg_->cfg.timeout_ms) +
                 " ms expired; killing the guest process tree");
    } catch (...) { // NOLINT(bugprone-empty-catch): watchdog is noexcept; a failed
        // best-effort diagnostic must never prevent the tree from being killed below.
    }
    std::lock_guard<std::mutex> ilock(mu_);
    if (inst_) {
        (void)inst_->cancel(ExitReason::timeout);
    }
}

bool Session::wait(std::chrono::milliseconds timeout, ExitResult& out, State& state) {
    if (!sm_.wait_terminal(timeout)) {
        state = sm_.get();
        return false;
    }
    state = sm_.get();
    std::lock_guard<std::mutex> lock(mu_);
    out = result_;
    if (state == State::cancelled && out.reason == ExitReason::none) {
        out.reason = ExitReason::cancelled;
    }
    return true;
}

Status Session::signal(int signo) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!inst_ || sm_.get() != State::running) {
        return {Code::invalid_state, "session is not running"};
    }
    return inst_->signal(signo);
}

Status Session::resize(std::uint16_t rows, std::uint16_t cols) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!inst_) {
        return {Code::invalid_state, "session has not been started"};
    }
    return inst_->resize(rows, cols);
}

Status Session::write_input(const std::uint8_t* data, std::size_t len, std::size_t& written) {
    std::lock_guard<std::mutex> lock(mu_);
    written = 0;
    if (!inst_) {
        return {Code::invalid_state, "session has not been started"};
    }
    return inst_->write_input(data, len, written);
}

Status Session::cancel() {
    cancel_requested_.store(true);
    if (sm_.get() == State::created) {
        if (sm_.transition(State::cancelled).is_ok()) {
            std::lock_guard<std::mutex> lock(mu_);
            result_.reason = ExitReason::cancelled;
            return Status::ok();
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (inst_) {
        return inst_->cancel(ExitReason::cancelled);
    }
    return Status::ok();
}

int Session::pty_master() const {
    std::lock_guard<std::mutex> lock(mu_);
    return inst_ ? inst_->pty_master() : -1;
}

} // namespace vhdp::core
