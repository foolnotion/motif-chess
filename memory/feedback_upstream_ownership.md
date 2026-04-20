---
name: Upstream ownership — surface issues, don't work around them
description: Bogdan controls chess libs and foolnotion overlay; agents must not implement packaging workarounds
type: feedback
---

When a packaging or CMake issue is traced to an upstream library (chesslib, any foolnotion overlay package), stop and surface the issue to Bogdan — do NOT implement a downstream workaround.

**Why:** Bogdan controls the upstream repositories and the foolnotion Nix overlay. He can fix issues at the source. Downstream workarounds waste effort, can make things worse, and obscure the real problem. He communicated this multiple times during Epic 1 and agents still invented workarounds.

**How to apply:** If a `find_package`, exported target, or CMake config issue traces back to an upstream library, describe the problem clearly and wait for Bogdan to fix it upstream. Do not add `find_dependency` workarounds, manual `target_link_libraries` hacks, or local patches to the package config.
