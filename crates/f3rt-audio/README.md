# f3rt Audio System

The Taito F3 uses a SQ-1 Plus (32-voice) design.

The purpose of this document is to explain how the HLE audio system works that is
included with the f3-recomp runtime.

The HLE takes learnings from the SQ-1 RTOS code present with the audiocpu sections from Taito F3 ROMs, and represents concepts at a high level so that any technical reader can understand the model like an audio pipeline rather than current approaches in MAME + other emulators where it instead emulates the m68k => ES5505 => ES5510 in tandem to generate audio.

While the HLE is designed by the author, answer, after reading through the LLM's explanation of how it works, more concrete and in-depth details about the hardware+decompilation itself are located in [[docs/notes]].

## Pipeline
