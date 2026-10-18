// F3 Sound Bench: browser UI of the Taito F3 music player (contract: tools/webplayer/README.md).
// Every note, voice curve, effect and level drawn here is prebaked by the native exporter (<id>.json + <id>.bin).
// Audio comes from the HLE engine compiled to WASM inside an AudioWorklet (worklet.js, protocol README 6.3).
// This file only reads typed-array views of the trace; it never derives musical data on its own.

const SR = 29762, CLOCK = 16000000, NONE = 0xffffffff, WIN = 512, MS = SR / 1000;

// ---------------------------------------------------------------- small helpers

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const pad = (n, w = 2) => String(n).padStart(w, "0");
const hx = (n, w = 2) => n.toString(16).toUpperCase().padStart(w, "0");

function h(tag, props, ...kids) {
	const e = document.createElement(tag);
	if (props) for (const [k, v] of Object.entries(props)) {
		if (v == null || v === false) continue;
		if (k === "class") e.className = v;
		else if (k === "text") e.textContent = v;
		else if (k === "style") e.style.cssText = v;
		else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
		else e.setAttribute(k, v === true ? "" : v);
	}
	for (const k of kids.flat(Infinity)) if (k != null && k !== false) e.append(k.nodeType ? k : String(k));
	return e;
}

// first index i in [lo,hi) with arr[i] >= v (arr ascending)
function lowerBound(arr, v, lo = 0, hi = arr.length) {
	while (lo < hi) { const m = (lo + hi) >>> 1; if (arr[m] < v) lo = m + 1; else hi = m; }
	return lo;
}
// first index i in [lo,hi) with arr[i] > v
function upperBound(arr, v, lo = 0, hi = arr.length) {
	while (lo < hi) { const m = (lo + hi) >>> 1; if (arr[m] <= v) lo = m + 1; else hi = m; }
	return lo;
}

const frameOfTick = (t) => Math.floor(Math.floor(t / CLOCK) * SR + (t % CLOCK) * SR / CLOCK);

function fmtTime(frames, decimals = 3) {
	const s = Math.max(0, frames) / SR, m = Math.floor(s / 60), r = s - m * 60;
	return `${m}:${pad(Math.floor(r))}${decimals ? "." + pad(Math.floor((r % 1) * 10 ** decimals), decimals) : ""}`;
}

const KEY_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const keyName = (k) => KEY_NAMES[k % 12] + (Math.floor(k / 12) - 1);
const progName = (p) => `P${hx(p & 0xff)}${p >> 14 ? "·b" + (p >> 14) : ""}`;
const isBlack = (k) => (1 << (k % 12)) & 0b010110101010;

// ---------------------------------------------------------------- colour (rgb555: every channel is a multiple of 8)

const q5 = (v) => Math.max(0, Math.min(248, Math.round(v))) & 0xf8;
const rgbHex = (r, g, b) => "#" + ((q5(r) << 16) | (q5(g) << 8) | q5(b)).toString(16).padStart(6, "0");
const colOf = (hex) => { const n = parseInt(hex.slice(1), 16); return { r: n >> 16, g: (n >> 8) & 255, b: n & 255, hex }; };
const rgba = (c, a) => `rgba(${c.r},${c.g},${c.b},${a})`;
function hsl(hh, s, l) {
	const a = s * Math.min(l, 1 - l);
	const f = (n) => { const k = (n + hh / 30) % 12; return (l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1))) * 255; };
	const r = q5(f(0)), g = q5(f(8)), b = q5(f(4));
	return { r, g, b, hex: rgbHex(r, g, b) };
}

const C = {
	bg: colOf("#001810"), bg2: colOf("#082820"), bg3: colOf("#103830"), line: colOf("#185040"), line2: colOf("#287058"),
	silk: colOf("#e0f0d8"), dim: colOf("#88b8a0"), cu: colOf("#d08840"), gold: colOf("#f8c830"), hot: colOf("#f04830"), cyan: colOf("#48d8e0"),
	mag: colOf("#e860c8"), grey: colOf("#688878"),
};
const PAIR_COL = [C.grey, C.cyan, C.gold, C.mag];
const INST_PAL = Array.from({ length: 24 }, (_, i) => hsl((i * 137.508 + 20) % 360, 0.72, [0.6, 0.5, 0.7][i % 3]));
const FILL = INST_PAL.map((c) => Array.from({ length: 8 }, (_, v) => rgba(c, 0.5 + (0.5 * v) / 7)));
const DIM = INST_PAL.map((c) => rgba(c, 0.14));
const TAIL = INST_PAL.map((c) => rgba(c, 0.38));
const FX_COL = { None: colOf("#103830"), Reverb: colOf("#48a0e0"), Hall: colOf("#7878e8"), Chorus: colOf("#c870e0"), Echo: colOf("#e8a038"), Mute: colOf("#687870") };

const PAIR_INFO = [
	{ buses: "0/1", serio: "unconnected", role: "silent" },
	{ buses: "2/3", serio: "ESP serio0", role: "dry in every program" },
	{ buses: "4/5", serio: "ESP serio1", role: "reverb/hall input, chorus, echo path" },
	{ buses: "6/7", serio: "ESP serio2", role: "reverb send" },
];

// ---------------------------------------------------------------- state

const FIXED = { headW: 176, rulerH: 64, fxH: 40, lvH: 58, autoH: 46 };
const PPF_MAX = 8; // px per output frame: one 1 ms tick = 238 px

const G = { games: null, set: null, manifest: null, songs: [], rom: null }; // current game
let S = null; // current parsed song
const view = { t0: 0, ppf: 0.02, w: 600, h: 300, laneH: 72, dpr: 1, scroll: 0 };
const sel = { note: -1, voice: 0, inst: -1, track: -1, region: -1 };
const opt = { follow: true, pitch: true, glide: true, lfo: false, color: "inst", keys: "fit", hideEmpty: false };
const eng = {
	ctx: null, node: null, ready: false, error: null, state: "paused", song: null, loaded: null,
	pos: { frame: 0, t: 0, state: "paused" }, shown: 0, token: 0, seek: null, resume: false, pending: null, autoplay: false, hold: [0, 0],
};
const reduceMotion = typeof matchMedia !== "undefined" ? matchMedia("(prefers-reduced-motion: reduce)") : { matches: false };

// ---------------------------------------------------------------- binary trace parsing (typed-array views only)

const CTOR = { u8: Uint8Array, i8: Int8Array, u16: Uint16Array, i16: Int16Array, u32: Uint32Array, i32: Int32Array, f32: Float32Array };

function colView(buf, c) {
	const T = CTOR[c.dtype];
	if (!T) throw new Error(`unknown dtype ${c.dtype}`);
	const sz = T.BYTES_PER_ELEMENT;
	if (c.offset % sz) return new T(buf.slice(c.offset, c.offset + c.length * sz)); // misaligned writer: copy
	return new T(buf, c.offset, c.length);
}
function table(buf, t) {
	const o = { rows: t.rows };
	for (const k in t.columns) o[k] = colView(buf, t.columns[k]);
	return o;
}

const LANE_NAMES = ["pitch", "glide", "lfo", "env_pitch", "env_filter", "env_amp", "left", "right", "k1", "k2"];
const onset = (V, lane) => V[lane + "0"] || V[lane + "_0"];

function parseTrace(j, buf) {
	if (j.format !== "f3-webplayer-trace" || j.version !== 1) throw new Error(`unsupported trace ${j.format} v${j.version}`);
	const dv = new DataView(buf);
	if (dv.getUint32(0, true) !== 0x52543346 || dv.getUint32(4, true) !== 1) throw new Error("bad .bin header (expected F3TR v1)");
	const S = { j, buf, song: j.song, loop: j.loop, span: j.span };
	S.sec = table(buf, j.sections); S.bar = table(buf, j.bars); S.rate = table(buf, j.rate_changes);
	S.note = table(buf, j.notes); S.voice = table(buf, j.voices); S.ev = table(buf, j.chan_events); S.reg = table(buf, j.regions);
	S.end = Math.max(j.span.frames, j.span.audio_frames);
	S.seq = new Map(j.sequences.map((s) => [s.id, s]));
	S.chanLanes = j.chan_lanes || [];

	// voices: end frames, lane tables with absolute frames
	const V = S.voice, nV = V.rows;
	S.vEnd = new Uint32Array(nV);
	let maxDur = 0;
	for (let v = 0; v < nV; v++) {
		const e = V.stop_frame[v] === NONE ? S.end : V.stop_frame[v];
		S.vEnd[v] = e; if (e - V.start_frame[v] > maxDur) maxDur = e - V.start_frame[v];
	}
	S.vMaxDur = maxDur;
	S.lane = {};
	for (const name of Object.keys(j.lanes || {})) {
		const L = table(buf, j.lanes[name]);
		L.first = V[name + "_first"]; L.count = V[name + "_count"];
		L.abs = new Uint32Array(L.rows);
		if (L.first && L.count) for (let v = 0; v < nV; v++) {
			let f = V.start_frame[v];
			for (let k = L.first[v], e = Math.min(L.rows, k + L.count[v]); k < e; k++) { f += L.dt[k]; L.abs[k] = f; }
		}
		S.lane[name] = L;
	}
	// voices ordered by start (active-voice queries for the board)
	S.vOrder = Uint32Array.from({ length: nV }, (_, i) => i).sort((a, b) => V.start_frame[a] - V.start_frame[b] || a - b);
	S.vStartSorted = new Uint32Array(nV);
	for (let i = 0; i < nV; i++) S.vStartSorted[i] = V.start_frame[S.vOrder[i]];

	// instruments + colours (stable per program id, collisions resolved in program order)
	const N = S.note;
	const progs = new Set(); for (let i = 0; i < N.rows; i++) progs.add(N.program[i]);
	for (const ins of j.instruments || []) progs.add(ins.program);
	const insJ = new Map((j.instruments || []).map((i) => [i.program, i]));
	S.inst = new Map(); const taken = new Set();
	for (const p of [...progs].sort((a, b) => a - b)) {
		let c = ((p & 0xff) + (p >> 14) * 5) % INST_PAL.length, g = 0;
		while (taken.has(c) && g++ < INST_PAL.length) c = (c + 1) % INST_PAL.length;
		taken.add(c);
		S.inst.set(p, { program: p, color: c, j: insJ.get(p) || null, notes: 0, tracks: new Set(), regions: new Map() });
	}
	S.nCol = new Uint8Array(N.rows); S.nPair = new Uint8Array(N.rows);
	S.noteOff = new Uint32Array(N.rows); S.noteEnd = new Uint32Array(N.rows);
	for (let i = 0; i < N.rows; i++) {
		const ins = S.inst.get(N.program[i]); ins.notes++; S.nCol[i] = ins.color;
		const vc = N.voice_count[i], vf = N.voice_first[i];
		let vstop = 0, vrel = 0;
		for (let k = 0; k < vc; k++) {
			const v = vf + k;
			if (S.vEnd[v] > vstop) vstop = S.vEnd[v];
			if (V.release_frame[v] !== NONE && V.release_frame[v] > vrel) vrel = V.release_frame[v];
			const rg = ins.regions.get(V.region[v]);
			if (rg) { rg.n++; rg.lo = Math.min(rg.lo, V.skey[v]); rg.hi = Math.max(rg.hi, V.skey[v]); } else ins.regions.set(V.region[v], { n: 1, lo: V.skey[v], hi: V.skey[v] });
		}
		S.nPair[i] = vc ? V.pair[vf] : 0;
		const on = N.on_frame[i];
		const off = N.off_frame[i] !== NONE ? N.off_frame[i] : vrel || vstop || on + 1;
		S.noteOff[i] = Math.max(off, on + 1);
		S.noteEnd[i] = Math.max(S.noteOff[i], vstop);
	}

	// tracks = channels (sequence, track)
	const byCh = new Map(), byKey = new Map(); S.tracks = [];
	const mk = (id, seq, track) => { const t = { id, seq, track, list: [], open: false, autos: [] }; S.tracks.push(t); byKey.set(seq * 256 + track, t); if (id != null) byCh.set(id, t); return t; };
	for (const c of j.channels || []) mk(c.id, c.sequence, c.track);
	for (let i = 0; i < N.rows; i++) {
		let t = byCh.get(N.channel[i]) || byKey.get(N.sequence[i] * 256 + N.track[i]);
		if (!t) t = mk(N.channel[i], N.sequence[i], N.track[i]);
		t.list.push(i);
	}
	// channel automation, grouped per (track, lane) and sorted by frame
	const E = S.ev, nl = Math.max(1, S.chanLanes.length);
	const trackOfCh = new Int32Array(65536).fill(-1);
	S.tracks.sort((a, b) => a.seq - b.seq || a.track - b.track);
	S.tracks.forEach((t, i) => { t.index = i; if (t.id != null) trackOfCh[t.id] = i; });
	const nT = S.tracks.length, counts = new Int32Array(nT * nl + 1);
	for (let k = 0; k < E.rows; k++) { const ti = trackOfCh[E.channel[k]]; if (ti >= 0 && E.lane[k] < nl) counts[ti * nl + E.lane[k] + 1]++; }
	for (let k = 1; k < counts.length; k++) counts[k] += counts[k - 1];
	const fill = counts.slice(); S.evOrder = new Int32Array(E.rows);
	for (let k = 0; k < E.rows; k++) { const ti = trackOfCh[E.channel[k]]; if (ti >= 0 && E.lane[k] < nl) S.evOrder[fill[ti * nl + E.lane[k]]++] = k; }
	for (const t of S.tracks) {
		t.n = Uint32Array.from(t.list); t.list = null;
		t.mx = new Uint32Array(t.n.length);
		let mx = 0, kmin = 127, kmax = 0; const pm = new Set(); let pairs = 0;
		for (let a = 0; a < t.n.length; a++) {
			const i = t.n[a]; if (S.noteEnd[i] > mx) mx = S.noteEnd[i]; t.mx[a] = mx;
			const k = N.kkey[i]; if (k < kmin) kmin = k; if (k > kmax) kmax = k;
			pm.add(N.program[i]); S.inst.get(N.program[i]).tracks.add(t);
			for (let q = 0; q < N.voice_count[i]; q++) pairs |= 1 << V.pair[N.voice_first[i] + q];
		}
		if (!t.n.length) { kmin = 60; kmax = 72; }
		t.kmin = kmin; t.kmax = kmax; t.programs = [...pm]; t.pairs = pairs;
		for (let li = 0; li < S.chanLanes.length; li++) {
			const a = counts[t.index * nl + li], b = counts[t.index * nl + li + 1];
			if (b <= a) continue;
			const sub = S.evOrder.subarray(a, b); sub.sort((p, q) => E.frame[p] - E.frame[q] || p - q);
			let lo = Infinity, hi = -Infinity;
			for (let k = a; k < b; k++) { const v = E.value[S.evOrder[k]]; if (v < lo) lo = v; if (v > hi) hi = v; }
			t.autos.push({ lane: li, name: S.chanLanes[li], from: a, to: b, min: lo, max: hi });
		}
	}
	S.tracks = S.tracks.filter((t) => t.n.length || t.autos.length);
	S.tracks.forEach((t, i) => (t.index = i));

	// levels / peaks / effects
	const lv = j.levels; S.lv = { rows: lv.rows, buses: colView(buf, lv.buses), out: colView(buf, lv.out) };
	S.peaks = colView(buf, j.peaks);
	const R = S.reg; S.regPeak = new Uint32Array(R.rows);
	for (let r = 0; r < R.rows; r++) { const o = R.peak_offset[r]; S.regPeak[r] = R.rows > 1 && R.peak_offset[R.rows - 1] < R.rows * 256 ? o * 2 : o; } // element index (offset may count column pairs)
	S.fx = j.effects || []; S.fxFrames = Float64Array.from(S.fx, (e) => e.frame);
	S.gain = j.gain || []; S.gainFrames = Float64Array.from(S.gain, (e) => e.frame);
	// kind==preset rows start a program segment (for the FX lane)
	S.fxSegs = [];
	for (let k = 0; k < S.fx.length; k++) {
		const e = S.fx[k];
		if (e.kind === "preset" || !S.fxSegs.length) S.fxSegs.push({ from: e.frame, to: S.end, program: e.program, row: k });
		else S.fxSegs[S.fxSegs.length - 1].program = e.program;
		if (S.fxSegs.length > 1) S.fxSegs[S.fxSegs.length - 2].to = S.fxSegs[S.fxSegs.length - 1].from;
	}
	return S;
}

