---
layout: home

hero:
  name: f3-recomp
  text: Taito F3 static recompiler and modern runtime
  tagline: Turn the Land Maker arcade ROM into native C code. Run it with a new runtime. Play 1v1 rollback netplay online.
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
    details: The f3rt library provides memory, interrupts, video, sound and EEPROM. An SDL3 frontend shows the picture. It plays audio and reads the keyboard.
  - title: Game-data video
    details: The runtime can build each frame from the game's own tile and sprite data. It can draw at 1x to 4x scale with extra border columns. The original FDP renderer stays available as a reference.
  - title: Rollback netplay
    details: Two players connect through a Go UDP relay. The game predicts missing input. It restores an earlier state when predictions differ. Checksums detect differences between machines.
  - title: Tested against MAME
    details: Tools compare frames, audio and CPU state with MAME captures and with an independent Musashi 68k core. Seeded gameplay tests run the full machine without interpreter fallback.
  - title: Built to be read
    details: The Developer section explains every subsystem, from the recompiler to the netplay protocol. Each page gives the real function, file and constant names.
---

## What this project is

f3-recomp is a static recompiler for Taito F3 arcade games. It follows the design of N64Recomp. The target game is **Land Maker Japan 2.01J** (`landmakrj`).

The repository does **not** contain any ROM data. You must supply your own legally obtained `landmakr` ROM files. The build reads them on your computer and writes C code into your build directory.

::: warning Scope
Only `landmakrj` is tested. The World set `landmakr` has a config file but is untested. Its program ROM files were not available to the authors.
:::

## Where to go next

| You want to | Read |
| --- | --- |
| Build and run the game | [Getting started](/guide/getting-started) |
| Play with a friend online | [Online play](/guide/netplay) |
| Look up a command-line option | [Command-line reference](/reference/cli) |
| Understand how the code works | [Developer overview](/developer/) |
