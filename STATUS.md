IN PROGRESS

# HLE audio — independent threaded sequencer and sample mixer

Task: `/Users/darien/Workspace/f3-stuff/prompts/hle-audio.md`.
Worktree `wt/hle-audio`, branch `hle-audio`, created from integration `faf81d9`.
Only this worktree is modified. Local commits use `ansxor <git@ansxor.ca>`; no pushes or ROM/assets in commits.

## Required invariants

- Existing emulated/native accurate sound remains the default and oracle. HLE is opt-in.
- HLE does not execute the sound 68000 or ES5505/ES5510 programs; it decodes music/SFX data and mixes sample ROM directly at a modern rate.
- Audio engine runs on its own thread and is never restored by game rollback. Main-side command accounting remains deterministic and rollback-able.
- Corrected input reconciles commands with small frame leeway; missing speculative SFX receive fast-fade cancellation. Music is not rewound.
- Strict native main execution and peer state synchronization must remain intact.

## Current phase

Phase 1: protocol/data/effects evidence. Existing documentation establishes a 1024-byte command ring inside 0x800 bytes of main-visible shared RAM, not an 8 KiB command buffer. Existing 0x8f is note release by sequence/track/key; its exact suitability for per-instance rollback cancellation is being verified. Integration now includes netplay snapshots, confirmed-only accurate audio and full-coverage tiered native code generation; those contracts are being inspected before integration changes.

Build environment: `PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages`.
No HLE implementation or acceptance result is claimed yet.
