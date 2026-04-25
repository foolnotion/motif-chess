#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "motif/http/server.hpp"

#include <glaze/json/write.hpp>
#include <httplib.h>
#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/http/error.hpp"
#include "motif/search/position_search.hpp"

// These structs must live in a named namespace — glaze reflection cannot
// resolve types in anonymous namespaces (no external linkage).
namespace motif::http::detail
{

struct health_response
{
    std::string status {"ok"};
};

struct error_response
{
    std::string error;
};

}  // namespace motif::http::detail

namespace motif::http
{

namespace
{

constexpr int http_ok {200};
constexpr int http_bad_request {400};
constexpr int http_internal_error {500};

void set_json_error(httplib::Response& res,
                    int const status,
                    std::string_view const message)
{
    std::string body {};
    [[maybe_unused]] auto const err =
        glz::write_json(detail::error_response {std::string {message}}, body);
    res.set_content(body, "application/json");
    res.status = status;
}

auto parse_size(std::string_view str) -> std::optional<std::size_t>
{
    if (str.empty()) {
        return std::nullopt;
    }
    std::size_t val {};
    auto const [ptr, ec] = std::from_chars(
        str.data(),
        str.data()
            + str.size(),  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        val);
    if (ec != std::errc {}
        || ptr
            != str.data()
                + str.size()) {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return std::nullopt;
    }
    return val;
}

void register_cors(httplib::Server& svr)
{
    svr.set_post_routing_handler(
        [](httplib::Request const& /*req*/, httplib::Response& res) -> void
        {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods",
                           "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
        });

    svr.Options(".*",
                [](httplib::Request const& /*req*/,
                   httplib::Response& res) -> void { res.status = http_ok; });
}

void register_routes(httplib::Server& svr,
                     motif::db::database_manager const& dbmgr)
{
    svr.Get("/health",
            [](httplib::Request const& /*req*/, httplib::Response& res) -> void
            {
                std::string body {};
                [[maybe_unused]] auto const err =
                    glz::write_json(detail::health_response {}, body);
                res.set_content(body, "application/json");
                res.status = http_ok;
            });

    auto const invalid_hash_handler = [](httplib::Request const& /*req*/,
                                         httplib::Response& res) -> void
    { set_json_error(res, http_bad_request, "invalid zobrist hash"); };

    svr.Get("/api/positions", invalid_hash_handler);
    svr.Get("/api/positions/", invalid_hash_handler);

    svr.Get(
        "/api/positions/:zobrist_hash",
        [&dbmgr](httplib::Request const& req, httplib::Response& res) -> void
        {
            auto const& hash_str = req.path_params.at("zobrist_hash");
            std::uint64_t hash_val {};
            {
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                auto const [ptr, ec] =
                    std::from_chars(hash_str.data(),
                                    hash_str.data() + hash_str.size(),
                                    hash_val);
                if (ec != std::errc {}
                    || ptr != hash_str.data() + hash_str.size())
                {
                    set_json_error(
                        res, http_bad_request, "invalid zobrist hash");
                    return;
                }
                // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            }

            constexpr std::size_t default_limit {100};
            constexpr std::size_t max_limit {500};

            auto const limit_str = req.get_param_value("limit");
            auto const offset_str = req.get_param_value("offset");
            auto const has_limit = req.has_param("limit");

            std::size_t limit = default_limit;
            std::size_t offset = 0;

            if (!limit_str.empty()) {
                auto parsed = parse_size(limit_str);
                if (!parsed) {
                    set_json_error(
                        res, http_bad_request, "invalid pagination parameters");
                    return;
                }
                limit = std::min(*parsed, max_limit);
            }
            if (!offset_str.empty()) {
                auto parsed = parse_size(offset_str);
                if (!parsed) {
                    set_json_error(
                        res, http_bad_request, "invalid pagination parameters");
                    return;
                }
                offset = *parsed;
            }

            if (has_limit && limit == 0) {
                res.set_content("[]", "application/json");
                res.status = http_ok;
                return;
            }

            auto matches = motif::search::position_search::find(
                dbmgr, hash_val, limit, offset);
            if (!matches) {
                set_json_error(res, http_internal_error, "search failed");
                return;
            }

            std::string body {};
            [[maybe_unused]] auto const err = glz::write_json(*matches, body);
            res.set_content(body, "application/json");
            res.status = http_ok;
        });
}

}  // namespace

struct server::impl
{
    httplib::Server svr;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::database_manager& database;

    explicit impl(motif::db::database_manager& mgr)
        : database {mgr}
    {
        register_cors(svr);
        register_routes(svr, database);
    }
};

server::server(motif::db::database_manager& database)
    : impl_ {std::make_unique<impl>(database)}
{
}

server::~server() = default;

auto server::start(std::uint16_t port) -> result<void>
{
    if (!impl_->svr.listen("0.0.0.0", port)) {
        return tl::unexpected {error_code::listen_failed};
    }
    return {};
}

auto server::stop() -> void
{
    impl_->svr.stop();
}

auto server::is_running() const -> bool
{
    return impl_->svr.is_running();
}

}  // namespace motif::http
