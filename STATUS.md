IN PROGRESS: Dear ImGui overlay and versus-only snapshot handoff.

# Scope

- Worktree `wt/imgui-netplay`, branch `imgui-netplay`, based on integration `faf81d9`.
- Contract: `/Users/darien/Workspace/f3-stuff/prompts/imgui-netplay.md`.
- Existing CPU/SDL GPU presentation and strict-native defaults retained. F1 becomes the menu; service moves to F3.
- Independent implementation slices: overlay/settings/input, reliable snapshot transport/relay, presentation-independent synchronization snapshots, and ROM-backed versus lifecycle investigation.
- Parent owns frontend/session integration, real-server oracle, actual UI verification, parity gates, documentation and local commits.

# Decisions

- Keep exact full local snapshots for save-state slots and local rollback. Add geometry-independent synchronization snapshots for handoff and peer checksums.
- Host/join authority is independent of player slot. Host supplies the match snapshot and delay; guest accepts the loaded state before the start barrier releases.
- Solo histories, EEPROM and local presentation settings need not agree before pairing. ROM/build/snapshot representation still must agree.
- Match-relative rollback time starts at zero while the imported machine preserves the host's absolute emulated clocks.

# Pending evidence

Exact ROM-backed versus entry/exit predicates, implementation verification, UI interaction, impaired real-server campaigns and preserved parity gates are not yet complete. No completion claim.