// effective y deviation anchor of a voice's pitch lane (README 4.8: pitch/256 is in key units, but fixed-pitch layers may sit elsewhere)
function pitchAnchor(v) {
	const V = S.voice, o = onset(V, "pitch")[v] / 256, sk = V.skey[v];
	return Math.abs(o - sk) <= 12.5 ? sk : o;
}

// value of a voice lane at frame f (linear between points, constant lane = onset column)
function laneAt(name, v, f) {
	const V = S.voice, L = S.lane[name], c = L && L.count ? L.count[v] : 0;
	if (!c) return onset(V, name)[v];
	const k0 = L.first[v], k1 = k0 + c, a = L.abs;
	if (f <= V.start_frame[v]) return onset(V, name)[v];
	if (f >= a[k1 - 1]) return L.value[k1 - 1];
	const k = upperBound(a, f, k0, k1); // a[k] > f
	if (k === k0) { const t = (f - V.start_frame[v]) / Math.max(1, a[k0] - V.start_frame[v]); return onset(V, name)[v] + (L.value[k0] - onset(V, name)[v]) * t; }
	const ta = a[k - 1], tb = a[k], t = tb > ta ? (f - ta) / (tb - ta) : 0;
	return L.value[k - 1] + (L.value[k] - L.value[k - 1]) * t;
}

// README 4.8: linear gain = interp(G, e/256), G[i] = (((i & 15) | 16) << 11 >> (16 - (i >> 4))) / 2048
const GTAB = Float64Array.from({ length: 257 }, (_, i) => { i = Math.min(i, 255); return (((((i & 15) | 16) << 11) >> (16 - (i >> 4))) / 2048); });
function volDb(e) {
	const x = e / 256, i = Math.floor(x), f = x - i;
	const g = GTAB[i] * (1 - f) + GTAB[Math.min(i + 1, 256)] * f;
	return g <= 0 ? -Infinity : 20 * Math.log10(g / GTAB[255]);
}

// README 4.4: continued looped playback maps onto the prebaked loop region
function mapPlayhead(p) {
	const L = S && S.loop;
	if (!L || p < L.end_frame) return { q: p, iter: 0 };
	const len = L.end_tick - L.start_tick, rel = (p / SR) * CLOCK - L.start_tick;
	const iter = Math.floor(rel / len);
	return { q: frameOfTick(L.start_tick + (rel - iter * len)), iter };
}

// ---------------------------------------------------------------- status / errors

function setStatus(text, kind = "") { const s = $("#status"); s.textContent = text; s.dataset.kind = kind; }
function fatal(msg) { const f = $("#fatal"); f.textContent = msg; f.hidden = !msg; }

async function fetchOk(url) {
	const r = await fetch(url);
	if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
	return r;
}
async function fetchBuf(url, label) {
	const r = await fetchOk(url);
	if (!label || !r.body) return r.arrayBuffer();
	const total = +r.headers.get("content-length") || 0, enc = r.headers.get("content-encoding"), rd = r.body.getReader(), parts = [];
	let got = 0;
	for (;;) {
		const { done, value } = await rd.read();
		if (done) break;
		parts.push(value); got += value.length;
		setStatus(`${label} ${(got / 1048576).toFixed(1)}${total && !enc ? "/" + (total / 1048576).toFixed(1) : ""} MiB`, "busy");
	}
	const out = new Uint8Array(got);
	let o = 0;
	for (const p of parts) { out.set(p, o); o += p.length; }
	return out.buffer;
}

// ---------------------------------------------------------------- engine (AudioWorklet protocol, README 6.3)

async function ensureContext() {
	if (eng.ctx) return eng.ctx;
	if (!eng.ctxPromise) eng.ctxPromise = (async () => {
		let ctx;
		try { ctx = new AudioContext({ sampleRate: SR, latencyHint: "interactive" }); }
		catch (e) { throw new Error(`This browser refuses AudioContext sampleRate ${SR} (${e.message}).`); }
		if (ctx.sampleRate !== SR) {
			ctx.close();
			throw new Error(`This browser runs audio at ${ctx.sampleRate} Hz instead of ${SR} Hz, so the engine cannot play. The timeline stays browsable.`);
		}
		await ctx.audioWorklet.addModule("worklet.js");
		eng.ctx = ctx;
		return ctx;
	})();
	return eng.ctxPromise;
}

function dropNode() {
	if (!eng.node) return;
	try { eng.node.disconnect(); eng.node.port.close(); } catch { /* already gone */ }
	eng.node = null; eng.ready = false; eng.loaded = null; eng.state = "paused";
}

async function compileWasm() {
	try { return await WebAssembly.compileStreaming(fetch("f3hle.wasm")); }
	catch { return WebAssembly.compile(await (await fetchOk("f3hle.wasm")).arrayBuffer()); }
}

