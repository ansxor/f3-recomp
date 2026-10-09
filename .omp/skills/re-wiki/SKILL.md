---
name: re-wiki
description: "LLM Wiki for reverse-engineering one system and its games: interlinked markdown notes on hardware, routines and emulation quirks."
version: 2.1.0-re
author: Hermes Agent (adapted for RE notes)
license: MIT
platforms: [linux, macos, windows]
metadata:
  hermes:
    tags: [wiki, knowledge-base, reverse-engineering, emulation, markdown]
    category: research
    related_skills: [obsidian]
---

# RE Wiki

Karpathy's LLM Wiki pattern, applied to RE notes for one system and its games.
The human curates sources and confirms findings; the agent files, cross-links, and proposes hypotheses.
Ground truth is the hardware: every claim points to evidence.

## Location
`./docs/notes`

## Layout
```
SCHEMA.md      conventions, system, games, tag taxonomy
index.md       one line per page: [[page]] (status) — summary
log.md         append-only: ## [YYYY-MM-DD] action | subject
raw/           IMMUTABLE: traces/ disasm/ tests/ emu-source/ docs/ assets/
hardware/      one page per component
games/         one page per game: ROM set, routine index, quirks relied on
routines/      <game>-<addr>-<name>.md, address-anchored code
quirks/        one falsifiable behaviour per page
comparisons/   e.g. hardware vs reference emulator
queries/       answers worth keeping
```

## Every session: orient first
Read `SCHEMA.md`, `index.md`, last ~30 lines of `log.md`. On large wikis, grep the topic and addresses before creating anything.

## SCHEMA.md (write on init, adapt to system)
```markdown
## System
[name, CPU(s), video/sound hardware]

## Games
[game-slug — ROM set/revision — rom sha256]

## Page frontmatter
---
title:
created: YYYY-MM-DD
updated: YYYY-MM-DD
type: hardware | game | routine | quirk | comparison | query
tags: []
sources: [raw/...]
games: []
addresses: []            # hex, full bus width, e.g. 0x0001F2A0
status: hypothesis | confirmed | refuted
evidence:                # required unless hypothesis
  - {kind: test|trace|emu-source|doc, ref: raw/..., note: ...}
contradictions: []
supersedes: []
---

## raw/ frontmatter
---
source_url:
ingested: YYYY-MM-DD
sha256:          # of body below frontmatter
game:
rom_sha256:
captured_with:   # tool + version, or "hardware"
---

## Tags
[10-20 tags, e.g. cpu, video, sprite, tilemap, palette, audio, io, dma, interrupt,
timing, vblank, raster, game-loop, object-system, rng, emulator-bug, undocumented]
Add a tag here before using it.
```

## Rules
- Never edit `raw/`.
- Every factual line ends with `^[raw/path]`. Unsourced = phrased as hypothesis.
- New claims are `hypothesis`. `refuted` needs test/trace evidence. `confirmed` needs test/trace evidence **and** the user's say-so or a passing test. Never self-confirm; docs agreeing ≠ confirmation.
- Conflicts: rank evidence (hardware test > trace > emu-source > doc > hearsay), not dates. Same level → keep both, set `contradictions`, flag for user.
- Refuted pages stay in place and in the index. `_archive/` is only for duplicates/out of scope.
- Each page links ≥2 others. Tags only from taxonomy. Split pages over ~200 lines.
- Page threshold: one trace or test is enough to create a page; skip passing mentions.
- Every action updates `index.md` and `log.md`. Rotate log at 500 entries to `log-YYYY.md`.

## Ingest
1. Save source to the right `raw/` subdir with raw frontmatter. Ask if ROM set or capture tool is unknown. On re-ingest, compare sha256: skip if same, flag drift if not.
2. Grep existing pages by name **and address**, including refuted ones.
3. Create/update pages per Rules; bump `updated`; add backlinks.
4. Update index and log (list every file touched).

## Query
Read index → grep names/addresses → read pages → answer citing [[pages]] and the status of each claim relied on. File non-trivial answers to `queries/` or `comparisons/`. Log it.

## Lint
Report by severity, then log `lint | N issues`:
1. confirmed/refuted without test/trace evidence; evidence refs missing from `raw/`
2. broken wikilinks
3. orphan pages; pages missing from index or with mismatched status
4. raw sha256 drift; `rom_sha256` not in SCHEMA games list
5. conflicting `addresses` across pages; filename address ≠ frontmatter
6. contested pages
7. open hypotheses, oldest first
8. lines missing provenance, unknown tags, pages >200 lines, log >500 entries

## Obsidian
The wiki is a vault as-is. Set attachment folder to `raw/assets/`. Dataview example:
`TABLE status, addresses FROM "quirks" WHERE status = "hypothesis"`
