#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>

#include "motif/import/pgn_reader.hpp"

#include <pgnlib/pgnlib.hpp>  // NOLINT(misc-include-cleaner)
#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <spdlog/spdlog.h>
#include <tl/expected.hpp>

#include "motif/import/error.hpp"

namespace motif::import
{

namespace
{

constexpr std::string_view event_tag_prefix {"[Event \""};

struct game_block
{
    std::string text;
    std::size_t following_offset {0};
    bool has_following_game {false};
};

auto read_raw_line(std::ifstream& file) -> std::string
{
    std::string line;
    char current_char = '\0';

    while (file.get(current_char)) {
        line.push_back(current_char);
        if (current_char == '\n') {
            break;
        }
    }

    return line;
}

auto is_event_tag_line(std::string_view line) noexcept -> bool
{
    return line.starts_with(event_tag_prefix);
}

auto update_brace_comment_state(std::string_view text,
                                bool in_brace_comment) noexcept -> bool
{
    for (auto current_char : text) {
        if (!in_brace_comment && current_char == '{') {
            in_brace_comment = true;
            continue;
        }
        if (in_brace_comment && current_char == '}') {
            in_brace_comment = false;
        }
    }

    return in_brace_comment;
}

auto to_offset(std::streampos position) -> std::size_t
{
    return static_cast<std::size_t>(position);
}

auto read_game_block(std::ifstream& file, std::size_t start_offset)
    -> result<game_block>
{
    auto block = game_block {};

    file.clear();
    file.seekg(static_cast<std::streamoff>(start_offset), std::ios::beg);
    if (!file) {
        return tl::unexpected(error_code::io_failure);
    }

    auto in_brace_comment = false;
    while (true) {
        const auto line_start = file.tellg();
        const auto line = read_raw_line(file);

        if (line.empty()) {
            if (file.bad()) {
                return tl::unexpected(error_code::io_failure);
            }
            break;
        }

        if (!block.text.empty() && !in_brace_comment && is_event_tag_line(line))
        {
            block.following_offset = to_offset(line_start);
            block.has_following_game = true;
            file.clear();
            file.seekg(line_start, std::ios::beg);
            return block;
        }

        block.text += line;
        in_brace_comment = update_brace_comment_state(line, in_brace_comment);
    }

    return block;
}

}  // namespace

pgn_reader::pgn_reader(std::filesystem::path path)
    : path_(std::move(path))
{
    if (!reset_to_offset(0)) {
        pending_io_failure_ = true;
    }
}

auto pgn_reader::next() -> result<pgn::game>
{
    if (pending_io_failure_) {
        pending_io_failure_ = false;
        return tl::unexpected(error_code::io_failure);
    }

    if (!has_next_game_) {
        return tl::unexpected(error_code::eof);
    }

    const auto offset_before = next_game_offset_;
    auto block_result = read_game_block(file_, next_game_offset_);
    if (!block_result) {
        has_next_game_ = false;
        next_game_offset_ = 0;
        return tl::unexpected(block_result.error());
    }

    has_next_game_ = block_result->has_following_game;
    next_game_offset_ = block_result->following_offset;
    ++game_number_;

    auto game_result = pgn::parse_string(block_result->text);
    if (!game_result || game_result->empty()) {
        auto log = spdlog::get("motif.import");
        if (log) {
            log->warn("pgn parse error at game {} (byte offset: {}): {}",
                      game_number_,
                      offset_before,
                      !game_result
                              && game_result.error()
                                  == pgn::parse_error::file_not_found
                          ? "file not found"
                          : "syntax error");
        }
        if (!game_result
            && game_result.error() == pgn::parse_error::file_not_found)
        {
            return tl::unexpected(error_code::io_failure);
        }
        return tl::unexpected(error_code::parse_error);
    }

    auto games = std::move(game_result).value();
    return std::move(games.front());
}

auto pgn_reader::seek_to_offset(std::size_t byte_offset) -> result<void>
{
    pending_io_failure_ = false;
    return reset_to_offset(byte_offset);
}

auto pgn_reader::game_number() const noexcept -> std::size_t
{
    return game_number_;
}

auto pgn_reader::byte_offset() const noexcept -> std::size_t
{
    if (!has_next_game_) {
        return 0;
    }
    return next_game_offset_;
}

auto pgn_reader::reset_to_offset(std::size_t byte_offset) -> result<void>
{
    game_number_ = 0;
    has_next_game_ = false;
    next_game_offset_ = 0;

    file_.close();
    file_ = std::ifstream(path_, std::ios::binary);
    if (!file_) {
        return tl::unexpected(error_code::io_failure);
    }

    file_.clear();
    file_.seekg(0, std::ios::beg);

    auto in_brace_comment = false;
    while (true) {
        const auto line_start = file_.tellg();
        const auto line = read_raw_line(file_);

        if (line.empty()) {
            if (file_.bad()) {
                return tl::unexpected(error_code::io_failure);
            }
            return {};
        }

        if (!in_brace_comment && to_offset(line_start) >= byte_offset
            && is_event_tag_line(line))
        {
            next_game_offset_ = to_offset(line_start);
            has_next_game_ = true;
            file_.clear();
            file_.seekg(line_start, std::ios::beg);
            return {};
        }

        in_brace_comment = update_brace_comment_state(line, in_brace_comment);
    }
}

}  // namespace motif::import
