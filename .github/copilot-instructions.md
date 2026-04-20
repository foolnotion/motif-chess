# Copilot Instructions — motif-chess

Read `CONVENTIONS.md` (project root) before writing any code. It covers:

- **Upstream ownership** — chess libs and foolnotion overlay belong to Bogdan; surface packaging
  problems, never work around them with CMake hacks.
- **DuckDB C API only** — `<duckdb.h>` exclusively; the C++ API (`duckdb::DuckDB`, `duckdb::Connection`)
  is incompatible with Clang 21 and banned.
- **`game_store::insert` duplicate policy** — always returns `error_code::duplicate` on conflict;
  callers skip-and-log, never treat as fatal.
- **Naming** — `lower_snake_case` everywhere; no `k_` prefix; `CamelCase` for template params only.
- **SQL** — raw string literals (`R"sql(...)sql"`); never `constexpr char const*`.
- **Error handling** — every public API returns `tl::expected<T, motif::<module>::error_code>`.
- **Module boundaries** — `motif_db/import/search/engine` are Qt-free; import/search access storage
  only through `motif_db` APIs.
- **Packaging** — `flake.nix` requires Bogdan's approval; `vcpkg.json` is end-of-epic only.
- **Read library docs** before grepping headers.

Full details, rationale, and story-done checklist are in `CONVENTIONS.md`.
