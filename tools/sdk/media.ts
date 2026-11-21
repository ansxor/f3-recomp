import { readFileSync, writeFileSync } from "node:fs";
import { inflateSync } from "node:zlib";
import type {
  ImageCompareOptions,
  ImageCompareResult,
  ImageData8,
  WavCompareOptions,
  WavCompareResult,
} from "./types.ts";

// ---- WAV ----------------------------------------------------------------------------------------

export interface WavData {
  rate: number;
  channels: number;
  /** Interleaved 16-bit samples. */
  samples: Int16Array;
  /** The raw bytes of the data chunk (as far as the file holds them). */
  data: Uint8Array;
}

/** Parse a 16-bit PCM WAV (any channel count). A data chunk longer than the file is clamped (streaming writers). */
export function readWav(source: string | Uint8Array): WavData {
  const bytes = typeof source === "string" ? new Uint8Array(readFileSync(source)) : source;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const tag = (o: number) => String.fromCharCode(...bytes.subarray(o, o + 4));
  if (bytes.length < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE") throw new Error("not a RIFF/WAVE file");
  let rate = 0;
  let channels = 0;
  let bits = 0;
  let format = 0;
  let data: Uint8Array | null = null;
  let offset = 12;
  while (offset + 8 <= bytes.length) {
    const id = tag(offset);
    let size = view.getUint32(offset + 4, true);
    const body = offset + 8;
    if (body + size > bytes.length) size = bytes.length - body;
    if (id === "fmt ") {
      format = view.getUint16(body, true);
      channels = view.getUint16(body + 2, true);
      rate = view.getUint32(body + 4, true);
      bits = view.getUint16(body + 14, true);
    } else if (id === "data") {
      data = bytes.subarray(body, body + size);
      break;
    }
    offset = body + size + (size & 1);
  }
  if (!rate || !channels) throw new Error("WAV has no fmt chunk");
  if (!data) throw new Error("WAV has no data chunk");
  if ((format !== 1 && format !== 0xfffe) || bits !== 16) throw new Error(`only 16-bit PCM WAV is supported (format ${format}, ${bits} bits)`);
  const frames = Math.floor(data.length / (2 * channels));
  const samples = new Int16Array(frames * channels);
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  for (let i = 0; i < samples.length; i++) samples[i] = dv.getInt16(i * 2, true);
  return { rate, channels, samples, data };
}

/** Encode 16-bit PCM samples (interleaved) as a WAV file. */
export function encodeWav(samples: Int16Array, rate: number, channels = 2): Uint8Array {
  const out = new Uint8Array(44 + samples.length * 2);
  const v = new DataView(out.buffer);
  const put = (o: number, s: string) => [...s].forEach((c, i) => (out[o + i] = c.charCodeAt(0)));
  put(0, "RIFF");
  v.setUint32(4, 36 + samples.length * 2, true);
  put(8, "WAVEfmt ");
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true);
  v.setUint16(22, channels, true);
  v.setUint32(24, rate, true);
  v.setUint32(28, rate * channels * 2, true);
  v.setUint16(32, channels * 2, true);
  v.setUint16(34, 16, true);
  put(36, "data");
  v.setUint32(40, samples.length * 2, true);
  for (let i = 0; i < samples.length; i++) v.setInt16(44 + i * 2, samples[i] as number, true);
  return out;
}

function fft(re: Float64Array, im: Float64Array, inverse: boolean): void {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j] as number, re[i] as number];
      [im[i], im[j]] = [im[j] as number, im[i] as number];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = ((inverse ? 2 : -2) * Math.PI) / len;
    const wr = Math.cos(ang);
    const wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1;
      let ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const a = i + k;
        const b = a + len / 2;
        const tr = (re[b] as number) * cr - (im[b] as number) * ci;
        const ti = (re[b] as number) * ci + (im[b] as number) * cr;
        re[b] = (re[a] as number) - tr;
        im[b] = (im[a] as number) - ti;
        re[a] = (re[a] as number) + tr;
        im[a] = (im[a] as number) + ti;
        const nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
  if (inverse) for (let i = 0; i < n; i++) {
    re[i] = (re[i] as number) / n;
    im[i] = (im[i] as number) / n;
  }
}

