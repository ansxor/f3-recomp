import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { runGame, runMotionProof, runSpriteCheck } from "./runtime.ts";
import type { RunHandle } from "./runtime.ts";
import type {
  CheckIrqResult,
  CoverageResult,
  DecisionsOptions,
  DecisionsResult,
  DisasmLine,
  DisasmOptions,
  DispatchFlowResult,
  FlowOptions,
  FlowResult,
  InspectOptions,
  InspectResult,
  LowerOptions,
  LowerResult,
  MotionProofOptions,
  MotionProofResult,
  ProfileMergeResult,
  ProfileSpec,
  RamCodeBlock,
  RomBytes,
  RomReadResult,
  Routine,
  RunOptions,
  Site,
  SpriteCheckOptions,
  SpriteCheckResult,
  VectorEntry,
  VersionInfo,
  XrefOptions,
  XrefResult,
} from "./types.ts";
import { REPO_ROOT, addressText, buildDirsFor, hexToBytes, isExecutable, normalizeHex } from "./util.ts";

export interface ClientOptions {
  /** f3recomp executable (default: $F3_RECOMP_EXECUTABLE, recomp/_build/default/bin/main.exe, `f3recomp` on PATH). */
  f3recompPath?: string;
  /** Repository root (default: the repository this SDK lives in). */
  root?: string;
}

/** An error response of `f3recomp serve` (or a dead server). */
export class RecompError extends Error {
  method: string;
  constructor(method: string, message: string) {
    super(message);
    this.name = "RecompError";
    this.method = method;
  }
}

export function resolveRecompExecutable(opts: ClientOptions = {}): string {
  const root = opts.root ?? REPO_ROOT;
  const explicit = opts.f3recompPath ?? process.env.F3_RECOMP_EXECUTABLE;
  if (explicit) return explicit;
  const built = join(root, "recomp", "_build", "default", "bin", "main.exe");
  if (isExecutable(built)) return built;
  return "f3recomp";
}

interface Pending {
  method: string;
  resolve: (value: unknown) => void;
  reject: (reason: Error) => void;
}

/**
 * A persistent `f3recomp serve` process spoken to in line-delimited JSON. Games, discovery results
 * and profile analyses stay cached in the server between calls.
 *
 * The child never keeps the calling process alive while idle: it is referenced only while a request
 * is outstanding, and closes itself when this process exits (stdin EOF).
 */
export class RecompClient {
  readonly executable: string;
  readonly root: string;
  #child: ChildProcess;
  #nextId = 1;
  #pending = new Map<number, Pending>();
  #buffer = "";
  #stderr = "";
  #failure: Error | null = null;
  #closed = false;
  #exited: Promise<void>;

  constructor(opts: ClientOptions = {}) {
    this.root = opts.root ?? REPO_ROOT;
    this.executable = resolveRecompExecutable(opts);
    this.#child = spawn(this.executable, ["serve"], { cwd: this.root, stdio: ["pipe", "pipe", "pipe"] });
    const child = this.#child;
    this.#exited = new Promise<void>((done) => child.once("close", () => done()));
    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");
    child.stdout?.on("data", (chunk: string) => this.#onData(chunk));
    child.stderr?.on("data", (chunk: string) => {
      this.#stderr = (this.#stderr + chunk).slice(-4000);
    });
    child.stdin?.on("error", () => {
      /* a dead server is reported through the pending requests */
    });
    child.on("error", (err: Error) => {
      this.#fail(new RecompError("spawn", `cannot start ${this.executable} serve: ${err.message}`));
    });
    child.on("close", (code: number | null, signal: string | null) => {
      if (this.#closed) this.#fail(new RecompError("serve", "client closed"));
      else {
        const tail = this.#stderr.trim();
        this.#fail(
          new RecompError("serve", `f3recomp serve exited (${signal ?? `code ${code}`})${tail ? `: ${tail}` : ""}`),
        );
      }
    });
    this.#refresh();
  }

  #onData(chunk: string): void {
    this.#buffer += chunk;
    let nl = this.#buffer.indexOf("\n");
    while (nl >= 0) {
      const line = this.#buffer.slice(0, nl).trim();
      this.#buffer = this.#buffer.slice(nl + 1);
      if (line) this.#onLine(line);
      nl = this.#buffer.indexOf("\n");
    }
  }

