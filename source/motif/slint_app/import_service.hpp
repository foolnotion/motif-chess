#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/import/import_pipeline.hpp"

namespace motif::slint_app
{

enum class import_error_code : std::uint8_t
{
    invalid_argument,
    no_database,
    busy,
};

enum class import_state : std::uint8_t
{
    idle,
    running,
    canceling,
    completed,
    canceled,
    failed,
};

struct import_snapshot
{
    import_state state {import_state::idle};
    motif::import::import_phase phase {motif::import::import_phase::idle};
    std::size_t processed {};
    std::size_t committed {};
    std::size_t skipped {};
    std::size_t errors {};
    std::size_t total {};
    std::chrono::milliseconds elapsed {};
    std::string error_message;
};

class import_service
{
  public:
    import_service() = default;
    ~import_service();

    import_service(import_service const&) = delete;
    auto operator=(import_service const&) -> import_service& = delete;
    import_service(import_service&&) = delete;
    auto operator=(import_service&&) -> import_service& = delete;

    [[nodiscard]] auto start(motif::db::database_manager* database, std::filesystem::path const& pgn_path)
        -> tl::expected<void, import_error_code>;
    void request_cancel() noexcept;
    [[nodiscard]] auto snapshot() const -> import_snapshot;
    [[nodiscard]] auto active() const -> bool;

  private:
    mutable std::mutex mutex_;
    std::mutex start_mutex_;
    std::unique_ptr<motif::import::import_pipeline> pipeline_;
    std::thread worker_;
    import_snapshot snapshot_;
    bool cancel_requested_ {false};
};

}  // namespace motif::slint_app