/** score[k] = sum_i c[i + k] * r[i] for k in 0..2L, by direct summation or FFT. */
function crossCorrelate(r: Float64Array, c: Float64Array, span: number): Float64Array {
  const n = r.length;
  const out = new Float64Array(span + 1);
  if (n * (span + 1) <= 2e7) {
    for (let k = 0; k <= span; k++) {
      let s = 0;
      for (let i = 0; i < n; i++) s += (r[i] as number) * (c[i + k] as number);
      out[k] = s;
    }
    return out;
  }
  let m = 1;
  while (m < c.length) m <<= 1;
  const ar = new Float64Array(m);
  const ai = new Float64Array(m);
  const br = new Float64Array(m);
  const bi = new Float64Array(m);
  ar.set(c);
  br.set(r);
  fft(ar, ai, false);
  fft(br, bi, false);
  for (let i = 0; i < m; i++) {
    // A * conj(B)
    const x = (ar[i] as number) * (br[i] as number) + (ai[i] as number) * (bi[i] as number);
    const y = (ai[i] as number) * (br[i] as number) - (ar[i] as number) * (bi[i] as number);
    ar[i] = x;
    ai[i] = y;
  }
  fft(ar, ai, true);
  for (let k = 0; k <= span; k++) out[k] = ar[k] as number;
  return out;
}

function mono(w: WavData, from: number, to: number): Float64Array {
  const out = new Float64Array(Math.max(0, to - from));
  for (let i = from; i < to; i++) {
    if (i < 0 || i >= w.samples.length / w.channels) continue;
    let s = 0;
    for (let ch = 0; ch < w.channels; ch++) s += w.samples[i * w.channels + ch] as number;
    out[i - from] = s / w.channels;
  }
  return out;
}

function dbfs(rms: number): number {
  return rms > 0 ? 20 * Math.log10(rms / 32768) : Number.NEGATIVE_INFINITY;
}

/**
 * Compare two 16-bit PCM WAVs. The candidate is aligned to the reference by the lag (|lag| <= maxLagSamples,
 * default 2048) that maximizes their cross-correlation; the statistics are over the aligned overlap and are
 * not gain-corrected. `lagSamples` > 0 means the candidate lags the reference: cand[i + lag] ~ ref[i].
 */
export function compareWav(refPath: string | WavData, candPath: string | WavData, opts: WavCompareOptions = {}): WavCompareResult {
  const ref = typeof refPath === "string" ? readWav(refPath) : refPath;
  const cand = typeof candPath === "string" ? readWav(candPath) : candPath;
  if (ref.channels !== cand.channels) throw new Error(`channel count differs (${ref.channels} vs ${cand.channels})`);
  if (ref.rate !== cand.rate && !opts.allowRateMismatch) {
    throw new Error(`sample rate differs (${ref.rate} vs ${cand.rate}); pass allowRateMismatch to compare anyway`);
  }
  const ch = ref.channels;
  const refFrames = ref.samples.length / ch;
  const candFrames = cand.samples.length / ch;
  const start = Math.max(0, Math.floor((opts.startSec ?? 0) * ref.rate));
  const end = Math.min(refFrames, opts.endSec === undefined ? refFrames : Math.floor(opts.endSec * ref.rate));
  if (end <= start) throw new Error("empty comparison window");

  const maxLag = opts.maxLagSamples ?? 2048;
  let lag = 0;
  if (opts.lag !== undefined) lag = opts.lag;
  else if (maxLag > 0) {
    const n = Math.min(end - start, Math.floor((opts.lagWindowSec ?? 20) * ref.rate));
    const r = mono(ref, start, start + n);
    const c = mono(cand, start - maxLag, start + n + maxLag);
    const score = crossCorrelate(r, c, 2 * maxLag);
    let top = Number.NEGATIVE_INFINITY;
    for (let k = 0; k < score.length; k++) top = Math.max(top, score[k] as number);
    let best = maxLag;
    let bestDist = Number.POSITIVE_INFINITY;
    for (let k = 0; k < score.length; k++) {
      // among (nearly) equal peaks prefer the smallest |lag|
      if ((score[k] as number) >= top - 1e-9 * Math.abs(top) && Math.abs(k - maxLag) < bestDist) {
        best = k;
        bestDist = Math.abs(k - maxLag);
      }
    }
    const bestScore = top;
    lag = best - maxLag;
    if (!(bestScore > 0)) lag = 0;
  }

  let refPeak = 0;
  let candPeak = 0;
  let refSq = 0;
  let candSq = 0;
  let errSq = 0;
  let dot = 0;
  let maxErr = 0;
  let within = 0;
  let count = 0;
  for (let i = start; i < end; i++) {
    const j = i + lag;
    if (j < 0 || j >= candFrames) continue;
    for (let c = 0; c < ch; c++) {
      const a = ref.samples[i * ch + c] as number;
      const b = cand.samples[j * ch + c] as number;
      const d = Math.abs(b - a);
      refPeak = Math.max(refPeak, Math.abs(a));
      candPeak = Math.max(candPeak, Math.abs(b));
      refSq += a * a;
      candSq += b * b;
      dot += a * b;
      errSq += d * d;
      if (d > maxErr) maxErr = d;
      if (d <= 1) within++;
      count++;
    }
  }
  if (count === 0) throw new Error("no overlapping samples after alignment");
  const refRms = Math.sqrt(refSq / count);
  const candRms = Math.sqrt(candSq / count);
  const errRms = Math.sqrt(errSq / count);
  let correlation: number;
  if (refSq === 0 || candSq === 0) correlation = refSq === 0 && candSq === 0 ? 1 : 0;
  else correlation = dot / Math.sqrt(refSq * candSq);
  const snrDb = errRms === 0 ? Number.POSITIVE_INFINITY : refRms === 0 ? Number.NEGATIVE_INFINITY : 20 * Math.log10(refRms / errRms);
  const byteEqual =
    ref.rate === cand.rate && ref.data.length === cand.data.length && ref.data.every((v, i) => v === cand.data[i]);
  return {
    refRate: ref.rate,
    candRate: cand.rate,
    channels: ch,
    lagSamples: lag,
    lagSeconds: lag / ref.rate,
    samplesCompared: count / ch,
    refPeak,
    candPeak,
    refRmsDb: dbfs(refRms),
    candRmsDb: dbfs(candRms),
    errorRms: errRms,
    maxError: maxErr,
    snrDb,
    correlation,
    withinOneLsbPct: (100 * within) / count,
    byteEqual,
  };
}

