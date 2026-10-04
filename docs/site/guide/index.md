# What is f3-recomp?

This guide explains the project and its limits. Use the page list below to find your task.

## What the project does

f3-recomp runs the Taito F3 arcade game **Land Maker** on a modern computer. It does this in two steps:

1. **Recompile.** A Python tool reads the game ROM files. It finds the 68EC020 instructions of the main CPU. It writes them as C functions. A second tool does the same for the 68000 sound CPU.
2. **Run.** The C++ runtime library `f3rt` provides memory, interrupts, video, sound, input and EEPROM. The SDL3 program `landmakr` links it with generated C code. It opens the game window.

The design follows N64Recomp. The recompiler and the runtime meet at one small interface, the runtime ABI. (ABI means application binary interface.)

```mermaid
flowchart LR
    ROM["Your ROM files"] --> RECOMP["Python recompiler"]
    RECOMP --> GEN["Generated C code"]
    GEN --> EXE["landmakr program"]
    RT["f3rt runtime library"] --> EXE
    EXE --> OUT["Window, sound, keyboard"]
```

## What the project does not do

- It does **not** include ROM files. No game data is in the repository. You must supply your own legally obtained `landmakr` ROM set.
- It does **not** support other F3 games. The only tested game is Land Maker Japan 2.01J (`landmakrj`).
- The World set (`landmakr`) has a config file in `games/landmakr/config.toml`. It is **untested**. Its program ROM files (`e61-19.20` to `e61-16.17`) were not available to the authors. The `landmakr` program rejects any set other than `landmakrj`.
- The relay server does not authenticate players and does not encrypt traffic. See [Online play](/guide/netplay).
- The authors tested the build on macOS with Apple silicon. The netplay client uses POSIX sockets. Windows is not a target of the project.

::: info Status
The build command is smoke-tested. Whether the game is fully playable frame by frame and sound sample by sample is tracked in `STATUS.md` in the repository. A successful build does not prove this. See the [STATUS.md file](https://github.com/ansxor/f3-recomp/blob/main/STATUS.md).
:::

## Pages in this guide

| Page | Task |
| --- | --- |
| [Getting started](/guide/getting-started) | Install tools, prepare ROM files, build and start the game. |
| [Controls and options](/guide/running) | Keys, EEPROM settings, headless runs, finite runs, output files. |
| [Video and presentation](/guide/video) | Choose the renderer. Set scale, border and filter. |
| [Sound](/guide/sound) | Choose the sound driver. Write WAV files and sound traces. |
| [Online play](/guide/netplay) | Start the relay server. Play a 1v1 match. |
| [Troubleshooting](/guide/troubleshooting) | Find the cause of an error message. |

For a complete list of command-line options, read the [command-line reference](/reference/cli). To learn how the code works, read the [developer documentation](/developer/).
