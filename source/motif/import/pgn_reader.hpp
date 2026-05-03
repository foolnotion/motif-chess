#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>

#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)

#include "motif/import/error.hpp"

namespace motif::import
{

class pgn_reader
{
  public:
    explicit pgn_reader(std::filesystem::path path);

    // Returns next game from the stream.
    // Errors: eof (stream exhausted), parse_error (bad game — caller should
    // call next() to continue),
    //         io_failure (file unreadable or not found — stream is done after
    //         this)
    [[nodiscard]] auto next() -> result<pgn::game>;

    // Re-open the source file, find the first [Event tag at or after
    // byte_offset, and reset the stream to begin there. Returns io_failure if
    // the file cannot be read.
    [[nodiscard]] auto seek_to_offset(std::size_t byte_offset) -> result<void>;

    // 1-based counter: increments on each next() call, including parse errors.
    [[nodiscard]] auto game_number() const noexcept -> std::size_t;

    // Byte offset of the game currently ready to be returned (before next() is
    // called). Returns 0 when the stream is exhausted or not yet started.
    [[nodiscard]] auto byte_offset() const noexcept -> std::size_t;

  private:
    [[nodiscard]] auto reset_to_offset(std::size_t byte_offset) -> result<void>;

    std::filesystem::path path_;
    std::ifstream file_;
    std::size_t next_game_offset_ {0};
    std::size_t game_number_ {0};
    bool has_next_game_ {false};
    bool pending_io_failure_ {false};
};

}  // namespace motif::import
