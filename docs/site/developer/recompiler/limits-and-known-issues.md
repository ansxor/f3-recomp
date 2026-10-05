# Limits and known issues

Static coverage, decoder support, native lowering, and observed execution are separate claims. This page states the boundaries of each claim.

## Coverage is not reachability

`all_aligned` decodes every nonexcluded even ROM address. It includes overlapping starts and data that resembles instructions. Explicit exclusions are reviewed metadata, not an entropy-derived guarantee.

A native count does not prove that all entries are executable code. A fallback count does not prove that the game executes those entries.

Recursive discovery follows known roots and scans bounded metadata. It can miss computed targets, script callbacks, and code with no discovered path.

See [Instruction discovery](/developer/recompiler/discovery).

## Three different failure categories

| Category | Generated behavior |
| --- | --- |
| Decoded instruction with native lowering | Register an entry that executes C statements. |
| Decoded instruction without lowering | Register an entry that calls one-instruction fallback. |
| Failed decode | Register a shared primary-word exception only when metadata proves its category; otherwise leave it missing. |

An illegal-looking extension of a valid primary opcode is not automatically a proven illegal instruction.

`lowering.json` reports `fallback_mnemonics`, `fallback_pcs`, and `undecoded_valid_primary_entries`. Inspect that report for the actual input image.

## Unsupported categories

The emitter is not a general implementation of every Motorola CPU model.

- Floating-point and other coprocessor F-line operations take the EC020 vector 11 exception.
- A-line primary words take vector 10.
- ILLEGAL and BKPT take vector 4; external BKPT instruction substitution is absent.
- Unknown MOVEC control selectors take vector 4 after privilege checking.
- RTE frame formats greater than two take vector 14.
- Unrecognized mnemonics or unsupported operand shapes return `None`.
- `divul` and `divsl` have no dedicated mnemonic branch, despite related prefix handling in EA decoding.
- Reserved full-index extension combinations and incomplete extensions cannot produce a supported EA.
- Cache/MMU hardware behavior and coprocessor exception frames are not modeled by these lowerings.

CAS and CAS2 implement guest-visible comparison and writes through callbacks. They do not establish host multiprocessor atomicity.

MOVES uses the ABI bus access path. SFC and DFC fields do not create separate translated host address spaces.

## Static code boundary

The generator translates the supplied ROM image. It does not install new native code when guest RAM receives instruction bytes.

An address without a registered entry needs runtime missing-entry handling. Strict-native mode rejects interpreter use rather than pretending the entry exists.

The block table keeps every decoded interior PC. This solves resume and overlapping-entry problems inside translated ROM, not dynamic code generation.

## Timing boundary

The cycle table models pinned reference instruction costs. It does not simulate each physical bus cycle, cache effect, or external wait state.

Native blocks yield at instruction boundaries. An instruction can overshoot a deadline. Bus callbacks can synchronize selected devices before access.

Matching instruction state under synchronized reference time does not prove free-running timing. Matching video frames does not prove audio waveform equivalence.

[docs/developer/DECISIONS.md](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/DECISIONS.md) records these distinctions and revision-specific native measurements.

## Build and output boundary

ROM verification checks configured lane size and hashes. It cannot make a different game version compatible with baked addresses.

Generated files contain user-supplied ROM-derived information. They remain local and ignored.

The generator does not clean stale shards from an existing output directory. `sources.cmake` lists only the current generated source set.

ABI v3 output refuses incompatible ABI headers at compile time. This does not validate a hand-edited runtime implementation against the contract.

## Evidence boundary

Use [Instruction reference](/developer/recompiler/instruction-reference) for implemented families and [Testing](/developer/testing/) for verification methods.

Historical counts in the overview describe their recorded run. Regenerate reports for current input and settings before making new coverage claims.
