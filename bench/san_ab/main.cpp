#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <chess.hpp>
#include <chesslib/board/board.hpp>
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/fen.hpp>
#include <chesslib/util/san.hpp>
#include <chesslib/util/san_replay.hpp>

#include "motif/import/pgn_reader.hpp"

namespace
{

static constexpr auto corpus_magic = std::array<char, 8> {'S', 'A', 'N', 'C', 'O', 'R', 'P', '1'};

struct corpus_game
{
    std::optional<std::string> initial_fen;
    std::vector<std::string> sans;
};

struct flat_game_header
{
    std::uint8_t fen_len;
    std::uint32_t num_moves;
};

struct flat_header
{
    char magic[8];
    std::uint32_t num_games;
    std::uint64_t total_moves;
};

struct run_stats
{
    std::size_t games {};
    std::size_t moves {};
    std::size_t failures {};
    std::uint64_t checksum {};
    std::chrono::nanoseconds elapsed {};
};

auto find_tag(std::vector<pgn::tag> const& tags, std::string_view key) -> std::optional<std::string>
{
    for (auto const& tag : tags) {
        if (tag.key == key) {
            return tag.value;
        }
    }
    return std::nullopt;
}

auto load_corpus_from_pgn(std::string const& path, std::size_t max_games) -> std::vector<corpus_game>
{
    auto corpus = std::vector<corpus_game> {};
    motif::import::pgn_reader reader {path};

    while (max_games == 0 || corpus.size() < max_games) {
        auto game_res = reader.next();
        if (!game_res) {
            if (game_res.error() == motif::import::error_code::parse_error) {
                continue;
            }
            break;
        }

        auto game = corpus_game {};
        if (auto fen = find_tag(game_res->tags, "FEN"); fen.has_value()) {
            game.initial_fen = std::move(*fen);
        }

        auto skip_game = false;
        game.sans.reserve(game_res->moves.size());
        for (auto const& node : game_res->moves) {
            if (node.san == "--") {
                skip_game = true;
                break;
            }
            game.sans.push_back(node.san);
        }

        if (!skip_game) {
            corpus.push_back(std::move(game));
        }
    }

    return corpus;
}

auto dump_corpus(std::vector<corpus_game> const& corpus, std::string const& out_path) -> bool
{
    std::uint64_t total_moves = 0;
    for (auto const& g : corpus) {
        total_moves += g.sans.size();
    }

    auto out = std::ofstream {out_path, std::ios::binary};
    if (!out) {
        std::cerr << "Cannot open output: " << out_path << '\n';
        return false;
    }

    auto hdr = flat_header {};
    std::memcpy(hdr.magic, corpus_magic.data(), 8);
    hdr.num_games = static_cast<std::uint32_t>(corpus.size());
    hdr.total_moves = total_moves;
    out.write(reinterpret_cast<char const*>(&hdr), sizeof(hdr));

    for (auto const& game : corpus) {
        auto const fen_len = static_cast<std::uint8_t>(game.initial_fen.has_value() ? game.initial_fen->size() : 0);
        auto const num_moves = static_cast<std::uint32_t>(game.sans.size());

        out.write(reinterpret_cast<char const*>(&fen_len), 1);
        if (fen_len > 0) {
            out.write(game.initial_fen->data(), fen_len);
        }
        out.write(reinterpret_cast<char const*>(&num_moves), 4);

        for (auto const& san : game.sans) {
            auto const san_len = static_cast<std::uint8_t>(san.size());
            out.write(reinterpret_cast<char const*>(&san_len), 1);
            out.write(san.data(), san_len);
        }
    }

    std::cout << "Dumped corpus: " << out_path << " (" << corpus.size() << " games, " << total_moves << " moves)\n";
    return true;
}

auto load_corpus_from_bin(std::string const& path) -> std::optional<std::vector<corpus_game>>
{
    auto in = std::ifstream {path, std::ios::binary | std::ios::ate};
    if (!in) {
        std::cerr << "Cannot open corpus: " << path << '\n';
        return std::nullopt;
    }

    auto const file_size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);