async function initEngine(set, manifest) {
	dropNode();
	eng.error = null;
	let ctx;
	try { ctx = await ensureContext(); } catch (e) { eng.error = e.message; fatal(e.message); setStatus("engine unavailable", "bad"); updateTransport(); return; }
	const rom = manifest.rom;
	let wasm, sound, samples;
	try {
		[wasm, sound, samples] = await Promise.all([
			fetchBuf("f3hle.wasm"),
			fetchBuf(G.base + rom.sound.file),
			fetchBuf(G.base + rom.samples.file, "loading samples"),
		]);
	} catch (e) { eng.error = e.message; fatal(`Engine files failed to load: ${e.message}`); setStatus("engine unavailable", "bad"); updateTransport(); return; }
	if (G.set !== set) return; // another game was picked meanwhile
	G.samples = samples; G.samples16 = new Uint16Array(samples); G.sampleWords = G.samples16.length;
	const node = new AudioWorkletNode(ctx, "f3-player", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
	node.port.onmessage = onEngineMessage;
	node.onprocessorerror = () => engineError("render", "the audio processor crashed");
	node.connect(ctx.destination);
	eng.node = node;
	const copy = samples.slice(0); // the UI keeps samples.bin for waveform drawing; the worklet gets its own copy
	setStatus("starting engine…", "busy");
	node.port.postMessage({ type: "init", wasm, sound, samples: copy }, [wasm, sound, copy]);
}

function engineError(stage, message) {
	eng.error = `${stage}: ${message}`;
	fatal(`Engine error (${stage}): ${message}`);
	setStatus(`engine error (${stage})`, "bad");
	if (eng.state === "playing" || eng.state === "seeking") { eng.pos.frame = eng.shown > 0 ? eng.shown : eng.pos.frame; eng.state = "paused"; }
	updateTransport(); invalidate();
}

function onEngineMessage(ev) {
	const m = ev.data;
	if (!m || !m.type) return;
	const now = eng.ctx ? eng.ctx.currentTime : 0;
	switch (m.type) {
		case "ready":
			eng.ready = true;
			setStatus(`engine ready · ${SR} Hz · ${((m.memory_bytes || 0) / 1048576).toFixed(0)} MiB`, "ok");
			if (eng.pending) { const p = eng.pending; eng.pending = null; postLoad(p.song, p.replay); }
			updateTransport();
			break;
		case "loaded":
			if (!S || m.song !== S.song.id) break;
			eng.loaded = m.song; eng.state = "paused";
			setStatus(`${m.song} ready`, "ok");
			updateTransport();
			if (eng.autoplay) { eng.autoplay = false; play(); }
			break;
		case "position":
			if (eng.state === "seeking") break;
			eng.pos = { frame: m.frame, t: now, state: m.state }; break;
		case "seeking":
			if (eng.seek && m.token === eng.seek.token) { eng.seek.frame = m.frame; invalidate(2 | 4); }
			break;
		case "seeked":
			if (eng.seek && m.token === eng.seek.token) {
				eng.pos = { frame: m.frame, t: now, state: "paused" }; eng.shown = -1;
				const again = eng.seek.resume; eng.seek = null; eng.state = "paused";
				$("#seekbar").hidden = true;
				if (again) play(); else updateTransport();
				invalidate();
			}
			break;
		case "error": engineError(m.stage || "engine", m.message || "unknown error"); break;
	}
}

function postLoad(song, replay) {
	eng.loaded = null; eng.state = "paused"; eng.seek = null; eng.shown = -1;
	eng.pos = { frame: 0, t: eng.ctx ? eng.ctx.currentTime : 0, state: "paused" };
	fatal("");
	eng.node.port.postMessage({ type: "load", song, replay }, [replay]);
}

function engineLoad(song, replay, autoplay) {
	eng.autoplay = autoplay; eng.loaded = null;
	if (eng.node && eng.ready) postLoad(song, replay);
	else { eng.pending = { song, replay }; if (!eng.error) setStatus("waiting for engine…", "busy"); }
	updateTransport();
}

const engineUsable = () => eng.node && eng.ready && S && eng.loaded === S.song.id;

async function play() {
	if (!S) return;
	if (!engineUsable()) { eng.autoplay = !eng.error; return; }
	if (eng.state === "playing") return;
	try { await eng.ctx.resume(); } catch (e) { engineError("play", e.message); return; }
	if (!eng.shown || eng.shown < 0) eng.shown = -1;
	eng.pos.t = eng.ctx.currentTime; eng.state = "playing";
	eng.node.port.postMessage({ type: "play" });
	updateTransport(); invalidate();
}
function pause() {
	if (eng.state !== "playing") return;
	eng.pos.frame = currentFrame(); eng.state = "paused"; eng.autoplay = false;
	eng.node.port.postMessage({ type: "pause" });
	updateTransport(); invalidate();
}
function stop() {
	if (!engineUsable()) return;
	eng.autoplay = false; eng.state = "paused"; eng.seek = null; $("#seekbar").hidden = true;
	eng.pos = { frame: 0, t: eng.ctx.currentTime, state: "paused" }; eng.shown = -1;
	eng.node.port.postMessage({ type: "stop" });
	updateTransport(); invalidate();
}
function togglePlay() { eng.state === "playing" ? pause() : play(); }

let lastSeekSent = 0;
function seekTo(frame, { force = false } = {}) {
	if (!engineUsable()) return;
	frame = Math.round(clamp(frame, 0, S.end));
	const now = performance.now();
	const resume = eng.seek ? eng.seek.resume : eng.state === "playing";
	if (!force && now - lastSeekSent < 90 && eng.seek && eng.seek.target === frame) return;
	lastSeekSent = now;
	const token = ++eng.token;
	eng.seek = { token, target: frame, frame: 0, resume };
	eng.state = "seeking"; eng.shown = -1;
	eng.node.port.postMessage({ type: "seek", frame, token });
	$("#seekbar").hidden = false; $("#seekbar").value = 0;
	updateTransport(); invalidate();
}

// audible playhead (frames since replay origin), smoothed between worklet reports with the context clock
function currentFrame() {
	if (eng.state === "seeking" && eng.seek) return eng.seek.target;
	if (eng.state === "playing" && eng.ctx) {
		const ctx = eng.ctx, lat = (ctx.outputLatency || ctx.baseLatency || 0) * SR;
		const el = Math.min(0.3, Math.max(0, ctx.currentTime - eng.pos.t));
		let f = Math.max(0, eng.pos.frame + el * SR - lat);
		if (eng.shown >= 0 && f < eng.shown && eng.shown - f < SR * 0.2) f = eng.shown; // keep the playhead monotone through report jitter
		eng.shown = f;
		return f;
	}
	return eng.pos.frame;
}

// ---------------------------------------------------------------- games and songs

async function boot() {
	try {
		const gj = await (await fetchOk("games.json")).json();
		if (gj.format !== "f3-webplayer-games") throw new Error("games.json: unexpected format");
		G.games = gj.games;
	} catch (e) { fatal(`Could not load games.json: ${e.message}. Serve the exported bundle directory (see tools/webplayer/README.md).`); setStatus("no bundle", "bad"); return; }
	const gsel = $("#game");
	gsel.replaceChildren(...G.games.map((g) => h("option", { value: g.set, text: g.title })));
	const [set, id] = location.hash.slice(1).split("/");
	await selectGame(G.games.some((g) => g.set === set) ? set : G.games[0].set, id);
}

async function selectGame(set, songId) {
	const ent = G.games.find((g) => g.set === set);
	$("#game").value = set;
	G.set = set; G.loadToken = (G.loadToken || 0) + 1;
	G.base = ent.manifest.replace(/[^/]*$/, "");
	setStatus("loading song list…", "busy");
	let man;
	try { man = await (await fetchOk(ent.manifest)).json(); } catch (e) { fatal(`Could not load ${ent.manifest}: ${e.message}`); setStatus("no manifest", "bad"); return; }
	if (G.set !== set) return;
	G.manifest = man; G.songs = man.songs || [];
	renderSongList();
	initEngine(set, man);
	const first = G.songs.find((s) => s.id === songId) || G.songs.find((s) => s.frames > 0) || G.songs[0];
	if (first) await loadSong(first, false);
}

async function loadSong(entry, autoplay) {
	const my = (G.loadToken = (G.loadToken || 0) + 1);
	G.current = entry.id;
	markSongList();
	setStatus(`loading ${entry.id}…`, "busy");
	fatal(eng.error ? `Engine error: ${eng.error}` : "");
	try {
		const [j, bin, replay] = await Promise.all([
			fetchOk(G.base + entry.trace).then((r) => r.json()), fetchBuf(G.base + entry.trace_bin), fetchBuf(G.base + entry.replay),
		]);
		if (my !== G.loadToken) return;
		const t0 = performance.now();
		const parsed = parseTrace(j, bin);
		if (my !== G.loadToken) return;
		S = parsed; S.entry = entry; S.parseMs = performance.now() - t0;
		onSong();
		engineLoad(entry.id, replay, autoplay || eng.state === "playing");
		history.replaceState(null, "", `#${G.set}/${entry.id}`);
		if (!eng.error) setStatus(`${entry.id}: ${S.note.rows} notes · ${S.voice.rows} voices · parsed in ${S.parseMs.toFixed(0)} ms`, "ok");
	} catch (e) {
		if (my !== G.loadToken) return;
		S = null; sel.note = -1; eng.loaded = null; eng.state = "paused";
		console.error(e);
		setStatus(`could not load ${entry.id}`, "bad");
		showEmpty(`Could not load ${entry.id}: ${e.message}`);
		onSong();
	}
}

function renderSongList() {
	const ul = $("#songlist");
	ul.replaceChildren(...G.songs.map((s) => {
		const dur = s.frames ? fmtTime(s.frames, 0) : "—";
		return h("li", { "data-id": s.id },
			h("button", { type: "button", onclick: () => loadSong(s, true), title: `${s.label} · ${s.end_reason || ""}` },
				h("span", { class: "pad" }), h("span", { class: "lb", text: s.label || s.id }),
				h("span", { class: "du", text: dur }), s.loop ? h("span", { class: "lp", title: "loops", text: "↻" }) : null));
	}));
	$("#songcount").textContent = `${G.songs.length} songs`;
	markSongList(); filterSongs();
}
function markSongList() {
	for (const li of $$("#songlist li")) {
		const on = li.dataset.id === G.current;
		li.firstChild.toggleAttribute("aria-current", on);
		if (on) li.firstChild.setAttribute("aria-current", "true"); else li.firstChild.removeAttribute("aria-current");
	}
}
function filterSongs() {
	const q = $("#filter").value.trim().toLowerCase();
	for (const li of $$("#songlist li")) li.hidden = !!q && !li.textContent.toLowerCase().includes(q) && !li.dataset.id.includes(q);
}

// ---------------------------------------------------------------- timeline: layout

const MONO = 'ui-monospace, "DejaVu Sans Mono", "Liberation Mono", Menlo, Consolas, monospace';
const el = {};
const cv = {};
C.bgA = colOf("#001008");
const FILL_P = PAIR_COL.map((c) => Array.from({ length: 8 }, (_, v) => rgba(c, 0.5 + (0.5 * v) / 7)));
const DIM_P = PAIR_COL.map((c) => rgba(c, 0.14));

const trackName = (t) => `S${pad(t.seq)}·T${t.track}`;

function trackKeys(t) {
	if (opt.keys === "all") return [21, 108];
	let lo = t.kmin - 1, hi = t.kmax + 1;
	if (hi - lo < 11) { lo = Math.floor((lo + hi) / 2 - 5.5); hi = lo + 11; }
	return [Math.max(0, lo), Math.min(127, hi)];
}
function geom(rw) {
	const [lo, hi] = trackKeys(rw.t), padY = 3;
	return { lo, hi, padY, kh: (rw.h - padY * 2) / (hi - lo + 1) };
}

function buildRows() {
	const rows = [];
	let y = 0;
	const add = (r) => { r.y = y; y += r.h; rows.push(r); };
	add({ type: "fx", h: FIXED.fxH });
	add({ type: "lv", h: FIXED.lvH });
	for (const t of S.tracks) {
		if (opt.hideEmpty && (!t.n || t.n.length === 0)) continue;
		add({ type: "trk", t, h: view.laneH });
		if (t.open) for (const a of t.autos) add({ type: "auto", t, a, h: FIXED.autoH });
	}
	S.rows = rows; S.rowsH = y; S.rowY = Float64Array.from(rows, (r) => r.y);
}
const rowAt = (yy) => clamp(upperBound(S.rowY, yy) - 1, 0, S.rows.length - 1);

function renderHeads() {
	const ol = el.heads;
	if (!S) { ol.replaceChildren(); return; }
	ol.replaceChildren(...S.rows.map((rw) => {
		const st = `top:${rw.y}px;height:${rw.h}px`;
		if (rw.type === "fx") return h("li", { class: "head glob", style: st }, h("b", { text: "FX" }), h("small", { text: "ES5510 program, parameter writes, gain" }));
		if (rw.type === "lv") return h("li", { class: "head glob", style: st }, h("b", { text: "LEVEL" }), h("small", { text: "peak per 512-frame window" }),
			h("span", { class: "leg" }, h("i", { style: `color:${C.gold.hex}`, text: "L" }), h("i", { style: `color:${C.cyan.hex}`, text: "R" }), "DAC"));
		if (rw.type === "auto") return h("li", { class: "head auto", style: st }, h("b", { text: rw.a.name }), h("small", { text: `${rw.a.min} … ${rw.a.max}` }));
		const t = rw.t;
		return h("li", { class: "head trk" + (t.index === sel.track ? " sel" : ""), style: st },
			h("button", { class: "tg", type: "button", disabled: !t.autos.length, "aria-expanded": String(t.open), title: t.autos.length ? `${t.open ? "Hide" : "Show"} automation lanes (${t.autos.length})` : "no automation", "aria-label": `${t.open ? "Hide" : "Show"} automation lanes of ${trackName(t)}`, "data-track": t.index, onclick: () => toggleTrack(t) }, t.open ? "▾" : "▸"),
			h("button", { class: "nm", type: "button", onclick: () => selectTrack(t.index), "aria-pressed": String(t.index === sel.track) }, trackName(t)),
			h("span", { class: "meta", text: `${t.n.length} notes` }),
			h("span", { class: "chips" }, t.programs.slice(0, 8).map((p) => h("i", { class: "chip", style: `background:${INST_PAL[S.inst.get(p).color].hex}`, title: progName(p) })),
				[1, 2, 3].filter((p) => t.pairs & (1 << p)).map((p) => h("i", { class: "pp", style: `color:${PAIR_COL[p].hex}`, title: `bus pair ${p}: ${PAIR_INFO[p].serio}`, text: "P" + p }))));
	}));
}

function layoutTimeline() {
	const L = el.lanes;
	view.dpr = window.devicePixelRatio || 1;
	view.w = Math.max(100, L.clientWidth - FIXED.headW);
	view.h = Math.max(60, L.clientHeight);
	const sb = L.offsetWidth - L.clientWidth;
	el.rulerBox.style.paddingRight = sb + "px";
	for (const [k, w, hh] of [["roll", view.w, view.h], ["cur", view.w, view.h], ["ruler", view.w, FIXED.rulerH]]) {
		const c = cv[k]; c.c.width = Math.round(w * view.dpr); c.c.height = Math.round(hh * view.dpr);
		c.c.style.width = w + "px"; c.c.style.height = hh + "px";
	}
	el.stage.style.height = view.h + "px";
	el.space.style.height = Math.max(S ? S.rowsH : 0, view.h) + "px";
	if (S) {
		clampView();
		setScroll(view.scroll);
	}
	invalidate();
}

function afterRows() {
	buildRows(); renderHeads();
	el.space.style.height = Math.max(S.rowsH, view.h) + "px";
	setScroll(view.scroll);
	invalidate();
}
function toggleTrack(t) {
	t.open = !t.open;
	afterRows();
	$(`[data-track="${t.index}"]`, el.heads)?.focus();
}
function selectTrack(i) {
	sel.track = sel.track === i ? -1 : i;
	for (const li of $$(".head.trk", el.heads)) {
		const on = li.querySelector(".tg").dataset.track === String(sel.track);
		li.classList.toggle("sel", on); li.querySelector(".nm").setAttribute("aria-pressed", String(on));
	}
	invalidate(1);
}

// ---------------------------------------------------------------- timeline: view (zoom, pan)

const ppfFit = () => view.w / (S.end * 1.03);
const ppfMin = () => ppfFit() * 0.6;

function clampView() {
	view.ppf = clamp(view.ppf, ppfMin(), PPF_MAX);
	const span = view.w / view.ppf;
	view.t0 = span >= S.end * 1.04 ? -S.end * 0.01 : clamp(view.t0, -span * 0.04, S.end - span * 0.96);
}
function fitView() { view.ppf = ppfFit(); view.t0 = -S.end * 0.01; clampView(); }
function zoomAt(factor, ax) {
	if (!S) return;
	const f0 = view.t0 + ax / view.ppf;
	view.ppf = clamp(view.ppf * factor, ppfMin(), PPF_MAX);
	view.t0 = f0 - ax / view.ppf;
	clampView(); invalidate(); updateZoomReadout();
}
function zoomBy(factor) {
	const playing = eng.state === "playing" && opt.follow;
	zoomAt(factor, playing && view.q != null ? clamp((view.q - view.t0) * view.ppf, 0, view.w) : view.w / 2);
}
function panBy(frames) { view.t0 += frames; clampView(); invalidate(); }
function zoomMs() { // one 1 ms timer tick across ~240 px
	if (!S) return;
	const f = view.q ?? view.t0 + view.w / view.ppf / 2;
	view.ppf = PPF_MAX; view.t0 = f - view.w / view.ppf / 2; clampView(); invalidate(); updateZoomReadout();
}
function setLaneH(h2, anchorY = view.h / 2) {
	if (!S) return;
	const old = view.laneH; view.laneH = clamp(Math.round(h2), 28, 420);
	if (view.laneH === old) return;
	const nTracksAbove = Math.max(0, (anchorY + view.scroll - 98) / old);
	afterRows();
	setScroll(Math.max(0, nTracksAbove * view.laneH + 98 - anchorY));
}
function updateZoomReadout() {
	const o = $("#zoomread");
	if (!S) { o.textContent = ""; return; }
	const pxMs = view.ppf * MS;
	o.textContent = pxMs >= 1 ? `${pxMs.toFixed(pxMs >= 10 ? 0 : 1)} px/ms` : `${(1 / pxMs).toFixed(1)} ms/px`;
}

// ---------------------------------------------------------------- timeline: drawing

let raf = 0, dirty = 0, lastT = 0;
function invalidate(bits = 7) { dirty |= bits; if (!raf) raf = requestAnimationFrame(frame); }

function frame(now) {
	raf = 0;
	const dt = lastT ? Math.min(0.1, (now - lastT) / 1000) : 0;
	lastT = now;
	const moving = eng.state === "playing" || eng.state === "seeking";
	if (S) {
		const p = currentFrame();
		const { q, iter } = mapPlayhead(p);
		if (q !== view.q || iter !== view.iter || p !== view.p) { view.q = q; view.iter = iter; view.p = p; dirty |= 2 | 4; updateReadouts(p, q, iter); }
		if (eng.state === "playing" && !S.loop && p > S.span.audio_frames + SR) stop();
		if (moving && opt.follow) followView(q);
		if (eng.seek && $("#seekbar")) $("#seekbar").value = eng.seek.target ? clamp(eng.seek.frame / eng.seek.target, 0, 1) : 1;
	}
	if (dirty & 1) { const t = performance.now(); drawRoll(); view.drawMs = performance.now() - t; }
	if (dirty & 2) drawRuler();
	if (dirty & 4) { drawCursor(); updateBoard(dt); }
	dirty = 0;
	if (moving || boardMoving) raf = requestAnimationFrame(frame); else lastT = 0;
}

function followView(q) {
	const span = view.w / view.ppf, t0 = view.t0;
	if (reduceMotion.matches) { if (q < t0 || q > t0 + span * 0.92) view.t0 = q - span * 0.08; }
	else if (q < t0 || q > t0 + span || q > t0 + span * 0.3) view.t0 = q - span * 0.3;
	clampView();
	if (view.t0 !== t0) dirty |= 1 | 2;
}

function drawRoll() {
	const ctx = cv.roll.x, W = view.w, H = view.h;
	ctx.setTransform(view.dpr, 0, 0, view.dpr, 0, 0);
	ctx.fillStyle = C.bg.hex; ctx.fillRect(0, 0, W, H);
	if (!S) return;
	const t0 = view.t0, ppf = view.ppf, t1 = t0 + W / ppf, sy = view.scroll, rows = S.rows;
	const r0 = rowAt(sy), r1 = rowAt(sy + H);
	for (let r = r0; r <= r1; r++) {
		const rw = rows[r], y = rw.y - sy;
		ctx.fillStyle = rw.type === "trk" ? (rw.t.index === sel.track ? C.bg3.hex : rw.t.index & 1 ? C.bg2.hex : C.bg.hex) : rw.type === "auto" ? C.bgA.hex : C.bg2.hex;
		ctx.fillRect(0, y, W, rw.h);
		ctx.fillStyle = C.line.hex; ctx.fillRect(0, y + rw.h - 1, W, 1);
	}
	drawGrid(ctx, W, H, t0, t1, ppf);
	for (let r = r0; r <= r1; r++) {
		const rw = rows[r], y = rw.y - sy;
		if (rw.type === "trk") drawTrackRow(ctx, rw, y, W, t0, t1, ppf);
		else if (rw.type === "auto") drawAutoRow(ctx, rw, y, W, t0, t1, ppf);
		else if (rw.type === "fx") drawFxRow(ctx, rw, y, W, t0, t1, ppf);
		else drawLevelRow(ctx, rw, y, W, t0, t1, ppf);
	}
}

function drawGrid(ctx, W, H, t0, t1, ppf) {
	const L = S.loop, X = (f) => (f - t0) * ppf;
	if (L) {
		const x0 = Math.max(0, X(L.start_frame)), x1 = Math.min(W, X(L.end_frame));
		if (x1 > x0) { ctx.fillStyle = rgba(C.cu, 0.07); ctx.fillRect(x0, 0, x1 - x0, H); }
	}
	const B = S.bar, nb = B.rows;
	if (nb) {
		const bars = new Path2D(), beats = new Path2D();
		for (let i = Math.max(0, upperBound(B.frame, t0) - 1); i < nb; i++) {
			const f = B.frame[i];
			if (f > t1) break;
			const sec = B.section[i], nextF = i + 1 < nb && B.section[i + 1] === sec ? B.frame[i + 1] : S.sec.end_frame[sec];
			const barPx = (nextF - f) * ppf;
			if (barPx >= 4 || B.bar[i] % 4 === 0) { const x = Math.round(X(f)) + 0.5; bars.moveTo(x, 0); bars.lineTo(x, H); }
			const nbeat = (S.seq.get(S.sec.sequence[sec]) || { numerator: 4 }).numerator || 4;
			if (barPx / nbeat >= 12) for (let b = 1; b < nbeat; b++) { const x = Math.round(X(f + ((nextF - f) * b) / nbeat)) + 0.5; beats.moveTo(x, 0); beats.lineTo(x, H); }
		}
		ctx.lineWidth = 1;
		ctx.strokeStyle = rgba(C.line2, 0.6); ctx.stroke(bars);
		ctx.strokeStyle = rgba(C.line, 0.7); ctx.stroke(beats);
	}
	const sc = S.sec;
	ctx.fillStyle = rgba(C.cu, 0.75);
	for (let k = 0; k < sc.rows; k++) {
		const f = sc.start_frame[k];
		if (f > t1) break;
		if (f >= t0 - 2) ctx.fillRect(Math.round(X(f)), 0, 1, H);
	}
	if (L && L.end_frame >= t0 && L.end_frame <= t1) { ctx.fillStyle = rgba(C.cu, 0.9); ctx.fillRect(Math.round(X(L.end_frame)) - 1, 0, 2, H); }
}

const visBuf = new Uint32Array(6000);

function drawTrackRow(ctx, rw, y0, W, t0, t1, ppf) {
	const t = rw.t, N = S.note, V = S.voice, g = geom(rw), kh = g.kh, X = (f) => (f - t0) * ppf;
	const ky = (k) => y0 + g.padY + (g.hi - k) * kh;
	ctx.save(); ctx.beginPath(); ctx.rect(0, y0, W, rw.h - 1); ctx.clip();
	if (kh >= 3.5) { ctx.fillStyle = "rgba(0,16,8,.35)"; for (let k = g.lo; k <= g.hi; k++) if (isBlack(k)) ctx.fillRect(0, ky(k), W, kh); }
	ctx.font = "9px " + MONO; ctx.textBaseline = "alphabetic";
	for (let k = g.lo; k <= g.hi; k++) if (k % 12 === 0) {
		const yy = Math.round(ky(k) + kh);
		ctx.fillStyle = rgba(C.line2, 0.55); ctx.fillRect(0, yy, W, 1);
		if (rw.h >= 56) { ctx.fillStyle = rgba(C.dim, 0.7); ctx.fillText(keyName(k), 3, yy - 2); }
	}
	const arr = t.n, mx = t.mx, nn = arr.length, hlInst = sel.inst >= 0, byPair = opt.color === "pair";
	const bh = Math.max(2, kh > 5 ? kh - 1 : kh), stripe = bh >= 5;
	let nv = 0, a = lowerBound(mx, t0), selR = null;
	for (; a < nn; a++) {
		const i = arr[a], on = N.on_frame[i];
		if (on > t1) break;
		const end = S.noteEnd[i];
		if (end < t0) continue;
		if (nv < visBuf.length) visBuf[nv++] = i;
		const x0 = (on - t0) * ppf, x1 = Math.max(x0 + 1, (S.noteOff[i] - t0) * ppf), w = x1 - x0;
		const y = ky(N.kkey[i]) + (kh - bh) / 2, ci = S.nCol[i], vc = N.voice_count[i], vb = N.vel[i] >> 4 & 7;
		const lit = !hlInst || N.program[i] === sel.inst;
		if (vc === 0) { ctx.strokeStyle = lit ? (byPair ? PAIR_COL[0].hex : INST_PAL[ci].hex) : DIM[ci]; ctx.strokeRect(x0 + 0.5, y + 0.5, Math.max(1, w - 1), Math.max(1, bh - 1)); }
		else { ctx.fillStyle = !lit ? DIM[ci] : byPair ? FILL_P[S.nPair[i]][vb] : FILL[ci][vb]; ctx.fillRect(x0, y, w, bh); }
		if (lit && end > S.noteOff[i]) {
			const xe = (end - t0) * ppf;
			if (xe - x1 > 2) { ctx.fillStyle = byPair ? rgba(PAIR_COL[S.nPair[i]], 0.38) : TAIL[ci]; ctx.fillRect(x1, y + bh * 0.35, xe - x1, Math.max(1, bh * 0.3)); }
		}
		if (stripe && w >= 3 && vc && lit) {
			const n = Math.min(vc, 3), vf = N.voice_first[i];
			for (let q = 0; q < n; q++) { ctx.fillStyle = PAIR_COL[V.pair[vf + q]].hex; ctx.fillRect(x0 + (q * w) / n, y + bh - 2, w / n, 2); }
		}
		if (i === sel.note) selR = [x0, y, w, bh];
	}
	if (nv && nv <= 2500) drawCurves(ctx, y0, rw.h, nv, ky, kh, t0, t1, ppf);
	if (selR) { ctx.strokeStyle = C.silk.hex; ctx.lineWidth = 2; ctx.strokeRect(selR[0] - 1, selR[1] - 1, Math.max(3, selR[2] + 2), selR[3] + 2); ctx.lineWidth = 1; }
	ctx.restore();
}

function drawCurves(ctx, y0, rh, nv, ky, kh, t0, t1, ppf) {
	const N = S.note, V = S.voice, want = [];
	if (opt.pitch && S.lane.pitch) want.push(["pitch", 0]);
	if (opt.glide && S.lane.glide) want.push(["glide", 1]);
	if (opt.lfo && S.lane.lfo) want.push(["lfo", 2]);
	if (!want.length) return;
	const paths = [new Path2D(), new Path2D(), new Path2D()], lfoAmp = Math.min(rh * 0.22, 16);
	for (let q = 0; q < nv; q++) {
		const i = visBuf[q], kk = N.kkey[i], vf = N.voice_first[i], vc = N.voice_count[i];
		if ((S.noteOff[i] - N.on_frame[i]) * ppf < 6) continue;
		for (let k = 0; k < vc; k++) {
			const v = vf + k, anchor = pitchAnchor(v);
			for (const [name, pi] of want) {
				const L = S.lane[name], c = L.count[v];
				if (!c) continue;
				const k0 = L.first[v], k1 = k0 + c, ab = L.abs, path = paths[pi];
				const a = Math.max(k0, lowerBound(ab, t0, k0, k1) - 1), b = Math.min(k1 - 1, upperBound(ab, t1, k0, k1));
				const yv = pi === 2 ? (val) => ky(kk) + kh / 2 - (val / 32768) * lfoAmp : (val) => ky(kk + val / 256 - anchor) + kh / 2;
				let on = false;
				if (a === k0 && ab[k0] > V.start_frame[v]) { path.moveTo((V.start_frame[v] - t0) * ppf, yv(onset(V, name)[v])); on = true; }
				for (let p = a; p <= b; p++) {
					const x = (ab[p] - t0) * ppf, y = yv(L.value[p]);
					if (on) path.lineTo(x, y); else { path.moveTo(x, y); on = true; }
				}
			}
		}
	}
	ctx.lineJoin = "round";
	if (opt.pitch && S.lane.pitch) { ctx.lineWidth = 3; ctx.strokeStyle = "rgba(0,16,8,.65)"; ctx.stroke(paths[0]); ctx.lineWidth = 1.25; ctx.strokeStyle = C.silk.hex; ctx.stroke(paths[0]); }
	if (opt.glide && S.lane.glide) { ctx.setLineDash([4, 3]); ctx.lineWidth = 1.5; ctx.strokeStyle = C.cu.hex; ctx.stroke(paths[1]); ctx.setLineDash([]); }
	if (opt.lfo && S.lane.lfo) { ctx.lineWidth = 1; ctx.strokeStyle = C.cyan.hex; ctx.stroke(paths[2]); }
	ctx.lineWidth = 1;
}

function evLower(a, f) { // first k in a.from..a.to with event frame >= f
	let lo = a.from, hi = a.to;
	while (lo < hi) { const m = (lo + hi) >>> 1; if (S.ev.frame[S.evOrder[m]] < f) lo = m + 1; else hi = m; }
	return lo;
}

function drawAutoRow(ctx, rw, y0, W, t0, t1, ppf) {
	const a = rw.a, E = S.ev, ord = S.evOrder, rh = rw.h, X = (f) => (f - t0) * ppf;
	const fo = (k) => E.frame[ord[k]], vo = (k) => E.value[ord[k]];
	ctx.save(); ctx.beginPath(); ctx.rect(0, y0, W, rh - 1); ctx.clip();
	const k0 = Math.max(a.from, evLower(a, t0) - 1);
	if (a.name === "program") {
		ctx.font = "9px " + MONO; ctx.textBaseline = "middle";
		for (let k = k0; k < a.to; k++) {
			const f = fo(k); if (f > t1) break;
			const nf = k + 1 < a.to ? fo(k + 1) : S.end, x0 = X(f), x1 = X(nf), ins = S.inst.get(vo(k));
			const c = ins ? INST_PAL[ins.color] : C.grey;
			ctx.fillStyle = rgba(c, 0.5); ctx.fillRect(x0, y0 + 6, Math.max(1, x1 - x0 - 1), rh - 12);
			ctx.fillStyle = c.hex; ctx.fillRect(x0, y0 + 6, 2, rh - 12);
			if (x1 - x0 > 36) { ctx.fillStyle = C.silk.hex; ctx.fillText(progName(vo(k)), Math.max(x0, 0) + 5, y0 + rh / 2); }
		}
		ctx.restore(); return;
	}
	let mn = a.min, mx = a.max;
	if (mn === mx) { mn -= 1; mx += 1; }
	const vy = (val) => y0 + 6 + (1 - (val - mn) / (mx - mn)) * (rh - 12), linear = a.name === "seq_volume";
	ctx.strokeStyle = rgba(C.line2, 0.5); ctx.fillStyle = rgba(C.dim, 0.5); ctx.font = "9px " + MONO; ctx.textBaseline = "middle";
	const zero = vy(0); if (mn < 0 && mx > 0) ctx.fillRect(0, Math.round(zero), W, 1);
	ctx.beginPath();
	let py = vy(vo(k0)), complete = true;
	ctx.moveTo(X(fo(k0)), py);
	for (let k = k0 + 1; k < a.to; k++) {
		const f = fo(k), x = X(f), y = vy(vo(k));
		if (linear) ctx.lineTo(x, y); else { ctx.lineTo(x, py); ctx.lineTo(x, y); }
		py = y;
		if (f > t1) { complete = false; break; }
	}
	if (complete) ctx.lineTo(W, py);
	ctx.lineWidth = 1.25; ctx.strokeStyle = C.gold.hex; ctx.stroke(); ctx.lineWidth = 1;
	ctx.fillStyle = C.gold.hex;
	let drawn = 0;
	for (let k = k0; k < a.to && drawn < 800; k++) { const f = fo(k); if (f > t1) break; if (f < t0) continue; ctx.fillRect(X(f) - 1.5, vy(vo(k)) - 1.5, 3, 3); drawn++; }
	ctx.restore();
}

function drawFxRow(ctx, rw, y0, W, t0, t1, ppf) {
	const X = (f) => (f - t0) * ppf;
	ctx.save(); ctx.beginPath(); ctx.rect(0, y0, W, rw.h - 1); ctx.clip();
	ctx.font = "10px " + MONO; ctx.textBaseline = "middle";
	for (const s of S.fxSegs) {
		if (s.to < t0 || s.from > t1) continue;
		const c = FX_COL[s.program] || C.grey, x0 = X(s.from), x1 = X(s.to);
		ctx.fillStyle = rgba(c, 0.32); ctx.fillRect(x0, y0 + 3, Math.max(1, x1 - x0 - 1), 17);
		ctx.fillStyle = c.hex; ctx.fillRect(x0, y0 + 3, 2, 17);
		if (x1 - x0 > 40) { ctx.fillStyle = C.silk.hex; ctx.fillText(s.program, Math.max(x0, 0) + 5, y0 + 12); }
	}
	ctx.fillStyle = C.cyan.hex;
	let lastX = -99;
	for (let k = lowerBound(S.fxFrames, t0); k < S.fx.length; k++) {
		const e = S.fx[k]; if (e.frame > t1) break;
		if (e.kind !== "param") continue;
		const x = Math.round(X(e.frame));
		ctx.fillRect(x, y0 + 22, 1, 8);
		if (x - lastX > 34 && ppf * MS > 0.15) { ctx.fillStyle = C.dim.hex; ctx.fillText(`p${e.index}=${e.value}`, x + 3, y0 + 26); ctx.fillStyle = C.cyan.hex; lastX = x; }
	}
	ctx.fillStyle = C.gold.hex;
	for (const e of S.gain) {
		if (e.frame < t0 - 40 || e.frame > t1) continue;
		const x = X(e.frame), yy = y0 + rw.h - 4 - (e.channel ? 0 : 0);
		ctx.beginPath(); ctx.moveTo(x - 3, yy - 7 + (e.channel ? 0 : 0)); ctx.lineTo(x + 3, yy - 7); ctx.lineTo(x, yy); ctx.fill();
	}
	ctx.restore();
}

function levelNorm(v) { return clamp((v - 105) / 150, 0, 1); } // README 4.12: u8 = 255 + 2.5 dB; -60 dB .. 0 dB

function drawLevelRow(ctx, rw, y0, W, t0, t1, ppf) {
	const lv = S.lv, rh = rw.h - 3, base = y0 + rw.h - 2;
	ctx.save(); ctx.beginPath(); ctx.rect(0, y0, W, rw.h - 1); ctx.clip();
	ctx.fillStyle = rgba(C.line2, 0.5);
	for (const v of [240, 195, 150]) ctx.fillRect(0, Math.round(base - levelNorm(v) * rh), W, 1);
	const outL = new Path2D(), outR = new Path2D(), pairs = [null, new Path2D(), new Path2D(), new Path2D()];
	const cols = Math.ceil(W);
	let first = true;
	for (let x = 0; x < cols; x++) {
		const f0 = t0 + x / ppf, f1 = t0 + (x + 1) / ppf;
		let w0 = Math.floor(f0 / WIN), w1 = Math.floor(f1 / WIN);
		if (w1 < w0) w1 = w0;
		if (w1 < 0 || w0 >= lv.rows) { continue; }
		w0 = Math.max(0, w0); w1 = Math.min(lv.rows - 1, w1);
		let l = 0, r = 0; const pk = [0, 0, 0, 0];
		for (let w = w0; w <= w1; w++) {
			if (lv.out[w * 2] > l) l = lv.out[w * 2];
			if (lv.out[w * 2 + 1] > r) r = lv.out[w * 2 + 1];
			for (let p = 1; p < 4; p++) { const m = Math.max(lv.buses[w * 8 + p * 2], lv.buses[w * 8 + p * 2 + 1]); if (m > pk[p]) pk[p] = m; }
		}
		const yl = base - levelNorm(l) * rh, yr = base - levelNorm(r) * rh;
		if (first) { outL.moveTo(x, yl); outR.moveTo(x, yr); for (let p = 1; p < 4; p++) pairs[p].moveTo(x, base - levelNorm(pk[p]) * rh); first = false; }
		else { outL.lineTo(x, yl); outR.lineTo(x, yr); for (let p = 1; p < 4; p++) pairs[p].lineTo(x, base - levelNorm(pk[p]) * rh); }
	}
	ctx.lineWidth = 1;
	for (let p = 1; p < 4; p++) { ctx.strokeStyle = rgba(PAIR_COL[p], 0.55); ctx.stroke(pairs[p]); }
	ctx.lineWidth = 1.25; ctx.strokeStyle = C.gold.hex; ctx.stroke(outL); ctx.strokeStyle = C.cyan.hex; ctx.stroke(outR); ctx.lineWidth = 1;
	ctx.restore();
}

// ---------------------------------------------------------------- timeline: ruler and cursor

const STEPS_MS = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000];
function fmtMs(ms, step) {
	const s = ms / 1000, m = Math.floor(s / 60), r = s - m * 60;
	return step < 1000 ? `${m}:${pad(Math.floor(r))}.${pad(Math.round((r % 1) * 1000), 3)}` : `${m}:${pad(Math.floor(r))}`;
}

