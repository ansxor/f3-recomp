import { closeSync, copyFileSync, existsSync, mkdirSync, openSync, readFileSync, readSync, readdirSync, statSync, unlinkSync, writeFileSync } from "node:fs";
import { basename, join, resolve } from "node:path";
import type { Game } from "./recomp.ts";
import type {
  GameplayResult,
  MotionProofOptions,
  MotionProofResult,
  RunOptions,
  RunResult,
  SpriteCheckOptions,
  SpriteCheckResult,
  SpriteEntry,
  SpriteListSummary,
  ToolOptions,
} from "./types.ts";
import { REPO_ROOT, capture, crcText, findInBuilds, lastJsonObject, parseHexNumber } from "./util.ts";

// ---- address -> dump file ---------------------------------------------------------------------

interface DumpLocation {
  file: string;
  offset: number;
}

/** Where an address of the main CPU map lives in a frame dump (work-RAM mirrors fold together). */
export function dumpLocation(address: number): DumpLocation {
  if (address >= 0x400000 && address < 0x440000) return { file: "mainram.bin", offset: (address - 0x400000) & 0x1ffff };
  if (address >= 0x440000 && address < 0x448000) return { file: "palette.bin", offset: address - 0x440000 };
  if (address >= 0x600000 && address < 0x640000) return { file: "graphics.bin", offset: address - 0x600000 };
  if (address >= 0x660000 && address < 0x660040) return { file: "control.bin", offset: address - 0x660000 };
  if (address >= 0xc00000 && address < 0xc00800) return { file: "shared.bin", offset: address - 0xc00000 };
  throw new Error(`0x${address.toString(16)} is not in a dump file (ram, palette, graphics 0x600000.., control, shared)`);
}

/** One record of a frame dump; b/w/l read big-endian unsigned values at a byte offset. */
export class RunRecord {
  readonly frame: number;
  readonly index: number;
  readonly address: number;
  readonly bytes: Uint8Array;

  constructor(frame: number, index: number, address: number, bytes: Uint8Array) {
    this.frame = frame;
    this.index = index;
    this.address = address;
    this.bytes = bytes;
  }

  #view(offset: number, size: number): DataView {
    if (offset < 0 || offset + size > this.bytes.length) {
      throw new RangeError(`offset ${offset} (+${size}) is outside the ${this.bytes.length}-byte record`);
    }
    return new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
  }

  b(offset: number): number {
    return this.#view(offset, 1).getUint8(offset);
  }

  w(offset: number): number {
    return this.#view(offset, 2).getUint16(offset);
  }

  l(offset: number): number {
    return this.#view(offset, 4).getUint32(offset);
  }
}

export interface RunHandleInit extends Partial<RunResult> {
  dir: string;
  gameId?: string | null;
  a5?: number | null;
}

/**
 * A finished run (its RunResult fields) plus readers for its frame dumps.
 * Addresses are numbers, hex strings ("0x4083a8", "$4083a8") or "a5-0x60ac" / "a5+0x10" (needs the game's a5).
 */
export class RunHandle implements RunResult {
  ok: boolean;
  exitCode: number | null;
  dir: string;
  frameCrc: string | null;
  stateCrc: string | null;
  framesRun: number | null;
  audioFrames: number | null;
  wavPath: string | null;
  dumpDir: string | null;
  output: string;
  summary: Record<string, string>;
  json: GameplayResult | null;
  gameId: string | null;
  a5: number | null;

  constructor(init: RunHandleInit) {
    this.dir = init.dir;
    this.ok = init.ok ?? true;
    this.exitCode = init.exitCode ?? null;
    this.frameCrc = init.frameCrc ?? null;
    this.stateCrc = init.stateCrc ?? null;
    this.framesRun = init.framesRun ?? null;
    this.audioFrames = init.audioFrames ?? null;
    this.wavPath = init.wavPath ?? null;
    this.dumpDir = init.dumpDir ?? null;
    this.output = init.output ?? "";
    this.summary = init.summary ?? {};
    this.json = init.json ?? null;
    this.gameId = init.gameId ?? null;
    this.a5 = init.a5 ?? null;
  }

  /** Dumped frame numbers (1-based), ascending. Empty without dumps. */
  frames(): number[] {
    const root = this.dumpDir;
    if (!root || !existsSync(root)) return [];
    return readdirSync(root)
      .map((n) => /^frame_(\d+)$/.exec(n))
      .filter((m): m is RegExpExecArray => m !== null)
      .map((m) => Number.parseInt(m[1] as string, 10))
      .sort((a, b) => a - b);
  }

