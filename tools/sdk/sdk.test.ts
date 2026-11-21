// Tests for the code-mode SDK. Runs under both runners:
//   node --test tools/sdk/sdk.test.ts
//   bun test tools/sdk/sdk.test.ts
// Needs only the built f3recomp (recomp/_build/default/bin/main.exe or $F3_RECOMP_EXECUTABLE) and no ROMs;
// the real-ROM sections run when roms/commandw or roms/landmakrj exist.

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, test } from "node:test";
import { deflateSync } from "node:zlib";
import {
  REPO_ROOT,
  RecompClient,
  RecompError,
  compareImage,
  compareWav,
  decodeBmp,
  decodePng,
  decodeSpriteList,
  disasmHex,
  closeDefaultRecompClient,
  encodeBmp,
  encodeWav,
  mergeProfiles,
  lowerHex,
  openGame,
  openRun,
  readWav,
  resolveRecompExecutable,
  runDifferential,
} from "./index.ts";
import type { DisasmInsn, DisasmLine, Game, ImageData8 } from "./index.ts";
import { isExecutable } from "./util.ts";

const scratch = mkdtempSync(join(tmpdir(), "f3sdk-test-"));
after(async () => {
  await closeDefaultRecompClient();
  rmSync(scratch, { recursive: true, force: true });
});

const recompExe = resolveRecompExecutable();
const haveRecomp = recompExe !== "f3recomp" ? isExecutable(recompExe) : true;
const haveRom = (id: string) => haveRecomp && existsSync(join(REPO_ROOT, "roms", id));
const haveTool = (id: string) => existsSync(join(REPO_ROOT, "build", id, "f3rt-tool"));
const haveBinary = (id: string) => existsSync(join(REPO_ROOT, "build", id, id));

// The runners disagree on the `skip` option (bun ignores it), so absent prerequisites just do not register tests.
const suiteIf = (cond: boolean, name: string, fn: () => void): void => {
  if (cond) describe(name, fn);
};
const testIf = (cond: boolean, name: string, fn: () => Promise<void> | void): void => {
  if (cond) test(name, fn);
};

const insn = (l: DisasmLine | undefined): DisasmInsn => {
  assert.ok(l?.ok, `expected a decoded instruction, got ${JSON.stringify(l)}`);
  return l as DisasmInsn;
};

// ---- RecompClient (no ROMs) ---------------------------------------------------------------------

suiteIf(haveRecomp, "RecompClient", () => {
  let client: RecompClient;
  before(() => {
    client = new RecompClient();
  });
  after(async () => {
    await client.close();
  });

  test("ping and version", async () => {
    assert.deepEqual(await client.ping(), { pong: true });
    const v = await client.version();
    assert.equal(v.name, "f3recomp");
    assert.equal(v.api, 1);
    for (const m of ["ping", "rom_read", "disasm", "inspect", "xref", "flow", "lower", "profile_merge"]) {
      assert.ok(v.methods.includes(m), `method ${m}`);
    }
  });

  test("disasmHex decodes instructions and illegal words", async () => {
    const lines = await client.disasmHex(0x400, "61000018 4e75 4afc", 3);
    assert.equal(lines.length, 3);
    const bsr = insn(lines[0]);
    assert.equal(bsr.pc, 0x400);
    assert.equal(bsr.mnemonic, "bsr");
    assert.deepEqual(bsr.targets, [0x41a]);
    assert.equal(bsr.refs[0]?.role, "call");
    const rts = insn(lines[1]);
    assert.equal(rts.pc, 0x404);
    assert.equal(rts.escape, "return");
    const bad = lines[2] as DisasmLine;
    assert.equal(bad.ok, false);
    assert.match(bad.text, /^dc\.w \$4afc/);
  });

  test("disasmHex accepts bytes and string pcs", async () => {
    const lines = await client.disasmHex("0x1000", new Uint8Array([0x4e, 0x71]));
    assert.equal(insn(lines[0]).pc, 0x1000);
    assert.equal(insn(lines[0]).mnemonic, "nop");
  });

  test("lowerHex returns C for each instruction", async () => {
    const r = await client.lowerHex(0x400, "7001 4e71", { count: 2 });
    assert.equal(r.count, 2);
    assert.equal(r.live_out, "XNZVC");
    const first = r.instructions[0] as { c?: string; error?: string };
    assert.ok(first.c && first.c.length > 0, JSON.stringify(first));
    assert.match(first.c, /d\[0x0u\] = 0x1u/);
  });

  test("repeated calls on one session are fast", async () => {
    const t0 = performance.now();
    await Promise.all(Array.from({ length: 200 }, () => client.disasmHex(0x400, "4e75", 1)));
    for (let i = 0; i < 100; i++) await client.ping();
    assert.ok(performance.now() - t0 < 5000, "300 calls within 5 s");
  });

  test("server errors reject with RecompError and the session survives", async () => {
    await assert.rejects(client.call("no_such_method"), (e: unknown) => e instanceof RecompError && /unknown method/.test(e.message));
    await assert.rejects(client.disasmHex(0, "zz"), RecompError);
    await assert.rejects(client.call("rom_read", { config: "nope.toml", rom_dir: "nope", len: 4, addr: 0 }), RecompError);
    assert.deepEqual(await client.ping(), { pong: true });
  });

  test("standalone helpers use a shared client", async () => {
    const lines = await disasmHex(0x2000, "4e75");
    assert.equal(insn(lines[0]).mnemonic, "rts");
    const lowered = await lowerHex(0x2000, "4e75", { count: 1 });
    assert.equal(lowered.instructions.length, 1);
    await assert.rejects(mergeProfiles(join(scratch, "out.profile"), [join(scratch, "missing.profile")]), RecompError);
  });

  test("close is idempotent and later calls reject", async () => {
    const c = new RecompClient();
    await c.ping();
    await c.close();
    await c.close();
    await assert.rejects(c.ping(), /closed/);
  });

  test("await using disposes the client", async () => {
    const c = new RecompClient();
    await c.ping();
    await c[Symbol.asyncDispose]();
    await assert.rejects(c.ping(), RecompError);
  });

  test("a missing executable rejects calls with a clear message", async () => {
    const c = new RecompClient({ f3recompPath: join(scratch, "no-such-f3recomp") });
    await assert.rejects(c.ping(), /cannot start .*no-such-f3recomp/);
    await c.close();
  });

  test("a server that dies fails pending and later calls", async () => {
    const script = join(scratch, "die.sh");
    writeFileSync(script, "#!/bin/sh\necho oops >&2\nexit 3\n", { mode: 0o755 });
    const c = new RecompClient({ f3recompPath: script });
    await assert.rejects(c.ping(), /exited.*oops/s);
    await assert.rejects(c.ping(), RecompError);
    await c.close();
  });

  test("an unclosed client does not keep the process alive", () => {
    const script = join(scratch, "no-close.mjs");
    writeFileSync(
      script,
      `import { RecompClient } from ${JSON.stringify(new URL("./index.ts", import.meta.url).href)};
const c = new RecompClient();
await c.ping();
console.log("done");
`,
    );
    const out = execFileSync(process.execPath, [script], { encoding: "utf8", timeout: 20000 });
    assert.match(out, /done/);
  });
});