function drawRuler() {
	const ctx = cv.ruler.x, W = view.w, H = FIXED.rulerH;
	ctx.setTransform(view.dpr, 0, 0, view.dpr, 0, 0);
	ctx.fillStyle = C.bg2.hex; ctx.fillRect(0, 0, W, H);
	ctx.fillStyle = C.line.hex; ctx.fillRect(0, H - 1, W, 1);
	if (!S) return;
	const t0 = view.t0, ppf = view.ppf, t1 = t0 + W / ppf, X = (f) => (f - t0) * ppf;
	ctx.font = "10px " + MONO; ctx.textBaseline = "middle";
	// sections (y 1..15)
	const sc = S.sec, L = S.loop;
	for (let k = 0; k < sc.rows; k++) {
		const a = sc.start_frame[k], b = sc.end_frame[k];
		if (b < t0) continue;
		if (a > t1) break;
		const x0 = X(a), x1 = X(b), inLoop = L && a >= L.start_frame;
		ctx.fillStyle = inLoop ? rgba(C.cu, k & 1 ? 0.34 : 0.2) : rgba(C.dim, k & 1 ? 0.24 : 0.12);
		ctx.fillRect(x0, 1, Math.max(1, x1 - x0 - 1), 14);
		const lx = Math.max(x0, 0) + 4;
		if (x1 - lx > 40) { ctx.fillStyle = C.silk.hex; ctx.fillText(`S${sc.sequence[k]} ph${sc.phase[k]}${sc.repeat[k] ? " #" + (sc.repeat[k] + 1) : ""}`, lx, 8); }
	}
	// loop bracket (y 17..24)
	if (L) {
		const x0 = X(L.start_frame), x1 = X(L.end_frame);
		ctx.fillStyle = C.cu.hex; ctx.fillRect(x0, 17, Math.max(1, x1 - x0), 6);
		if (x1 - x0 > 70) { ctx.fillStyle = C.bg.hex; ctx.fillText("LOOP ↻ repeats this region", Math.max(x0, 0) + 4, 20.5); }
	}
	// rate changes (y 25..35)
	const rc = S.rate; ctx.fillStyle = C.gold.hex;
	for (let k = lowerBound(rc.frame, t0 - 600); k < rc.rows; k++) {
		const f = rc.frame[k]; if (f > t1) break;
		const x = X(f); ctx.beginPath(); ctx.moveTo(x - 3, 26); ctx.lineTo(x + 3, 26); ctx.lineTo(x, 33); ctx.fill();
		if (k + 1 >= rc.rows || (rc.frame[k + 1] - f) * ppf > 40) { ctx.fillStyle = C.dim.hex; ctx.fillText(`rate ${rc.rate[k]}`, x + 6, 30); ctx.fillStyle = C.gold.hex; }
	}
	// bars (y 36..49)
	const B = S.bar;
	ctx.fillStyle = C.line2.hex;
	for (let i = Math.max(0, upperBound(B.frame, t0) - 1); i < B.rows; i++) {
		const f = B.frame[i]; if (f > t1) break;
		const sec = B.section[i], nextF = i + 1 < B.rows && B.section[i + 1] === sec ? B.frame[i + 1] : sc.end_frame[sec], barPx = (nextF - f) * ppf;
		const x = Math.round(X(f));
		ctx.fillStyle = C.line2.hex; ctx.fillRect(x, 36, 1, 13);
		const stride = Math.max(1, Math.ceil(30 / Math.max(barPx, 1)));
		if (B.bar[i] % stride === 0 && barPx >= 6) { ctx.fillStyle = C.silk.hex; ctx.fillText(String(B.bar[i] + 1), x + 3, 42); }
		const nb = (S.seq.get(sc.sequence[sec]) || { numerator: 4 }).numerator || 4;
		if (barPx / nb >= 8) { ctx.fillStyle = C.line2.hex; for (let b = 1; b < nb; b++) ctx.fillRect(Math.round(X(f + ((nextF - f) * b) / nb)), 43, 1, 6); }
	}
	// time (y 50..63)
	const pxMs = ppf * MS, step = STEPS_MS.find((s) => s * pxMs >= 90) || 300000;
	const lead = String(step)[0], div = lead === "2" ? 4 : 5, minor = step / div;
	const m0 = Math.floor(t0 / MS / step) * step;
	for (let ms = m0; ms * MS <= t1; ms += step) {
		const x = Math.round(X(ms * MS));
		ctx.fillStyle = C.silk.hex; ctx.fillRect(x, 51, 1, 12);
		ctx.fillText(fmtMs(ms, step), x + 3, 57);
		if (minor * pxMs >= 6) { ctx.fillStyle = C.dim.hex; for (let j = 1; j < div; j++) ctx.fillRect(Math.round(X((ms + minor * j) * MS)), 58, 1, 5); }
	}
	// playhead
	if (view.q != null) {
		const x = X(view.q);
		if (x >= -6 && x <= W + 6) {
			ctx.fillStyle = C.gold.hex; ctx.fillRect(Math.round(x), 0, 1, H);
			ctx.beginPath(); ctx.moveTo(x - 5, 0); ctx.lineTo(x + 5, 0); ctx.lineTo(x, 8); ctx.fill();
		}
	}
}