  /** Path of a file of one frame's dump. */
  framePath(frame: number, name: string): string {
    if (!this.dumpDir) throw new Error(`run ${this.dir} has no dumps`);
    return join(this.dumpDir, `frame_${String(frame).padStart(4, "0")}`, name);
  }

  /** Resolve a number, hex string or a5±offset to an absolute address. */
  address(address: number | string): number {
    if (typeof address === "number") return address;
    const m = /^\s*a5\s*([+-])\s*(?:0x|\$)?([0-9a-f]+)\s*$/i.exec(address);
    if (!m) return parseHexNumber(address);
    if (this.a5 === null) throw new Error(`a5 is unknown for this run; pass { a5 } or { game } to openRun`);
    const off = Number.parseInt(m[2] as string, 16);
    return m[1] === "-" ? this.a5 - off : this.a5 + off;
  }

  /** Bytes at an address in one frame's dump. */
  mem(frame: number, address: number | string, size = 1): Uint8Array {
    const { file, offset } = dumpLocation(this.address(address));
    const path = this.framePath(frame, file);
    let fd: number;
    try {
      fd = openSync(path, "r");
    } catch {
      const frameDir = join(path, "..");
      if (!existsSync(frameDir)) throw new Error(`frame ${frame} is not dumped under ${this.dumpDir}`);
      throw new Error(`frame ${frame} has no ${file} (run with keep including it)`);
    }
    try {
      const buf = new Uint8Array(size);
      const got = readSync(fd, buf, 0, size, offset);
      if (got !== size) throw new Error(`${file} of frame ${frame} ends before offset ${offset + size}`);
      return buf;
    } finally {
      closeSync(fd);
    }
  }

  b(frame: number, address: number | string): number {
    return this.mem(frame, address, 1)[0] as number;
  }

  w(frame: number, address: number | string): number {
    const m = this.mem(frame, address, 2);
    return ((m[0] as number) << 8) | (m[1] as number);
  }

  l(frame: number, address: number | string): number {
    const m = this.mem(frame, address, 4);
    return (((m[0] as number) << 24) | ((m[1] as number) << 16) | ((m[2] as number) << 8) | (m[3] as number)) >>> 0;
  }

  /** [frame, value] for every dumped frame (or the given frames); size 1..6 bytes, big-endian. */
  series(address: number | string, size = 1, frames?: number[]): [number, number][] {
    if (!Number.isInteger(size) || size < 1 || size > 6) throw new RangeError("series size must be 1..6 bytes");
    return (frames ?? this.frames()).map((n) => {
      let v = 0;
      for (const byte of this.mem(n, address, size)) v = v * 256 + byte;
      return [n, v] as [number, number];
    });
  }

  /** `count` records of `size` (default `stride`) bytes from `base` in one frame. */
  table(base: number | string, stride: number, count: number, frame: number, size?: number): RunRecord[] {
    const start = this.address(base);
    const out: RunRecord[] = [];
    for (let k = 0; k < count; k++) {
      const address = start + k * stride;
      out.push(new RunRecord(frame, k, address, this.mem(frame, address, size ?? stride)));
    }
    return out;
  }

  /** Walk the sprite list of one frame as the chip does (needs graphics.bin in the dump). */
  sprites(frame: number, opts: { bank?: number; limit?: number } = {}): { entries: SpriteEntry[]; summary: SpriteListSummary } {
    const ram = this.mem(frame, 0x600000, 0x10000);
    return decodeSpriteList(ram, opts.bank ?? 0, opts.limit ?? 0x400);
  }
}

/** Reopen a run directory written by Game.run(). */
export async function openRun(
  dir: string,
  gameId?: string | null,
  opts: { a5?: number | null; game?: Game } = {},
): Promise<RunHandle> {
  const path = resolve(REPO_ROOT, dir);
  let id = gameId ?? opts.game?.id ?? null;
  if (!id) {
    for (const marker of [join(path, "game"), join(path, "..", "game")]) {
      if (existsSync(marker)) {
        id = readFileSync(marker, "utf8").trim();
        break;
      }
    }
  }
  const dumps = existsSync(join(path, "dumps")) ? join(path, "dumps") : path;
  const hasFrames = existsSync(dumps) && readdirSync(dumps).some((n) => /^frame_\d+$/.test(n));
  const runTxt = join(path, "run.txt");
  const output = existsSync(runTxt) ? readFileSync(runTxt, "utf8") : "";
  const summary = parseSummary(output);
  const wav = join(path, "audio.wav");
  return new RunHandle({
    dir: path,
    gameId: id,
    a5: opts.a5 ?? (opts.game ? await opts.game.a5().catch(() => null) : null),
    dumpDir: hasFrames ? dumps : null,
    output,
    summary,
    frameCrc: summary.frame_crc ? crcText(summary.frame_crc) : null,
    stateCrc: summary.state_crc ? crcText(summary.state_crc) : null,
    wavPath: existsSync(wav) ? wav : null,
  });
}