    if (file_size < sizeof(flat_header)) {
        std::cerr << "File too small for corpus header\n";
        return std::nullopt;
    }

    auto hdr = flat_header {};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (std::memcmp(hdr.magic, corpus_magic.data(), 8) != 0) {
        std::cerr << "Bad corpus magic\n";
        return std::nullopt;
    }

    auto corpus = std::vector<corpus_game> {};
    corpus.reserve(hdr.num_games);

    for (std::uint32_t g = 0; g < hdr.num_games; ++g) {
        auto& game = corpus.emplace_back();

        std::uint8_t fen_len = 0;
        in.read(reinterpret_cast<char*>(&fen_len), 1);
        if (fen_len > 0) {
            auto fen = std::string(fen_len, '\0');
            in.read(fen.data(), fen_len);
            game.initial_fen = std::move(fen);
        }

        std::uint32_t num_moves = 0;
        in.read(reinterpret_cast<char*>(&num_moves), 4);
        game.sans.resize(num_moves);

        for (std::uint32_t m = 0; m < num_moves; ++m) {
            std::uint8_t san_len = 0;
            in.read(reinterpret_cast<char*>(&san_len), 1);
            game.sans[m].resize(san_len);
            in.read(game.sans[m].data(), san_len);
        }
    }

    return corpus;
}

auto count_moves(std::vector<corpus_game> const& corpus) -> std::size_t
{
    std::size_t total = 0;
    for (auto const& g : corpus) {
        total += g.sans.size();
    }
    return total;
}

auto run_chesslib(std::vector<corpus_game> const& corpus, std::size_t rounds) -> run_stats
{
    auto stats = run_stats {};
    auto const started = std::chrono::steady_clock::now();

    for (std::size_t round = 0; round < rounds; ++round) {
        for (auto const& game : corpus) {
            auto board = game.initial_fen.has_value() ? chesslib::fen::read_or_throw(*game.initial_fen) : chesslib::board {};

            ++stats.games;
            for (auto const& san : game.sans) {
                auto move_res = chesslib::san::from_string(board, san);
                if (!move_res) {
                    ++stats.failures;
                    break;
                }

                chesslib::move_maker maker {board, *move_res};
                maker.make();
                ++stats.moves;
                stats.checksum ^= board.hash();
            }
        }
    }

    stats.elapsed = std::chrono::steady_clock::now() - started;
    return stats;
}

auto run_chesslib_replay(std::vector<corpus_game> const& corpus, std::size_t rounds) -> run_stats
{
    auto stats = run_stats {};
    auto const started = std::chrono::steady_clock::now();

    for (std::size_t round = 0; round < rounds; ++round) {
        for (auto const& game : corpus) {
            auto board = game.initial_fen.has_value() ? chesslib::fen::read_or_throw(*game.initial_fen) : chesslib::board {};

            chesslib::san::replayer replay {board};

            ++stats.games;
            for (auto const& san : game.sans) {
                auto move_res = replay.play(san);
                if (!move_res) {
                    ++stats.failures;
                    break;
                }
                ++stats.moves;
                stats.checksum ^= board.hash();
            }
        }
    }

    stats.elapsed = std::chrono::steady_clock::now() - started;
    return stats;
}

auto run_chess_library(std::vector<corpus_game> const& corpus, std::size_t rounds) -> run_stats
{
    auto stats = run_stats {};
    auto const started = std::chrono::steady_clock::now();

    for (std::size_t round = 0; round < rounds; ++round) {
        for (auto const& game : corpus) {
            auto board = game.initial_fen.has_value() ? chess::Board {*game.initial_fen} : chess::Board {};

            ++stats.games;
            for (auto const& san : game.sans) {
                try {
                    auto move = chess::uci::parseSan(board, san);
                    board.makeMove(move);
                    ++stats.moves;
                    stats.checksum ^= board.hash();
                } catch (std::exception const&) {
                    ++stats.failures;
                    break;
                }
            }
        }
    }

    stats.elapsed = std::chrono::steady_clock::now() - started;
    return stats;
}

