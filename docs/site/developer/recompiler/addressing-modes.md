# Addressing modes

Effective-address decoding controls operand reads, writes, and register side effects. It also supports addresses used without reading their contents.

Source: [`recomp/emitter.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/emitter.py), functions `_decode_ea`, `_full_ea`, `_pc_base`, and `_gen_write`.

## Operand description

`_EA` holds register and immediate metadata, address setup, a memory address expression, read statements, and post-access steps. `val_expr` names the operand value.

The emitter uses `f3_read8`, `f3_read16`, and `f3_read32` for memory. Writes use the matching callbacks. Generated code does not dereference guest addresses.

| Form | Lowering rule |
| --- | --- |
| `Dn` | Read the requested low byte or word, or the full long value. |
| `An` | Read the low word for word operands, otherwise the full register. |
| `CCR`, `SR`, `USP` | Use their ABI fields; instruction handlers supply privilege and flag checks. |
| Immediate | Mask the literal to the operand width. |
| `(An)` | Use the address register directly. |
| `(An)+` | Save the old address, then increment after the required access. |
| `-(An)` | Decrement before the access. |
| `d16(An)` | Add the displacement to the base register. |
| `d8(An,Xn)` | Add the base, signed displacement, and scaled index. |
| `d16(PC)` | Add the displacement to the EA extension-word address. |
| `d8(PC,Xn)` | Add that PC base, displacement, and index. |
| Absolute word | Sign-extend the 16-bit address to 32 bits. |
| Absolute long | Use the 32-bit address literal. |
| Full indexed | Decode base/outer displacement widths and suppression bits from raw bytes. |

A byte step through A7 is two bytes. Other steps equal the operand size.

Data-register byte and word writes preserve the upper bits. Address-register word writes sign-extend to the full register.

`LEA`, `PEA`, `JMP`, and `JSR` use the address expression, not its operand read statements. Memory-indirect address calculation can still require a pointer read.

## Index values

The index can be D0–D7 or A0–A7. A word index sign-extends its low 16 bits. A long index uses all 32 bits.

Scale factors are 1, 2, 4, or 8. Address addition and scaling wrap modulo 2^32.

## Full extensions

Capstone can report unsigned word displacements. `_full_ea` therefore reads the extension and displacement bytes directly.

Let `B` be the base, `BD` the signed base displacement, `I` the scaled index, and `OD` the signed outer displacement.

| Full-extension form | Address expression |
| --- | --- |
| No indirection | `B + BD + I` |
| Preindexed | `f3_read32(cpu, B + BD + I) + OD` |
| Postindexed | `f3_read32(cpu, B + BD) + I + OD` |

Base suppression sets `B` to zero. Index suppression sets `I` to zero. Displacements can be null, signed word, or signed long.

The decoder rejects reserved bit 3, reserved base-displacement size zero, and indirect selector four. It also rejects missing extension bytes.

Capstone sometimes labels a brief zero-displacement extension as a base-displacement form. `_full_ea` checks bit 8 and handles that brief form.

Full-format PC-relative operands can appear as address-register indexed operands with `base_reg=PC`. `_base_expr` recognizes PC explicitly. It never creates address register A16.

### Observed preindexed example

Synthetic bytes `20 30 19 22 ff fc 00 04` at `0x1000` decode as a preindexed long MOVE.

Capstone displays the base word as `$fffc`. The emitted address correctly treats it as minus four:

```c
uint32_t ea_src = f3_read32(cpu, cpu->a[0] + 0xfffffffcu + (cpu->d[1] * 1u)) + 0x4u;
uint32_t val_src = f3_read32(cpu, ea_src);
uint32_t move_value = val_src;
cpu->d[0] = (move_value);
cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = move_value; cpu->cc_width = 4;
cpu->pc = 0x00001008u;
cpu->cycles += 18u;
```

The first read fetches the indirect pointer. The second read fetches the operand. These are distinct guest bus accesses.

## Extension position

The PC-relative base is the EA extension address, not the final instruction address.

`_pc_base` skips a prefix word for bitfields, `CMP2`, `CHK2`, `CAS`, `MOVES`, `MOVEM`, and long multiply/divide forms.

It also skips the immediate data for immediate arithmetic and logic, and the bit number for immediate bit operations.

A `MOVE` destination extension follows the source extension. `_ea_extension_bytes` computes the source span before `_full_ea` reads the destination.

## Access order

The `src`, `tst`, `cmp_dst`, and `btst` roles move postincrement into read statements. Other destinations keep it after their write.

`post_inc_on_read=True` gives selected handlers the same read-side behavior. Special families such as `MOVEM` use separate transfer rules.

For long predecrement `MOVE`, `_gen_write` emits a low-word write at address plus two, then a high-word write at the address.

See [Instruction emission](/developer/recompiler/emission) and [Instruction reference](/developer/recompiler/instruction-reference).
