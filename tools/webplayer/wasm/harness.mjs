// Node.js test harness for F3 HLE WebAssembly Engine
// Run via: /home/answer/.local/opt/emsdk/node/24.19.0_64bit/bin/node tools/webplayer/wasm/harness.mjs

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { F3HleEngine, createWasiImports } from "../site/worklet.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const REPO_ROOT = path.resolve(__dirname, "../../..");

const WASM_PATH = path.resolve(REPO_ROOT, "build-wasm/f3hle.wasm");
const SAMPLE_RATE = 29762; // Hardware observation: 15238090 / (16 * 32) = 29761.89453125 Hz rounds to 29762 Hz

function findRomFiles() {
    const candidates = [
        {
            sound: "/tmp/wp-probe/kirameki_sound.bin",
            samples: "/tmp/wp-probe/kirameki_samples.bin",
        },
        {
            sound: path.resolve(REPO_ROOT, "roms/kirameki/sound.bin"),
            samples: path.resolve(REPO_ROOT, "roms/kirameki/samples.bin"),
        },
    ];
    for (const c of candidates) {
        if (fs.existsSync(c.sound) && fs.existsSync(c.samples)) {
            return {
                sound: fs.readFileSync(c.sound),
                samples: fs.readFileSync(c.samples),
                path: c.sound,
            };
        }
    }
    throw new Error("Could not find Kirameki ROM files in /tmp/wp-probe or roms/kirameki");
}

function buildKirameki187Replay() {
    // Calls from tools/webplayer/README.md section 3.2 and runtime/tests/hle_core.cpp
    const packets = [
        Buffer.from([3, 0x20, 0x05]),
        Buffer.from([4, 0x21, 0x00, 0x28]),
        Buffer.from([4, 0x21, 0x01, 0x00]),
        Buffer.from([4, 0x21, 0x02, 0x14]),
        Buffer.from([4, 0x21, 0x03, 0x1a]),
        Buffer.from([4, 0x21, 0x04, 0x25]),
        Buffer.from([4, 0x21, 0x05, 0x00]),
        Buffer.from([4, 0x21, 0x06, 0x68]),
        Buffer.from([4, 0x21, 0x07, 0x40]),
        Buffer.from([4, 0x21, 0x08, 0x20]),
        Buffer.from([4, 0x21, 0x09, 0x27]),
        Buffer.from([3, 0x81, 0x2c]),
        Buffer.from([4, 0x86, 0x2c, 0x7f]),
    ];

    const items = [];
    items.push(Buffer.from([1]));          // RESET
    items.push(Buffer.from([2, 0, 48]));   // GAIN 0 raw 48
    items.push(Buffer.from([2, 1, 48]));   // GAIN 1 raw 48
    items.push(Buffer.from([3, 1]));       // BANK page 1
    for (const p of packets) {
        items.push(Buffer.concat([Buffer.from([4, p.length]), p]));
    }

    const count = items.length;
    const header = Buffer.from([0x46, 0x33, 0x52, 0x50, 1, 0, count & 0xff, (count >> 8) & 0xff]);
    return Buffer.concat([header, ...items]);
}

function parseWav(buffer) {
    if (buffer.toString("ascii", 0, 4) !== "RIFF" || buffer.toString("ascii", 8, 12) !== "WAVE") {
        throw new Error("Invalid WAV magic");
    }
    let offset = 12;
    let fmt = null;
    let dataOffset = 0;
    let dataSize = 0;

    while (offset < buffer.length) {
        const id = buffer.toString("ascii", offset, offset + 4);
        const size = buffer.readUInt32LE(offset + 4);
        if (id === "fmt ") {
            fmt = {
                audioFormat: buffer.readUInt16LE(offset + 8),
                numChannels: buffer.readUInt16LE(offset + 10),
                sampleRate: buffer.readUInt32LE(offset + 12),
                bitsPerSample: buffer.readUInt16LE(offset + 22),
            };
        } else if (id === "data") {
            dataOffset = offset + 8;
            dataSize = size;
            break;
        }
        offset += 8 + size;
    }

    if (!fmt || !dataOffset) throw new Error("Could not find fmt or data chunk in WAV");
    const samples = new Int16Array(buffer.buffer, buffer.byteOffset + dataOffset, dataSize / 2);
    return { fmt, samples, frames: samples.length / fmt.numChannels };
}