auto verify_final_positions(std::vector<corpus_game> const& corpus) -> std::optional<std::string>
{
    for (std::size_t game_index = 0; game_index < corpus.size(); ++game_index) {
        auto const& game = corpus[game_index];

        auto chesslib_board = game.initial_fen.has_value() ? chesslib::fen::read_or_throw(*game.initial_fen) : chesslib::board {};
        auto chess_library_board = game.initial_fen.has_value() ? chess::Board {*game.initial_fen} : chess::Board {};

        for (std::size_t move_index = 0; move_index < game.sans.size(); ++move_index) {
            auto const& san = game.sans[move_index];

            auto move_res = chesslib::san::from_string(chesslib_board, san);
            if (!move_res) {
                return "chesslib failed at game " + std::to_string(game_index + 1) + ", move " + std::to_string(move_index + 1) + ": "
                    + san;
            }

            chesslib::move_maker maker {chesslib_board, *move_res};
            maker.make();

            try {
                auto move = chess::uci::parseSan(chess_library_board, san);
                chess_library_board.makeMove(move);
            } catch (std::exception const& ex) {
                return "chess-library failed at game " + std::to_string(game_index + 1) + ", move " + std::to_string(move_index + 1) + ": "
                    + san + " (" + ex.what() + ")";
            }
        }

        auto const chesslib_fen = chesslib::fen::write(chesslib_board);
        auto const chess_library_fen = chess_library_board.getFen();
        auto normalize_fen_ignoring_ep = [](std::string const& fen) -> std::string
        {
            auto fields = std::vector<std::string> {};
            auto field = std::string {};
            auto stream = std::istringstream {fen};
            while (stream >> field) {
                fields.push_back(field);
            }

            if (fields.size() >= 4) {
                fields[3] = "-";
            }

            auto normalized = std::string {};
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (i != 0) {
                    normalized += ' ';
                }
                normalized += fields[i];
            }
            return normalized;
        };

        if (normalize_fen_ignoring_ep(chesslib_fen) != normalize_fen_ignoring_ep(chess_library_fen)) {
            return "final FEN mismatch at game " + std::to_string(game_index + 1) + "\nchesslib: " + chesslib_fen
                + "\nchess-library: " + chess_library_fen;
        }
    }

    return std::nullopt;
}

auto ns_per_move(run_stats const& stats) -> double
{
    return stats.moves == 0 ? 0.0 : static_cast<double>(stats.elapsed.count()) / static_cast<double>(stats.moves);
}

auto moves_per_second(run_stats const& stats) -> double
{
    return stats.elapsed.count() == 0 ? 0.0
                                      : (static_cast<double>(stats.moves) * 1'000'000'000.0) / static_cast<double>(stats.elapsed.count());
}

enum class bench_target : std::uint8_t
{
    both,
    chesslib_only,
    chess_library_only,
    replay_only,
};

struct cli_args
{
    std::string input_path;
    std::size_t max_games {0};
    std::size_t rounds {3};
    std::string dump_path;
    bench_target target {bench_target::both};
    bool no_verify {false};
    bool is_bin {false};
};

auto parse_args(int argc, char** argv) -> std::optional<cli_args>
{
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <pgn-path> [--dump corpus.bin] [max-games] [rounds]\n"
                  << "  " << argv[0] << " <corpus.bin> [--only chesslib|chess-library] [rounds]\n"
                  << "\nOptions:\n"
                  << "  --dump <path>      Write flat corpus file from PGN\n"
                  << "  --only <lib>       Run only chesslib or chess-library\n"
                  << "  --no-verify        Skip correctness verification\n";
        return std::nullopt;
    }

    auto args = cli_args {};
    args.input_path = argv[1];
    args.is_bin = args.input_path.ends_with(".bin");

    auto positional = std::vector<std::string> {};

    for (int i = 2; i < argc; ++i) {
        auto const arg = std::string_view {argv[i]};
        if (arg == "--dump" && i + 1 < argc) {
            args.dump_path = argv[++i];
        } else if (arg == "--only" && i + 1 < argc) {
            ++i;
            auto const lib = std::string_view {argv[i]};
            if (lib == "chesslib") {
                args.target = bench_target::chesslib_only;
            } else if (lib == "chess-library") {
                args.target = bench_target::chess_library_only;
            } else if (lib == "replay") {
                args.target = bench_target::replay_only;
            } else {
                std::cerr << "Unknown library: " << argv[i] << '\n';
                return std::nullopt;
            }
        } else if (arg == "--no-verify") {
            args.no_verify = true;
        } else {
            positional.push_back(argv[i]);
        }
    }

    if (args.is_bin) {
        if (!positional.empty()) {
            args.rounds = static_cast<std::size_t>(std::stoull(positional[0]));
        }
    } else {
        if (!positional.empty()) {
            args.max_games = static_cast<std::size_t>(std::stoull(positional[0]));
        }
        if (positional.size() > 1) {
            args.rounds = static_cast<std::size_t>(std::stoull(positional[1]));
        }
    }

    return args;
}

