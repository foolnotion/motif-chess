---
name: CONVENTIONS.md is the single source of truth for project rules
description: A CONVENTIONS.md file needs to be created; it will be referenced by CLAUDE.md and .github/copilot-instructions.md for multi-tool retention
type: project
---

The project uses Claude, GitHub Copilot, and Copilot in OpenCode. Durable project rules must be in a single file (`CONVENTIONS.md`) that all three tools can read, rather than scattered across tool-specific configs.

**Why:** During Epic 1, conventions communicated verbally (upstream ownership, naming, SQL style) were not retained across agent sessions because they weren't written to any durable, always-loaded file. CLAUDE.md alone doesn't cover Copilot tools.

**How to apply:** 
- `CONVENTIONS.md` at repo root is the authoritative source — write conventions there
- `CLAUDE.md` should reference or incorporate it
- `.github/copilot-instructions.md` should reference or incorporate it
- This file does not yet exist as of 2026-04-18 — it is an Epic 1 retrospective action item for Bogdan to create
