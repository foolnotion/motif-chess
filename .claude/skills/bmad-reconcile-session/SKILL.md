---
name: bmad-reconcile-session
description: 'Apply pending session claims to entity frontmatter and surface conflicts. Use when the user says "reconcile session", "apply session", or "run reconciliation".'
---

# Reconcile Session Workflow

**Goal:** Read `pending-reconciliation` session files, apply non-conflicting claims to entity
frontmatter, write conflict files for anything that needs human resolution, and mark sessions
as `committed` or `conflicted`.

**Your Role:** Structural bookkeeper. Apply changes mechanically and exactly. Do not interpret
or rewrite claim values. Surface conflicts; do not resolve them.

## Conventions

- `{project-root}` resolves to the project working directory.
- `{meta}` = `{project-root}/../motif-chess-meta/meta`
- `{artifacts}` = `{project-root}/../motif-chess-meta/bmad-output/implementation-artifacts`
- `{specs}` = `{project-root}/../motif-chess-meta/specs`

## On Activation

### Step 1: Resolve the Workflow Block

Run: `python3 {project-root}/_bmad/scripts/resolve_customization.py --skill {skill-root} --key workflow`

If the script fails, load `{skill-root}/customize.toml` directly.

### Step 2–6: Standard activation (persistent facts → config → greet → append steps).

## Execution

<workflow>

  <step n="1" goal="Discover pending sessions">
    <action>List all files in `{meta}/sessions/` matching `session-*.yaml`</action>
    <action>Read each file and filter for `status: pending-reconciliation`</action>
    <check if="no pending sessions found">
      <output>✅ No pending sessions — nothing to reconcile.</output>
      <action>HALT</action>
    </check>
    <output>Found {{count}} session(s) pending reconciliation:
{{list of session ids and agents}}</output>
  </step>

  <step n="2" goal="Load registry and build slug→file index">
    <action>Read `{meta}/registry.yaml` — maps slugs to UUIDs</action>
    <action>Build a lookup: slug → absolute path of the entity's .md file
      - Stories: `{artifacts}/{slug-without-prefix}.md` using filename pattern matching
      - Specs: `{specs}/NNN-name/spec.md` where NNN matches spec number
    </action>
    <action>For each claim target in all pending sessions, verify the entity file exists.
      If a target slug resolves to no file, mark that claim as `unresolvable` and continue.</action>
  </step>

  <step n="3" goal="Group claims by (target, field) and detect conflicts">
    <action>Collect all claims across all pending sessions into a flat list</action>
    <action>Group by (target slug, field path)</action>
    <action>A conflict exists when two or more claims in the group set different values
      AND come from different sessions</action>
    <action>Claims that set the same value are idempotent — not a conflict</action>
    <action>Partition into:
      - `safe_claims`: one claim per (target, field), or multiple with identical values
      - `conflicts`: multiple claims with differing values on the same (target, field)
    </action>
    <output>
      Safe claims: {{safe_count}}
      Conflicts: {{conflict_count}}
    </output>
  </step>

  <step n="4" goal="Apply safe claims to entity frontmatter">
    <critical>Edit only the YAML frontmatter block (between the opening and closing `---`).
      Never touch the markdown prose body.</critical>
    <critical>For nested fields (e.g. `provenance.modified`), update the correct sub-key only.</critical>
    <critical>For `acceptance_criteria[id=X].status`, find the AC entry with matching id and
      update its status field only.</critical>

    <action>For each safe claim, read the target entity file</action>
    <action>Apply the field update to the frontmatter</action>
    <action>Update `provenance.modified` to `{at: "today", by: "reconciler"}` if not already
      set by one of the safe claims</action>
    <action>Write the file back</action>
    <output>✅ Applied: {{target}}.{{field}} = {{value}}</output>
  </step>

  <step n="5" goal="Write conflict files for unresolved claims">
    <action>For each conflict group, write `{meta}/conflicts/conflict-{date}-{short-uuid}.yaml`:

```yaml
id: conflict-{date}-{short-uuid}
uuid: <generate-fresh-uuid>
type: conflict
status: pending
field: {target}.{field}
versions:
  - value: {value-a}
    source: {session-id-a}
    agent: {agent-a}
    at: "{session-a-ended}"
  - value: {value-b}
    source: {session-id-b}
    agent: {agent-b}
    at: "{session-b-ended}"
resolution: null
resolved_by: null
resolved_at: null
```
    </action>
    <output>⚠️  Conflict written: {meta}/conflicts/conflict-{date}-{short-uuid}.yaml
      Field: {{target}}.{{field}}
      Agents: {{agent-a}} vs {{agent-b}}
      Manual resolution required.</output>
  </step>

  <step n="6" goal="Mark sessions as committed or conflicted">
    <action>For each processed session:
      - If ALL its claims were applied (none conflicted): set `status: committed`, add
        `reconciled_at: "today"`
      - If ANY of its claims produced a conflict: set `status: conflicted`, add
        `reconciled_at: "today"`, add `conflicts: [list of conflict file ids]`
    </action>
    <action>Write the updated session file</action>
  </step>

  <step n="7" goal="Summarize and surface next actions">
    <output>
## Reconciliation Complete

**Sessions processed:** {{total}}
**Claims applied:** {{applied}}
**Conflicts written:** {{conflict_count}}

{{#if conflicts}}
### ⚠️ Conflicts requiring human resolution

{{for each conflict}}
- `{meta}/conflicts/{{id}}` — `{{field}}`
  {{agent-a}} set `{{value-a}}`, {{agent-b}} set `{{value-b}}`
  Resolve by editing the conflict file: fill `resolution`, `resolved_by`, `resolved_at`,
  then apply the winning value to `{{target}}`'s frontmatter and delete the conflict file.
{{/for}}
{{/if}}

{{#if unresolvable_claims}}
### ❓ Unresolvable claims (target file not found)
{{list}}
Check that the slug exists in registry.yaml and the entity file is at the expected path.
{{/if}}
    </output>
  </step>

</workflow>
