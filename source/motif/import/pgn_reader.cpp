#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "motif/import/pgn_reader.hpp"

#include <pgnlib/import.hpp>  // NOLINT(misc-include-cleaner)
#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <spdlog/spdlog.h>
#include <tl/expected.hpp>

#include "motif/import/error.hpp"

namespace motif::import
{

namespace
{

constexpr std::string_view event_tag_prefix {"[Event \""};

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

auto update_brace_comment_state(std::string_view text, bool in_brace_comment) noexcept -> bool
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

// pgn::import_stream leaves tag values as raw bytes with backslash escapes
// undecoded (see pgnlib/import.hpp); unescape \" and \\ to match the
// canonical values the old pgn::parse_string-based path produced.
auto unescape_tag_value(std::string_view raw) -> std::string
{
    std::string value;
    value.reserve(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index) {
        if (raw[index] == '\\' && index + 1 < raw.size() && (raw[index + 1] == '"' || raw[index + 1] == '\\')) {
            ++index;
        }
        value.push_back(raw[index]);
    }
    return value;
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

    if (!stream_.has_value() || iterator_ == stream_->end()) {
        return tl::unexpected(error_code::eof);
    }

    auto const offset_before = base_offset_ + iterator_.byte_offset();
    ++game_number_;

    auto item = std::move(*iterator_);
    if (!item) {
        ++iterator_;
        auto log = spdlog::get("motif.import");
        if (log) {
            log->warn("pgn parse error at game {} (byte offset: {}): {}",
                      game_number_,
                      offset_before,
                      item.error() == pgn::parse_error::file_not_found ? "file not found" : "syntax error");
        }
        if (item.error() == pgn::parse_error::file_not_found) {
            return tl::unexpected(error_code::io_failure);
        }
        return tl::unexpected(error_code::parse_error);
    }

    auto game = pgn::game {};
    game.result = item->result;
    game.tags.reserve(item->tags.size());
    for (auto const& tag : item->tags) {
        game.tags.push_back(pgn::tag {.key = std::string {tag.key}, .value = unescape_tag_value(tag.value)});
    }
    game.moves.reserve(item->moves.size());
    for (auto const& move : item->moves) {
        game.moves.push_back(pgn::move_node {
            .number = move.number,
            .san = std::string {move.san},
            .comment = {},
            .nags = {},
            .variations = {},
        });
    }
    ++iterator_;
    return game;
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
    if (!stream_.has_value() || iterator_ == stream_->end()) {
        return eof_offset_;
    }
    return base_offset_ + iterator_.byte_offset();
}

auto pgn_reader::reset_to_offset(std::size_t byte_offset) -> result<void>
{
    game_number_ = 0;
    stream_.reset();
    iterator_ = pgn::import_stream::iterator {};
    base_offset_ = 0;

    file_.close();
    file_ = std::ifstream(path_, std::ios::binary);
    if (!file_) {
        return tl::unexpected(error_code::io_failure);
    }

    std::error_code fserr;
    eof_offset_ = std::filesystem::file_size(path_, fserr);
    if (fserr) {
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
            // byte_offset == 0 (nothing to resume) and byte_offset >=
            // eof_offset_ (a checkpoint taken exactly at the end of a
            // now-fully-processed file -- nothing left to import) both
            // legitimately reach EOF without a match. Any other byte_offset
            // reaching EOF without a match means the requested resume point
            // was never found -- e.g. the source file was truncated below a
            // checkpointed offset -- and must be reported rather than
            // silently treated as "nothing to import".
            if (byte_offset > 0 && byte_offset < eof_offset_) {
                return tl::unexpected(error_code::io_failure);
            }
            return {};
        }

        if (!in_brace_comment && to_offset(line_start) >= byte_offset && is_event_tag_line(line)) {
            base_offset_ = to_offset(line_start);
            file_.clear();
            file_.seekg(line_start, std::ios::beg);
            stream_.emplace(file_);
            iterator_ = stream_->begin();
            return {};
        }

        in_brace_comment = update_brace_comment_state(line, in_brace_comment);
    }
}

}  // namespace motif::import
