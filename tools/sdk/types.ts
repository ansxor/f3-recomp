// Types for the f3recomp JSON API (recomp/JSON-API.md) and the f3rt-tool --json results.
// Type-only module: always import with `import type`.

export type Hex = string;
export type RomRegion = "rom" | "sound" | "sprites" | "sprites_hi" | "tiles" | "tiles_hi" | "samples" | "eeprom";

export interface VersionInfo {
  name: string;
  api: number;
  methods: string[];
}

export interface RomReadResult {
  region: string;
  addr: number;
  length: number;
  /** Size of the whole assembled region. */
  size: number;
  bytes: Hex;
}

/** Bytes read from a ROM region, with where they came from. */
export type RomBytes = Uint8Array & { region: string; addr: number; regionSize: number };

export type DisasmEscape = "return" | "exception-return" | "computed" | "module-call" | "raises" | "halt";
export type RefRole = "read" | "write" | "rmw" | "call" | "branch" | "lea";

export interface InsnRef {
  role: RefRole;
  target?: number;
  a5_disp?: number;
  size: number;
  indexed: boolean;
}

export interface DisasmInsn {
  pc: number;
  length: number;
  bytes: Hex;
  ok: true;
  op_name: string;
  mnemonic: string;
  operands: string;
  text: string;
  targets: number[];
  falls_through: boolean;
  escape: DisasmEscape | null;
  refs: InsnRef[];
}

export interface DisasmIllegal {
  pc: number;
  length: number;
  bytes: Hex;
  ok: false;
  reason: string;
  vector: number;
  text: string;
}

export type DisasmLine = DisasmInsn | DisasmIllegal;

export interface DisasmOptions {
  pc: number | string;
  count?: number;
  /** List instructions starting below this address instead of `count`. */
  to?: number | string;
  region?: "rom" | "sound";
  /** Resolve disp(a5) operands against this base. */
  a5?: number | string;
  /** Disassemble these bytes (hex string or bytes) placed at `pc` instead of reading the ROM. */
  hex?: string | Uint8Array;
}

export interface LowerOptions {
  pc: number | string;
  count?: number;
  /** Letters of XNZVC live after the last instruction (default all). */
  liveOut?: string;
  region?: "rom" | "sound";
  hex?: string | Uint8Array;
}

export type LoweredInsn = DisasmLine & ({ c: string } | { error: string });

export interface LowerResult {
  pc: number;
  count: number;
  live_out: string;
  instructions: LoweredInsn[];
}

/** Block profile: `true`/omitted = profiles/<id>.profile when it exists, `false` = none. */
export type ProfileSpec = boolean | string | { tiers: string } | { slim: string };

export interface InspectOptions {
  /** Leave out `routines` (the default is the full listing). */
  summary?: boolean;
  a5?: number | string;
  profile?: ProfileSpec;
}

export type RoutineKind = "vector" | "ram" | "config" | "call" | "indirect";

export interface Routine {
  entry: number;
  name: string;
  kind: RoutineKind;
  kinds: string[];
  vectors: string[];
  region: "rom" | "ram";
  instructions: number;
  bytes: number;
  ranges: [number, number][];
  callers: number[];
  callees: number[];
  entry_hits?: number | null;
  hits?: number;
}

export interface Site {
  pc: number;
  routine: number | null;
  text: string;
  hits?: number;
}

export interface VectorEntry {
  number: number;
  name: string;
  raw: number;
  target: number;
  valid: boolean;
}

export interface RamCodeBlock {
  destination: number;
  entry: number;
  length: number;
  instructions: number;
  illegal_words: number;
  exits: number[];
  callers: number[];
  source?: number;
  transform?: "negate_be32";
}

export interface InspectResult {
  game: string;
  rom_size: number;
  rom_crc32: Hex;
  rom_sha256: Hex;
  a5: number | null;
  a5_info: { value: number | null; source: "explicit" | "detected" | null; candidates: { value: number; count: number }[] };
  vectors: VectorEntry[];
  routine_count: number;
  summary: {
    by_kind: Record<RoutineKind, number>;
    live_instructions: number;
    decoded_instructions: number;
    jump_tables: number;
  };
  routines?: Routine[];
  ram_code: RamCodeBlock[];
  unresolved_indirect_sites: Site[];
  profile: null | { identities: number; executed: number; cold_misses: number };
}

export type XrefRole = "all" | RefRole;

export interface XrefOptions {
  role?: XrefRole;
  limit?: number;
  a5?: number | string;
  profile?: ProfileSpec;
}

export interface XrefRef {
  pc: number;
  routine: number | null;
  op_name: string;
  text: string;
  role: RefRole;
  target: number | null;
  a5_disp: number | null;
  size: number;
  indexed: boolean;
  hits?: number;
}

export interface XrefResult {
  target: string;
  match: { kind: string; start?: number; end?: number; offset?: number; address?: number | null };
  a5: number | null;
  total: number;
  truncated: boolean;
  refs: XrefRef[];
}