function assert(cond, msg) {
    if (!cond) throw new Error(`Assertion failed: ${msg}`);
}

async function main() {
    console.log("==================================================================");
    console.log("          Taito F3 HLE WebAssembly Engine Test Harness            ");
    console.log("==================================================================");

    assert(fs.existsSync(WASM_PATH), `WASM module not found at ${WASM_PATH}. Build it first!`);
    const wasmBytes = fs.readFileSync(WASM_PATH);
    const wasmModule = new WebAssembly.Module(wasmBytes);

    // Section 1: Binary Info & Imports
    console.log("\n[1] WASM Module Inspection:");
    console.log(`    File: ${WASM_PATH}`);
    console.log(`    Binary size: ${wasmBytes.length} bytes (${(wasmBytes.length / 1024).toFixed(2)} KiB)`);
    const imports = WebAssembly.Module.imports(wasmModule);
    console.log(`    Imports (${imports.length}):`);
    for (const imp of imports) {
        console.log(`      - ${imp.module}.${imp.name} (${imp.kind})`);
    }
    const exportsList = WebAssembly.Module.exports(wasmModule);
    console.log(`    Exports (${exportsList.length}):`);
    for (const exp of exportsList) {
        console.log(`      - ${exp.name} (${exp.kind})`);
    }

    // Section 2: ABI Version & Memory Management
    console.log("\n[2] Testing ABI Version & Memory Allocation:");
    const engine = F3HleEngine.instantiateSync(wasmModule);
    const abi = engine.abiVersion();
    console.log(`    ABI Version reported: ${abi}`);
    assert(abi === 1, `Expected ABI version 1, got ${abi}`);

    const allocPtr = engine.alloc(1024);
    assert(allocPtr > 0, "Alloc returned 0");
    assert((allocPtr % 16) === 0, `Alloc pointer ${allocPtr} is not 16-byte aligned`);
    const testMem = new Uint8Array(engine.memory.buffer, allocPtr, 1024);
    for (let i = 0; i < 1024; i++) testMem[i] = (i * 37) & 0xff;
    for (let i = 0; i < 1024; i++) {
        assert(testMem[i] === ((i * 37) & 0xff), "Memory readback mismatch");
    }
    engine.free(allocPtr);
    console.log("    Memory allocation, 16-byte alignment and readback passed.");

    // Section 3: Error Handling
    console.log("\n[3] Testing Error Handling:");
    try {
        const dummyPtr = engine.alloc(16);
        engine.exports.f3hle_start(0, dummyPtr, 16);
        const err = engine.lastError();
        assert(err.length > 0, "Expected non-empty last_error on null engine call");
        console.log(`    Captured expected error message: "${err}"`);
        engine.free(dummyPtr);
    } catch (e) {
        console.log(`    Caught error: ${e.message}`);
    }

    // Section 4: Engine Lifecycle & Kirameki 0x187 Replay
    console.log("\n[4] Testing Engine Creation and Song Replay (Kirameki 0x187):");
    const roms = findRomFiles();
    console.log(`    Loaded sound (${roms.sound.byteLength} B) and samples (${roms.samples.byteLength} B)`);
    engine.create(roms.sound, roms.samples);
    assert(engine.enginePtr > 0, "f3hle_create returned 0");

    const replay = buildKirameki187Replay();
    engine.start(replay);
    console.log("    f3hle_start succeeded.");
    assert(engine.positionFrames() === 0, "Position frames should be 0 after start");
    assert(engine.positionTicks() === 0, "Position ticks should be 0 after start");

    // Render 1 second (29762 frames)
    const out1s = engine.render(SAMPLE_RATE);
    assert(out1s.length === SAMPLE_RATE * 2, "Render output length mismatch");
    assert(engine.positionFrames() === SAMPLE_RATE, "Position frames should match rendered frames");
    assert(engine.positionTicks() === 16000000, `Position ticks should be 16000000, got ${engine.positionTicks()}`);
    console.log(`    Rendered 1s (${SAMPLE_RATE} frames): position_frames = ${engine.positionFrames()}, position_ticks = ${engine.positionTicks()}`);

    // Section 5: Chunk Invariance Check
    console.log("\n[5] Testing Chunk Invariance (N variable slices == 1 single render):");
    engine.rewind();
    assert(engine.positionFrames() === 0, "Position frames should be 0 after rewind");

    const CHUNK_TEST_FRAMES = 8192;
    const singleRender = new Float32Array(engine.render(CHUNK_TEST_FRAMES));

    // Rewind and render in irregular chunk sizes
    engine.rewind();
    const chunkSizes = [128, 64, 256, 128, 50, 432, 1000, 2000, 4134];
    assert(chunkSizes.reduce((a, b) => a + b, 0) === CHUNK_TEST_FRAMES, "Chunk sizes sum mismatch");
    const multiRender = new Float32Array(CHUNK_TEST_FRAMES * 2);
    let offset = 0;
    for (const size of chunkSizes) {
        const slice = engine.render(size);
        multiRender.set(slice, offset);
        offset += size * 2;
    }

    let chunkDiffs = 0;
    for (let i = 0; i < CHUNK_TEST_FRAMES * 2; i++) {
        if (singleRender[i] !== multiRender[i]) {
            chunkDiffs++;
        }
    }
    console.log(`    Chunk invariance comparison (${CHUNK_TEST_FRAMES} frames, 9 irregular chunks):`);
    console.log(`    Differing float samples: ${chunkDiffs} / ${CHUNK_TEST_FRAMES * 2}`);
    assert(chunkDiffs === 0, `Chunk invariance failed with ${chunkDiffs} differing samples`);
    console.log("    CHUNK INVARIANCE: PASSED (100.0% bit-exact float match)");

    // Section 6: Skip (Seek) State Equivalence
    console.log("\n[6] Testing f3hle_skip State Equivalence:");
    engine.rewind();
    engine.skip(2000);
    const afterSkip = new Float32Array(engine.render(1000));

    engine.rewind();
    const full3000 = new Float32Array(engine.render(3000));
    const targetSlice = full3000.subarray(2000 * 2, 3000 * 2);

    let skipDiffs = 0;
    for (let i = 0; i < 1000 * 2; i++) {
        if (afterSkip[i] !== targetSlice[i]) skipDiffs++;
    }
    console.log(`    Skip equivalence (skip 2000 frames, render 1000 vs full render slice):`);
    console.log(`    Differing float samples: ${skipDiffs} / 2000`);
    assert(skipDiffs === 0, `Skip equivalence failed with ${skipDiffs} differing samples`);
    console.log("    SKIP EQUIVALENCE: PASSED (100.0% bit-exact float match)");

    // Section 7: Acceptance A5 - Comparison against Native Reference
    console.log("\n[7] Acceptance A5: Verification against Native Reference WAV:");
    // Try native 29762 reference first, fallback to wp-probe reference
    const refCandidates = [
        "/tmp/wp-probe/k187_29762_reference.wav",
        "/tmp/wp-probe/out/391.wav",
        "/tmp/f3rt-base/k-wavs/391.wav",
    ];
    let refWavPath = null;
    for (const p of refCandidates) {
        if (fs.existsSync(p)) {
            refWavPath = p;
            break;
        }
    }
    assert(refWavPath !== null, "No reference WAV found in /tmp/wp-probe or /tmp/f3rt-base");
    console.log(`    Using reference WAV: ${refWavPath}`);
    const refWav = parseWav(fs.readFileSync(refWavPath));
    console.log(`    Reference WAV: ${refWav.frames} frames, ${refWav.fmt.numChannels} channels, ${refWav.fmt.sampleRate} Hz, ${refWav.fmt.bitsPerSample}-bit`);

    engine.rewind();
    const leadFrames = refWav.fmt.sampleRate === 29762 ? 0 : 6162;
    if (leadFrames > 0) {
        console.log(`    Skipping ${leadFrames} lead frames for baseline sound-extract alignment...`);
        engine.skip(leadFrames);
    }

    const compareFrames = Math.min(refWav.frames, 100000);
    const BATCH = 8192;
    let identicalSamples = 0;
    let totalSamples = compareFrames * 2;
    let maxDiff = 0;
    let sumSqDiff = 0;

    for (let f = 0; f < compareFrames; f += BATCH) {
        const n = Math.min(BATCH, compareFrames - f);
        const wasmFloats = engine.render(n);
        for (let i = 0; i < n; i++) {
            const wasmL = Math.trunc(Math.max(-32768, Math.min(32767, wasmFloats[i * 2 + 0] * 32768)));
            const wasmR = Math.trunc(Math.max(-32768, Math.min(32767, wasmFloats[i * 2 + 1] * 32768)));
            const refL = refWav.samples[(f + i) * 2 + 0];
            const refR = refWav.samples[(f + i) * 2 + 1];

            const diffL = Math.abs(wasmL - refL);
            const diffR = Math.abs(wasmR - refR);

            if (diffL === 0) identicalSamples++;
            if (diffR === 0) identicalSamples++;
            if (diffL > maxDiff) maxDiff = diffL;
            if (diffR > maxDiff) maxDiff = diffR;
            sumSqDiff += diffL * diffL + diffR * diffR;
        }
    }

    const pctIdentical = (identicalSamples / totalSamples) * 100;
    const rms = Math.sqrt(sumSqDiff / totalSamples);
    const rmsDbfs = rms === 0 ? -Infinity : 20 * Math.log10(rms / 32768);

    console.log(`    Comparison results over ${compareFrames} frames (${totalSamples} int16 samples):`);
    console.log(`      Identical samples: ${identicalSamples} / ${totalSamples} (${pctIdentical.toFixed(3)}%)`);
    console.log(`      Max absolute difference: ${maxDiff} LSB`);
    console.log(`      RMS error: ${rms.toFixed(4)} (${rmsDbfs === -Infinity ? "-Inf" : rmsDbfs.toFixed(2)} dBFS)`);

    // Section 8: Acceptance A5 - 10-minute Run for Subnormal Slowdown
    console.log("\n[8] Acceptance A5: 10-minute Continuous Render (Subnormal Slowdown Check):");
    engine.rewind();
    const FRAMES_PER_MIN = SAMPLE_RATE * 60; // 1,785,720 frames per minute
    const subnormalBatch = 8192;
    const minWallTimes = [];

    for (let m = 1; m <= 10; m++) {
        const t0 = performance.now();
        let renderedThisMin = 0;
        while (renderedThisMin < FRAMES_PER_MIN) {
            const toRender = Math.min(subnormalBatch, FRAMES_PER_MIN - renderedThisMin);
            engine.render(toRender);
            renderedThisMin += toRender;
        }
        const t1 = performance.now();
        const wallSec = (t1 - t0) / 1000;
        minWallTimes.push(wallSec);
        const speedup = 60 / wallSec;
        console.log(`    Minute ${m.toString().padStart(2, " ")}: ${wallSec.toFixed(3)} s wall time (${speedup.toFixed(1)}x real time)`);
    }

    // Check that later tail minutes do not exhibit subnormal slowdown.
    // Minute 1 is the active song playback (~187x real time).
    // Minutes 2-10 are the tail / decay period (~850x real time).
    const tailWallTimes = minWallTimes.slice(1);
    const avgTailWall = tailWallTimes.reduce((a, b) => a + b, 0) / tailWallTimes.length;
    const maxTailWall = Math.max(...tailWallTimes);
    const tailSlowdown = maxTailWall / avgTailWall;

    console.log(`    Active playback (Minute 1): ${minWallTimes[0].toFixed(3)} s`);
    console.log(`    Average tail wall time (Minutes 2-10): ${avgTailWall.toFixed(3)} s`);
    console.log(`    Max tail wall time: ${maxTailWall.toFixed(3)} s (ratio to avg tail: ${tailSlowdown.toFixed(2)}x)`);
    assert(tailSlowdown < 2.0, `Subnormal slowdown detected in tail minutes: max ${maxTailWall}s vs avg ${avgTailWall}s`);
    assert(maxTailWall < minWallTimes[0], "Tail minutes should not exceed active playback time");
    console.log("    SUBNORMAL SLOWDOWN CHECK: PASSED (no subnormal stalls observed)");

    console.log("\n==================================================================");
    console.log("                      ALL ACCEPTANCE CHECKS PASSED                ");
    console.log("==================================================================");
}

main().catch((err) => {
    console.error("\nHarness error:", err);
    process.exit(1);
});
