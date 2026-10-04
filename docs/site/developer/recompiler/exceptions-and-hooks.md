# Exceptions, privilege and hooks

Native code uses runtime helpers for exceptions and status changes. Guest calls and returns use guest memory, not the host call stack.

Sources: [`emitter.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/emitter.py) and [`generate.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/generate.py).

## Guest control flow and stack

BRA and Bcc calculate the target from instruction address plus two and the signed displacement. BSR pushes the sequential PC before changing PC.

JMP calculates an EA and assigns PC. JSR captures that target before decrementing A7 and pushing the return address. This handles stack-register aliasing.

RTS reads a long PC at A7 and advances A7 by four. RTD also applies its signed word displacement.

RTR first pops a CCR word, then a long PC. It preserves the upper SR byte and clears pending flags.

LINK pushes the selected address register, copies A7 into it, then applies a signed word or long displacement to A7.

UNLK copies the frame register into A7, restores the register from memory, and advances the stack. UNLK A7 avoids a second increment.

DBcc does not decrement when its condition is true. Otherwise, it decrements only the low register word and branches unless the result is `-1`.

Scc writes `0xff` for true or zero for false. It does not change condition codes.

## Exception selection

| Instruction or condition | Vector | Saved PC passed by the emitter |
| --- | --- | --- |
| A-line primary word | 10 | Current instruction |
| F-line primary word | 11 | Current instruction |
| ILLEGAL or BKPT | 4 | Current instruction |
| Privilege failure | 8 | Current instruction |
| Unsupported MOVEC control | 4 | Current instruction |
| Division by zero | 5 | Next instruction |
| CHK or trapping CHK2 | 6 | Next instruction |
| TRAPV or true TRAPcc | 7 | Next instruction |
| TRAP `#n` | `32 + n` | Next instruction |
| Unsupported RTE frame format | 14 | Next instruction |

The primary opcode overrides misleading Capstone mnemonics for A-line and F-line words. The board's EC020 has no coprocessor.

BKPT has no external instruction substitution here. It takes the illegal-instruction exception.

The runtime helper owns frame construction, vector reads, stack selection, and exception timing. Generated code flushes flags before calling it.

## Privilege and SR

The emitter tests SR's supervisor bit `0x2000` before RESET, STOP, MOVES, MOVEC, RTE, and transfers involving SR or USP.

Immediate ANDI, ORI, and EORI to SR also require supervisor mode. CCR-only forms do not require that check.

`f3_set_sr` handles complete SR changes and active-stack changes. Direct CCR writes preserve the upper SR byte and mask the low flags to `0x1f`.

RESET calls `f3_reset_devices`, then advances PC and charges its instruction cost. STOP sets SR, sets `stopped`, advances PC, and returns.

## MOVEC controls

Supported selectors are SFC=`0`, DFC=`1`, CACR=`2`, USP=`0x800`, VBR=`0x801`, CAAR=`0x802`, MSP=`0x803`, and ISP=`0x804`.

The ABI stores ISP in `ssp`. Reads and writes of the active MSP/ISP use A7. The SR master bit selects the active supervisor bank.

SFC and DFC writes mask to three bits. CACR writes mask to four bits. Other listed controls use the register value.

An unknown control selector raises vector 4 after the privilege check. A stored cache-control field does not implement a physical cache.

## RTE formats

RTE reads the format nibble from the word at stack offset six. Formats zero and one occupy eight bytes; format two occupies twelve.

Format one restores SR and continues the loop on the resulting stack. Formats zero and two restore SR and PC, then finish.

Formats greater than two raise format error. Bus-fault and coprocessor frames are outside this runtime exception model.

## Hooks

`[[hooks]]` entries provide `address` and `symbol`. The generator requires a discovered instruction address and a valid C identifier.

Duplicate addresses fail generation. Reused symbols produce one external declaration per symbol in each shard.

Before the instruction label executes, generated code flushes flags and calls `symbol(cpu)`. The hook has the ABI `void symbol(f3_cpu *cpu)`.

If the hook changes PC, stops the CPU, or halts it, the block returns without executing that instruction.

If PC remains at the hook address and execution remains active, the original instruction executes. Hooks are not automatic instruction replacements.

A hook owns its intentional state and cycle changes. The generator does not add a separate hook timing charge.

## Fallback

An unsupported lowering flushes flags and calls `f3_fallback`. A false result halts the CPU. The block always returns afterward.

A decoded unsupported instruction differs from a failed decode. Exhaustive generation can register proven primary-word exceptions for some failed decodes.

A failed valid-primary decode remains a missing entry. Runtime dispatch policy controls the response to missing entries and strict-native execution.

See [Runtime dispatch](/developer/runtime/cpu-abi#f3-dispatch), [CPU ABI](/developer/runtime/cpu-abi), and [Limits](/developer/recompiler/limits-and-known-issues).