// ---- runDifferential with a stand-in harness ------------------------------------------------------

describe("runDifferential", () => {
  test("parses differential_accounting.json from the output directory", async () => {
    const fake = join(scratch, "fake-differential.mjs");
    writeFileSync(
      fake,
      `import { mkdirSync, writeFileSync } from "node:fs";
const args = process.argv.slice(2);
const out = args[args.indexOf("--output") + 1];
mkdirSync(out, { recursive: true });
writeFileSync(out + "/differential_accounting.json", JSON.stringify({
  total_cases: 10, supported_count: 8, unsupported_count: 2,
  supported_by_mnemonic: { move: 8 }, unsupported_by_mnemonic: { callm: 2 } }));
console.log("args:" + args.join(" "));
`,
    );
    const out = join(scratch, "diff-out");
    const r = await runDifferential({ python: [process.execPath, fake], output: out, cases: 7, seed: 3, filter: "move" });
    assert.equal(r.ok, true);
    assert.equal(r.outputDir, out);
    assert.equal(r.accounting?.supported_count, 8);
    assert.deepEqual(r.accounting?.unsupported_by_mnemonic, { callm: 2 });
    assert.match(r.output, /-m tools\.differential --output .* --cases 7 --seed 3 --filter move/);
  });
});

// ---- compareWav ----------------------------------------------------------------------------------

function lcg(seed: number): () => number {
  let s = seed >>> 0;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s / 0x100000000;
  };
}

/** Deterministic stereo test signal: two tones plus noise, `frames` sample frames. */
function signal(frames: number, seed = 1): Int16Array {
  const rnd = lcg(seed);
  const out = new Int16Array(frames * 2);
  for (let i = 0; i < frames; i++) {
    const l = 6000 * Math.sin(i * 0.05) + 2500 * Math.sin(i * 0.31) + 1500 * (rnd() - 0.5);
    const r = 5000 * Math.sin(i * 0.07 + 1) + 2000 * Math.sin(i * 0.23) + 1500 * (rnd() - 0.5);
    out[i * 2] = Math.round(l);
    out[i * 2 + 1] = Math.round(r);
  }
  return out;
}

function shifted(samples: Int16Array, lag: number): Int16Array {
  const out = new Int16Array(samples.length);
  const n = samples.length / 2;
  for (let i = 0; i < n; i++) {
    const j = i - lag;
    if (j >= 0 && j < n) {
      out[i * 2] = samples[j * 2] as number;
      out[i * 2 + 1] = samples[j * 2 + 1] as number;
    }
  }
  return out;
}

function wavFile(name: string, samples: Int16Array, rate = 32000): string {
  const path = join(scratch, name);
  writeFileSync(path, encodeWav(samples, rate, 2));
  return path;
}

