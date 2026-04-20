---
name: Use DuckDB C API not C++ API
description: DuckDB 1.5.1-dev C++ header is incompatible with Clang 21; use duckdb.h (C API) instead
type: feedback
---

Always use the DuckDB C API (`#include <duckdb.h>`, `duckdb_database`, `duckdb_connection`, `duckdb_query`, `duckdb_appender_*`) instead of the C++ API (`#include <duckdb.hpp>`, `duckdb::DuckDB`, `duckdb::Connection`).

**Why:** DuckDB 1.5.1-dev's `profiling_utils.hpp` has an incomplete-type bug — `unique_ptr<ActiveTimer>` is used before `ActiveTimer` is fully defined. Clang 21 rejects this strictly (GCC is lenient). Marking headers SYSTEM did not help because Clang still instantiates templates. The C API is a pure C header with no templates, so it compiles cleanly.

**How to apply:** Whenever writing or reviewing code in `motif_db` that touches DuckDB, use C API handles and functions. Store `duckdb_database` and `duckdb_connection` as direct members (not unique_ptr) with explicit null-guarded cleanup in destructors and `close()`.