function parseSummary(text: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const line of text.split("\n")) for (const m of line.matchAll(/(\w+)=(\S+)/g)) out[m[1] as string] = m[2] as string;
  return out;
}

function gameExecutable(game: Game, explicit?: string): string {
  if (explicit) return resolve(game.root, explicit);
  const names = [game.id, game.id.replace(/j$/, "")];
  const found = findInBuilds(game.id, names, game.root);
  if (!found) throw new Error(`no built executable for ${game.id}; build it (cmake --build build/${game.id}) or pass exe`);
  return found;
}

function toolExecutable(game: Game, explicit?: string): string {
  const path = explicit ?? process.env.F3_TOOL_EXECUTABLE;
  if (path) return resolve(game.root, path);
  const found = findInBuilds(game.id, ["f3rt-tool"], game.root);
  if (!found) throw new Error(`no f3rt-tool for ${game.id}; build it (cmake --build build/${game.id} --target f3rt-tool) or pass exe`);
  return found;
}

/** Headless run of `game`; see RunOptions. Throws only when the run could not start. */
export async function runGame(game: Game, opts: RunOptions = {}): Promise<RunHandle> {
  const frames = opts.frames ?? 1200;
  const mode = opts.mode ?? "game";
  const dir = resolve(game.root, opts.out ?? join("build", "sdk", `${game.id}-${frames}-${process.pid}`));
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, "game"), `${game.id}\n`);
  const dumpEvery = opts.dumpEvery ?? 0;
  const dumpDir = dumpEvery > 0 ? join(dir, "dumps") : null;
  const wavPath = opts.wav ? (typeof opts.wav === "string" ? resolve(game.root, opts.wav) : join(dir, "audio.wav")) : null;

  let exe: string;
  let args: string[];
  if (mode === "gameplay") {
    exe = toolExecutable(game, opts.exe);
    args = ["gameplay", "--json", "--rom-dir", game.romDir, "--frames", String(frames)];
    if (opts.seed !== undefined) args.push("--seed", String(opts.seed));
    if (opts.set) args.push("--set", opts.set);
    if (dumpDir) args.push("--dump-dir", dumpDir);
  } else {
    exe = gameExecutable(game, opts.exe);
    args = ["--rom-dir", game.romDir, "--headless", "--no-audio", "--unthrottled", "--frames", String(frames)];
    if (opts.set) args.push("--set", opts.set);
    if (dumpDir) args.push("--dump-dir", dumpDir, "--dump-start", String(opts.dumpStart ?? 1), "--dump-every", String(dumpEvery));
    if (opts.inputs) {
      const script = resolve(game.root, opts.inputs);
      if (!existsSync(script)) throw new Error(`no input script at ${script}`);
      const copy = join(dir, "inputs.txt");
      if (script !== copy) copyFileSync(script, copy);
      args.push("--inputs", copy);
    }
  }
  if (wavPath) args.push("--wav", wavPath);
  args.push(...(opts.args ?? []));

  const done = await capture(exe, args, { cwd: game.root, timeoutMs: opts.timeoutMs });
  const output = done.stdout + done.stderr + (done.timedOut ? `\nrun timed out after ${opts.timeoutMs} ms\n` : "");
  writeFileSync(join(dir, "run.txt"), output);

  let json: GameplayResult | null = null;
  let summary: Record<string, string>;
  if (mode === "gameplay") {
    json = lastJsonObject<GameplayResult>(done.stdout);
    summary = {};
    for (const [k, v] of Object.entries(json ?? {})) summary[k] = String(v);
  } else {
    summary = parseSummary(output);
  }
  const ok = done.code === 0 && !done.timedOut && (json ? json.status === "SUCCESS" : true);

  if (dumpDir && opts.keep !== undefined && existsSync(dumpDir)) pruneDumps(dumpDir, opts.keep);

  const num = (s: string | undefined): number | null => (s !== undefined && Number.isFinite(Number(s)) ? Number(s) : null);
  const handle = new RunHandle({
    ok,
    exitCode: done.code,
    dir,
    gameId: game.id,
    frameCrc: summary.frame_crc ? crcText(summary.frame_crc) : null,
    stateCrc: summary.state_crc ? crcText(summary.state_crc) : null,
    framesRun: num(summary.frames),
    audioFrames: num(summary.audio_frames),
    wavPath: wavPath && existsSync(wavPath) ? wavPath : null,
    dumpDir: dumpDir && existsSync(dumpDir) ? dumpDir : null,
    output,
    summary,
    json,
  });
  if (dumpDir) handle.a5 = await game.a5().catch(() => null);
  return handle;
}