// ---- images -------------------------------------------------------------------------------------

function paeth(a: number, b: number, c: number): number {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

const PNG_MAGIC = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

/** Decode a non-interlaced 8-bit PNG (grayscale, gray+alpha, RGB, RGBA, indexed). */
export function decodePng(source: string | Uint8Array): ImageData8 {
  const bytes = typeof source === "string" ? new Uint8Array(readFileSync(source)) : source;
  if (!PNG_MAGIC.every((v, i) => bytes[i] === v)) throw new Error("not a PNG file");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let width = 0;
  let height = 0;
  let depth = 0;
  let colorType = -1;
  let interlace = 0;
  let palette: Uint8Array | null = null;
  let trns: Uint8Array | null = null;
  const idat: Uint8Array[] = [];
  let offset = 8;
  while (offset + 8 <= bytes.length) {
    const size = view.getUint32(offset);
    const type = String.fromCharCode(...bytes.subarray(offset + 4, offset + 8));
    const body = bytes.subarray(offset + 8, offset + 8 + size);
    if (type === "IHDR") {
      width = view.getUint32(offset + 8);
      height = view.getUint32(offset + 12);
      depth = body[8] as number;
      colorType = body[9] as number;
      interlace = body[12] as number;
    } else if (type === "PLTE") palette = body;
    else if (type === "tRNS") trns = body;
    else if (type === "IDAT") idat.push(body);
    else if (type === "IEND") break;
    offset += 12 + size;
  }
  if (!width || !height) throw new Error("PNG has no IHDR");
  if (depth !== 8) throw new Error(`only 8-bit PNG is supported (bit depth ${depth})`);
  if (interlace) throw new Error("interlaced PNG is not supported");
  const channelsByType: Record<number, number> = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };
  const channels = channelsByType[colorType];
  if (!channels) throw new Error(`unsupported PNG color type ${colorType}`);
  if (colorType === 3 && !palette) throw new Error("indexed PNG has no PLTE");

  let total = 0;
  for (const part of idat) total += part.length;
  const joined = new Uint8Array(total);
  let at = 0;
  for (const part of idat) {
    joined.set(part, at);
    at += part.length;
  }
  const raw = new Uint8Array(inflateSync(joined));
  const stride = width * channels;
  if (raw.length < height * (stride + 1)) throw new Error("PNG image data is truncated");
  const pix = new Uint8Array(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)] as number;
    const src = y * (stride + 1) + 1;
    const dst = y * stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= channels ? (pix[dst + x - channels] as number) : 0;
      const b = y > 0 ? (pix[dst - stride + x] as number) : 0;
      const c = y > 0 && x >= channels ? (pix[dst - stride + x - channels] as number) : 0;
      const v = raw[src + x] as number;
      let out: number;
      switch (filter) {
        case 0: out = v; break;
        case 1: out = v + a; break;
        case 2: out = v + b; break;
        case 3: out = v + ((a + b) >> 1); break;
        case 4: out = v + paeth(a, b, c); break;
        default: throw new Error(`bad PNG filter type ${filter}`);
      }
      pix[dst + x] = out & 0xff;
    }
  }
  const rgba = new Uint8Array(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    const p = i * channels;
    const o = i * 4;
    switch (colorType) {
      case 0: rgba[o] = rgba[o + 1] = rgba[o + 2] = pix[p] as number; rgba[o + 3] = 255; break;
      case 2: rgba[o] = pix[p] as number; rgba[o + 1] = pix[p + 1] as number; rgba[o + 2] = pix[p + 2] as number; rgba[o + 3] = 255; break;
      case 3: {
        const idx = pix[p] as number;
        const pal = palette as Uint8Array;
        rgba[o] = pal[idx * 3] as number;
        rgba[o + 1] = pal[idx * 3 + 1] as number;
        rgba[o + 2] = pal[idx * 3 + 2] as number;
        rgba[o + 3] = trns && idx < trns.length ? (trns[idx] as number) : 255;
        break;
      }
      case 4: rgba[o] = rgba[o + 1] = rgba[o + 2] = pix[p] as number; rgba[o + 3] = pix[p + 1] as number; break;
      default: rgba[o] = pix[p] as number; rgba[o + 1] = pix[p + 1] as number; rgba[o + 2] = pix[p + 2] as number; rgba[o + 3] = pix[p + 3] as number;
    }
  }
  return { width, height, rgba };
}