const hover = { x: -1, y: -1, on: false };
function drawCursor() {
	const ctx = cv.cur.x, W = view.w, H = view.h;
	ctx.setTransform(view.dpr, 0, 0, view.dpr, 0, 0);
	ctx.clearRect(0, 0, W, H);
	if (!S) return;
	if (hover.on) { ctx.fillStyle = rgba(C.silk, 0.22); ctx.fillRect(Math.round(hover.x), 0, 1, H); }
	if (view.q != null) {
		const x = (view.q - view.t0) * view.ppf;
		if (x >= -2 && x <= W + 2) { ctx.fillStyle = C.gold.hex; ctx.fillRect(Math.round(x) - (view.ppf > 0.5 ? 0 : 0), 0, 1.5, H); }
	}
}

// ---------------------------------------------------------------- timeline: scroll, pointer, wheel, keyboard

function setScroll(y) {
	const max = S ? Math.max(0, S.rowsH - view.h) : 0;
	const v = clamp(y, 0, max);
	view.scroll = v;
	if (el.heads) el.heads.style.transform = `translateY(${-v}px)`;
	updateScrollbar();
	invalidate(1 | 4);
}
function setFollow(on) { opt.follow = on; $("#ofollow").checked = on; if (on) invalidate(); }
function manualMove() { if (opt.follow && (eng.state === "playing" || eng.state === "seeking")) setFollow(false); }

const ptrs = new Map();
let drag = null;
const stageXY = (e) => { const r = el.stage.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };

function onDown(e) {
	if (!S) return;
	el.stage.setPointerCapture(e.pointerId);
	const [x, y] = stageXY(e);
	ptrs.set(e.pointerId, { x, y });
	hideTip();
	if (ptrs.size === 2) {
		const [a, b] = [...ptrs.values()], mid = (a.x + b.x) / 2;
		drag = { kind: "pinch", d0: Math.hypot(a.x - b.x, a.y - b.y) || 1, ppf0: view.ppf, f0: view.t0 + mid / view.ppf };
		return;
	}
	if (ptrs.size === 1) drag = { kind: "pan", x0: x, y0: y, t0: view.t0, s0: view.scroll, moved: false, button: e.button };
	el.stage.focus({ preventScroll: true });
}
function onMove(e) {
	const [x, y] = stageXY(e);
	if (ptrs.has(e.pointerId)) ptrs.set(e.pointerId, { x, y });
	if (drag && drag.kind === "pinch" && ptrs.size >= 2) {
		const [a, b] = [...ptrs.values()], mid = (a.x + b.x) / 2;
		manualMove();
		view.ppf = clamp(drag.ppf0 * (Math.hypot(a.x - b.x, a.y - b.y) / drag.d0), ppfMin(), PPF_MAX);
		view.t0 = drag.f0 - mid / view.ppf; clampView(); invalidate(); updateZoomReadout();
	} else if (drag && drag.kind === "pan") {
		const dx = x - drag.x0, dy = y - drag.y0;
		if (!drag.moved && Math.hypot(dx, dy) > 4) { drag.moved = true; manualMove(); el.stage.classList.add("grab"); }
		if (drag.moved) { view.t0 = drag.t0 - dx / view.ppf; clampView(); setScroll(drag.s0 - dy); invalidate(); }
	} else if (e.pointerType !== "touch") {
		hover.x = x; hover.y = y; hover.on = true; invalidate(4);
		showTip(e, x, y);
	}
}
function onUp(e) {
	const was = ptrs.delete(e.pointerId);
	if (drag && drag.kind === "pan" && was && !drag.moved && e.type === "pointerup" && drag.button === 0) clickAt(drag.x0, drag.y0);
	if (!ptrs.size) { drag = null; el.stage.classList.remove("grab"); } else if (drag && drag.kind === "pinch") drag = null;
}
function onLeave() { hover.on = false; hideTip(); invalidate(4); }

function clickAt(x, y) {
	const hit = hitAt(x, y);
	if (!hit) return;
	if (hit.type === "trk") {
		setTrack(hit.rw.t.index);
		selectNote(hit.note);
	} else selectNote(-1);
}
function onDbl(e) {
	if (!S) return;
	const [x] = stageXY(e);
	seekTo(view.t0 + x / view.ppf);
}

function onWheel(e) {
	if (!S) return;
	e.preventDefault();
	const [x, y] = stageXY(e);
	let dy = e.deltaY, dx = e.deltaX;
	if (e.deltaMode === 1) { dy *= 16; dx *= 16; } else if (e.deltaMode === 2) { dy *= view.h; dx *= view.w; }
	if (e.ctrlKey || e.metaKey) {
		const k = Math.abs(dy) < 30 ? 0.012 : 0.0035;
		const follow = opt.follow && eng.state === "playing" && view.q != null;
		zoomAt(Math.exp(-dy * k), follow ? clamp((view.q - view.t0) * view.ppf, 0, view.w) : Math.max(0, x));
	} else if (e.shiftKey || e.altKey) {
		setLaneH(view.laneH * Math.exp(-dy * 0.002), y);
	} else if (Math.abs(dx) > Math.abs(dy)) {
		manualMove();
		panBy(dx / view.ppf);
	} else {
		setScroll(view.scroll + dy);
	}
}

function stepNote(dir) {
	if (!S) return;
	const t = S.tracks[sel.track >= 0 ? sel.track : sel.note >= 0 ? S.nTrack[sel.note] : 0];
	if (!t || !t.n.length) return;
	const N = S.note;
	let a;
	if (sel.note >= 0 && S.nTrack[sel.note] === t.index) a = t.n.indexOf(sel.note) + dir;
	else a = dir > 0 ? lowerBound(t.mx, view.t0) : t.n.length - 1;
	a = clamp(a, 0, t.n.length - 1);
	setTrack(t.index);
	selectNote(t.n[a]);
	const on = N.on_frame[t.n[a]], span = view.w / view.ppf;
	if (on < view.t0 || on > view.t0 + span * 0.95) { manualMove(); view.t0 = on - span * 0.3; clampView(); }
	revealTrack(t.index);
	invalidate();
}
function revealTrack(ti) {
	const rw = S.rows.find((r) => r.type === "trk" && r.t.index === ti);
	if (!rw) return;
	if (rw.y < view.scroll) setScroll(rw.y - 4); else if (rw.y + rw.h > view.scroll + view.h) setScroll(rw.y + rw.h - view.h + 4);
}
function stepTrack(dir) {
	if (!S || !S.tracks.length) return;
	const ti = clamp((sel.track < 0 ? (dir > 0 ? -1 : S.tracks.length) : sel.track) + dir, 0, S.tracks.length - 1);
	setTrack(ti); revealTrack(ti);
}

function onKey(e) {
	if (!S || e.ctrlKey || e.metaKey || e.altKey) return;
	const span = view.w / view.ppf;
	let used = true;
	switch (e.key) {
		case "ArrowLeft": manualMove(); panBy(-span * (e.shiftKey ? 0.5 : 0.1)); break;
		case "ArrowRight": manualMove(); panBy(span * (e.shiftKey ? 0.5 : 0.1)); break;
		case "ArrowUp": setScroll(view.scroll - 48); break;
		case "ArrowDown": setScroll(view.scroll + 48); break;
		case "PageUp": stepTrack(-1); break;
		case "PageDown": stepTrack(1); break;
		case "+": case "=": zoomBy(1.25); break;
		case "-": case "_": zoomBy(0.8); break;
		case "0": fitView(); invalidate(); updateZoomReadout(); break;
		case "1": zoomMs(); break;
		case "[": setLaneH(view.laneH / 1.25, view.h / 2); break;
		case "]": setLaneH(view.laneH * 1.25, view.h / 2); break;
		case "Home": manualMove(); view.t0 = -span * 0.02; clampView(); invalidate(); break;
		case "End": manualMove(); view.t0 = S.end - span * 0.98; clampView(); invalidate(); break;
		case ",": stepNote(-1); break;
		case ".": stepNote(1); break;
		case "Enter": if (sel.note >= 0) seekTo(S.note.on_frame[sel.note]); else used = false; break;
		case "Escape": selectNote(-1); sel.inst = -1; renderInstruments(); invalidate(); break;
		case "f": setFollow(!opt.follow); break;
		default: used = false;
	}
	if (used) e.preventDefault();
}

// ---------------------------------------------------------------- hit testing and tooltip

function hitAt(x, y) {
	if (!S) return null;
	const yy = y + view.scroll;
	if (yy >= S.rowsH) return null;
	const rw = S.rows[rowAt(yy)], f = view.t0 + x / view.ppf;
	if (rw.type !== "trk") return { type: rw.type, rw, f };
	const t = rw.t, N = S.note, g = geom(rw), tol = 2 / view.ppf;
	let best = -1, bd = 1e9;
	for (let a = lowerBound(t.mx, f - tol); a < t.n.length; a++) {
		const i = t.n[a], on = N.on_frame[i];
		if (on > f + tol) break;
		if (S.noteEnd[i] < f - tol) continue;
		const cy = rw.y + g.padY + (g.hi - N.kkey[i]) * g.kh + g.kh / 2, dy = Math.abs(yy - cy);
		if (dy > Math.max(g.kh, 5) / 2 + 1) continue;
		const score = dy + (f >= on - tol && f <= S.noteOff[i] + tol ? 0 : 50);
		if (score < bd) { bd = score; best = i; }
	}
	return { type: "trk", rw, f, note: best };
}

function pairText(p) { return `pair ${p} (buses ${PAIR_INFO[p].buses}, ${PAIR_INFO[p].serio})`; }

function tipText(hit) {
	if (!hit) return "";
	const f = hit.f, t = fmtTime(f);
	if (hit.type === "trk") {
		if (hit.note < 0) return `${trackName(hit.rw.t)} · ${t}`;
		const i = hit.note, N = S.note, V = S.voice, vc = N.voice_count[i], vf = N.voice_first[i];
		const pairs = [...new Set(Array.from({ length: vc }, (_, k) => V.pair[vf + k]))];
		return [`${trackName(hit.rw.t)} · ${keyName(N.kkey[i])} (${N.kkey[i]}) · vel ${N.vel[i]}`,
			`${progName(N.program[i])} · ${fmtTime(N.on_frame[i])} → ${fmtTime(S.noteOff[i])} (${((S.noteOff[i] - N.on_frame[i]) / SR).toFixed(2)} s)`,
			vc ? `${vc} voice${vc > 1 ? "s" : ""} → ${pairs.map((p) => "pair " + p).join(", ")}` : "no voice (rejected or unknown program)"].join("\n");
	}
	if (hit.type === "auto") {
		const a = hit.rw.a, k = evLower(a, f + 1) - 1;
		return `${trackName(hit.rw.t)} · ${a.name}${k >= a.from ? " = " + S.ev.value[S.evOrder[k]] : ""} · ${t}`;
	}
	if (hit.type === "fx") {
		const k = upperBound(S.fxFrames, f) - 1;
		return k >= 0 ? `${S.fx[k].program} · p[${S.fx[k].p.join(", ")}] · ${t}` : t;
	}
	if (hit.type === "lv") {
		const w = Math.floor(f / WIN);
		if (w < 0 || w >= S.lv.rows) return t;
		const d = (v) => ((v - 255) / 2.5).toFixed(1);
		return `DAC L ${d(S.lv.out[w * 2])} dB · R ${d(S.lv.out[w * 2 + 1])} dB · ${t}`;
	}
	return t;
}
function showTip(e, x, y) {
	const txt = tipText(hitAt(x, y));
	if (!txt) { hideTip(); return; }
	const tip = el.tip;
	tip.textContent = txt; tip.hidden = false;
	const box = el.tl.getBoundingClientRect(), tw = tip.offsetWidth, th = tip.offsetHeight;
	let tx = e.clientX - box.left + 14, ty = e.clientY - box.top + 16;
	if (tx + tw > box.width - 4) tx = e.clientX - box.left - tw - 10;
	if (ty + th > box.height - 4) ty = e.clientY - box.top - th - 10;
	tip.style.transform = `translate(${Math.max(2, tx)}px,${Math.max(2, ty)}px)`;
}
function hideTip() { el.tip.hidden = true; }

function setTrack(i) {
	sel.track = i;
	for (const li of $$(".head.trk", el.heads)) {
		const on = li.querySelector(".tg").dataset.track === String(i);
		li.classList.toggle("sel", on); li.querySelector(".nm").setAttribute("aria-pressed", String(on));
	}
	invalidate(1);
}
function selectNote(i) {
	sel.note = i; sel.voice = 0;
	renderNote();
	if (i >= 0) showTab("note");
	invalidate(1 | 4);
}

// ---------------------------------------------------------------- inspector helpers

function showTab(name) { $(`#t-${name}`).checked = true; }
function activeTab() { return $$('input[name="tab"]').find((r) => r.checked).id.slice(2); }

function valueNode(v, depth = 0) {
	if (v == null) return "—";
	if (typeof v === "boolean") return v ? "yes" : "no";
	if (typeof v === "number") return Number.isInteger(v) ? String(v) : String(+v.toFixed(5));
	if (typeof v === "string") return /^[0-9a-f]{24,}$/i.test(v) ? h("code", { class: "hex", text: v }) : v;
	if (Array.isArray(v)) return v.every((x) => typeof x !== "object" || x == null) ? `[${v.map((x) => (typeof x === "number" && !Number.isInteger(x) ? +x.toFixed(4) : x)).join(", ")}]` : v.map((x) => valueNode(x, depth + 1));
	return depth > 3 ? JSON.stringify(v) : kv(v, [], depth + 1);
}
function kv(obj, skip = [], depth = 0) {
	const dl = h("dl", { class: "kv" });
	for (const [k, v] of Object.entries(obj)) {
		if (skip.includes(k)) continue;
		dl.append(h("dt", { text: k.replaceAll("_", " ") }), h("dd", null, valueNode(v, depth)));
	}
	return dl;
}
const row2 = (k, v) => [h("dt", { text: k }), h("dd", null, v)];
const empty = (t) => h("p", { class: "empty", text: t });
const FLAG_NAMES = [[1, "loop"], [2, "reverse"], [4, "bidirectional"], [8, "direct"], [16, "legato"], [32, "retrigger"], [64, "handoff"]];
const flagChips = (f) => FLAG_NAMES.filter(([b]) => f & b).map(([, n]) => h("span", { class: "tag", text: n }));
const REGION_FLAGS = [[1, "loop"], [2, "reverse"], [4, "bidirectional"]];
const FILTER_MODES = ["LP·LP·HP·HP", "LP·LP·LP·HP", "LP·LP·LP2·LP2", "LP·LP·LP·LP2"];
const instSwatch = (p) => h("i", { class: "sw", style: `background:${INST_PAL[S.inst.get(p).color].hex}` });

// ---------------------------------------------------------------- inspector: note and voice

function noteTrack(i) { return S.tracks[S.nTrack[i]]; }

