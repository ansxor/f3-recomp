# Instruction reference

This page covers the native lowering families in `lower`. Support depends on the operand form, not only the mnemonic.

Sources: [`emitter.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/emitter.py), [`cpu_ops.h`](https://github.com/ansxor/f3-recomp/blob/main/recomp/cpu_ops.h), and [`bitfield.h`](https://github.com/ansxor/f3-recomp/blob/main/recomp/bitfield.h).

## Transfers and register operations

| Family | Native behavior |
| --- | --- |
| NOP | Advance PC and cycles. Preserve flags. |
| MOVE | Capture the source, write the destination, and produce logic flags for ordinary destinations. |
| MOVEA | Sign-extend a word source; replace the address register; preserve flags. |
| MOVEQ | Sign-extend the immediate byte to a long data register; produce logic flags. |
| CLR | Write zero and produce logic flags. It does not emit an ordinary operand read. |
| TST | Read without writing; produce logic flags. |
| EXG | Exchange two address/data registers through a temporary; preserve flags. |
| EXT.W | Sign-extend byte to word; preserve the upper word; produce word logic flags. |
| EXT.L | Sign-extend word to long; produce long logic flags. |
| EXTB | Sign-extend byte to long; produce long logic flags. |
| SWAP | Exchange data-register halves; produce long logic flags. |
| LEA | Write the calculated address to An; preserve flags. |
| PEA | Push the calculated address on the guest stack; preserve flags. |
| MOVEP | Transfer two or four bytes at address offsets 0, 2, 4, and 6. Preserve bus order and flags. |
| MOVES | Perform a privileged memory/register transfer. Sign-extend loads into An; preserve upper bits for partial Dn loads. |

SR, CCR, and USP transfers have separate flush and privilege rules. See [Exceptions and hooks](/developer/recompiler/exceptions-and-hooks).

## MOVEM

Register-to-memory predecrement visits A7–A0, then D7–D0, restricted by the register mask. Other stores visit D0–D7, then A0–A7.

Predecrement keeps the original base register value during transfers. It writes the final address back after the list completes.

Long predecrement stores write the low word first, then the high word. Word loads sign-extend into the complete destination register.

Postincrement loads write the final address back after all loads. This final update takes precedence when the base register appears in the mask.

An empty mask still evaluates the EA where the handler requires it. MOVEM preserves condition codes.

Bytes `48 e7 c0 00` at `0x1000` generate this native predecrement example:

```c
uint32_t movem_addr = cpu->a[7];
movem_addr -= 4u;
f3_write16(cpu, movem_addr + 2u, (uint16_t)cpu->d[1]);
f3_write16(cpu, movem_addr, (uint16_t)(cpu->d[1] >> 16));
movem_addr -= 4u;
f3_write16(cpu, movem_addr + 2u, (uint16_t)cpu->d[0]);
f3_write16(cpu, movem_addr, (uint16_t)(cpu->d[0] >> 16));
cpu->a[7] = movem_addr;
cpu->pc = 0x00001004u;
cpu->cycles += 10u;
```

## Arithmetic and logic

ADD, ADDI, ADDQ, SUB, SUBI, and SUBQ mask ordinary results to the operand width. They set X eagerly and leave NZVC pending.

ADDA and SUBA sign-extend word sources and use full address-register arithmetic. Address-register quick forms also preserve flags.

CMP, CMPI, and CMPM create pending compare flags without writing the result. CMPM completes source and destination postincrements after their reads.

CMPA sign-extends a word source and compares full 32-bit values. Comparisons preserve X.

NEG uses subtraction from zero. NEGX also subtracts X and retains cumulative Z.

ADDX and SUBX include X in the operation. They use eager carry/borrow, overflow, N, and cumulative Z. Byte/word writes preserve upper data-register bits.

AND, ANDI, OR, ORI, EOR, EORI, and NOT produce logic flags. Immediate SR/CCR forms change the selected status bits instead.

## Decimal and packed data

ABCD and SBCD use decimal correction and include X. They support data registers and paired predecrement memory operands.

Their carry updates both C and X. Zero results retain old Z. The implementation also computes N/V to match the reference behavior.

NBCD negates one decimal byte with X. Its no-change decimal case follows the reference flags and write behavior.

PACK and UNPK read the primary opcode to distinguish forms. This avoids Capstone's memory UNPK/SBCD mnemonic ambiguity.

PACK combines two unpacked digits into one byte. UNPK expands one byte into two digits. Both apply the extension-word adjustment and preserve flags.

Memory forms perform separate byte accesses and predecrements. A7 uses a two-byte step for each byte transfer.

## Multiply and divide

Word MULU/MULS multiply two 16-bit operands into a 32-bit result. Their helpers preserve X, set N/Z, and clear V/C.

Long multiply uses a 64-bit host product. A register pair receives high and low halves. A single register receives the low half and reports overflow.

Word DIVU/DIVS divide a 32-bit dividend by a 16-bit divisor. The destination packs remainder in the upper word and quotient in the lower word.

Overflow retains the destination and sets V. Word overflow clears C while preserving undefined N/Z. X remains intact.

Signed word division handles `0x80000000 / -1` explicitly according to MAME's defined edge behavior.

Long division decodes the extension directly. It selects quotient and remainder registers and a 32-bit or 64-bit dividend.

The helper preserves destinations on overflow. Signed 64-bit minimum divided by minus one sets overflow without host undefined division.

Division by zero calls vector 5 with the next PC. The generated code returns immediately when the helper reports an exception.

The emitter accepts `divu` and `divs` mnemonic families. Distinct `divul` or `divsl` decoder mnemonics do not have a separate lowering branch.

## Bits and shifts

BTST, BSET, BCLR, and BCHG use bit numbers modulo 32 for data registers, or modulo eight for memory bytes.

The helper sets Z from the original bit and preserves other flags. Modifying forms then write the new operand.

ASL, ASR, LSL, LSR, ROL, ROR, ROXL, and ROXR use matching `f3_` helpers. Register counts are masked to six bits.

Memory forms use a word operand and count one. Register forms support byte, word, and long data-register destinations.

Helpers handle zero counts, counts at or beyond the width, carry, and ASL overflow. Ordinary rotates preserve X; extend rotates include it in the ring.

## Bitfields

All eight operations call `f3_bitfield`: BFTST, BFEXTU, BFCHG, BFEXTS, BFCLR, BFFFO, BFSET, and BFINS.

Offset and width can be immediate or dynamic register values. Width normalizes to 1–32; an encoded zero means 32.

Register fields wrap around the 32-bit register. Memory offsets use floor division by eight, including negative offsets.

The memory helper reads a one-to-five-byte window. It preserves unaffected bits and uses ordered width-specific writes.

The helper snapshots insert data and the original field before aliased register writes. Extract forms zero-extend or sign-extend the field.

BFFFO writes the offset plus the first set-bit position. A zero field uses the width as that position.

N/Z describe the original field, except BFINS uses inserted data. V/C clear and X remains unchanged.

## Tests, bounds, and atomic-style operations

TAS sets flags from the original byte, then sets bit seven. Data-register upper bits remain intact.

CAS compares memory with Dc. On equality, it writes Du. Otherwise, it replaces the selected low part of Dc with memory.

CAS2 reads two operands before update. Both comparisons must match before writes occur. Flags describe the first failed comparison, or the second comparison.

CAS2.W retains the pinned reference's sign-extension rule for address-bank-selected failure operands. These callbacks do not implement host multiprocessor atomicity.

CHK compares a signed data-register value against a bound. An out-of-range value raises vector 6 and selects N according to negativity.

CMP2 and CHK2 read consecutive lower and upper bounds. They set Z on a boundary and C outside the interval.

The extension chooses address/data register interpretation and whether an outside value raises vector 6.

## Control flow and system families

BRA, BSR, Bcc, DBcc, Scc, JMP, JSR, RTS, RTR, RTD, LINK, and UNLK use guest PC and stack state.

TRAP, TRAPV, TRAPcc, STOP, RESET, MOVEC, RTE, ILLEGAL, BKPT, A-line, and F-line cases have native exception or system handling.

See [Exceptions and hooks](/developer/recompiler/exceptions-and-hooks) for vectors, frames, privilege, and control registers.

Unsupported operand forms return `None`. See [Limits](/developer/recompiler/limits-and-known-issues) before treating a mnemonic list as a complete ISA guarantee.
