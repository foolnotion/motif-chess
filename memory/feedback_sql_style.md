---
name: SQL must be raw string literals, not constexpr char const*
description: SQL strings must use C++ raw string literals (R"sql(...)sql") consistently, never constexpr char const*
type: feedback
---

All SQL strings in the codebase must be written as C++ raw string literals, e.g.:

```cpp
auto const query = R"sql(
    SELECT id FROM game ORDER BY id
)sql";
```

Never as `constexpr char const* query = "..."` or similar.

**Why:** Story 1.3–1.5 produced inconsistent SQL style — multiline raw strings in some files, `constexpr char const*` in others. Bogdan flagged this as an annoying style inconsistency. Raw strings are more readable for multi-line SQL and should be used uniformly.

**How to apply:** Any time you write SQL in a `.cpp` file, use raw string literals. If you see `constexpr char const*` for SQL in existing code, normalize it on touch.