function renderNote() {
	const box = $("#note-body");
	box.replaceChildren();
	scope.v = -1;
	if (!S || sel.note < 0) { box.append(empty("Click a note on the timeline. Its voices, curves and bus routing appear here.")); return; }
	const i = sel.note, N = S.note, V = S.voice, vc = N.voice_count[i], vf = N.voice_first[i], ins = S.inst.get(N.program[i]);
	const sec = N.section[i], fxk = upperBound(S.fxFrames, N.on_frame[i]) - 1;
	box.append(
		h("div", { class: "notehead" },
			h("b", { class: "big", text: keyName(N.kkey[i]) }),
			h("span", { class: "sub", text: `key ${N.kkey[i]} · seq key ${N.key[i]} · vel ${N.vel[i]}` }),
			h("button", { type: "button", class: "chipbtn", "aria-pressed": String(sel.inst === N.program[i]), onclick: () => { sel.inst = sel.inst === N.program[i] ? -1 : N.program[i]; renderInstruments(); renderNote(); invalidate(1); }, title: "Highlight every note of this instrument" }, instSwatch(N.program[i]), progName(N.program[i]), " ", sel.inst === N.program[i] ? "highlighted" : "highlight")),
		h("dl", { class: "kv" },
			row2("track", `${trackName(noteTrack(i))} (channel ${N.channel[i]})`),
			row2("on", `${fmtTime(N.on_frame[i])} · frame ${N.on_frame[i]}`),
			row2("off", N.off_frame[i] === NONE ? "never (voice ends it)" : `${fmtTime(N.off_frame[i])} · hold ${((S.noteOff[i] - N.on_frame[i]) / SR).toFixed(3)} s`),
			row2("tail", `${fmtTime(S.noteEnd[i])} · rings ${((S.noteEnd[i] - S.noteOff[i]) / SR).toFixed(3)} s after release`),
			row2("section", sec < S.sec.rows ? `#${sec} · S${S.sec.sequence[sec]} phase ${S.sec.phase[sec]} repeat ${S.sec.repeat[sec]} · transpose ${S.sec.transpose[sec]}` : String(sec)),
			row2("effect", fxk >= 0 ? `${S.fx[fxk].program} at note start` : "none yet")));
	if (!vc) { box.append(empty("This note has no voice: the HLE rejected it (key weight, velocity gate or unknown program).")); return; }
	const pairs = [...new Set(Array.from({ length: vc }, (_, k) => V.pair[vf + k]))];
	box.append(h("div", { class: "route" }, h("b", { text: "Routing" }), pairs.map((p) => h("div", { class: "rt" }, h("i", { class: "pp", style: `color:${PAIR_COL[p].hex}`, text: "P" + p }), `${pairText(p)}: ${PAIR_INFO[p].role}`))));
	sel.voice = clamp(sel.voice, 0, vc - 1);
	box.append(h("h3", { text: `Voices (${vc})` }), h("ul", { class: "voices", role: "listbox", "aria-label": "Voices of the selected note" },
		Array.from({ length: vc }, (_, k) => {
			const v = vf + k;
			return h("li", null, h("button", { type: "button", role: "option", "aria-selected": String(k === sel.voice), onclick: () => { sel.voice = k; renderNote(); } },
				h("b", { text: `L${V.layer[v]}` }), ` slot ${V.slot[v]} · `, h("i", { class: "pp", style: `color:${PAIR_COL[V.pair[v]].hex}`, text: "P" + V.pair[v] }), ` · region ${V.region[v]} `, flagChips(V.flags[v])));
		})));
	const v = vf + sel.voice;
	box.append(voiceDetail(v), h("h3", { text: "Voice curves" }), h("canvas", { id: "scope", "aria-label": "Voice curves over time" }),
		h("p", { class: "note", text: "Lanes without points are constant at their onset value (dashed). Pitch and glide are plotted in semitones against the sounding key; the gold line is the playhead." }));
	scope.c = $("#scope"); scope.v = v; sizeScope(); drawScope();
}

function voiceDetail(v) {
	const V = S.voice, R = S.reg, r = V.region[v];
	const dl = h("dl", { class: "kv" },
		row2("layer / slot", `${V.layer[v]} / ${V.slot[v]}`),
		row2("bus pair", h("span", null, h("i", { class: "pp", style: `color:${PAIR_COL[V.pair[v]].hex}`, text: "P" + V.pair[v] }), " ", pairText(V.pair[v]))),
		row2("sounding key", `${keyName(V.skey[v])} (${V.skey[v]})`),
		row2("filter mode", `${V.filter_mode[v]} · ${FILTER_MODES[V.filter_mode[v]] || "?"}`),
		row2("start", `${fmtTime(V.start_frame[v])}`),
		row2("release", V.release_frame[v] === NONE ? "—" : fmtTime(V.release_frame[v])),
		row2("stop", V.stop_frame[v] === NONE ? "—" : fmtTime(V.stop_frame[v])),
		row2("flags", V.flags[v] ? flagChips(V.flags[v]) : "none"),
		row2("lanes", LANE_NAMES.filter((n) => S.lane[n] && S.lane[n].count[v]).map((n) => `${n}×${S.lane[n].count[v]}`).join(" ") || "all constant"));
	if (r < R.rows) dl.append(...row2("sample region", h("span", null, `#${r} · words ${R.start[r]}…${R.end[r]} (${R.end[r] - R.start[r]}) `, REGION_FLAGS.filter(([b]) => R.flags[r] & b).map(([, n]) => h("span", { class: "tag", text: n })), " ",
		h("button", { type: "button", class: "mini", onclick: () => openRegion(r) }, "open"))));
	const wrap = h("div", null, dl);
	if (r < R.rows) wrap.append(h("canvas", { class: "peak", width: 256, height: 40, "data-region": r, "aria-label": `waveform overview of sample region ${r}` }));
	queueMicrotask(() => $$("canvas.peak", wrap).forEach(drawPeakThumb));
	return wrap;
}

// ---- scope: per-voice curves

const scope = { c: null, v: -1 };
const SCOPE = [
	["pitch", "pitch", "st"], ["glide", "glide", "st"], ["lfo", "lfo", ""], ["env_pitch", "env pitch", ""], ["env_filter", "env filter", ""], ["env_amp", "env amp", ""],
	["left", "left vol", "dB"], ["right", "right vol", "dB"], ["k1", "k1", ""], ["k2", "k2", ""],
];
function sizeScope() {
	const c = scope.c;
	if (!c) return;
	const w = Math.max(200, c.parentElement.clientWidth - 2), hh = SCOPE.length * 40 + 18, dpr = window.devicePixelRatio || 1;
	c.width = Math.round(w * dpr); c.height = Math.round(hh * dpr); c.style.width = w + "px"; c.style.height = hh + "px";
}
function drawScope() {
	const c = scope.c, v = scope.v;
	if (!c || v < 0 || !S) return;
	const ctx = c.getContext("2d"), dpr = window.devicePixelRatio || 1, W = c.width / dpr, V = S.voice;
	ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
	ctx.fillStyle = C.bgA.hex; ctx.fillRect(0, 0, W, c.height / dpr);
	const st = V.start_frame[v], en = Math.max(S.vEnd[v], st + 1), x0 = 64, pw = W - x0 - 6, X = (f) => x0 + ((f - st) / (en - st)) * pw;
	ctx.font = "9px " + MONO; ctx.textBaseline = "middle";
	const anchor = pitchAnchor(v), col = INST_PAL[S.nCol[V.note[v]]];
	SCOPE.forEach(([name, label, unit], li) => {
		const y0 = 4 + li * 40, hh = 34, L = S.lane[name];
		let tf;
		if (name === "pitch" || name === "glide") tf = (e) => e / 256 - anchor;
		else if (name === "left" || name === "right") tf = (e) => Math.max(-60, volDb(e));
		else if (name === "k1" || name === "k2") tf = (e) => (e & 0xfff0) / 65536;
		else tf = (e) => e;
		const pts = [[st, tf(onset(V, name)[v])]];
		const cnt = L ? L.count[v] : 0;
		for (let k = 0; k < cnt; k++) pts.push([L.abs[L.first[v] + k], tf(L.value[L.first[v] + k])]);
		pts.push([en, pts[pts.length - 1][1]]);
		let lo = Infinity, hi = -Infinity;
		for (const p of pts) { if (p[1] < lo) lo = p[1]; if (p[1] > hi) hi = p[1]; }
		if (name.startsWith("env")) { lo = 0; hi = 32767; }
		else if (name === "left" || name === "right") { lo = -60; hi = 0; }
		else if (name === "k1" || name === "k2") { lo = 0; hi = 1; }
		else if (name === "pitch" || name === "glide" || name === "lfo") { const m = Math.max(name === "lfo" ? 1024 : 1, Math.abs(lo), Math.abs(hi)); lo = -m; hi = m; }
		const Y = (val) => y0 + 3 + (1 - (val - lo) / (hi - lo || 1)) * (hh - 6);
		ctx.fillStyle = C.bg2.hex; ctx.fillRect(x0, y0, pw, hh);
		if (lo < 0 && hi > 0) { ctx.fillStyle = rgba(C.line2, 0.6); ctx.fillRect(x0, Math.round(Y(0)), pw, 1); }
		if (V.release_frame[v] !== NONE) { ctx.fillStyle = rgba(C.hot, 0.7); ctx.fillRect(Math.round(X(V.release_frame[v])), y0, 1, hh); }
		ctx.beginPath();
		pts.forEach((p, k) => (k ? ctx.lineTo(X(p[0]), Y(p[1])) : ctx.moveTo(X(p[0]), Y(p[1]))));
		ctx.lineWidth = 1.5; ctx.strokeStyle = col.hex;
		ctx.setLineDash(cnt ? [] : [3, 3]); ctx.stroke(); ctx.setLineDash([]); ctx.lineWidth = 1;
		ctx.fillStyle = cnt ? C.silk.hex : C.dim.hex; ctx.textAlign = "left"; ctx.fillText(label, 2, y0 + 9);
		ctx.fillStyle = C.dim.hex; ctx.fillText(unit || (cnt ? `${cnt} pts` : "const"), 2, y0 + 21);
		ctx.textAlign = "right"; ctx.fillStyle = C.dim.hex;
		const f2 = (n) => (Math.abs(n) >= 1000 ? Math.round(n) : +n.toFixed(2));
		ctx.fillText(String(f2(hi)), x0 - 3, y0 + 7); ctx.fillText(String(f2(lo)), x0 - 3, y0 + hh - 5); ctx.textAlign = "left";
	});
	ctx.fillStyle = C.dim.hex;
	ctx.fillText(fmtTime(st), x0, SCOPE.length * 40 + 8); ctx.textAlign = "right"; ctx.fillText(fmtTime(en), x0 + pw, SCOPE.length * 40 + 8); ctx.textAlign = "left";
	if (view.p != null && S.loop == null || view.q != null) {
		const q = view.q;
		if (q >= st && q <= en) { ctx.fillStyle = C.gold.hex; ctx.fillRect(Math.round(X(q)), 0, 1, SCOPE.length * 40); }
	}
}

// ---- sample waveforms (peaks overview, and deep zoom straight from samples.bin)

function drawPeakThumb(cvs) {
	const r = +cvs.dataset.region, ctx = cvs.getContext("2d"), W = cvs.width, H = cvs.height;
	ctx.fillStyle = C.bgA.hex; ctx.fillRect(0, 0, W, H);
	if (!S || r >= S.reg.rows) return;
	const o = S.regPeak[r];
	ctx.fillStyle = C.line2.hex; ctx.fillRect(0, H / 2, W, 1);
	ctx.fillStyle = C.cyan.hex;
	for (let c = 0; c < 256 && c < W; c++) {
		const mn = S.peaks[o + c * 2], mxv = S.peaks[o + c * 2 + 1];
		const y0 = H / 2 - (mxv / 128) * (H / 2), y1 = H / 2 - (mn / 128) * (H / 2);
		ctx.fillRect(c * (W / 256), y0, Math.max(1, W / 256), Math.max(1, y1 - y0));
	}
}

const wave = { c: null, a: 0, b: 1, s: 0, e: 1, label: "" };
function openRegion(r) {
	sel.region = r; showTab("inst");
	renderInstruments();
}
function waveRange(start, end, label) { wave.s = start; wave.e = end; wave.a = start; wave.b = end; wave.label = label; drawWave(); }
function drawWave() {
	const c = wave.c;
	if (!c) return;
	const dpr = window.devicePixelRatio || 1, W = Math.max(100, c.clientWidth), H = 120;
	c.width = Math.round(W * dpr); c.height = H * dpr; c.style.height = H + "px";
	const ctx = c.getContext("2d");
	ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
	ctx.fillStyle = C.bgA.hex; ctx.fillRect(0, 0, W, H);
	ctx.fillStyle = C.line2.hex; ctx.fillRect(0, H / 2, W, 1);
	const a = Math.floor(wave.a), b = Math.ceil(wave.b), n = b - a, s16 = G.samples16;
	const info = $("#waveinfo");
	if (!s16) { ctx.fillStyle = C.dim.hex; ctx.fillText("samples.bin not loaded yet", 8, 20); return; }
	const i8 = (w) => { const v = s16[w] >> 8; return v > 127 ? v - 256 : v; };
	ctx.fillStyle = C.cyan.hex;
	if (n <= W / 3) { // sample-accurate stems
		ctx.strokeStyle = C.cyan.hex; ctx.beginPath();
		for (let w = a; w < b; w++) { const x = ((w - wave.a) / n) * W, y = H / 2 - (i8(w) / 128) * (H / 2 - 2); w === a ? ctx.moveTo(x, y) : ctx.lineTo(x, y); }
		ctx.stroke();
		for (let w = a; w < b; w++) ctx.fillRect(((w - wave.a) / n) * W - 1, H / 2 - (i8(w) / 128) * (H / 2 - 2) - 1, 3, 3);
	} else for (let x = 0; x < W; x++) {
		const w0 = a + Math.floor((x / W) * n), w1 = Math.max(w0 + 1, a + Math.floor(((x + 1) / W) * n));
		let mn = 127, mx = -128;
		const step = Math.max(1, Math.floor((w1 - w0) / 256));
		for (let w = w0; w < w1 && w < s16.length; w += step) { const v = i8(w); if (v < mn) mn = v; if (v > mx) mx = v; }
		ctx.fillRect(x, H / 2 - (mx / 128) * (H / 2 - 2), 1, Math.max(1, ((mx - mn) / 128) * (H / 2 - 2)));
	}
	if (wave.a < wave.s || wave.b > wave.e) { ctx.fillStyle = rgba(C.hot, 0.2); }
	info.textContent = `${wave.label} · words ${a}…${b} (${n}) · ${n <= W / 3 ? "samples" : (n / W).toFixed(1) + " words/px"}`;
}
function waveWheel(e) {
	e.preventDefault();
	const r = wave.c.getBoundingClientRect(), fx = (e.clientX - r.left) / r.width, span = wave.b - wave.a;
	if (e.ctrlKey || e.deltaY !== 0) {
		const ns = clamp(span * Math.exp(e.deltaY * 0.0025), 16, wave.e - wave.s), at = wave.a + fx * span;
		wave.a = clamp(at - fx * ns, wave.s, wave.e - ns); wave.b = wave.a + ns;
	}
	drawWave();
}
function wavePan(e) {
	if (!(e.buttons & 1)) return;
	const span = wave.b - wave.a, d = (-e.movementX / wave.c.clientWidth) * span;
	wave.a = clamp(wave.a + d, wave.s, wave.e - span); wave.b = wave.a + span; drawWave();
}
function waveBox(initial) {
	const c = h("canvas", { class: "wave", tabindex: 0, "aria-label": "Sample waveform. Wheel to zoom, drag to pan, plus and minus keys zoom." });
	wave.c = c;
	c.addEventListener("wheel", waveWheel, { passive: false });
	c.addEventListener("pointermove", wavePan);
	c.addEventListener("pointerdown", (e) => c.setPointerCapture(e.pointerId));
	c.addEventListener("keydown", (e) => {
		const span = wave.b - wave.a;
		if (e.key === "+" || e.key === "=") { const m = (wave.a + wave.b) / 2, ns = Math.max(16, span / 1.5); wave.a = m - ns / 2; wave.b = m + ns / 2; }
		else if (e.key === "-") { const ns = Math.min(wave.e - wave.s, span * 1.5), m = (wave.a + wave.b) / 2; wave.a = clamp(m - ns / 2, wave.s, wave.e - ns); wave.b = wave.a + ns; }
		else if (e.key === "ArrowLeft") { wave.a = Math.max(wave.s, wave.a - span * 0.1); wave.b = wave.a + span; }
		else if (e.key === "ArrowRight") { wave.a = Math.min(wave.e - span, wave.a + span * 0.1); wave.b = wave.a + span; }
		else if (e.key === "0") { wave.a = wave.s; wave.b = wave.e; }
		else return;
		e.preventDefault(); drawWave();
	});
	queueMicrotask(() => initial && waveRange(...initial));
	return c;
}

// ---------------------------------------------------------------- inspector: instruments

