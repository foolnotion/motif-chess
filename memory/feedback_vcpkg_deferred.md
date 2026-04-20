---
name: vcpkg entries are end-of-epic alignment, not mid-story obligation
description: Don't add vcpkg.json entries mid-story while Nix packaging is in flux; defer to end of epic
type: feedback
---

Do not add `vcpkg.json` dependency entries mid-story when Nix packaging issues are already being worked. vcpkg alignment is end-of-epic cleanup, not a mid-story obligation.

**Why:** During Epic 1, agents kept adding vcpkg entries mid-story while simultaneously firefighting Nix issues. Mixing both packaging systems' concerns at once added noise and confusion. Bogdan had to explicitly push back on this.

**How to apply:** When implementing a story, focus on making the Nix build work. Note any vcpkg.json gaps but defer adding them until the story is otherwise complete, or batch them at epic end. Never block a story on vcpkg alignment unless the story is specifically about cross-platform packaging.