/** Decode an uncompressed 24- or 32-bit BMP (bottom-up or top-down). 32-bit alpha is taken as opaque unless BITFIELDS gives an alpha mask. */
export function decodeBmp(source: string | Uint8Array): ImageData8 {
  const bytes = typeof source === "string" ? new Uint8Array(readFileSync(source)) : source;
  if (bytes[0] !== 0x42 || bytes[1] !== 0x4d) throw new Error("not a BMP file");
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const dataOffset = v.getUint32(10, true);
  const headerSize = v.getUint32(14, true);
  if (headerSize < 40) throw new Error("only BITMAPINFOHEADER and later BMP headers are supported");
  const width = v.getInt32(18, true);
  const rawHeight = v.getInt32(22, true);
  const bpp = v.getUint16(28, true);
  const compression = v.getUint32(30, true);
  if (bpp !== 24 && bpp !== 32) throw new Error(`only 24- and 32-bit BMP is supported (${bpp} bits)`);
  if (compression !== 0 && compression !== 3) throw new Error(`compressed BMP is not supported (compression ${compression})`);
  const height = Math.abs(rawHeight);
  const topDown = rawHeight < 0;
  const bytesPer = bpp / 8;
  const stride = Math.ceil((width * bpp) / 32) * 4;
  if (dataOffset + stride * height > bytes.length) throw new Error("BMP pixel data is truncated");
  let masks: [number, number, number, number] | null = null;
  if (compression === 3 && bpp === 32) {
    masks = [v.getUint32(54, true), v.getUint32(58, true), v.getUint32(62, true), headerSize >= 56 ? v.getUint32(66, true) : 0];
  }
  const channel = (px: number, mask: number): number => {
    if (!mask) return 255;
    const shift = Math.clz32(mask & -mask) ^ 31;
    const max = mask >>> shift;
    return Math.round((((px & mask) >>> shift) * 255) / max);
  };
  const rgba = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; y++) {
    const row = dataOffset + (topDown ? y : height - 1 - y) * stride;
    for (let x = 0; x < width; x++) {
      const p = row + x * bytesPer;
      const o = (y * width + x) * 4;
      if (masks) {
        const px = v.getUint32(p, true);
        rgba[o] = channel(px, masks[0]);
        rgba[o + 1] = channel(px, masks[1]);
        rgba[o + 2] = channel(px, masks[2]);
        rgba[o + 3] = channel(px, masks[3]);
      } else {
        rgba[o] = bytes[p + 2] as number;
        rgba[o + 1] = bytes[p + 1] as number;
        rgba[o + 2] = bytes[p] as number;
        rgba[o + 3] = 255;
      }
    }
  }
  return { width, height, rgba };
}

