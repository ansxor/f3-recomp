import { spawn } from "node:child_process";
import { accessSync, constants, existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

/** Repository root (this file lives in <root>/tools/sdk). */
export const REPO_ROOT: string = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

export function isExecutable(path: string): boolean {
  try {
    accessSync(path, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

export function hexToBytes(hex: string): Uint8Array {
  if (hex.length % 2) throw new Error(`odd-length hex string (${hex.length} digits)`);
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) {
    const hi = nibble(hex.charCodeAt(2 * i));
    const lo = nibble(hex.charCodeAt(2 * i + 1));
    if (hi < 0 || lo < 0) throw new Error(`invalid hex digit near offset ${2 * i}`);
    out[i] = (hi << 4) | lo;
  }
  return out;
}

function nibble(c: number): number {
  if (c >= 48 && c <= 57) return c - 48;
  if (c >= 97 && c <= 102) return c - 87;
  if (c >= 65 && c <= 70) return c - 55;
  return -1;
}

export function bytesToHex(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += (bytes[i] as number).toString(16).padStart(2, "0");
  return s;
}

/** Hex string for a request: bytes become lowercase hex; strings lose whitespace and a 0x prefix. */
export function normalizeHex(hex: string | Uint8Array): string {
  if (typeof hex !== "string") return bytesToHex(hex);
  return hex.replace(/^\s*0x/i, "").replace(/[\s_]/g, "").toLowerCase();
}

/** An address for the CLI/serve: integers become `0x..` strings (xref and flow take strings). */
export function addressText(value: number | string): string {
  return typeof value === "number" ? `0x${value.toString(16)}` : value;
}

export function parseHexNumber(text: string): number {
  const m = /^\s*(?:0x|\$)?([0-9a-f]+)\s*$/i.exec(text);
  if (!m) throw new Error(`not a hexadecimal number: ${JSON.stringify(text)}`);
  return Number.parseInt(m[1] as string, 16);
}

/** `0x%08x`, zero padded to at least 8 digits. */
export function crcText(value: string | number): string {
  const n = typeof value === "number" ? value : Number.parseInt(value.replace(/^0x/i, ""), 16);
  return `0x${n.toString(16).padStart(8, "0")}`;
}

export interface Captured {
  code: number | null;
  signal: string | null;
  stdout: string;
  stderr: string;
  timedOut: boolean;
}

/** Run a command to completion, collecting its output. Never throws for a nonzero exit. */
export function capture(
  command: string,
  args: string[],
  opts: { cwd?: string; env?: Record<string, string | undefined>; timeoutMs?: number } = {},
): Promise<Captured> {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, {
      cwd: opts.cwd ?? REPO_ROOT,
      env: { ...process.env, ...opts.env },
      stdio: ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    let timedOut = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (d: string) => (stdout += d));
    child.stderr.on("data", (d: string) => (stderr += d));
    child.on("error", (err) => {
      if (timer) clearTimeout(timer);
      reject(new Error(`cannot run ${command}: ${err.message}`));
    });
    child.on("close", (code, signal) => {
      if (timer) clearTimeout(timer);
      resolvePromise({ code, signal, stdout, stderr, timedOut });
    });
    if (opts.timeoutMs) {
      timer = setTimeout(() => {
        timedOut = true;
        child.kill("SIGKILL");
      }, opts.timeoutMs);
    }
  });
}

/** Build directories configured for a game: build/<id> first, then any build*\/ whose cache says F3_GAME=<id>. */
export function buildDirsFor(gameId: string, root: string = REPO_ROOT): string[] {
  const dirs: string[] = [];
  const direct = join(root, "build", gameId);
  if (existsSync(direct)) dirs.push(direct);
  let entries: string[] = [];
  try {
    entries = readdirSync(root).filter((n) => n.startsWith("build"));
  } catch {
    /* no root listing */
  }
  const candidates = [join(root, "build"), ...entries.map((n) => join(root, n))];
  for (const base of candidates) {
    let subdirs: string[] = [base];
    if (base === join(root, "build")) {
      try {
        subdirs = readdirSync(base).map((n) => join(base, n));
      } catch {
        subdirs = [];
      }
    }
    for (const dir of subdirs) {
      if (dirs.includes(dir)) continue;
      try {
        const cache = readFileSync(join(dir, "CMakeCache.txt"), "utf8");
        if (new RegExp(`^F3_GAME:STRING=${gameId.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}$`, "m").test(cache)) dirs.push(dir);
      } catch {
        /* not a build directory */
      }
    }
  }
  return dirs;
}

export function findInBuilds(gameId: string, names: string[], root: string = REPO_ROOT): string | null {
  for (const dir of buildDirsFor(gameId, root)) {
    for (const name of names) {
      const p = join(dir, name);
      if (isExecutable(p) && !isDirectory(p)) return p;
    }
  }
  return null;
}

function isDirectory(path: string): boolean {
  try {
    readdirSync(path);
    return true;
  } catch {
    return false;
  }
}

/** Last line of `text` that parses as a JSON object. */
export function lastJsonObject<T>(text: string): T | null {
  const lines = text.split("\n");
  for (let i = lines.length - 1; i >= 0; i--) {
    const line = (lines[i] as string).trim();
    if (!line.startsWith("{")) continue;
    try {
      return JSON.parse(line) as T;
    } catch {
      /* keep looking */
    }
  }
  return null;
}