function pruneDumps(dumpDir: string, keepSpec: string | string[]): void {
  const keep = new Set((Array.isArray(keepSpec) ? keepSpec : [keepSpec]).flatMap((s) => s.split(",")).map((s) => s.trim()).filter(Boolean));
  if (keep.has("graphics.bin")) keep.add("sprite_writers.bin");
  keep.add("cpu.json");
  for (const frame of readdirSync(dumpDir)) {
    const frameDir = join(dumpDir, frame);
    if (!statSync(frameDir).isDirectory()) continue;
    for (const file of readdirSync(frameDir)) if (!keep.has(basename(file))) unlinkSync(join(frameDir, file));
  }
}

// ---- f3rt-tool ----------------------------------------------------------------------------------

async function runTool<T extends { status: string }>(
  game: Game,
  sub: string,
  flags: string[],
  opts: ToolOptions,
): Promise<T & { ok: boolean; exitCode: number | null }> {
  const exe = toolExecutable(game, opts.exe);
  const args = [sub, "--rom-dir", resolve(game.root, opts.romDir ?? game.romDir)];
  if (opts.set) args.push("--set", opts.set);
  args.push(...flags, "--json", ...(opts.args ?? []));
  const done = await capture(exe, args, { cwd: game.root, timeoutMs: opts.timeoutMs });
  const json = lastJsonObject<T>(done.stdout);
  if (!json) {
    throw new Error(`f3rt-tool ${sub} printed no JSON result (exit ${done.code}): ${(done.stderr || done.stdout).trim().slice(-600)}`);
  }
  return { ...json, ok: done.code === 0, exitCode: done.code };
}

export async function runSpriteCheck(game: Game, opts: SpriteCheckOptions = {}): Promise<SpriteCheckResult> {
  const flags: string[] = [];
  if (opts.frames !== undefined) flags.push("--frames", String(opts.frames));
  if (opts.compareEvery !== undefined) flags.push("--compare-every", String(opts.compareEvery));
  if (opts.seed !== undefined) flags.push("--seed", String(opts.seed));
  if (opts.noInputs) flags.push("--no-inputs");
  for (const b of opts.behaviours ?? []) flags.push("--behaviour", b);
  const r = await runTool<SpriteCheckResult>(game, "sprite-check", flags, opts);
  r.ok = r.ok && r.status === "PASS";
  return r;
}

export async function runMotionProof(game: Game, opts: MotionProofOptions = {}): Promise<MotionProofResult> {
  const flags: string[] = [];
  if (opts.frames !== undefined) flags.push("--frames", String(opts.frames));
  if (opts.seed !== undefined) flags.push("--seed", String(opts.seed));
  if (opts.every !== undefined) flags.push("--every", String(opts.every));
  if (opts.scale !== undefined) flags.push("--scale", String(opts.scale));
  if (opts.interp) flags.push("--interp", opts.interp);
  if (opts.spriteGroups !== undefined) flags.push("--sprite-groups", opts.spriteGroups ? "on" : "off");
  if (opts.dumpDir) flags.push("--dump-dir", resolve(game.root, opts.dumpDir));
  const r = await runTool<MotionProofResult>(game, "motion", flags, opts);
  r.ok = r.ok && r.status === "SUCCESS";
  return r;
}

// ---- sprite list ----------------------------------------------------------------------------------

function signed12(value: number): number {
  const v = value & 0xfff;
  return v & 0x800 ? v - 0x1000 : v;
}

class Axis {
  blockScale = 0x100;
  pos = 0;
  blockPos = 0;
  global = 0;
  subglobal = 0;