export type EdgeKind = "call" | "tail" | "table-call" | "table-jump" | "fall";

export interface Callee {
  routine: number;
  site: number;
  kind: EdgeKind;
  hits?: number;
  repeated: boolean;
  children: Callee[];
}

export interface FlowResult {
  target: string;
  pc: number;
  entry: number;
  name: string;
  exact: boolean;
  kind: RoutineKind;
  vectors: string[];
  callers: { routine: number; site: number; kind: EdgeKind; hits?: number }[];
  callees: Callee[];
  unresolved: Site[];
}

export interface DispatchFlowResult {
  target: "?dispatch";
  dispatch: true;
  callers: { routine: number | null; site: number; kind: "dispatch" }[];
  callees: { routine: number; site: null; kind: "dispatch"; children: [] }[];
  unresolved: Site[];
}

export interface FlowOptions {
  depth?: number;
  profile?: ProfileSpec;
}

export interface CoverageResult {
  game: string;
  unsupported: Record<string, number>;
  total: number;
}

export type IrqHandlerVerdict =
  | { handler: number; verdict: "proven"; flags: string; undeclared: boolean }
  | { handler: number; verdict: "unknown"; at: number; why: string };

export interface CheckIrqResult {
  game: string;
  interrupt_observes: string;
  handlers: IrqHandlerVerdict[];
}

export interface DecisionsOptions {
  spriteOnly?: boolean;
  /** Path to a sprite-writers file. */
  spriteWriters?: string;
  profile?: ProfileSpec;
}

export interface DecisionRow {
  pc: number;
  disposition: string;
  length: number | null;
  flow: string | null;
  reads: string | null;
  writes: string | null;
  live_out: string | null;
  idle: string | null;
  chain_candidates: number[];
}

export interface DecisionsResult {
  game: string;
  sprite_only: boolean;
  columns: string[];
  rows: DecisionRow[];
}

export interface ProfileMergeResult {
  output: string;
  inputs: number;
  roms: number;
  ever_executed: number;
  cold_misses: number;
}

// ---- runtime ----------------------------------------------------------------------------------

/** Options of Game.run(). */
export interface RunOptions {
  /** "game" (default) runs the built game binary headless; "gameplay" runs `f3rt-tool gameplay --json`. */
  mode?: "game" | "gameplay";
  frames?: number;
  /** Input script (grammar: runtime/debug/input_script.hpp). Copied into the run directory. */
  inputs?: string;
  /** Run directory (default build/sdk/<game>-<frames>-<pid>). */
  out?: string;
  /** Dump machine state from this 1-based frame (default 1) every `dumpEvery` frames (0 = no dumps). */
  dumpStart?: number;
  dumpEvery?: number;
  /** Dump files to keep per frame, e.g. "mainram.bin,graphics.bin"; cpu.json is always kept. Default: all. */
  keep?: string | string[];
  /** Write audio as WAV: true = <out>/audio.wav. */
  wav?: boolean | string;
  seed?: number;
  /** Executable override (game binary, or f3rt-tool for mode "gameplay"). */
  exe?: string;
  /** ROM set name (gameplay mode). */
  set?: string;
  /** Extra command-line arguments appended as given. */
  args?: string[];
  timeoutMs?: number;
}

export interface RunResult {
  ok: boolean;
  exitCode: number | null;
  /** Run directory. */
  dir: string;
  /** `0x%08x`, null when the run printed none. */
  frameCrc: string | null;
  stateCrc: string | null;
  /** Frames the run reports having emulated. */
  framesRun: number | null;
  audioFrames: number | null;
  wavPath: string | null;
  /** <dir>/dumps when dumps were requested. */
  dumpDir: string | null;
  /** Combined stdout and stderr (also saved as <dir>/run.txt). */
  output: string;
  /** key=value pairs of the summary lines (strings), or the gameplay JSON fields. */
  summary: Record<string, string>;
  /** Parsed `f3rt-tool gameplay --json` object (gameplay mode). */
  json: GameplayResult | null;
}

export interface GameplayResult {
  status: "SUCCESS" | "FAILURE";
  error?: string;
  set?: string;
  seed?: number;
  frames?: number;
  pc?: string;
  frame_crc?: string;
  steps?: number;
  native_blocks?: number;
  audio_frames?: number;
  audio_peak?: number;
  nonzero_samples?: number;
  fps?: number;
}

export interface ToolOptions {
  /** f3rt-tool executable (default: $F3_TOOL_EXECUTABLE, build/<game>/f3rt-tool, a build*\/ with this F3_GAME). */
  exe?: string;
  /** ROM directory passed as --rom-dir (default: the tool's compiled-in default). */
  romDir?: string;
  set?: string;
  timeoutMs?: number;
  args?: string[];
}

export interface SpriteCheckOptions extends ToolOptions {
  frames?: number;
  compareEvery?: number;
  seed?: number;
  noInputs?: boolean;
  behaviours?: string[];
}

