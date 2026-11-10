#pragma once

#include <cstddef>
#include <cstdint>

namespace motif::db
{

// The decoded resident size one chain's active runs may reach when a snapshot is
// opened. Admission never exceeds it, whatever limits a caller configures.
inline constexpr std::uint64_t max_chain_resident_bytes = std::uint64_t {64} << 20U;

inline constexpr std::size_t default_soft_chain_runs = 8;
inline constexpr std::uint64_t default_soft_chain_encoded_bytes = std::uint64_t {4} << 20U;
inline constexpr std::size_t default_hard_chain_runs = 16;
inline constexpr std::uint64_t default_hard_chain_encoded_bytes = std::uint64_t {16} << 20U;

// Admission thresholds for an immutable patch chain (tree patch runs, or
// postings metadata runs). Initial tunables from the PR 6 design, not measured
// optima; tests pass small values.
//
//   soft: maintenance should fold when a chain grows past this, but publication
//         still proceeds;
//   hard: a publication that would grow a chain past this MUST fold first
//         (synchronously, in this PR) so no oversized chain is ever published.
struct patch_chain_limits
{
    std::size_t soft_runs {default_soft_chain_runs};
    std::uint64_t soft_encoded_bytes {default_soft_chain_encoded_bytes};
    std::size_t hard_runs {default_hard_chain_runs};
    std::uint64_t hard_encoded_bytes {default_hard_chain_encoded_bytes};
    // Decoded resident memory of the active runs (actual allocated bytes).
    std::uint64_t hard_decoded_bytes {max_chain_resident_bytes};
};

struct patch_chain_usage
{
    std::size_t runs {};
    std::uint64_t encoded_bytes {};
    std::uint64_t decoded_bytes {};
};

enum class patch_chain_admission : std::uint8_t
{
    // Append the run.
    admit,
    // Append the run, but a fold is due soon.
    admit_and_recommend_fold,
    // Fold the chain together with the pending run into a new checkpoint
    // instead of appending.
    fold_required,
};

// Decides what publishing `pending` (the run about to be appended) on top of
// `current` requires. A run too large for the resident budget on its own also
// lands in fold_required.
[[nodiscard]] constexpr auto classify_chain_growth(patch_chain_limits const& limits,
                                                   patch_chain_usage const& current,
                                                   patch_chain_usage const& pending) noexcept -> patch_chain_admission
{
    auto const runs = current.runs + pending.runs;
    auto const encoded = current.encoded_bytes + pending.encoded_bytes;
    auto const decoded = current.decoded_bytes + pending.decoded_bytes;
    if (runs > limits.hard_runs || encoded > limits.hard_encoded_bytes || decoded > limits.hard_decoded_bytes) {
        return patch_chain_admission::fold_required;
    }
    if (runs > limits.soft_runs || encoded > limits.soft_encoded_bytes) {
        return patch_chain_admission::admit_and_recommend_fold;
    }
    return patch_chain_admission::admit;
}

}  // namespace motif::db