describe("compareWav", () => {
  test("WAV writer/reader round trip with a canonical header", () => {
    const s = signal(100);
    const bytes = encodeWav(s, 32000, 2);
    assert.equal(String.fromCharCode(...bytes.subarray(0, 4)), "RIFF");
    assert.equal(String.fromCharCode(...bytes.subarray(8, 16)), "WAVEfmt ");
    const w = readWav(bytes);
    assert.equal(w.rate, 32000);
    assert.equal(w.channels, 2);
    assert.deepEqual(w.samples, s);
  });

  test("identical files", () => {
    const s = signal(4000);
    const a = wavFile("same-a.wav", s);
    const b = wavFile("same-b.wav", s);
    const r = compareWav(a, b);
    assert.equal(r.byteEqual, true);
    assert.equal(r.lagSamples, 0);
    assert.equal(r.lagSeconds, 0);
    assert.equal(r.errorRms, 0);
    assert.equal(r.maxError, 0);
    assert.equal(r.snrDb, Number.POSITIVE_INFINITY);
    assert.ok(Math.abs(r.correlation - 1) < 1e-12);
    assert.equal(r.withinOneLsbPct, 100);
    assert.equal(r.refRate, 32000);
    assert.equal(r.candRate, 32000);
    assert.equal(r.refPeak, r.candPeak);
    assert.equal(r.refRmsDb, r.candRmsDb);
    assert.ok(r.refRmsDb < 0 && r.refRmsDb > -30);
  });

  test("known lag shift is found (direct search)", () => {
    const s = signal(6000, 4);
    const ref = wavFile("lag-ref.wav", s);
    for (const lag of [37, -12, 0]) {
      const r = compareWav(ref, wavFile(`lag-${lag}.wav`, shifted(s, lag)), { maxLagSamples: 200 });
      assert.equal(r.lagSamples, lag, `lag ${lag}`);
      assert.equal(r.lagSeconds, lag / 32000);
      assert.ok(r.correlation > 0.999, `correlation ${r.correlation}`);
      assert.equal(r.maxError, 0);
      assert.equal(r.byteEqual, lag === 0);
    }
  });

  test("known lag shift is found (FFT search on a long signal)", () => {
    const s = signal(200_000, 9);
    const r = compareWav(wavFile("long-ref.wav", s), wavFile("long-cand.wav", shifted(s, 1500)));
    assert.equal(r.lagSamples, 1500);
    assert.equal(r.maxError, 0);
    assert.equal(r.samplesCompared, 200_000 - 1500);
  });

  test("explicit lag and disabled alignment", () => {
    const s = signal(3000, 5);
    const ref = wavFile("explicit-ref.wav", s);
    const cand = wavFile("explicit-cand.wav", shifted(s, 20));
    assert.equal(compareWav(ref, cand, { lag: 20 }).maxError, 0);
    const none = compareWav(ref, cand, { maxLagSamples: 0 });
    assert.equal(none.lagSamples, 0);
    assert.ok(none.maxError > 0);
  });

  test("scaled gain: aligned, correlated, level difference visible", () => {
    const s = signal(5000, 6);
    const half = s.map((v) => Math.round(v * 0.5));
    const r = compareWav(wavFile("gain-ref.wav", s), wavFile("gain-cand.wav", half));
    assert.equal(r.lagSamples, 0);
    assert.ok(r.correlation > 0.9999);
    assert.ok(Math.abs(r.candPeak / r.refPeak - 0.5) < 0.01);
    assert.ok(Math.abs(r.refRmsDb - r.candRmsDb - 6.02) < 0.05, `${r.refRmsDb} ${r.candRmsDb}`);
    // error is half the reference: snr = 6.02 dB
    assert.ok(Math.abs(r.snrDb - 6.02) < 0.1, `snr ${r.snrDb}`);
    assert.equal(r.byteEqual, false);
    assert.ok(r.withinOneLsbPct < 100);
  });

  test("silence", () => {
    const zero = new Int16Array(2000);
    const a = wavFile("silent-a.wav", zero);
    const b = wavFile("silent-b.wav", zero);
    const r = compareWav(a, b);
    assert.equal(r.byteEqual, true);
    assert.equal(r.refRmsDb, Number.NEGATIVE_INFINITY);
    assert.equal(r.refPeak, 0);
    assert.equal(r.correlation, 1);
    assert.equal(r.snrDb, Number.POSITIVE_INFINITY);
    assert.equal(r.lagSamples, 0);
    const vs = compareWav(a, wavFile("tone.wav", signal(2000)));
    assert.equal(vs.correlation, 0);
    assert.equal(vs.byteEqual, false);
    assert.equal(vs.snrDb, Number.NEGATIVE_INFINITY);
  });

  test("window selection", () => {
    const s = signal(8000, 7);
    const bad = s.slice();
    for (let i = 4000; i < 8000; i++) bad[i * 2] = 0; // corrupt the second half of the left channel
    const ref = wavFile("win-ref.wav", s, 8000);
    const cand = wavFile("win-cand.wav", bad, 8000);
    const first = compareWav(ref, cand, { startSec: 0, endSec: 0.5, maxLagSamples: 0 });
    assert.equal(first.maxError, 0);
    assert.equal(first.samplesCompared, 4000);
    const second = compareWav(ref, cand, { startSec: 0.5, maxLagSamples: 0 });
    assert.ok(second.maxError > 1000);
    assert.throws(() => compareWav(ref, cand, { startSec: 2 }), /empty comparison window/);
  });

  test("rate and format mismatches", () => {
    const s = signal(500);
    const a = wavFile("rate-a.wav", s, 32000);
    const b = wavFile("rate-b.wav", s, 44100);
    assert.throws(() => compareWav(a, b), /sample rate differs/);
    assert.equal(compareWav(a, b, { allowRateMismatch: true }).byteEqual, false);
    const junk = join(scratch, "junk.wav");
    writeFileSync(junk, "not a wav file at all");
    assert.throws(() => compareWav(a, junk), /not a RIFF\/WAVE/);
  });

  test("a streaming header (data size beyond the file) is clamped", () => {
    const bytes = encodeWav(signal(300), 32000, 2);
    new DataView(bytes.buffer).setUint32(40, 0xffffffff, true);
    assert.equal(readWav(bytes).samples.length, 600);
  });
});

// ---- compareImage --------------------------------------------------------------------------------

const crcTable = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(bytes: Uint8Array): number {
  let c = 0xffffffff;
  for (const b of bytes) c = (crcTable[(c ^ b) & 0xff] as number) ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type: string, data: Uint8Array): Uint8Array {
  const out = new Uint8Array(12 + data.length);
  const v = new DataView(out.buffer);
  v.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  v.setUint32(8 + data.length, crc32(out.subarray(4, 8 + data.length)));
  return out;
}

const paethPredictor = (a: number, b: number, c: number) => {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
};