function renderInstruments() {
	const box = $("#inst-body");
	box.replaceChildren();
	if (!S) { box.append(empty("Load a song to list its instruments.")); return; }
	const list = [...S.inst.values()].sort((a, b) => b.notes - a.notes);
	box.append(h("p", { class: "note", text: "Select an instrument to highlight its notes on the timeline. Press Escape on the timeline to clear." }),
		h("ul", { class: "inst-list" }, list.map((ins) => h("li", null,
			h("button", { type: "button", "aria-pressed": String(sel.inst === ins.program), onclick: () => { sel.inst = sel.inst === ins.program ? -1 : ins.program; renderInstruments(); invalidate(1); } },
				instSwatch(ins.program), h("b", { text: progName(ins.program) }), h("span", { class: "k", text: ins.j ? ins.j.kind : "no data" }),
				h("span", { class: "n", text: `${ins.notes} notes · ${ins.tracks.size} trk` }))))));
	if (sel.inst >= 0 && S.inst.has(sel.inst)) box.append(instDetail(S.inst.get(sel.inst)));
	else if (sel.region >= 0 && sel.region < S.reg.rows) box.append(waveSection(sel.region));
}

function waveSection(r, ins) {
	const R = S.reg, flags = REGION_FLAGS.filter(([b]) => R.flags[r] & b).map(([, n]) => n).join(" ");
	const sec = h("section", { class: "wavebox" }, h("h3", { text: `Sample region #${r}` }), waveBox([R.start[r], R.end[r], `region ${r}${flags ? " · " + flags : ""}`]), h("output", { id: "waveinfo", class: "note" }),
		h("div", { class: "row" }, h("button", { type: "button", class: "mini", onclick: () => { wave.a = wave.s; wave.b = wave.e; drawWave(); } }, "fit"), h("span", { class: "note", text: "wheel zoom · drag pan · + − keys" })));
	return sec;
}

function instDetail(ins) {
	const box = h("section", { class: "instdetail" });
	const j = ins.j;
	box.append(h("h3", null, instSwatch(ins.program), `${progName(ins.program)} · ${j ? j.kind : "not described"}`),
		h("dl", { class: "kv" },
			row2("program id", `0x${hx(ins.program, 4)} (bank ${ins.program >> 14}, program ${ins.program & 0xff})`),
			row2("used by", `${ins.notes} notes in ${[...ins.tracks].map(trackName).join(", ")}`),
			j ? row2("glide rate", String(j.glide_rate ?? "—")) : null));
	// samples that this instrument's voices actually triggered (derived from the voices table)
	const used = [...ins.regions.entries()].sort((a, b) => b[1].n - a[1].n);
	if (used.length) box.append(h("h3", { text: "Samples used by its notes" }), h("ul", { class: "regions" }, used.map(([r, u]) => h("li", null,
		h("button", { type: "button", "aria-pressed": String(sel.region === r), onclick: () => { sel.region = r; renderInstruments(); } },
			h("canvas", { class: "peak", width: 128, height: 28, "data-region": r }), h("span", null, h("b", { text: `#${r}` }), ` ${u.n} voices · keys ${keyName(u.lo)}…${keyName(u.hi)}`))))));
	if (sel.region >= 0 && sel.region < S.reg.rows) box.append(waveSection(sel.region));
	if (!j) { box.append(empty("The exporter wrote no parameters for this program.")); queueMicrotask(() => $$("canvas.peak", box).forEach(drawPeakThumb)); return box; }
	if (j.kind === "absent") box.append(empty("This program selects an unpopulated bank (absent)."));
	for (const ly of j.layers || []) box.append(layerBox(ly, j));
	if (j.keys && j.keys.length) box.append(h("h3", { text: "Key regions (direct program)" }), h("table", { class: "grid" },
		h("tr", null, ["keys", "sample", "tune", "fine", "decay", "rel", "level", "filter", "pair"].map((t) => h("th", { text: t }))),
		j.keys.map((k) => h("tr", { class: k.region != null ? "click" : "", tabindex: k.region != null ? 0 : null, onclick: () => k.region != null && (sel.region = k.region, renderInstruments()), onkeydown: (e) => e.key === "Enter" && k.region != null && (sel.region = k.region, renderInstruments()) },
			h("td", { text: `${keyName(k.key_lo)}–${keyName(k.key_hi)}` }), h("td", { text: `${k.sample ?? "—"}${k.reverse ? " rev" : ""}` }), h("td", { text: k.transpose ?? "" }), h("td", { text: k.fine ?? "" }), h("td", { text: k.decay ?? "" }), h("td", { text: k.release ?? "" }),
			h("td", { text: k.level ?? "" }), h("td", { text: k.filter ?? "" }), h("td", { text: k.pan_pair ?? "" })))));
	queueMicrotask(() => $$("canvas.peak", box).forEach(drawPeakThumb));
	return box;
}

function layerBox(ly, j) {
	const d = h("details", { class: "layer", open: true }, h("summary", { text: `Layer ${ly.layer ?? "?"}${ly.enabled === false ? " (disabled)" : ""}` }));
	if (ly.envelopes && ly.envelopes.length) {
		const names = ["pitch", "filter", "amplitude"], row = h("div", { class: "envs" });
		ly.envelopes.forEach((env, e) => {
			const c = h("canvas", { class: "env", width: 150, height: 72, "aria-label": `${names[e] || "envelope " + e} envelope stages` });
			row.append(h("figure", null, c, h("figcaption", null, `${names[e] || "env " + e}`)));
			queueMicrotask(() => drawEnvelope(c, env));
		});
		d.append(row, h("p", { class: "note", text: "Schematic: stage length is proportional to level change divided by rate; the dashed span is the held sustain until note-off." }));
	}
	const rest = {};
	for (const [k, v] of Object.entries(ly)) if (!["layer", "enabled", "envelopes", "splits", "raw"].includes(k)) rest[k] = v;
	if (Object.keys(rest).length) d.append(kv(rest));
	if (ly.envelopes) for (const [e, env] of ly.envelopes.entries()) { const extra = { ...env }; delete extra.levels; delete extra.rates; if (Object.keys(extra).length) d.append(h("details", { class: "sub" }, h("summary", { text: `envelope ${e} flags` }), kv(extra))); }
	if (ly.splits && ly.splits.length) d.append(h("h4", { text: "Split regions" }), h("table", { class: "grid" },
		h("tr", null, ["keys", "words", "flags", "tune"].map((t) => h("th", { text: t }))),
		ly.splits.map((s) => {
			const go = () => { if (s.region != null) sel.region = s.region; else { const r = findRegion(s.start, s.end); if (r >= 0) sel.region = r; } renderInstruments(); };
			return h("tr", { class: "click", tabindex: 0, onclick: go, onkeydown: (e) => e.key === "Enter" && go() },
				h("td", { text: `${keyName(s.key_lo)}–${keyName(s.key_hi)}` }), h("td", { text: `${s.start ?? "?"}…${s.end ?? "?"}` }),
				h("td", { text: [s.loop && "loop", s.bidirectional && "bidi", s.reverse && "rev"].filter(Boolean).join(" ") }), h("td", { text: s.tuning ?? "" }));
		})));
	if (ly.raw) d.append(h("details", { class: "sub" }, h("summary", { text: "raw patch bytes" }), h("code", { class: "hex", text: Array.isArray(ly.raw) ? ly.raw.map((b) => hx(b)).join("") : String(ly.raw) })));
	return d;
}
function findRegion(s, e) { const R = S.reg; for (let r = 0; r < R.rows; r++) if (R.start[r] === s && R.end[r] === e) return r; return -1; }

function drawEnvelope(c, env) {
	const ctx = c.getContext("2d"), W = c.width, H = c.height;
	ctx.fillStyle = C.bgA.hex; ctx.fillRect(0, 0, W, H);
	const lv = env.levels, rt = env.rates;
	if (!Array.isArray(lv) || !Array.isArray(rt) || !lv.length) { ctx.fillStyle = C.dim.hex; ctx.font = "10px " + MONO; ctx.fillText("no stage data", 8, 20); return; }
	const n = Math.min(lv.length, rt.length), dur = [];
	let prev = 0, total = 0;
	for (let i = 0; i < n; i++) { const dd = (Math.abs(lv[i] - prev) + 4) / (rt[i] + 4); dur.push(dd); total += dd; prev = lv[i]; }
	const gap = n > 3 ? total * 0.25 : 0, tw = total + gap, X = (t) => 6 + (t / tw) * (W - 12), Y = (l) => H - 8 - (l / 127) * (H - 20);
	ctx.strokeStyle = C.gold.hex; ctx.lineWidth = 1.5; ctx.fillStyle = C.gold.hex; ctx.font = "9px " + MONO;
	ctx.beginPath(); ctx.moveTo(X(0), Y(0));
	let t = 0; const pts = [[0, 0]];
	for (let i = 0; i < n; i++) {
		if (i === 3 && gap) { ctx.setLineDash([3, 3]); ctx.lineTo(X(t + gap), Y(lv[2])); ctx.stroke(); ctx.setLineDash([]); ctx.beginPath(); ctx.moveTo(X(t + gap), Y(lv[2])); t += gap; }
		t += dur[i]; ctx.lineTo(X(t), Y(lv[i])); pts.push([t, lv[i]]);
	}
	ctx.stroke();
	pts.slice(1).forEach(([tt, l], i) => { const x = X(tt + (i >= 3 && gap ? 0 : 0)); ctx.fillRect(x - 2, Y(l) - 2, 4, 4); ctx.fillText(String(i), x - 3, Math.max(9, Y(l) - 5)); });
	ctx.fillStyle = C.dim.hex; ctx.fillText(`L ${lv.join(" ")}`, 4, 9); ctx.fillText(`R ${rt.join(" ")}`, 4, H - 1);
}

// ---------------------------------------------------------------- inspector: mixer / effects, song

const fxNow = { row: -2 };
function renderMixer() {
	const box = $("#mix-body");
	box.replaceChildren();
	fxNow.row = -2;
	if (!S) { box.append(empty("Load a song to see its mixer and effects timeline.")); return; }
	const m = S.j.mixer || {};

	const pathOl = h("ol", { class: "path" },
		PAIR_INFO.map((p, i) => h("li", null, h("i", { class: "pp", style: `color:${PAIR_COL[i].hex}`, text: "P" + i }), ` buses ${p.buses} → ${p.serio} — ${p.role}`)),
		h("li", { text: "ES5510 ESP program mixes the serial inputs into the DAC pair (L/R shown in DAC order)" }));

	const fxOl = h("ol", { class: "fxlist", id: "fxlist" },
		S.fx.map((e, k) => h("li", { "data-k": k },
			h("button", { type: "button", onclick: () => seekTo(e.frame), title: "Seek here" },
				h("span", { class: "t", text: fmtTime(e.frame) }),
				h("b", { text: e.program, style: `color:${(FX_COL[e.program] || C.grey).hex}` }),
				h("span", { class: "d", text: e.kind === "preset" ? `preset 0x${hx(e.preset ?? 0)}` : `p${e.index} = ${e.value}` })))));

	const gainUl = h("ul", { class: "fxlist" },
		S.gain.map((e) => h("li", null,
			h("button", { type: "button", onclick: () => seekTo(e.frame) },
				h("span", { class: "t", text: fmtTime(e.frame) }),
				h("b", { text: `ch ${e.channel}` }),
				h("span", { class: "d", text: `raw ${e.raw} · ×${(+e.linear).toFixed(4)}` })))));

	box.append(
		h("h3", { text: "Signal path" }),
		pathOl,
		h("dl", { class: "kv" }, row2("output boost", m.output_boost ?? "—"), row2("ES5510 input gain", m.input_gain ?? "—"), row2("gain model", m.gain_model ?? "—"), m.dc_blocker_hz != null ? row2("DC blocker", `${m.dc_blocker_hz} Hz`) : null),
		h("h3", { text: "Now" }), h("div", { id: "fxnow" }),
		h("h3", { text: `Effects timeline (${S.fx.length})` }),
		fxOl,
		h("h3", { text: `Gain writes (${S.gain.length})` }),
		gainUl
	);
	if (m.programs) box.append(h("details", null, h("summary", { text: "Effect programs (exporter table)" }), kv(m.programs)));
	if (m.pairs) box.append(h("details", null, h("summary", { text: "Pair table (exporter)" }), valueNode(m.pairs)));
	updateFxNow(true);
}
function updateFxNow(force) {
	if (!S) return;
	const k = upperBound(S.fxFrames, view.q ?? 0) - 1;
	if (!force && k === fxNow.row) return;
	fxNow.row = k;
	const box = $("#fxnow");
	if (!box) return;
	for (const li of $$("#fxlist li")) li.classList.toggle("cur", +li.dataset.k === k);
	if (k < 0) { box.replaceChildren(empty("No effect written yet at this position.")); return; }
	const e = S.fx[k];
	box.replaceChildren(h("div", { class: "fxhead" }, h("b", { text: e.program, style: `color:${(FX_COL[e.program] || C.grey).hex}` }), h("span", { text: e.kind === "preset" ? ` preset 0x${hx(e.preset ?? 0)}` : ` after p${e.index}=${e.value}` })),
		h("div", { class: "pbars", role: "img", "aria-label": `effect parameters ${e.p.join(", ")}` }, e.p.map((v, i) => h("span", { title: `p${i} = ${v}` }, h("i", { style: `height:${(v / 127) * 100}%` }), h("small", { text: i })))),
		e.info ? h("details", null, h("summary", { text: "derived parameters" }), kv(e.info)) : null);
}

function renderSong() {
	const box = $("#song-body");
	box.replaceChildren();
	if (!S) { box.append(empty("No song loaded.")); return; }
	const sp = S.span, L = S.loop;
	box.append(
		h("dl", { class: "kv" },
			row2("label", S.song.label), row2("code", `0x${hx(S.song.code, 3)} · bank ${S.song.bank ?? "none"}${S.song.page != null ? " (page " + S.song.page + ")" : ""}`),
			row2("set", S.j.set), row2("length", `${fmtTime(sp.frames)} (${sp.frames} frames) · audio rendered to ${fmtTime(sp.audio_frames)}`),
			row2("end", sp.end_reason === "loop_wrap" ? "loops at the sequencer wrap" : sp.end_reason === "song_end" ? "song ends" : "stopped at the hard cap")),
		h("h3", { text: "Loop" }));
	if (L) box.append(h("dl", { class: "kv" },
		row2("intro", `0 → ${fmtTime(L.start_frame)}`), row2("loop body", `${fmtTime(L.start_frame)} → ${fmtTime(L.end_frame)} (${((L.end_frame - L.start_frame) / SR).toFixed(3)} s)`), row2("resume phase", L.resume_phase), row2("traced iterations", L.iterations)),
		h("p", { class: "note", text: "Audio keeps looping live; the timeline repeats iteration 1. For loop iteration 2 and up the notes shown are iteration 1's: ringing voices, portamento from the previous key and effect tails differ slightly." }));
	else box.append(empty("This song does not loop."));
	box.append(h("h3", { text: `Sequences (${S.j.sequences.length})` }), h("table", { class: "grid" },
		h("tr", null, ["id", "bars", "meter", "rate", "header loop"].map((t) => h("th", { text: t }))),
		S.j.sequences.map((s) => h("tr", null, h("td", { text: s.id }), h("td", { text: s.bars }), h("td", { text: `${s.numerator}/${1 << s.exponent}` }), h("td", { text: s.rate }),
			h("td", { title: "parsed from the header but never read by the HLE", text: `${s.header_loop.start}–${s.header_loop.end} (unused)` })))));
	box.append(h("h3", { text: `Sections (${S.sec.rows})` }), h("table", { class: "grid" },
		h("tr", null, ["#", "seq", "phase", "rep", "start", "bars", "rate"].map((t) => h("th", { text: t }))),
		Array.from({ length: S.sec.rows }, (_, k) => h("tr", { class: "click", tabindex: 0, onclick: () => seekTo(S.sec.start_frame[k]), onkeydown: (e) => e.key === "Enter" && seekTo(S.sec.start_frame[k]) },
			h("td", { text: k }), h("td", { text: S.sec.sequence[k] }), h("td", { text: S.sec.phase[k] }), h("td", { text: S.sec.repeat[k] }), h("td", { text: fmtTime(S.sec.start_frame[k], 2) }), h("td", { text: S.sec.bars[k] }), h("td", { text: S.sec.rate[k] })))));
	box.append(h("h3", { text: "Keyboard" }), h("dl", { class: "kv keys" },
		row2("Space", "play / pause"), row2("← →", "pan (Shift: half page)"), row2("↑ ↓ / PgUp PgDn", "scroll lanes / select track"), row2("+ − 0 1", "zoom in, out, fit song, 1 ms ticks"),
		row2("[ ]", "lane height"), row2(", .", "previous / next note"), row2("Enter", "seek to selected note"), row2("F", "follow playhead"), row2("Esc", "clear selection"),
		row2("Ctrl+wheel", "zoom (pinch works too)"), row2("Alt+wheel", "lane height"), row2("Shift+wheel", "pan"), row2("double-click", "seek"), row2("ruler click/drag", "seek")));
}

