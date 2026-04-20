---
name: Use official documentation instead of reading raw source
description: User prefers consulting official docs over reading large raw source files for API lookups
type: feedback
---

When looking up API methods for external libraries (DuckDB, SQLite, etc.), consult official documentation rather than reading raw source/header files.

**Why:** Reading large raw source files consumes excessive context and the user called this out explicitly.

**How to apply:** For DuckDB API lookups, use the DuckDB documentation site. For other libraries, find official docs before falling back to source inspection.