/** Encode raw samples (`channels` bytes per pixel, rows top to bottom) as PNG, cycling the five filter types per row. */
function encodePng(width: number, height: number, colorType: number, channels: number, raw: Uint8Array, palette?: Uint8Array): Uint8Array {
  const stride = width * channels;
  const rows = new Uint8Array(height * (stride + 1));
  for (let y = 0; y < height; y++) {
    const filter = y % 5;
    rows[y * (stride + 1)] = filter;
    for (let x = 0; x < stride; x++) {
      const a = x >= channels ? (raw[y * stride + x - channels] as number) : 0;
      const b = y > 0 ? (raw[(y - 1) * stride + x] as number) : 0;
      const c = y > 0 && x >= channels ? (raw[(y - 1) * stride + x - channels] as number) : 0;
      const pred = [0, a, b, (a + b) >> 1, paethPredictor(a, b, c)][filter] as number;
      rows[y * (stride + 1) + 1 + x] = ((raw[y * stride + x] as number) - pred) & 0xff;
    }
  }
  const ihdr = new Uint8Array(13);
  const v = new DataView(ihdr.buffer);
  v.setUint32(0, width);
  v.setUint32(4, height);
  ihdr[8] = 8;
  ihdr[9] = colorType;
  const parts = [new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), pngChunk("IHDR", ihdr)];
  if (palette) parts.push(pngChunk("PLTE", palette));
  parts.push(pngChunk("IDAT", new Uint8Array(deflateSync(rows))), pngChunk("IEND", new Uint8Array(0)));
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

function gradient(width: number, height: number): ImageData8 {
  const rgba = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const o = (y * width + x) * 4;
      rgba[o] = (x * 37 + y * 11) & 0xff;
      rgba[o + 1] = (x * 5 + y * 53) & 0xff;
      rgba[o + 2] = (x * y + 7) & 0xff;
      rgba[o + 3] = 255;
    }
  }
  return { width, height, rgba };
}

function rgbBytes(img: ImageData8): Uint8Array {
  const out = new Uint8Array(img.width * img.height * 3);
  for (let i = 0; i < img.width * img.height; i++) {
    out[i * 3] = img.rgba[i * 4] as number;
    out[i * 3 + 1] = img.rgba[i * 4 + 1] as number;
    out[i * 3 + 2] = img.rgba[i * 4 + 2] as number;
  }
  return out;
}

function pngFile(name: string, img: ImageData8): string {
  const path = join(scratch, name);
  writeFileSync(path, encodePng(img.width, img.height, 2, 3, rgbBytes(img)));
  return path;
}

function bmpFile(name: string, img: ImageData8): string {
  const path = join(scratch, name);
  writeFileSync(path, encodeBmp(img));
  return path;
}

describe("compareImage", () => {
  const img = gradient(13, 11); // odd width exercises BMP row padding
  const imgB = { ...img, rgba: img.rgba.slice() };
  imgB.rgba[(5 * 13 + 4) * 4] = (imgB.rgba[(5 * 13 + 4) * 4] as number) ^ 0x40; // one pixel, one channel, differs by 64

  test("PNG decoder handles all five filters and every 8-bit color type", () => {
    assert.deepEqual(decodePng(encodePng(13, 11, 2, 3, rgbBytes(img))), img);
    assert.deepEqual(decodePng(encodePng(13, 11, 6, 4, img.rgba)), img);
    const gray = new Uint8Array(13 * 11).map((_, i) => (i * 7) & 0xff);
    const g = decodePng(encodePng(13, 11, 0, 1, gray));
    assert.equal(g.rgba[4 * 6], gray[6]);
    assert.equal(g.rgba[4 * 6 + 2], gray[6]);
    const ga = new Uint8Array(13 * 11 * 2).map((_, i) => (i * 3) & 0xff);
    assert.equal(decodePng(encodePng(13, 11, 4, 2, ga)).rgba[3], ga[1]);
    const palette = new Uint8Array([255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 8, 7]);
    const idx = new Uint8Array(13 * 11).map((_, i) => i % 4);
    const p = decodePng(encodePng(13, 11, 3, 1, idx, palette));
    assert.deepEqual([...p.rgba.subarray(4, 8)], [0, 255, 0, 255]);
    assert.deepEqual([...p.rgba.subarray(12, 16)], [9, 8, 7, 255]);
  });

  test("PNG errors", () => {
    assert.throws(() => decodePng(new Uint8Array([1, 2, 3])), /not a PNG/);
    const png = encodePng(2, 2, 2, 3, new Uint8Array(12));
    png[24] = 16; // IHDR bit depth
    assert.throws(() => decodePng(png), /8-bit/);
  });

  test("identical PNGs", () => {
    const r = compareImage(pngFile("same-a.png", img), pngFile("same-b.png", img));
    assert.equal(r.exactMatch, true);
    assert.equal(r.mismatchedPixels, 0);
    assert.equal(r.mismatchPct, 0);
    assert.equal(r.maxChannelDiff, 0);
    assert.equal(r.mae, 0);
    assert.equal(r.psnrDb, Number.POSITIVE_INFINITY);
    assert.equal(r.width, 13);
    assert.equal(r.height, 11);
    assert.equal(r.totalPixels, 143);
  });

  test("a one pixel difference", () => {
    const diffPath = join(scratch, "one-pixel-diff.bmp");
    const r = compareImage(pngFile("one-a.png", img), pngFile("one-b.png", imgB), { diffPath });
    assert.equal(r.exactMatch, false);
    assert.equal(r.mismatchedPixels, 1);
    assert.ok(Math.abs(r.mismatchPct - 100 / 143) < 1e-9);
    assert.equal(r.maxChannelDiff, 64);
    assert.ok(Math.abs(r.mae - 64 / (143 * 3)) < 1e-12);
    assert.ok(Math.abs(r.psnrDb - 10 * Math.log10((255 * 255) / ((64 * 64) / (143 * 3)))) < 1e-9);
    const d = decodeBmp(diffPath);
    assert.deepEqual([...d.rgba.subarray((5 * 13 + 4) * 4, (5 * 13 + 4) * 4 + 3)], [255, 0, 0]);
    assert.notDeepEqual([...d.rgba.subarray(0, 3)], [255, 0, 0]);
  });

  test("tolerance and alpha", () => {
    const a = pngFile("tol-a.png", img);
    const b = pngFile("tol-b.png", imgB);
    assert.equal(compareImage(a, b, { tolerance: 64 }).mismatchedPixels, 0);
    assert.equal(compareImage(a, b, { tolerance: 63 }).mismatchedPixels, 1);
    const withAlpha = { ...img, rgba: img.rgba.slice() };
    withAlpha.rgba[3] = 0;
    assert.equal(compareImage(img, withAlpha).exactMatch, true);
    assert.equal(compareImage(img, withAlpha, { alpha: true }).mismatchedPixels, 1);
  });

  test("identical BMPs, including 32-bit top-down", () => {
    const r = compareImage(bmpFile("same-a.bmp", img), bmpFile("same-b.bmp", img));
    assert.equal(r.exactMatch, true);
    assert.deepEqual(decodeBmp(encodeBmp(img)), img);
    // hand-built 2x2 32-bit BI_RGB top-down BMP (BGRA)
    const bmp = new Uint8Array(54 + 16);
    const v = new DataView(bmp.buffer);
    bmp[0] = 0x42;
    bmp[1] = 0x4d;
    v.setUint32(2, bmp.length, true);
    v.setUint32(10, 54, true);
    v.setUint32(14, 40, true);
    v.setInt32(18, 2, true);
    v.setInt32(22, -2, true);
    v.setUint16(26, 1, true);
    v.setUint16(28, 32, true);
    bmp.set([3, 2, 1, 0, 6, 5, 4, 0, 9, 8, 7, 0, 12, 11, 10, 0], 54);
    const d = decodeBmp(bmp);
    assert.deepEqual([...d.rgba], [1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255]);
  });

  test("cross-format PNG vs BMP", () => {
    assert.equal(compareImage(pngFile("x.png", img), bmpFile("x.bmp", img)).exactMatch, true);
    const r = compareImage(bmpFile("y.bmp", img), pngFile("y.png", imgB));
    assert.equal(r.mismatchedPixels, 1);
    assert.equal(r.maxChannelDiff, 64);
  });

  test("dimension mismatch is an error", () => {
    const small = gradient(12, 11);
    assert.throws(() => compareImage(pngFile("dim-a.png", img), pngFile("dim-b.png", small)), /dimensions differ: 13x11 vs 12x11/);
    assert.throws(() => compareImage(bmpFile("dim-a.bmp", img), pngFile("dim-c.png", small)), /dimensions differ/);
  });

  test("unknown formats are rejected", () => {
    const path = join(scratch, "x.txt");
    writeFileSync(path, "hello");
    assert.throws(() => compareImage(path, path), /unrecognized image format/);
  });
});