// ---------------------------------------------------------------- LCD digits, readouts, transport buttons

const SEG = "http://www.w3.org/2000/svg";
function makeLcd(output, pattern) {
	const svg = document.createElementNS(SEG, "svg");
	let x = 0;
	const slots = [];
	for (const ch of pattern) {
		const punct = ch === ":" || ch === ".";
		const w = punct ? 5 : 12;
		if (!punct) { const g = document.createElementNS(SEG, "use"); g.setAttribute("href", "#s8"); g.setAttribute("x", x); g.setAttribute("class", "ghost"); svg.append(g); }
		const u = document.createElementNS(SEG, "use");
		u.setAttribute("x", x); u.setAttribute("href", punct ? (ch === ":" ? "#sc" : "#sp") : "#s0"); u.setAttribute("class", "lit");
		svg.append(u); slots.push([u, punct, ch]); x += w;
	}
	svg.setAttribute("viewBox", `0 0 ${x} 18`); svg.setAttribute("width", x * 1.3); svg.setAttribute("height", 18 * 1.3); svg.setAttribute("aria-hidden", "true");
	output.replaceChildren(svg);
	output._slots = slots; output._text = "";
}
function setLcd(output, text) {
	if (output._text === text) return;
	output._text = text;
	output._slots.forEach(([u, punct], i) => { if (!punct) u.setAttribute("href", "#s" + (text[i] || "0").toLowerCase()); });
	output.setAttribute("aria-label", output.dataset.label + " " + text);
}

function updateReadouts(p, q, iter) {
	const s = Math.max(0, p) / SR, m = Math.floor(s / 60), r = s - m * 60;
	setLcd(el.lcdTime, `${pad(Math.min(99, m))}:${pad(Math.floor(r))}.${pad(Math.floor((r % 1) * 1000), 3)}`);
	let bar = 0, secIdx = -1;
	if (S.bar.rows) { const b = Math.max(0, upperBound(S.bar.frame, q) - 1); bar = S.bar.bar[b] + 1; secIdx = S.bar.section[b]; }
	setLcd(el.lcdBar, pad(Math.min(bar, 999), 3));
	const L = S.loop;
	let txt = "", tip = "";
	if (L) {
		if (p < L.start_frame && iter === 0) { txt = "INTRO"; tip = "Before the loop body"; }
		else { txt = `LOOP ${iter + 1}`; tip = iter ? `Playing loop iteration ${iter + 1}. The timeline repeats the prebaked iteration 1 (README 4.4): ringing voices, portamento and effect tails differ slightly.` : "First pass through the loop body: the data shown is exact."; }
	} else if (S.span.end_reason === "song_end" && p > S.span.frames) txt = "TAIL";
	el.loopbadge.textContent = txt; el.loopbadge.hidden = !txt; el.loopbadge.title = tip;
	el.loopbadge.dataset.iter = iter > 0 ? "repeat" : "";
	el.secread.textContent = secIdx >= 0 ? `S${S.sec.sequence[secIdx]} · rate ${S.sec.rate[secIdx]}` : "";
	updateFxNow(false);
	if (sel.note >= 0 && scope.v >= 0 && activeTab() === "note") drawScope();
}

function updateTransport() {
	const playing = eng.state === "playing" || (eng.state === "seeking" && eng.seek && eng.seek.resume);
	const b = el.bplay;
	b.querySelector("use").setAttribute("href", playing ? "#i-pause" : "#i-play");
	b.setAttribute("aria-label", playing ? "Pause" : "Play"); b.title = (playing ? "Pause" : "Play") + " (Space)";
	b.setAttribute("aria-pressed", String(playing));
	const dead = !!eng.error && !eng.node;
	b.disabled = !S || dead; el.bstop.disabled = !S || dead;
	for (const id of ["zin", "zout", "zfit", "zms", "vin", "vout"]) $("#" + id).disabled = !S;
}

// ---------------------------------------------------------------- board: OTIS U31 voices, bus meters, ESP, DAC

let boardMoving = false;
const slotState = Array.from({ length: 32 }, () => ({ fill: "", op: -1, prog: -1 }));
const meterState = { b: new Float32Array(8), out: new Float32Array(2) };

function initBoard() {
	const gv = $("#vcells");
	gv.replaceChildren();
	for (let s = 0; s < 32; s++) {
		const row = s >> 4, col = s & 15, x = 14 + col * 20, y = 30 + row * 34;
		const r = document.createElementNS(SEG, "rect");
		r.setAttribute("x", x); r.setAttribute("y", y); r.setAttribute("width", 16); r.setAttribute("height", 24);
		r.setAttribute("rx", 2); r.setAttribute("class", "vc"); r.setAttribute("data-slot", s);
		const t = document.createElementNS(SEG, "text");
		t.setAttribute("x", x + 8); t.setAttribute("y", y + 15); t.setAttribute("class", "vclbl"); t.textContent = s;
		gv.append(r, t);
	}
}

function updateBoard(dt) {
	if (!S) return;
	const q = view.q ?? 0, V = S.voice, N = S.note;
	// active voices at frame q: walk back from upper bound of start_frame
	const hi = upperBound(S.vStartSorted, q), active = new Int32Array(32).fill(-1);
	let placed = 0;
	for (let i = hi - 1; i >= 0 && placed < 32; i--) {
		const v = S.vOrder[i];
		if (V.start_frame[v] < q - S.vMaxDur - 100) break; // voices never start earlier than maxDur ago
		if (q >= V.start_frame[v] && q < S.vEnd[v]) { const s = V.slot[v]; if (s < 32 && active[s] < 0) { active[s] = v; placed++; } }
	}
	const cells = $$("#vcells rect.vc");
	for (let s = 0; s < 32; s++) {
		const v = active[s], st = slotState[s], c = cells[s];
		if (v < 0) {
			if (st.op !== 0) { c.style.fill = C.bg2.hex; c.style.opacity = "0.35"; st.fill = C.bg2.hex; st.op = 0; st.prog = -1; }
			continue;
		}
		const p = N.program[V.note[v]], ins = S.inst.get(p);
		const hex = INST_PAL[ins.color].hex;
		const a = laneAt("env_amp", v, q) / 32767;
		const op = (0.35 + a * 0.65).toFixed(2);
		if (st.fill !== hex || st.op !== op) { c.style.fill = hex; c.style.opacity = op; st.fill = hex; st.op = op; st.prog = p; }
	}
	// meters: level from 512-frame window, with peak decay
	const w = clamp(Math.floor(q / WIN), 0, S.lv.rows - 1), lv = S.lv;
	const decay = eng.state === "playing" && dt > 0 ? Math.exp(-dt * 6) : 0.96;
	let moving = false;
	const dac = [levelNorm(lv.out[w * 2]), levelNorm(lv.out[w * 2 + 1])];
	for (let ch = 0; ch < 2; ch++) {
		const target = dac[ch];
		let v = meterState.out[ch];
		v = target >= v ? target : v * decay;
		meterState.out[ch] = v;
		const bar = $("#m-dac" + ch);
		if (bar) bar.setAttribute("width", Math.max(0, Math.round(v * 48)));
		if (v > 0.01) moving = true;
	}
	for (let p = 1; p < 4; p++) {
		const val = Math.max(levelNorm(lv.buses[w * 8 + p * 2]), levelNorm(lv.buses[w * 8 + p * 2 + 1]));
		let v = meterState.b[p]; v = val >= v ? val : v * decay; meterState.b[p] = v;
		const bar = $("#m-p" + p);
		if (bar) bar.setAttribute("width", Math.max(0, Math.round(v * 44)));
		if (v > 0.01) moving = true;
	}
	const fxk = upperBound(S.fxFrames, q) - 1, prg = fxk >= 0 ? S.fx[fxk].program : "Mute";
	const efx = $("#esp-prog"); if (efx && efx.textContent !== prg) efx.textContent = prg;
	boardMoving = moving;
}

// ---------------------------------------------------------------- empty overlay

function showEmpty(msg) {
	const o = $("#empty");
	if (!msg) { o.hidden = true; return; }
	o.hidden = false; o.querySelector("p").textContent = msg;
}

function onSong() {
	if (!S) {
		showEmpty("No song loaded.");
		renderHeads(); renderNote(); renderInstruments(); renderMixer(); renderSong();
		updateReadouts(0, 0, 0); updateTransport(); invalidate();
		return;
	}
	showEmpty("");
	// reverse map: note -> track index
	S.nTrack = new Int32Array(S.note.rows);
	for (const t of S.tracks) for (let a = 0; a < t.n.length; a++) S.nTrack[t.n[a]] = t.index;
	sel.note = -1; sel.track = -1; sel.inst = -1; sel.region = -1;
	setLcd(el.lcdCode, hx(S.song.code, 3));
	setLcd(el.lcdBar, "001");
	updateReadouts(0, 0, 0);
	buildRows(); renderHeads();
	setScroll(0);
	layoutTimeline(); fitView(); updateZoomReadout();
	renderNote(); renderInstruments(); renderMixer(); renderSong();
	updateTransport(); invalidate();
}

// ---------------------------------------------------------------- ruler drag (seeking)

let rulerDrag = false;
function rulerSeek(e) {
	if (!S) return;
	const r = el.rulerBox.getBoundingClientRect(), x = e.clientX - r.left - FIXED.headW;
	seekTo(view.t0 + x / view.ppf);
}
function setupRuler() {
	const rb = el.rulerBox;
	rb.addEventListener("pointerdown", (e) => {
		if (!S || e.button !== 0) return;
		rb.setPointerCapture(e.pointerId); rulerDrag = true;
		rulerSeek(e);
	});
	rb.addEventListener("pointermove", (e) => { if (rulerDrag) rulerSeek(e); });
	const up = (e) => { if (rulerDrag) { rulerSeek(e); rulerDrag = false; } };
	rb.addEventListener("pointerup", up); rb.addEventListener("pointercancel", up);
}

// ---------------------------------------------------------------- custom scrollbar / scroll

function updateScrollbar() {
	const sb = $("#sb-thumb");
	if (!sb || !S) return;
	const total = Math.max(1, S.rowsH), viewH = view.h;
	if (total <= viewH) { sb.hidden = true; return; }
	sb.hidden = false;
	const trackH = el.lanes.clientHeight - 4, thumbH = Math.max(16, (viewH / total) * trackH);
	const top = (view.scroll / (total - viewH)) * (trackH - thumbH);
	sb.style.height = thumbH + "px"; sb.style.transform = `translateY(${top}px)`;
}
function setupScrollbar() {
	const bar = $("#sb-track"), thumb = $("#sb-thumb");
	let drag = null;
	thumb.addEventListener("pointerdown", (e) => {
		e.stopPropagation(); thumb.setPointerCapture(e.pointerId);
		drag = { y0: e.clientY, s0: view.scroll };
	});
	thumb.addEventListener("pointermove", (e) => {
		if (!drag || !S) return;
		const trackH = el.lanes.clientHeight - 4, thumbH = thumb.offsetHeight;
		const dy = e.clientY - drag.y0;
		const ds = (dy / (trackH - thumbH)) * Math.max(0, S.rowsH - view.h);
		setScroll(drag.s0 + ds);
	});
	const up = () => (drag = null);
	thumb.addEventListener("pointerup", up); thumb.addEventListener("pointercancel", up);
	bar.addEventListener("pointerdown", (e) => {
		if (e.target === thumb || !S) return;
		const r = bar.getBoundingClientRect(), frac = (e.clientY - r.top) / r.height;
		setScroll(frac * (S.rowsH - view.h));
	});
}

// ---------------------------------------------------------------- init

function wireControls() {
	el.bplay = $("#bplay"); el.bstop = $("#bstop");
	el.bplay.addEventListener("click", () => { ensureContext(); togglePlay(); });
	el.bstop.addEventListener("click", stop);
	$("#zin").addEventListener("click", () => zoomBy(1.3));
	$("#zout").addEventListener("click", () => zoomBy(0.77));
	$("#zfit").addEventListener("click", () => { fitView(); invalidate(); updateZoomReadout(); });
	$("#zms").addEventListener("click", zoomMs);
	$("#vin").addEventListener("click", () => setLaneH(view.laneH * 1.3, view.h / 2));
	$("#vout").addEventListener("click", () => setLaneH(view.laneH / 1.3, view.h / 2));
	$("#ofollow").addEventListener("change", (e) => setFollow(e.target.checked));
	$("#ohideempty").addEventListener("change", (e) => { opt.hideEmpty = e.target.checked; afterRows(); });
	$("#opitch").addEventListener("change", (e) => { opt.pitch = e.target.checked; invalidate(1); });
	$("#oglide").addEventListener("change", (e) => { opt.glide = e.target.checked; invalidate(1); });
	$("#olfo").addEventListener("change", (e) => { opt.lfo = e.target.checked; invalidate(1); });
	$("#ocolor").addEventListener("change", (e) => { opt.color = e.target.value; invalidate(1); });
	$("#okeys").addEventListener("change", (e) => { opt.keys = e.target.value; invalidate(1); });
	$("#game").addEventListener("change", (e) => selectGame(e.target.value));
	$("#filter").addEventListener("input", filterSongs);
	$("#tgl-songs").addEventListener("click", () => { const a = $("#sidebar"); a.hidden = !a.hidden; layoutTimeline(); });
	$("#tgl-insp").addEventListener("click", () => { const a = $("#insp"); a.hidden = !a.hidden; layoutTimeline(); });
	$("#tgl-board").addEventListener("click", () => { const a = $("#board"); a.hidden = !a.hidden; });
	$$('input[name="tab"]').forEach((r) => r.addEventListener("change", () => {
		if (r.id === "t-inst") renderInstruments();
		else if (r.id === "t-mix") renderMixer();
		else if (r.id === "t-note") renderNote();
		else if (r.id === "t-song") renderSong();
	}));
	window.addEventListener("keydown", (e) => {
		if (e.target.matches("input, select, textarea, button")) return;
		if (e.code === "Space") { e.preventDefault(); ensureContext(); togglePlay(); }
	});
}

function init() {
	el.tl = $("#tl"); el.lanes = $("#lanes"); el.space = $("#space"); el.heads = $("#heads"); el.headsCol = $("#heads-col"); el.stage = $("#stage");
	el.rulerBox = $("#ruler-box"); el.tip = $("#tip"); el.loopbadge = $("#loopbadge"); el.secread = $("#secread");
	el.lcdTime = $("#lcd-time"); el.lcdBar = $("#lcd-bar"); el.lcdCode = $("#lcd-code");
	makeLcd(el.lcdTime, "00:00.000"); makeLcd(el.lcdBar, "000"); makeLcd(el.lcdCode, "000");
	for (const id of ["roll", "cur", "ruler"]) cv[id] = { c: $("#" + id), x: $("#" + id).getContext("2d") };
	const stage = el.stage;
	stage.addEventListener("pointerdown", onDown);
	stage.addEventListener("pointermove", onMove);
	stage.addEventListener("pointerup", onUp);
	stage.addEventListener("pointercancel", onUp);
	stage.addEventListener("pointerleave", onLeave);
	stage.addEventListener("wheel", onWheel, { passive: false });
	stage.addEventListener("dblclick", onDbl);
	stage.addEventListener("keydown", onKey);
	el.headsCol.addEventListener("wheel", onWheel, { passive: false });
	setupRuler(); setupScrollbar(); initBoard(); wireControls();
	const ro = new ResizeObserver(() => layoutTimeline());
	ro.observe(el.lanes); ro.observe(el.rulerBox);
	window.addEventListener("hashchange", () => {
		const [set, id] = location.hash.slice(1).split("/");
		if (set && set !== G.set) selectGame(set, id);
		else if (id && id !== G.current) { const ent = G.songs.find((s) => s.id === id); if (ent) loadSong(ent, false); }
	});
	boot();
}

window.addEventListener("DOMContentLoaded", init);
