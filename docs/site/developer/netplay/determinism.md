# Determinism rules

**What you will learn.** This page explains what "deterministic" means for this project. You see which parts of the code must stay repeatable, how the project proves it, and what to do when you change the emulator.

## Definition

The emulator is deterministic when one rule is true. Two machines start from the same state. They get the same input words for each frame. After the same number of frames, their complete state is byte-for-byte equal.

Rollback netcode depends on this rule in two ways.

- Both clients run the same frames with the same inputs. They must reach the same state without sending state to each other.
- A resimulation after a rollback must give the same result as the first run would give with the correct inputs. The first run and the resimulation use the same code.

If the rule fails, the clients drift apart. The periodic checksum then reports a desync.

## Sources of non-determinism and the project answer

The table lists the usual sources. The third column shows how the code handles each source.

| Source | Risk | How the project handles it |
| --- | --- | --- |
| Host wall clock | The game would run differently on fast and slow computers. | Simulation scheduling uses integer machine clocks: `Machine::hardware_cycles`, `Machine::next_vblank` and `f3_cpu::cycles`. The source review recorded in NETPLAY.md finds no host clock or random-source sampling in machine, renderer or audio stepping. |
| Host random numbers | Each client would get different values. | Simulation code never calls a host random function. The random numbers of the game are part of the game RAM and are saved in the snapshot. |
| Input timing | A key press between two frames could change a frame in one client only. | The frontend turns key events into a 16-bit `InputWord`. `apply_inputs` writes that word to the machine once at the start of each frame. The machine input state is a pure function of the two words. |
| CPU interpreter | The interpreter and the recompiled code may differ in rare cases. | Netplay requires the strict native main CPU. `Rollback` refuses to start when `allow_main_fallback` or `fallback_instructions` is set. It throws an error when a fallback happens during a frame. |
| Uninitialized memory | A random byte in a saved record would change the checksum. | Snapshot records are 1-byte packed. Save visitors zero-initialize records or explicitly initialize all their fields. |
| Host pointers | A pointer differs for each process. | Snapshots hold no pointer. The Musashi sound CPU callbacks are rebound when a snapshot loads. |
| Diagnostic counters | Counters grow during a resimulation. | Counters such as `native_blocks`, `fallback_instructions` and the video fallback counters are not part of the state. |
| Persistent EEPROM | Two players could have different saved settings. | Netplay always starts with an erased 93C46 image (64 words of `0xffff`). The frontend rejects `--eeprom`. The handshake compares the EEPROM CRC. |
| Different ROMs | The games are different. | The handshake compares seven ROM region CRCs. |
| Different build or compiler | Machine code and floating point behavior can differ. | The handshake compares a SHA-256 build hash. See [Build identity](/developer/netplay/build-identity). |
| Network jitter and loss | Inputs arrive at different times. | Time of arrival changes only the schedule of predictions and rollbacks. It does not change the final state, because confirmed frames use only actual inputs. |

::: info
The audio mixer stores some values as `float`: `volume_gain`, `otis_gain`, `output_gain` and the queued PCM samples. Floating-point results can differ between compilers, CPU types and flags. This is one reason why the handshake accepts only an identical build. NETPLAY.md says the build check is "deliberately conservative": it does not claim cross-platform determinism.
:::

## What the simulation reads

The simulation reads three kinds of data only.

1. The ROM data. It never changes after load.
2. Mutable machine state. Snapshot visitors retain state that affects future simulation. Immutable configuration and derived caches remain in the configured machine.
3. The two input words for the current frame.

Host time appears only in code outside the simulation. The next list shows where.

- The client nonce uses `std::random_device` in the transport.
- The relay impairment simulator uses a seeded pseudo-random generator.
- Round-trip time, send intervals and timeouts use `std::chrono::steady_clock`.
- Frame pacing in the frontend uses `std::chrono::steady_clock`.
- The window title shows live values.

None of these values enter the machine.

## How the project proves determinism

The oracle tool has a snapshot mode. It does the following for each test point `N` and each depth `K`.

1. Run to frame `N`. Save a snapshot.
2. Run `K` more frames with a fixed input schedule. Record the final CRC, RAM, palette, graphics RAM, control RAM, shared RAM, framebuffer, PCM audio and a full sound trace.
3. Load the snapshot. Check that the CRC equals the CRC from step 1.
4. Run the same `K` frames again. In the middle of the replay, sleep for 1 ms to disturb host timing.
5. Compare every recorded output byte for byte.

The tool counts scalar `operator new(size_t)` calls during measured save/load calls. It stops if the count is not zero. This check covers the implementation's allocation contract, not every possible C allocator or the whole replay process.

The second proof is end-to-end. The oracle runs a single-machine **reference** with the same delayed inputs. It then runs two real client processes through the relay, including packet loss and reordering. Final state CRC, framebuffer CRC, confirmed audio CRC and stereo-frame count must match the reference and both clients. CRC equality is evidence, not a collision-free proof for all possible states. See [Oracle and verification](/developer/netplay/oracle).

## Rules for contributors

Follow these rules when you change the runtime.

1. **Add all new machine state to the snapshot.** State that is not in the snapshot keeps its value during a rollback. The resimulation then starts from a wrong state. See the checklist in [Snapshots](/developer/netplay/snapshots).
2. **Do not read host time, host random numbers or the environment in simulation code.**
3. **Do not add a pointer, a padded field or an uninitialized field to a `Canonical*` record.** Use fixed-width integers and value-initialize the record.
4. **Do not count work in a field that the snapshot saves.** Keep diagnostic counters out of the state.
5. **Do not emit host output during a netplay resimulation.** Device rendering and mixing must still run. Keep SDL presentation and WAV writes outside replay. The separate snapshot proof can record both passes into separate diagnostic traces.
6. **Keep the build fingerprint accurate.** If you add a source folder that affects the simulation, add it to the file patterns in `CMakeLists.txt`. See [Build identity](/developer/netplay/build-identity).
7. **Run the oracle snapshot suite** after any change to a device or a renderer. See [Oracle and verification](/developer/netplay/oracle).

## Related pages

- [Snapshots](/developer/netplay/snapshots)
- [Rollback engine](/developer/netplay/rollback)
- [Debugging a desync](/developer/netplay/debugging)
- [Machine, memory and scheduling](/developer/runtime/machine)

## Source

- [Canonical state and rollback](https://github.com/ansxor/f3-recomp/blob/main/runtime/netplay.cpp).
- [Snapshot proof](https://github.com/ansxor/f3-recomp/blob/main/tools/netplay_oracle.cpp).
- [Determinism design and recorded review](https://github.com/ansxor/f3-recomp/blob/main/docs/NETPLAY.md).

