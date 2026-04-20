---
name: No k_ prefix for constants
description: User dislikes k_ prefix on constants — use plain descriptive names
type: feedback
---

Do not use `k_` prefix for named constants (e.g. `k_ddl`, `k_create_position`). Use plain descriptive names instead (`ddl`, `create_position`).

**Why:** User finds the `k_` Hungarian-style prefix annoying and meaningless.

**How to apply:** Any time declaring a `constexpr` or `const` variable, skip the `k_` prefix entirely.
