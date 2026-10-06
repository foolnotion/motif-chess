#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "motif/engine/engine_manager.hpp"
#include "motif/engine/error.hpp"

namespace motif::slint_app
{

// One analysis the panel asks for. `generation` and `epoch` are stamped by the
// caller and come back on every event, so a late event of a replaced session can
// be recognised and dropped. Generation 0 is reserved for "no session".
struct analysis_request
{
    std::uint64_t epoch {0};
    std::uint64_t generation {0};
    std::string engine_name;
    std::string engine_path;
    std::string fen;
    int multipv {1};
    // Exactly one of the two limits is set.
    std::optional<int> depth;
    std::optional<int> movetime_ms;
};

// The engine could not be launched or did not answer the UCI handshake. The
// reason is deliberately not carried: the panel shows generic text.
struct analysis_start_failure
{
    motif::engine::error_code code {motif::engine::error_code::engine_start_failed};
};

struct analysis_event
{
    std::uint64_t epoch {0};
    std::uint64_t generation {0};
    std::variant<motif::engine::info_event, motif::engine::complete_event, motif::engine::error_event, analysis_start_failure> payload;
};

// Toolkit-neutral owner of the engine sessions behind the analysis panel. It
// wraps one motif::engine::engine_manager and runs everything that can block
// (launching the engine and its handshake, replaying history, stopping and
// releasing a session, destroying the manager) on its own worker thread, one
// command at a time. Callers are never blocked, except by wait_until_idle() and
// shutdown(), which are for tests and process exit.
//
// At most one session exists: a new start or stop supersedes whatever is queued
// and the previous session is released before the next one is launched. Events
// reach the sink on the engine's reader thread or on the worker thread, never on
// the caller's thread. An event is delivered only while its generation is still
// the latest one asked for, so after start() or stop() returns the old session
// can no longer reach the sink.
//
// A launch that is still in its blocking UCI handshake is cancelled, not waited
// for, by a newer start(), by stop() and by shutdown(): the worker then finishes
// once the engine process is terminated and its reader joined, and the cancelled
// launch reports neither a start failure nor any other event.
class analysis_service
{
  public:
    // Called from other threads: it must be cheap, thread-safe and non-blocking,
    // and it must not call back into this service.
    using event_sink = std::function<void(analysis_event)>;

    explicit analysis_service(event_sink sink);
    ~analysis_service();

    analysis_service(analysis_service const&) = delete;
    auto operator=(analysis_service const&) -> analysis_service& = delete;
    analysis_service(analysis_service&&) = delete;
    auto operator=(analysis_service&&) -> analysis_service& = delete;

    // Replaces the running session, if any, by a session for `request`.
    void start(analysis_request request);

    // Ends the running session, if any.
    void stop();

    // Blocks until every queued command has been carried out.
    void wait_until_idle();

    // Ends the session and the worker. Blocks. Idempotent; later start() and
    // stop() calls do nothing.
    void shutdown();

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace motif::slint_app
