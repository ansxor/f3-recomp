---
source_url: https://github.com/12Me21/taito-f3
ingested: 2026-10-09
sha256:              # n/a: content is pinned by the submodule commit below
git_commit: a070cbd0a79d8b504e1a33938172ec1f0be48ca7
game:
rom_sha256:
captured_with: 12Me21/taito-f3 notes (git a070cbd), git submodule at raw/docs/taito-f3/
---

12Me21's Taito F3 hardware notes: PCB tracing, chip pinouts, PAL dumps, oscilloscope timing captures
and FDP/FDA hardware tests, plus the draft website under `website/`. Mounted as a git submodule so
the files stay byte-identical to upstream; the files carry no per-file frontmatter. This page is
their provenance record.

Drift check: `git -C docs/notes/raw/docs/taito-f3 rev-parse HEAD` must equal `git_commit`. Moving
the submodule to a new commit is a re-ingest: update `git_commit`, diff the changed files, and
re-check every page that cites them.

Evidence kind: `doc` (see [[SCHEMA]]).
