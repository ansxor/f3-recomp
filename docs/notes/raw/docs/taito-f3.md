---
source_url: https://github.com/12Me21/taito-f3
ingested: 2026-10-10
sha256:              # n/a: content is pinned by the submodule commit below
git_commit: eaa5a0ab624464f5ae904a6227414d09d75f64ee
game:
rom_sha256:
captured_with: 12Me21/taito-f3 notes (git eaa5a0a), git submodule at raw/docs/taito-f3/
---

12Me21's Taito F3 hardware notes: PCB tracing, chip pinouts, PAL dumps, oscilloscope timing captures
and FDP/FDA hardware tests, plus the draft website under `website/`. Mounted as a git submodule so
the files stay byte-identical to upstream; the files carry no per-file frontmatter. This page is
their provenance record.

Drift check: `git -C docs/notes/raw/docs/taito-f3 rev-parse HEAD` must equal `git_commit`. Moving
the submodule to a new commit is a re-ingest: update `git_commit`, diff the changed files, and
re-check every page that cites them.

Evidence kind: `doc` (see [[SCHEMA]]).
