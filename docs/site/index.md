---
layout: home

hero:
  name: f3-recomp
  text: Land Maker static recompiler
  tagline: Native CPU execution, an SDL3 runtime and optional two-player rollback netplay for Land Maker Japan 2.01J.
  actions:
    - theme: brand
      text: User guide
      link: /guide/
    - theme: alt
      text: Command-line reference
      link: /reference/cli
    - theme: alt
      text: Developer docs
      link: /developer/

features:
  - title: Static recompilation
    details: Python tools read the 68EC020 main program and the 68000 sound program. They write C functions that use the runtime ABI. Normal gameplay runs the main CPU without an instruction interpreter.
  - title: Modern runtime
    details: The f3rt library provides memory, interrupts, video, sound and EEPROM. The SDL3 frontend provides an F1 menu, keyboard/gamepad remapping, offline slots and screenshots.
  - title: Game-data video
    details: The runtime can build each frame from the game's own tile and sprite data. It can draw at 1x to 4x scale with extra border columns. The original FDP renderer stays available as a reference.
  - title: Rollback netplay
    details: Two players connect through a Go UDP relay. A fresh host snapshot starts versus rollback after both clients load. Confirmed exit or disconnect returns local; presentation settings stay independent.
  - title: Reference-based validation
    details: Video and sound devices are derived from MAME. Finite captures and seeded runs compare output with MAME and CPU execution with Musashi; this is not physical-board verification.
  - title: Developer documentation
    details: Explore the runtime, recompiler, netplay protocol and the Land Maker-specific work needed before another F3 game can be supported.
---

## What this project is

f3-recomp statically recompiles the main and sound CPU programs of **Land Maker Japan 2.01J** (`landmakrj`) to C and runs them with a new runtime. Its design is inspired by [N64Recomp](https://github.com/N64Recomp/N64Recomp). Taito F3 is the underlying platform, not a claim of support for its full game library.

The repository does **not** contain any ROM data. You must supply your own legally obtained `landmakr` ROM files. The build reads them on your computer and writes C code into your build directory.

## Game support

| Game | Status |
| --- | --- |
| Land Maker Japan 2.01J (`landmakrj`) | Supported player build; exercised by finite reference captures, seeded gameplay and netplay checks. Coverage is not exhaustive. |
| Land Maker World (`landmakr`) | Config and ROM-loader entry only; untested and rejected by the generated `landmakr` player build. |
| Other Taito F3 games | Not implemented. |

The [portability audit](/developer/porting) distinguishes shared platform code from Land Maker-specific assumptions. For historical validation results and their limits, see [Developer evidence](/developer/evidence).

## Where to go next

| You want to | Read |
| --- | --- |
| Build and run the game | [Getting started](/guide/getting-started) |
| Play with a friend online | [Online play](/guide/netplay) |
| Look up a command-line option | [Command-line reference](/reference/cli) |
| Understand how the code works | [Developer overview](/developer/) |

## Credits and licensing

- [MAME](https://github.com/mamedev/mame): individually BSD-3-Clause video and sound-device algorithms adapted for this runtime; author credits and the pinned revision are in [runtime/LICENSES.txt](https://github.com/ansxor/f3-recomp/blob/main/runtime/LICENSES.txt).
- [Musashi](https://github.com/kstenerud/Musashi): the reference 68k CPU core, with its MIT-style notice and separate SoftFloat Release 2b terms retained in the source.
- [SDL3](https://github.com/libsdl-org/SDL): window, input, audio and GPU presentation; zlib-licensed system dependency.
- [Capstone](https://github.com/capstone-engine/capstone): instruction decoding for recompilation.

The repository has no project-wide license grant. Dependency notices apply to their respective components, not to the entire project or to ROM data. Supply your own legally obtained ROMs; no game assets are distributed.