auto print_stats(std::string_view label, run_stats const& stats) -> void
{
    std::cout << label << ": moves=" << stats.moves << " failures=" << stats.failures
              << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(stats.elapsed).count()
              << " ns_per_move=" << ns_per_move(stats) << " moves_per_sec=" << moves_per_second(stats) << " checksum=" << stats.checksum
              << '\n';
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    auto args_opt = parse_args(argc, argv);
    if (!args_opt) {
        return 1;
    }
    auto const& args = *args_opt;

    std::vector<corpus_game> corpus;

    if (args.is_bin) {
        auto loaded = load_corpus_from_bin(args.input_path);
        if (!loaded) {
            return 1;
        }
        corpus = std::move(*loaded);
    } else {
        corpus = load_corpus_from_pgn(args.input_path, args.max_games);

        if (!args.dump_path.empty()) {
            if (!dump_corpus(corpus, args.dump_path)) {
                return 1;
            }
            if (args.rounds == 0) {
                return 0;
            }
        }
    }

    if (corpus.empty()) {
        std::cerr << "No games loaded from corpus\n";
        return 1;
    }

    std::cout << "Loaded corpus: games=" << corpus.size() << " moves=" << count_moves(corpus) << " (from " << (args.is_bin ? "bin" : "pgn")
              << ")\n";

    if (!args.no_verify && args.target != bench_target::chess_library_only) {
        auto const verification_error = verify_final_positions(corpus);
        if (verification_error.has_value()) {
            std::cerr << "Correctness check failed: " << *verification_error << '\n';
            return 2;
        }
        std::cout << "Correctness: OK\n";
    }

    auto chesslib_stats = std::optional<run_stats> {};
    auto chess_library_stats = std::optional<run_stats> {};

    if (args.target == bench_target::both || args.target == bench_target::chesslib_only) {
        chesslib_stats.emplace(run_chesslib(corpus, args.rounds));
        print_stats("chesslib", *chesslib_stats);
    }

    if (args.target == bench_target::replay_only) {
        auto const stats = run_chesslib_replay(corpus, args.rounds);
        print_stats("chesslib-replay", stats);
    }

    if (args.target == bench_target::both || args.target == bench_target::chess_library_only) {
        chess_library_stats.emplace(run_chess_library(corpus, args.rounds));
        print_stats("chess-library", *chess_library_stats);
    }

    if (args.target == bench_target::both) {
        auto const replay_stats = run_chesslib_replay(corpus, args.rounds);
        print_stats("chesslib-replay", replay_stats);
    }

    if (chesslib_stats.has_value() && chess_library_stats.has_value() && chess_library_stats->elapsed.count() != 0) {
        auto const speedup =
            static_cast<double>(chesslib_stats->elapsed.count()) / static_cast<double>(chess_library_stats->elapsed.count());
        std::cout << "relative_speedup_chess_library_vs_chesslib=" << speedup << 'x' << '\n';
    }

    return 0;
}
