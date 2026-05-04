#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "motif/search/error.hpp"

namespace motif::db
{
class database_manager;
}  // namespace motif::db

namespace motif::search::opening_tree
{

constexpr auto default_prefetch_depth = std::size_t {5};

struct node;

struct node_continuation
{
    std::string san;
    std::uint64_t result_hash {};
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::optional<double> average_white_elo;
    std::optional<double> average_black_elo;
    std::optional<std::string> eco;
    std::optional<std::string> opening_name;
    std::unique_ptr<node> subtree;
};

struct node
{
    std::uint64_t zobrist_hash {};
    std::vector<node_continuation> continuations;
    bool is_expanded {false};
};

struct tree
{
    node root;
    std::size_t prefetch_depth {default_prefetch_depth};
};

[[nodiscard]] auto open(motif::db::database_manager const& database,
                        std::uint64_t root_hash,
                        std::size_t prefetch_depth = default_prefetch_depth) -> result<tree>;

[[nodiscard]] auto expand(motif::db::database_manager const& database, node& n) -> result<void>;

}  // namespace motif::search::opening_tree