// ---- RunHandle dump readers (synthetic dumps) -----------------------------------------------------

describe("RunHandle dump reader", () => {
  const dir = join(scratch, "run");
  const frameDir = (n: number) => join(dir, "dumps", `frame_${String(n).padStart(4, "0")}`);

  before(() => {
    for (const n of [1, 2, 5]) {
      const ram = new Uint8Array(0x20000);
      for (let i = 0; i < ram.length; i++) ram[i] = (i * 3 + n) & 0xff;
      ram.set([0x12, 0x34, 0x56, 0x78], 0x1000); // 0x401000
      ram.set([0x00, n], 0x2000); // word counter at 0x402000
      for (let k = 0; k < 4; k++) ram.set([0xa0 + k, k, 0xff, 0xee], 0x3000 + k * 8); // 4 records of 8 bytes at 0x403000
      mkdirSync(frameDir(n), { recursive: true });
      writeFileSync(join(frameDir(n), "mainram.bin"), ram);
    }
    writeFileSync(join(dir, "game"), "commandw\n");
  });

  test("frames are listed in order", async () => {
    const run = await openRun(dir, undefined, { a5: 0x40f000 });
    assert.equal(run.gameId, "commandw");
    assert.deepEqual(run.frames(), [1, 2, 5]);
    assert.equal(run.dumpDir, join(dir, "dumps"));
  });

  test("mem, b, w and l", async () => {
    const run = await openRun(dir, "commandw", { a5: 0x40f000 });
    assert.deepEqual([...run.mem(1, 0x401000, 4)], [0x12, 0x34, 0x56, 0x78]);
    assert.equal(run.b(1, 0x401001), 0x34);
    assert.equal(run.w(1, 0x401000), 0x1234);
    assert.equal(run.l(1, 0x401000), 0x12345678);
    assert.equal(run.l(1, "0x401000"), 0x12345678);
    assert.equal(run.l(1, "$401000"), 0x12345678);
    assert.equal(run.mem(1, 0x401000).length, 1);
    // high-bit longs stay unsigned
    assert.equal(run.l(1, 0x403000), 0xa000ffee);
  });

  test("work RAM mirrors fold and a5 offsets resolve", async () => {
    const run = await openRun(dir, "commandw", { a5: 0x40f000 });
    assert.equal(run.w(1, 0x421000), 0x1234);
    assert.equal(run.w(1, "a5-0xe000"), 0x1234); // 0x40f000 - 0xe000 = 0x401000
    assert.equal(run.w(1, "a5 - 0xd000"), run.w(1, 0x402000));
    assert.equal(run.address("a5+0x10"), 0x40f010);
    const bare = await openRun(dir, "commandw");
    assert.throws(() => bare.w(1, "a5-0x10"), /a5 is unknown/);
  });

  test("series over every dumped frame or chosen frames", async () => {
    const run = await openRun(dir, "commandw");
    assert.deepEqual(run.series(0x402000, 2), [
      [1, 1],
      [2, 2],
      [5, 5],
    ]);
    assert.deepEqual(run.series(0x402001, 1, [5, 1]), [
      [5, 5],
      [1, 1],
    ]);
    assert.deepEqual(run.series(0x401000, 4)[0], [1, 0x12345678]);
    assert.throws(() => run.series(0x401000, 7), /1\.\.6/);
  });

  test("table records", async () => {
    const run = await openRun(dir, "commandw");
    const rows = run.table(0x403000, 8, 4, 2);
    assert.equal(rows.length, 4);
    assert.equal(rows[2]?.index, 2);
    assert.equal(rows[2]?.frame, 2);
    assert.equal(rows[2]?.address, 0x403010);
    assert.equal(rows[2]?.b(0), 0xa2);
    assert.equal(rows[2]?.b(1), 2);
    assert.equal(rows[2]?.w(2), 0xffee);
    assert.equal(rows[3]?.l(0), 0xa303ffee);
    assert.equal(rows[0]?.bytes.length, 8);
    assert.throws(() => rows[0]?.l(6), RangeError);
    assert.equal(run.table(0x403000, 8, 2, 1, 2)[1]?.bytes.length, 2);
  });

  test("errors name the problem", async () => {
    const run = await openRun(dir, "commandw");
    assert.throws(() => run.mem(3, 0x401000), /frame 3 is not dumped/);
    assert.throws(() => run.mem(1, 0x660000), /frame 1 has no control\.bin/);
    assert.throws(() => run.mem(1, 0x800000), /not in a dump file/);
    assert.throws(() => run.mem(1, 0x41ffff, 4), /ends before/);
    assert.throws(() => run.mem(1, "zzz"), /not a hexadecimal number/);
  });

  test("keeps run.txt output, summary and CRCs", async () => {
    const d = join(scratch, "run-summary");
    mkdirSync(d, { recursive: true });
    writeFileSync(join(d, "run.txt"), "set=x frames=30 frame_crc=0x2aa9b3cc audio_frames=15147\nFINAL state_crc=0xa0e66697\n");
    const run = await openRun(d, "commandw");
    assert.equal(run.frameCrc, "0x2aa9b3cc");
    assert.equal(run.stateCrc, "0xa0e66697");
    assert.equal(run.summary.audio_frames, "15147");
    assert.deepEqual(run.frames(), []);
    assert.equal(run.dumpDir, null);
  });
});

