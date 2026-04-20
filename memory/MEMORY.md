# Memory Index

- [Use DuckDB C API not C++ API](feedback_duckdb_c_api.md) — DuckDB 1.5.1-dev C++ header breaks Clang 21; always use duckdb.h C API
- [Use official documentation instead of reading raw source](feedback_use_official_docs.md) — consult official docs for API lookups, not raw source files
- [No k_ prefix for constants](feedback_no_k_prefix.md) — use plain descriptive names, not k_ddl/k_foo Hungarian style
- [Upstream ownership — surface issues, don't work around them](feedback_upstream_ownership.md) — Bogdan controls chess libs and foolnotion overlay; stop and ask, never invent workarounds
- [SQL must be raw string literals](feedback_sql_style.md) — use R"sql(...)sql", never constexpr char const* for SQL strings
- [vcpkg entries are end-of-epic alignment only](feedback_vcpkg_deferred.md) — don't add vcpkg.json mid-story while Nix issues are in flight
- [CONVENTIONS.md is the single source of truth](project_conventions_file.md) — multi-tool project; conventions must land in CONVENTIONS.md, not just CLAUDE.md
