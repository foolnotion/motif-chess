#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "motif/slint_app/import_service.hpp"

#include <tl/expected.hpp>

#include "motif/import/error.hpp"

namespace motif::slint_app
{

import_service::~import_service()
{
    auto const start_lock = std::scoped_lock {start_mutex_};
    request_cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

auto import_service::start(motif::db::database_manager* database, std::filesystem::path const& pgn_path)
    -> tl::expected<void, import_error_code>
{
    if (database == nullptr) {
        return tl::unexpected {import_error_code::no_database};
    }
    if (pgn_path.empty()) {
        return tl::unexpected {import_error_code::invalid_argument};
    }

    auto const start_lock = std::scoped_lock {start_mutex_};
    {
        auto const lock = std::scoped_lock {mutex_};
        if (snapshot_.state == import_state::running || snapshot_.state == import_state::canceling) {
            return tl::unexpected {import_error_code::busy};
        }
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        auto const lock = std::scoped_lock {mutex_};
        pipeline_ = std::make_unique<motif::import::import_pipeline>(*database);
        snapshot_ = import_snapshot {
            .state = import_state::running,
            .phase = motif::import::import_phase::idle,
            .processed = 0,
            .committed = 0,
            .skipped = 0,
            .errors = 0,
            .total = 0,
            .elapsed = std::chrono::milliseconds {0},
            .error_message = {},
        };
        cancel_requested_ = false;
    }

    worker_ = std::thread(
        [this, pgn_path]() -> void
        {
            motif::import::import_pipeline* pipeline = nullptr;
            {
                auto const lock = std::scoped_lock {mutex_};
                pipeline = pipeline_.get();
            }
            auto result = pipeline->run(pgn_path);
            auto const progress = pipeline->progress();

            auto const lock = std::scoped_lock {mutex_};
            snapshot_.phase = progress.phase;
            snapshot_.processed = progress.games_processed;
            snapshot_.committed = progress.games_committed;
            snapshot_.skipped = progress.games_skipped;
            snapshot_.errors = progress.errors;
            snapshot_.total = progress.total_games;
            snapshot_.elapsed = progress.elapsed;
            if (result) {
                snapshot_.committed = result->committed;
                snapshot_.skipped = result->skipped;
                snapshot_.errors = result->errors;
                snapshot_.elapsed = result->elapsed;
                snapshot_.state = cancel_requested_ ? import_state::canceled : import_state::completed;
            } else {
                snapshot_.state = import_state::failed;
                snapshot_.error_message =
                    result.error().message.empty() ? std::string {motif::import::to_string(result.error())} : result.error().message;
            }
            pipeline_.reset();
        });
    return {};
}

void import_service::request_cancel() noexcept
{
    auto const lock = std::scoped_lock {mutex_};
    if (snapshot_.state != import_state::running && snapshot_.state != import_state::canceling) {
        return;
    }
    cancel_requested_ = true;
    snapshot_.state = import_state::canceling;
    if (pipeline_) {
        pipeline_->request_stop();
    }
}

auto import_service::snapshot() const -> import_snapshot
{
    auto const lock = std::scoped_lock {mutex_};
    auto result = snapshot_;
    if (pipeline_) {
        auto const progress = pipeline_->progress();
        result.phase = progress.phase;
        result.processed = progress.games_processed;
        result.committed = progress.games_committed;
        result.skipped = progress.games_skipped;
        result.errors = progress.errors;
        result.total = progress.total_games;
        result.elapsed = progress.elapsed;
    }
    return result;
}

auto import_service::active() const -> bool
{
    auto const lock = std::scoped_lock {mutex_};
    return snapshot_.state == import_state::running || snapshot_.state == import_state::canceling;
}

}  // namespace motif::slint_app