describe("decodeSpriteList", () => {
  function entry(ram: Uint8Array, bank: number, slot: number, words: number[]): void {
    const dv = new DataView(ram.buffer);
    words.forEach((w, i) => dv.setUint16(bank * 0x8000 + slot * 16 + i * 2, w));
  }

  test("walks entries, positions and list end", () => {
    const ram = new Uint8Array(0x10000);
    entry(ram, 0, 0, [5, 0, 10, 20, 0x0012, 0, 0, 0]);
    entry(ram, 0, 1, [0x77, 0, 4, 6, 0x0034, 1, 0, 0]); // tile 0x10077, relative to nothing special
    entry(ram, 0, 2, [0, 0, 0, 0, 0, 0, 0x8002, 0]); // jump to itself: end
    const { entries, summary } = decodeSpriteList(ram);
    assert.equal(entries.length, 3);
    assert.equal(entries[0]?.tile, 5);
    assert.equal(entries[0]?.color, 0x12);
    assert.equal(entries[0]?.x, 10);
    assert.equal(entries[0]?.y, 20);
    assert.equal(entries[0]?.scaleX, 256);
    assert.equal(entries[0]?.address, 0x600000);
    assert.equal(entries[1]?.tile, 0x10077);
    assert.equal(entries[1]?.address, 0x600010);
    assert.equal(entries[2]?.drawn, false);
    assert.equal(summary.end, 2);
    assert.equal(summary.drawn, 2);
    assert.deepEqual(summary.banksRead, [0]);
    assert.deepEqual(summary.lastUsed, [0, 2]); // the end entry's jump word counts as used
  });

  test("jumps, bank commands and signed 12-bit positions", () => {
    const ram = new Uint8Array(0x10000);
    entry(ram, 0, 0, [0, 0, 0, 0x8000, 0, 1, 0, 0]); // command: switch to bank 1
    entry(ram, 1, 1, [9, 0x0100, 0xfff0 & 0x0fff, 0xfff & 0x0fff, 0, 0, 0x8005, 0]); // x = -16, y = -1, jump to 5
    entry(ram, 1, 5, [0, 0, 0, 0, 0, 0, 0x8005, 0]); // end
    const { entries, summary } = decodeSpriteList(ram);
    assert.equal(summary.commands, 1);
    assert.deepEqual(summary.switches, [[0, 0, 1]]);
    assert.equal(summary.jumps, 1);
    assert.deepEqual(summary.banksRead, [0, 1]);
    const moved = entries[1] as NonNullable<(typeof entries)[number]>;
    assert.equal(moved.bank, 1);
    assert.equal(moved.x, -16);
    assert.equal(moved.y, -1);
    assert.equal(moved.scaleY, 0x100 - 1);
    assert.ok(moved.notes.some((n) => n.startsWith("jump to 0x005")));
    assert.equal(summary.end, 5);
  });
});

// ---- real ROM integration -------------------------------------------------------------------------