  update(scroll: number, posw: number, multi: boolean, blockCtrl: number, zoom: number): void {
    let value = signed12(posw);
    if (scroll & 1) this.subglobal = value;
    if (scroll & 2) this.global = value;
    if (!(scroll & 8)) {
      value += this.global;
      if (!(scroll & 4)) value += this.subglobal;
    }
    if (blockCtrl === 0 && !multi) {
      this.blockPos = value * 256;
      this.blockScale = 0x100 - zoom;
    }
    if (blockCtrl === 0 || blockCtrl === 2) this.pos = this.blockPos;
    else if (blockCtrl === 3) this.pos += this.blockScale * 16;
  }
}

/**
 * Walk the Taito F3 sprite list like the hardware (runtime/renderer/decode.cpp decode_sprite_list) from sprite
 * RAM (graphics.bin 0x0000-0xffff, big-endian words). Two banks of 0x400 16-byte entries (0x0000 / 0x8000); a
 * command entry (word 3 bit 15) switches banks; word 6 bit 15 jumps to entry w6 & 0x3ff (a jump to itself ends
 * the list).
 *
 * Entry words: w0 tile (bit 16 = w5 bit 0); w1 zoom (x low byte, y high byte; 0 = full size); w2/w3 x/y
 * (12-bit signed, scroll mode in w2 bits 12-15); w4 high byte sprite control (bit 0 flip x, 1 flip y, 2 colour
 * lock, 3 multi/continue block, 4-5 y block control, 6-7 x block control), w4 low byte colour (bits 6-7 priority).
 */
export function decodeSpriteList(
  spriteRam: Uint8Array,
  startBank = 0,
  limit = 0x400,
): { entries: SpriteEntry[]; summary: SpriteListSummary } {
  const view = new DataView(spriteRam.buffer, spriteRam.byteOffset, spriteRam.byteLength);
  const x = new Axis();
  const y = new Axis();
  let color = 0;
  let multi = false;
  let bank = startBank;
  const entries: SpriteEntry[] = [];
  const summary: SpriteListSummary = {
    startBank,
    end: null,
    jumps: 0,
    commands: 0,
    drawn: 0,
    visited: 0,
    banksRead: [],
    switches: [],
    lastUsed: null,
  };
  let offs = 0;
  let visited = 0;
  while (offs < 0x400 && visited < limit) {
    visited++;
    const base = bank * 0x8000 + offs * 16;
    if (base + 16 > spriteRam.length) break;
    const words: number[] = [];
    for (let i = 0; i < 8; i++) words.push(view.getUint16(base + 2 * i));
    const [w0, w1, w2, w3, w4, w5, w6] = words as [number, number, number, number, number, number, number, number];
    const notes: string[] = [];
    const thisBank = bank;
    if (!summary.banksRead.includes(bank)) summary.banksRead.push(bank);
    if (words.slice(0, 7).some((w) => w !== 0)) summary.lastUsed = [bank, offs];
    if (w3 & 0x8000) {
      summary.commands++;
      if ((w5 & 1) !== bank) summary.switches.push([bank, offs, w5 & 1]);
      bank = w5 & 1;
      notes.push(
        `command: bank ${bank}, flipscreen ${w5 & 0x2000 ? 1 : 0}, extra planes ${(w5 >> 8) & 3}, trails ${w5 & 2 ? 1 : 0}`,
      );
    }
    const slot = offs;
    let end = false;
    if (w6 & 0x8000) {
      const target = w6 & 0x3ff;
      if (target === offs) {
        notes.push("list end (jump to itself)");
        end = true;
      } else {
        summary.jumps++;
        notes.push(`jump to 0x${target.toString(16).padStart(3, "0")}`);
        offs = target - 1;
      }
    }
    const control = w4 >> 8;
    const scroll = (w2 >> 12) & 0xf;
    if (!end) {
      if (!(control & 4)) color = w4 & 0xff;
      x.update(scroll, w2, multi, (control >> 6) & 3, w1 & 0xff);
      y.update(scroll, w3, multi, (control >> 4) & 3, w1 >> 8);
      multi = Boolean(control & 8);
    }
    const tile = w0 | ((w5 & 1) << 16);
    const drawn = tile !== 0 && !end;
    if (drawn) summary.drawn++;
    entries.push({
      slot,
      bank: thisBank,
      address: 0x600000 + thisBank * 0x8000 + slot * 16,
      words,
      tile,
      color,
      x: x.pos / 256,
      y: y.pos / 256,
      scaleX: x.blockScale,
      scaleY: y.blockScale,
      control,
      drawn,
      notes,
    });
    if (end) {
      summary.end = slot;
      break;
    }
    offs++;
  }
  summary.visited = visited;
  return { entries, summary };
}
