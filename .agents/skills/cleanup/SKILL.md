---
name: cleanup
description: "Clean up code changes so they follow every project rule in this skill's references/: go through the rules one by one, find violations in the diff (working tree, a commit range, a branch or a PR), fix them in place, verify the build and tests, and report what changed. Use before committing or shipping, or when asked to clean up changes or bring them in line with the rules."
---

# cleanup: bring the diff in line with the project rules

Each file in `references/` next to this skill is one rule. They are the source of truth: read them
fresh every run, and do not work from memory.

## 1. Pick the change set

Argument given → use it: a commit/range (`HEAD~3..HEAD`), a branch (`main...<branch>`), a PR number
(`gh pr diff <n>`), or paths. No argument → everything not yet on `main`:

```bash
git diff --merge-base main            # committed + staged + unstaged vs where the branch left main
git status --porcelain                # untracked files count too: read them whole
git diff --merge-base main -M --name-status   # renames/moves (R…) and new files (A…)
```

If that is empty, say so and stop. Work from added/changed lines, plus enough surrounding context to
judge them (`git diff -U20`, or read the file). Changed code includes file names and moves, comments,
docs, and commit/PR text, not only code lines.

## 2. Check and fix each rule, one at a time

`ls .agents/skills/cleanup/references/*.md`, then take the rules in file order. For each one: read
it in full, scan the whole change set for violations, fix them, then move on to the next. Do not
batch rules into one skim. Before deciding, look beyond the diff where the rule needs it: sibling
files, other uses of a value (`git grep`), the existing pattern the rule points to.

Apply every fix you are sure of directly, without asking, the way the rule describes. A fix is
complete only with its follow-through: every reference, include, build entry and use updated, and
nothing left behind at the old location.

Limits:
- Only touch code the change introduced or modified, plus what a fix needs. Leave pre-existing
  violations alone and list them as pre-existing.
- If it is unclear whether something violates a rule, read more code first. If it stays unclear,
  leave it and report it as UNCLEAR with the question you could not settle.
- Leave a fix out and report it as NOT FIXED when it needs a design choice the user has to make, or
  a refactor that reaches far beyond the change. Do not invent a name or meaning you cannot justify
  from the code.
- Do not commit, stage, or push.

## 3. Verify

1. Repeat step 2 on the new diff. It must find no new violations; fix any the first round
   introduced.
2. Build and test what you touched. Build trees live in `build/<name>/` (e.g. `build/landmakrj`,
   `build/verify_all`); use an existing one that covers the changed code:
   `cmake --build build/<name> --target <targets>`, and if tests or test-adjacent code changed,
   `ctest --test-dir build/<name> -R '^runtime-'`. For Python changes, run the relevant
   `uv run pytest`. If a fix broke the build or a test, repair it; if it was failing before your
   changes, say so.

## 4. Report

One section per rule, in file order:

```
### <rule name> — CLEAN | FIXED (n) | UNCLEAR (n) | NOT FIXED (n) | N/A
- path/to/file.cpp:123 — what broke the rule → what you changed
```

N/A gets a few words on why (e.g. "no C/C++ changes"). NOT FIXED and UNCLEAR items say why and what
the user needs to decide. Then list pre-existing violations you left alone, the build/test commands
you ran with their result, and a one-line total (`4 fixed, 1 unclear across 5 rules`).