suiteIf(haveRom("commandw"), "commandw integration", () => {
  let game: Game;
  before(async () => {
    game = await openGame("commandw");
  });
  after(async () => {
    await game.close();
  });

  test("resolves config, ROM directory and the default profile", () => {
    assert.equal(game.id, "commandw");
    assert.equal(game.config, join(REPO_ROOT, "games", "commandw", "config.toml"));
    assert.equal(game.romDir, join(REPO_ROOT, "roms", "commandw"));
  });

  test("readRom: main and sound regions, words and longs", async () => {
    const main = await game.readRom("rom", 0, 16);
    assert.ok(main instanceof Uint8Array);
    assert.equal(main.length, 16);
    assert.equal(main.region, "rom");
    assert.equal(main.regionSize, 0x100000);
    const sound = await game.readRom("sound", 0, 16);
    assert.equal(sound.region, "sound");
    assert.equal(sound.regionSize, 0x80000);
    const longs = await game.longs(0, 2);
    assert.equal(longs[0], new DataView(main.buffer, main.byteOffset).getUint32(0));
    assert.equal(longs[1], new DataView(main.buffer, main.byteOffset).getUint32(4));
    const words = await game.words(0, 4);
    assert.equal(words[0], (main[0] as number) * 256 + (main[1] as number));
    const signedWords = await game.words(0, 4, { signed: true });
    assert.ok(signedWords.every((w, i) => w === ((words[i] as number) << 16) >> 16));
    const soundLongs = await game.longs(0, 2, { region: "sound" });
    assert.equal(soundLongs[0], new DataView(sound.buffer, sound.byteOffset).getUint32(0));
    await assert.rejects(game.readRom("nonsense", 0, 4), RecompError);
  });

  test("disasm: count, to, sound region, raw hex", async () => {
    const five = await game.disasm(0x400, 5);
    assert.equal(five.length, 5);
    assert.equal(five[0]?.pc, 0x400);
    const upto = await game.disasm({ pc: 0x400, to: 0x410 });
    assert.ok(upto.length > 0 && upto.every((l) => l.pc < 0x410));
    const snd = await game.disasm({ pc: 0, region: "sound", count: 2 });
    assert.equal(snd[0]?.pc, 0xc00000);
    const raw = await game.disasm({ pc: 0x2000, hex: "4e75", count: 1 });
    assert.equal(insn(raw[0]).mnemonic, "rts");
  });

  test("inspect, vectors, routines, ramCode, unresolvedIndirect", async () => {
    const s = await game.inspect({ summary: true });
    assert.equal(s.game, "commandw");
    assert.equal(s.rom_size, 0x100000);
    assert.equal(s.routines, undefined);
    assert.match(s.rom_crc32, /^[0-9a-f]{8}$/);
    assert.ok(typeof s.a5 === "number");
    assert.equal(await game.a5(), s.a5);
    const full = await game.inspect();
    assert.equal(full.routines?.length, full.routine_count);
    const reset = (await game.vectors()).find((v) => v.name === "reset");
    assert.equal(reset?.number, 1);
    assert.ok(reset?.valid);
    const routines = await game.routines();
    assert.ok(routines.some((r) => r.vectors.includes("irq2")));
    assert.ok(Array.isArray(await game.ramCode()));
    assert.ok(Array.isArray(await game.unresolvedIndirect()));
    assert.equal((await game.inspect({ summary: true, a5: 0x400000 })).a5, 0x400000);
  });

  test("profile handling", async () => {
    const withDefault = await game.inspect({ summary: true });
    const without = await game.inspect({ summary: true, profile: false });
    assert.equal(without.profile, null);
    if (existsSync(join(REPO_ROOT, "profiles", "commandw.profile"))) {
      assert.ok(withDefault.profile && withDefault.profile.executed > 0);
      const explicit = await game.inspect({ summary: true, profile: join(REPO_ROOT, "profiles", "commandw.profile") });
      assert.deepEqual(explicit.profile, withDefault.profile);
    }
  });

  test("xref by region, address range, a5 form and number", async () => {
    const sprites = await game.xref("sprites", { limit: 3 });
    assert.equal(sprites.match.kind, "region:sprites");
    assert.ok(sprites.refs.length <= 3 && sprites.total >= sprites.refs.length);
    const writes = await game.xref("sprites", { role: "write", limit: 50 });
    assert.ok(writes.refs.every((r) => r.role === "write" || r.role === "rmw"));
    const a5 = await game.xref("a5-0x60ac", { limit: 5 });
    assert.equal(a5.match.kind, "a5");
    const num = await game.xref(0x600000, { limit: 2 });
    assert.equal(num.match.kind, "address");
    const range = await game.xref("600000-60ffff", { limit: 2 });
    assert.equal(range.match.kind, "range");
  });

  test("flow by vector name, pc and ?dispatch", async () => {
    const irq2 = (await game.flow("irq2", { depth: 1 })) as { name: string; vectors: string[]; kind: string; entry: number };
    assert.ok(irq2.vectors.includes("irq2"));
    assert.equal(irq2.kind, "vector");
    const byPc = (await game.flow(irq2.entry, { depth: 0 })) as { entry: number; exact: boolean };
    assert.equal(byPc.entry, irq2.entry);
    assert.equal(byPc.exact, true);
    const dispatch = (await game.flow("?dispatch")) as { dispatch?: true; callers: unknown[] };
    assert.equal(dispatch.dispatch, true);
    assert.ok(Array.isArray(dispatch.callers));
    await assert.rejects(game.flow("not-a-target"), RecompError);
  });

  test("coverage, checkIrq, decisions", async () => {
    const cov = await game.coverage();
    assert.equal(cov.game, "commandw");
    assert.ok(cov.total >= 0 && typeof cov.unsupported === "object");
    const irq = await game.checkIrq();
    assert.ok(irq.handlers.length > 0);
    assert.ok(irq.handlers.every((h) => h.verdict === "proven" || h.verdict === "unknown"));
    const dec = await game.decisions({ spriteOnly: true, profile: false }); // the full table has ~500k rows and takes 30 s
    assert.equal(dec.game, "commandw");
    assert.equal(dec.sprite_only, true);
    assert.deepEqual(dec.columns.slice(0, 2), ["pc", "disposition"]);
    assert.ok(dec.rows.length > 0);
  });

  test("lower: from the ROM and from hex", async () => {
    const rom = await game.lower({ pc: 0x400, count: 1 });
    assert.equal(rom.instructions.length, 1);
    assert.ok("c" in (rom.instructions[0] as object));
    const hex = await game.lower({ pc: 0x500, hex: "7001", liveOut: "" });
    assert.equal(hex.live_out, "");
    assert.ok("c" in (hex.instructions[0] as object));
  });

  test("repeated analysis calls hit the server cache", async () => {
    await game.xref("a5-0x60ac");
    const t0 = performance.now();
    for (let i = 0; i < 5; i++) await game.xref("a5-0x60ac", { limit: 10 });
    assert.ok(performance.now() - t0 < 1500, `5 cached xrefs took ${performance.now() - t0} ms`);
  });

  test("a shared client serves several games and unknown games fail cleanly", async () => {
    const client = game.client;
    const other = client.game("commandw", { profile: false });
    assert.equal((await other.inspect({ summary: true })).profile, null);
    await other.close(); // evicts only
    assert.deepEqual(await client.ping(), { pong: true });
    await assert.rejects(openGame("no-such-game", { client }), /no config for game/);
    await assert.rejects(openGame("commandw", { client, romDir: join(scratch, "no-roms") }), /no ROM directory/);
  });

  testIf(haveTool("commandw"), "f3rt-tool gameplay --json run", async () => {
    const out = join(scratch, "gameplay");
    const a = await game.run({ mode: "gameplay", frames: 60, seed: 1, out });
    assert.equal(a.ok, true, a.output);
    assert.equal(a.json?.status, "SUCCESS");
    assert.match(a.frameCrc ?? "", /^0x[0-9a-f]{8}$/);
    assert.ok((a.audioFrames ?? 0) > 0);
    assert.equal(a.framesRun, 60);
    const b = await game.run({ mode: "gameplay", frames: 60, seed: 1, out: join(scratch, "gameplay2") });
    assert.equal(b.frameCrc, a.frameCrc);
    const failed = await game.run({ mode: "gameplay", frames: 10, out: join(scratch, "gameplay3"), args: ["--bogus-flag"] });
    assert.equal(failed.ok, false);
  });

  testIf(haveBinary("commandw"), "headless game run with dumps, WAV and dump readers", async () => {
    const out = join(scratch, "headless");
    const run = await game.run({ frames: 40, dumpStart: 38, dumpEvery: 1, keep: ["mainram.bin", "graphics.bin"], wav: true, out });
    assert.equal(run.ok, true, run.output);
    assert.match(run.frameCrc ?? "", /^0x[0-9a-f]{8}$/);
    assert.match(run.stateCrc ?? "", /^0x[0-9a-f]{8}$/);
    assert.ok((run.audioFrames ?? 0) > 0);
    assert.equal(run.wavPath, join(out, "audio.wav"));
    assert.equal(readWav(run.wavPath as string).rate > 0, true);
    assert.deepEqual(run.frames(), [38, 39, 40]);
    assert.ok(!existsSync(run.framePath(38, "palette.bin")));
    assert.ok(existsSync(run.framePath(38, "cpu.json")));
    assert.equal(run.mem(40, 0x400000, 16).length, 16);
    assert.equal(run.series(0x400000, 2).length, 3);
    assert.equal(run.table(0x400000, 16, 4, 39).length, 4);
    assert.ok(run.sprites(39).entries.length > 0);
    assert.equal(typeof run.b(39, "a5-0x60ac"), "number");
    // the run directory can be reopened
    const reopened = await openRun(out);
    assert.equal(reopened.gameId, "commandw");
    assert.equal(reopened.frameCrc, run.frameCrc);
    assert.deepEqual(reopened.frames(), [38, 39, 40]);
    // the same run twice is deterministic (dumping flushes lazy flags, so state_crc is only comparable between dumping runs)
    const again = await game.run({ frames: 40, dumpStart: 38, dumpEvery: 1, keep: "cpu.json", wav: true, out: join(scratch, "headless2") });
    assert.equal(again.frameCrc, run.frameCrc);
    assert.equal(again.stateCrc, run.stateCrc);
  });

  testIf(haveTool("commandw"), "spriteCheck and motionProof return typed results", async () => {
    const sc = await game.spriteCheck({ frames: 60, compareEvery: 30 });
    assert.ok(sc.status === "PASS" || sc.status === "FAIL");
    assert.equal(sc.ok, sc.status === "PASS");
    assert.equal(typeof sc.state_crc, "string");
    const mp = await game.motionProof({ frames: 60 });
    assert.ok(mp.status === "SUCCESS" || mp.status === "FAILURE");
    assert.equal(mp.ok, mp.status === "SUCCESS");
    await assert.rejects(game.spriteCheck({ args: ["--help"] }), /no JSON/);
  });
});

suiteIf(haveRom("landmakrj"), "landmakrj integration", () => {
  let game: Game;
  before(async () => {
    game = await openGame("landmakrj");
  });
  after(async () => {
    await game.close();
  });

  test("inspect, disasm and xref", async () => {
    const s = await game.inspect({ summary: true });
    assert.equal(s.game, "landmakrj");
    assert.ok(s.routine_count > 100);
    assert.equal(insn((await game.disasm(0x400, 1))[0]).pc, 0x400);
    const x = await game.xref("sprites", { limit: 2 });
    assert.ok(x.total > 0);
  });

  test("irq flow and IRQ proof", async () => {
    const f = (await game.flow("irq2", { depth: 1 })) as { vectors: string[] };
    assert.ok(f.vectors.includes("irq2"));
    assert.ok((await game.checkIrq()).handlers.length > 0);
  });
});