/** Encode RGBA as a 24-bit bottom-up BMP (alpha dropped). */
export function encodeBmp(image: ImageData8): Uint8Array {
  const { width, height, rgba } = image;
  const stride = Math.ceil((width * 24) / 32) * 4;
  const out = new Uint8Array(54 + stride * height);
  const v = new DataView(out.buffer);
  out[0] = 0x42;
  out[1] = 0x4d;
  v.setUint32(2, out.length, true);
  v.setUint32(10, 54, true);
  v.setUint32(14, 40, true);
  v.setInt32(18, width, true);
  v.setInt32(22, height, true);
  v.setUint16(26, 1, true);
  v.setUint16(28, 24, true);
  v.setUint32(34, stride * height, true);
  for (let y = 0; y < height; y++) {
    const row = 54 + (height - 1 - y) * stride;
    for (let x = 0; x < width; x++) {
      const s = (y * width + x) * 4;
      out[row + x * 3] = rgba[s + 2] as number;
      out[row + x * 3 + 1] = rgba[s + 1] as number;
      out[row + x * 3 + 2] = rgba[s] as number;
    }
  }
  return out;
}

/** Decode a PNG or BMP, chosen by the file's magic bytes. */
export function decodeImage(source: string | Uint8Array): ImageData8 {
  const bytes = typeof source === "string" ? new Uint8Array(readFileSync(source)) : source;
  if (bytes[0] === 0x89 && bytes[1] === 0x50) return decodePng(bytes);
  if (bytes[0] === 0x42 && bytes[1] === 0x4d) return decodeBmp(bytes);
  throw new Error("unrecognized image format (PNG and BMP are supported)");
}

/** Pixel comparison of two PNG/BMP images (paths or decoded). Throws when the dimensions differ. */
export function compareImage(
  a: string | ImageData8,
  b: string | ImageData8,
  opts: ImageCompareOptions = {},
): ImageCompareResult {
  const ia = typeof a === "string" ? decodeImage(a) : a;
  const ib = typeof b === "string" ? decodeImage(b) : b;
  if (ia.width !== ib.width || ia.height !== ib.height) {
    throw new Error(`image dimensions differ: ${ia.width}x${ia.height} vs ${ib.width}x${ib.height}`);
  }
  const tolerance = opts.tolerance ?? 0;
  const channels = opts.alpha ? 4 : 3;
  const total = ia.width * ia.height;
  let mismatched = 0;
  let maxDiff = 0;
  let sumAbs = 0;
  let sumSq = 0;
  const diff = opts.diffPath ? new Uint8Array(total * 4) : null;
  for (let i = 0; i < total; i++) {
    let pixelMax = 0;
    for (let c = 0; c < channels; c++) {
      const d = Math.abs((ia.rgba[i * 4 + c] as number) - (ib.rgba[i * 4 + c] as number));
      sumAbs += d;
      sumSq += d * d;
      if (d > pixelMax) pixelMax = d;
    }
    if (pixelMax > maxDiff) maxDiff = pixelMax;
    const bad = pixelMax > tolerance;
    if (bad) mismatched++;
    if (diff) {
      diff[i * 4] = bad ? 255 : (ia.rgba[i * 4] as number) >> 2;
      diff[i * 4 + 1] = bad ? 0 : (ia.rgba[i * 4 + 1] as number) >> 2;
      diff[i * 4 + 2] = bad ? 0 : (ia.rgba[i * 4 + 2] as number) >> 2;
      diff[i * 4 + 3] = 255;
    }
  }
  if (diff && opts.diffPath) writeFileSync(opts.diffPath, encodeBmp({ width: ia.width, height: ia.height, rgba: diff }));
  const n = total * channels;
  const mse = n ? sumSq / n : 0;
  return {
    width: ia.width,
    height: ia.height,
    totalPixels: total,
    mismatchedPixels: mismatched,
    mismatchPct: total ? (100 * mismatched) / total : 0,
    maxChannelDiff: maxDiff,
    mae: n ? sumAbs / n : 0,
    psnrDb: mse === 0 ? Number.POSITIVE_INFINITY : 10 * Math.log10((255 * 255) / mse),
    exactMatch: maxDiff === 0,
  };
}