export interface SpriteCheckResult {
  status: "PASS" | "FAIL" | "ERROR";
  set?: string;
  seed?: number;
  frames?: number;
  steps?: number;
  native_blocks?: number;
  state_compares?: number;
  max_groups_frame?: number;
  group_frames?: number;
  group_entries?: number;
  unresolved_entries?: number;
  invocations?: number;
  spliced?: number;
  aborted?: number;
  state_crc?: string;
  error: string;
  /** exit code 0 and status PASS */
  ok: boolean;
  exitCode: number | null;
}

export interface MotionProofOptions extends ToolOptions {
  frames?: number;
  seed?: number;
  every?: number;
  scale?: number;
  interp?: "off" | "linear" | "fit";
  spriteGroups?: boolean;
  dumpDir?: string;
}

export interface MotionProofResult {
  status: "SUCCESS" | "FAILURE";
  error?: string;
  frames?: number;
  seed?: number;
  samples?: number;
  supported?: number;
  fallback?: number;
  paired?: number;
  visible_intermediate_frames?: number;
  visible_midpoint_frames?: number;
  eligible_geometry_draws?: number;
  audio_crc?: string;
  native_crc?: string;
  state_crc?: string;
  ok: boolean;
  exitCode: number | null;
}

export interface SpriteEntry {
  slot: number;
  bank: number;
  /** Word address of the entry in sprite RAM. */
  address: number;
  words: number[];
  tile: number;
  color: number;
  x: number;
  y: number;
  scaleX: number;
  scaleY: number;
  control: number;
  drawn: boolean;
  notes: string[];
}

export interface SpriteListSummary {
  startBank: number;
  end: number | null;
  jumps: number;
  commands: number;
  drawn: number;
  visited: number;
  banksRead: number[];
  switches: [bank: number, slot: number, to: number][];
  lastUsed: [bank: number, slot: number] | null;
}

// ---- media ------------------------------------------------------------------------------------

export interface WavCompareOptions {
  /** Compare only this time window of the reference (seconds). */
  startSec?: number;
  endSec?: number;
  /** Largest candidate delay searched, in sample frames (default 2048, 0 = no alignment). */
  maxLagSamples?: number;
  /** Use this lag instead of searching. */
  lag?: number;
  /** Seconds of signal the lag search looks at (default 20). */
  lagWindowSec?: number;
  /** Compare although the sample rates differ (samples are matched one to one). */
  allowRateMismatch?: boolean;
}

export interface WavCompareResult {
  refRate: number;
  candRate: number;
  channels: number;
  /** The candidate lags the reference by this many sample frames: cand[i + lag] ~ ref[i]. */
  lagSamples: number;
  lagSeconds: number;
  samplesCompared: number;
  /** Peaks in 16-bit LSB. */
  refPeak: number;
  candPeak: number;
  /** dB relative to full scale (-Infinity for silence). */
  refRmsDb: number;
  candRmsDb: number;
  /** RMS and max of cand - ref after alignment, in LSB. */
  errorRms: number;
  maxError: number;
  /** 20 log10(ref rms / error rms): Infinity for identical audio. */
  snrDb: number;
  /** Normalized correlation of the aligned samples (1 for two silent files). */
  correlation: number;
  withinOneLsbPct: number;
  /** The sample data of both files is byte for byte the same. */
  byteEqual: boolean;
}

export interface ImageData8 {
  width: number;
  height: number;
  /** RGBA, row-major, top row first. */
  rgba: Uint8Array;
}

export interface ImageCompareOptions {
  /** A pixel mismatches when a channel differs by more than this (default 0). */
  tolerance?: number;
  /** Include the alpha channel (default: RGB only). */
  alpha?: boolean;
  /** Write a diff image (BMP): mismatching pixels red, others the dimmed reference. */
  diffPath?: string;
}

export interface ImageCompareResult {
  width: number;
  height: number;
  totalPixels: number;
  mismatchedPixels: number;
  mismatchPct: number;
  maxChannelDiff: number;
  /** Mean absolute error per channel value (0..255). */
  mae: number;
  /** Infinity when identical. */
  psnrDb: number;
  exactMatch: boolean;
}

export interface DifferentialOptions {
  /** Command prefix (default ["uv","run","python"]). */
  python?: string[];
  musashi?: string;
  output?: string;
  cases?: number;
  seed?: number;
  filter?: string;
  instructions?: string;
  compileOnly?: boolean;
  verbose?: boolean;
  timeoutMs?: number;
}

export interface DifferentialAccounting {
  total_cases: number;
  supported_count: number;
  unsupported_count: number;
  supported_by_mnemonic: Record<string, number>;
  unsupported_by_mnemonic: Record<string, number>;
  unsupported_details?: unknown;
}

export interface DifferentialResult {
  ok: boolean;
  exitCode: number | null;
  outputDir: string;
  /** Combined stdout and stderr. */
  output: string;
  accounting: DifferentialAccounting | null;
}