  #onLine(line: string): void {
    let msg: { id: unknown; ok: boolean; result?: unknown; error?: string };
    try {
      msg = JSON.parse(line);
    } catch {
      return;
    }
    const pending = typeof msg.id === "number" ? this.#pending.get(msg.id) : undefined;
    if (!pending) return;
    this.#pending.delete(msg.id as number);
    if (msg.ok) pending.resolve(msg.result);
    else pending.reject(new RecompError(pending.method, String(msg.error)));
    this.#refresh();
  }

  #fail(err: Error): void {
    this.#failure ??= err;
    for (const [, p] of this.#pending) p.reject(this.#failure);
    this.#pending.clear();
  }

  /** Keep the process alive only while a request is outstanding. */
  #refresh(): void {
    const busy = this.#pending.size > 0;
    const streams = [this.#child, this.#child.stdin, this.#child.stdout, this.#child.stderr] as unknown[];
    for (const s of streams) {
      const handle = s as { ref?: () => void; unref?: () => void } | null;
      if (!handle) continue;
      if (busy) handle.ref?.();
      else handle.unref?.();
    }
  }

  /** One request. Rejects with RecompError on an error response or when the server died. */
  call<T = unknown>(method: string, params: Record<string, unknown> = {}): Promise<T> {
    if (this.#failure) return Promise.reject(this.#failure);
    if (this.#closed) return Promise.reject(new RecompError(method, "client closed"));
    const id = this.#nextId++;
    return new Promise<T>((resolvePromise, reject) => {
      this.#pending.set(id, { method, resolve: resolvePromise as (v: unknown) => void, reject });
      this.#refresh();
      const line = `${JSON.stringify({ id, method, params })}\n`;
      this.#child.stdin?.write(line, (err) => {
        if (err) {
          this.#pending.delete(id);
          reject(this.#failure ?? new RecompError(method, `cannot write to f3recomp serve: ${err.message}`));
        }
      });
    });
  }

  ping(): Promise<{ pong: true }> {
    return this.call("ping");
  }

  version(): Promise<VersionInfo> {
    return this.call("version");
  }

  /** Disassemble hex bytes placed at `pc` (no game needed). */
  disasmHex(pc: number | string, hex: string | Uint8Array, count?: number): Promise<DisasmLine[]> {
    return this.call("disasm", { pc, hex: normalizeHex(hex), ...(count === undefined ? {} : { count }) });
  }

  /** Lower hex bytes to C (no game needed). */
  lowerHex(
    pc: number | string,
    hex: string | Uint8Array,
    opts: { count?: number; liveOut?: string } = {},
  ): Promise<LowerResult> {
    return this.call("lower", {
      pc,
      hex: normalizeHex(hex),
      ...(opts.count === undefined ? {} : { count: opts.count }),
      ...(opts.liveOut === undefined ? {} : { live_out: opts.liveOut }),
    });
  }

  /** Merge block profiles into one file. */
  mergeProfiles(output: string, inputs: string[]): Promise<ProfileMergeResult> {
    return this.call("profile_merge", { inputs: inputs.map((p) => resolve(this.root, p)), output: resolve(this.root, output) });
  }

  /** Open a game on this client (see openGame for the options). */
  game(gameId: string, opts: GameOptions = {}): Game {
    return new Game(this, gameId, opts, false);
  }

  /** Stop the server (idempotent). Resolves once the process has exited. */
  async close(): Promise<void> {
    if (!this.#closed) {
      this.#closed = true;
      this.#fail(new RecompError("serve", "client closed"));
      this.#child.stdin?.end();
      const timer = setTimeout(() => this.#child.kill("SIGKILL"), 2000);
      this.#child.ref();
      await this.#exited;
      clearTimeout(timer);
    } else await this.#exited;
  }

  [Symbol.asyncDispose](): Promise<void> {
    return this.close();
  }
}

export interface GameOptions {
  /** games/<id>/config.toml by default. */
  config?: string;
  /** ROM directory: $F3_ROM_DIR, then roms/<id>, then the F3_ROM_DIR of a build cache for the game. */
  romDir?: string;
  /**
   * Block profile for hit counts: omitted/true = profiles/<id>.profile when it exists, false = none,
   * a path = that file (tiers), {slim: path} = a slim profile.
   */
  profile?: ProfileSpec;
  /** Share a client (the Game then does not own the server). */
  client?: RecompClient;
  f3recompPath?: string;
  root?: string;
}

export type OpenGameOptions = GameOptions;

function resolveRomDir(gameId: string, root: string, explicit?: string): string {
  if (explicit) return resolve(root, explicit);
  const env = process.env.F3_ROM_DIR;
  if (env) return resolve(root, env);
  const local = join(root, "roms", gameId);
  if (existsSync(local)) return local;
  for (const dir of buildDirsFor(gameId, root)) {
    try {
      const m = /^F3_ROM_DIR:PATH=(.+)$/m.exec(readFileSync(join(dir, "CMakeCache.txt"), "utf8"));
      if (m?.[1]?.trim()) return m[1].trim();
    } catch {
      /* no cache */
    }
  }
  return local;
}

/** Disassembly and analysis of one game through a shared f3recomp server. */
export class Game {
  readonly id: string;
  readonly root: string;
  readonly config: string;
  readonly romDir: string;
  readonly client: RecompClient;
  #profile: ProfileSpec;
  #ownsClient: boolean;
  #a5: number | null | undefined;

  constructor(client: RecompClient, gameId: string, opts: GameOptions, ownsClient: boolean) {
    this.client = client;
    this.#ownsClient = ownsClient;
    this.id = gameId;
    this.root = opts.root ?? client.root;
    this.config = resolve(this.root, opts.config ?? join("games", gameId, "config.toml"));
    this.romDir = resolveRomDir(gameId, this.root, opts.romDir);
    this.#profile = opts.profile ?? true;
  }

  get profile(): ProfileSpec {
    return this.#profile;
  }

  #params(extra: Record<string, unknown> = {}): Record<string, unknown> {
    return { config: this.config, rom_dir: this.romDir, ...extra };
  }

  #profileParams(spec: ProfileSpec | undefined): Record<string, string> {
    const chosen = spec ?? this.#profile;
    if (chosen === false) return {};
    if (chosen === true) {
      const p = join(this.root, "profiles", `${this.id}.profile`);
      return existsSync(p) ? { profile_tiers: p } : {};
    }
    if (typeof chosen === "string") return { profile_tiers: resolve(this.root, chosen) };
    if ("slim" in chosen) return { profile_slim: resolve(this.root, chosen.slim) };
    return { profile_tiers: resolve(this.root, chosen.tiers) };
  }

  /** Bytes of an assembled ROM region (rom, sound, sprites, sprites_hi, tiles, tiles_hi, samples, eeprom). */
  async readRom(region: string, addr: number, len: number): Promise<RomBytes> {
    const r = await this.client.call<RomReadResult>("rom_read", this.#params({ region, addr, len }));
    const bytes = hexToBytes(r.bytes) as RomBytes;
    bytes.region = r.region;
    bytes.addr = r.addr;
    bytes.regionSize = r.size;
    return bytes;
  }

  /** Big-endian 16-bit words from a ROM region (default the main program). */
  async words(addr: number, count: number, opts: { signed?: boolean; region?: string } = {}): Promise<number[]> {
    const b = await this.readRom(opts.region ?? "rom", addr, count * 2);
    const view = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const out: number[] = [];
    for (let i = 0; i + 2 <= b.length; i += 2) out.push(opts.signed ? view.getInt16(i) : view.getUint16(i));
    return out;
  }

  /** Big-endian 32-bit longs from a ROM region (default the main program). */
  async longs(addr: number, count: number, opts: { region?: string } = {}): Promise<number[]> {
    const b = await this.readRom(opts.region ?? "rom", addr, count * 4);
    const view = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const out: number[] = [];
    for (let i = 0; i + 4 <= b.length; i += 4) out.push(view.getUint32(i));
    return out;
  }

  /**
   * Disassemble `count` instructions (default 16) from `pc`, or those starting below `to`.
   * `disasm(0x400, 8)`, `disasm({ pc: 0x400, to: 0x440 })`, `disasm({ pc: 0, region: "sound" })`,
   * `disasm({ pc: 0x1000, hex: "4e75" })`.
   */
  disasm(pcOrOpts: number | string | DisasmOptions, count?: number): Promise<DisasmLine[]> {
    const o: DisasmOptions = typeof pcOrOpts === "object" ? pcOrOpts : { pc: pcOrOpts, count };
    const params: Record<string, unknown> = { pc: o.pc };
    if (o.count !== undefined) params.count = o.count;
    if (o.to !== undefined) params.to = o.to;
    if (o.a5 !== undefined) params.a5 = o.a5;
    if (o.hex !== undefined) {
      params.hex = normalizeHex(o.hex);
      return this.client.call("disasm", params);
    }
    if (o.region !== undefined) params.region = o.region;
    return this.client.call("disasm", this.#params(params));
  }

  /** Vectors, routines, RAM code, unresolved indirect sites and the detected a5 base. */
  inspect(opts: InspectOptions = {}): Promise<InspectResult> {
    const params: Record<string, unknown> = { routines: !opts.summary, ...this.#profileParams(opts.profile) };
    if (opts.a5 !== undefined) params.a5 = opts.a5;
    return this.client.call("inspect", this.#params(params));
  }

  async vectors(): Promise<VectorEntry[]> {
    return (await this.inspect({ summary: true })).vectors;
  }

  async routines(opts: Pick<InspectOptions, "profile"> = {}): Promise<Routine[]> {
    return (await this.inspect(opts)).routines ?? [];
  }

  async ramCode(): Promise<RamCodeBlock[]> {
    return (await this.inspect({ summary: true })).ram_code;
  }

  async unresolvedIndirect(opts: Pick<InspectOptions, "profile"> = {}): Promise<Site[]> {
    return (await this.inspect({ summary: true, ...opts })).unresolved_indirect_sites;
  }

  /** The a5 base register value detected in live code (cached), or null. */
  async a5(): Promise<number | null> {
    if (this.#a5 === undefined) this.#a5 = (await this.inspect({ summary: true, profile: false })).a5;
    return this.#a5;
  }

  /**
   * Cross-references. `target`: an address (number or hex), "a5-0x60ac", "START-END" or a region name
   * (rom ram palette io timer sprites pf0..pf3 pf2-alt pf3-alt text charram lines pivot control shared sound-reset).
   */
  xref(target: number | string, opts: XrefOptions = {}): Promise<XrefResult> {
    const params: Record<string, unknown> = { target: addressText(target), ...this.#profileParams(opts.profile) };
    if (opts.role !== undefined) params.role = opts.role;
    if (opts.limit !== undefined) params.limit = opts.limit;
    if (opts.a5 !== undefined) params.a5 = opts.a5;
    return this.client.call("xref", this.#params(params));
  }

  /** Call flow of a PC, a vector name ("irq2", "reset", "trap5") or "?dispatch". */
  flow(target: number | string, opts: FlowOptions = {}): Promise<FlowResult | DispatchFlowResult> {
    const params: Record<string, unknown> = { target: addressText(target), ...this.#profileParams(opts.profile) };
    if (opts.depth !== undefined) params.depth = opts.depth;
    return this.client.call("flow", this.#params(params));
  }

  coverage(): Promise<CoverageResult> {
    return this.client.call("coverage", this.#params());
  }

  checkIrq(): Promise<CheckIrqResult> {
    return this.client.call("check_irq", this.#params());
  }

  decisions(opts: DecisionsOptions = {}): Promise<DecisionsResult> {
    const params: Record<string, unknown> = { ...this.#profileParams(opts.profile) };
    if (opts.spriteOnly !== undefined) params.sprite_only = opts.spriteOnly;
    if (opts.spriteWriters !== undefined) params.sprite_writers = resolve(this.root, opts.spriteWriters);
    return this.client.call("decisions", this.#params(params));
  }

  /** Lower instructions to C: from the ROM at `pc`, or from `hex` bytes placed there. */
  lower(opts: LowerOptions): Promise<LowerResult> {
    const params: Record<string, unknown> = { pc: opts.pc };
    if (opts.count !== undefined) params.count = opts.count;
    if (opts.liveOut !== undefined) params.live_out = opts.liveOut;
    if (opts.hex !== undefined) {
      params.hex = normalizeHex(opts.hex);
      return this.client.call("lower", params);
    }
    if (opts.region !== undefined) params.region = opts.region;
    return this.client.call("lower", this.#params(params));
  }

  /** Headless run of the built game (or `f3rt-tool gameplay --json`); see RunOptions. */
  run(opts: RunOptions = {}): Promise<RunHandle> {
    return runGame(this, opts);
  }

  /** `f3rt-tool sprite-check --json`. */
  spriteCheck(opts: SpriteCheckOptions = {}): Promise<SpriteCheckResult> {
    return runSpriteCheck(this, opts);
  }

  /** `f3rt-tool motion --json`. */
  motionProof(opts: MotionProofOptions = {}): Promise<MotionProofResult> {
    return runMotionProof(this, opts);
  }

  /** Evict this game from the server cache; closes the server when this Game opened it. */
  async close(): Promise<void> {
    if (this.#ownsClient) await this.client.close();
    else await this.client.call("close", this.#params());
  }

  [Symbol.asyncDispose](): Promise<void> {
    return this.close();
  }
}

/**
 * Open a game: starts an `f3recomp serve` (or uses `opts.client`), resolving games/<id>/config.toml,
 * roms/<id> and profiles/<id>.profile. Throws when the config or ROM directory is missing.
 */
export async function openGame(gameId: string, opts: GameOptions = {}): Promise<Game> {
  const owns = !opts.client;
  const client = opts.client ?? new RecompClient({ f3recompPath: opts.f3recompPath, root: opts.root });
  const game = new Game(client, gameId, opts, owns);
  try {
    if (!existsSync(game.config)) throw new Error(`no config for game ${JSON.stringify(gameId)} at ${game.config}`);
    if (!existsSync(game.romDir)) throw new Error(`no ROM directory for ${gameId} at ${game.romDir} (pass romDir or set F3_ROM_DIR)`);
    await client.ping();
  } catch (err) {
    if (owns) await client.close();
    throw err;
  }
  return game;
}

// ---- standalone helpers on a shared lazily started client ---------------------------------------

let defaultClient: RecompClient | undefined;

/** A process-wide client started on first use; it never keeps the process alive. */
export function defaultRecompClient(): RecompClient {
  if (!defaultClient) defaultClient = new RecompClient();
  return defaultClient;
}

export async function closeDefaultRecompClient(): Promise<void> {
  const c = defaultClient;
  defaultClient = undefined;
  if (c) await c.close();
}

export function disasmHex(
  pc: number | string,
  hex: string | Uint8Array,
  count?: number,
  opts: { client?: RecompClient } = {},
): Promise<DisasmLine[]> {
  return (opts.client ?? defaultRecompClient()).disasmHex(pc, hex, count);
}

export function lowerHex(
  pc: number | string,
  hex: string | Uint8Array,
  opts: { count?: number; liveOut?: string; client?: RecompClient } = {},
): Promise<LowerResult> {
  return (opts.client ?? defaultRecompClient()).lowerHex(pc, hex, opts);
}

export function mergeProfiles(
  output: string,
  inputs: string[],
  opts: { client?: RecompClient } = {},
): Promise<ProfileMergeResult> {
  return (opts.client ?? defaultRecompClient()).mergeProfiles(output, inputs);
}
