// AudioWorklet processor for Taito F3 HLE audio player.
// Output sample rate: 29762 Hz (ES5505 hardware observation: 15238090 / (16 * 32) = 29761.89453125 Hz).
// Protocol per tools/webplayer/README.md section 6.3.

export function createWasiImports(module) {
    const imports = {
        env: {
            emscripten_notify_memory_growth: () => {},
        },
        wasi_snapshot_preview1: {
            proc_exit: (code) => { throw new Error(`proc_exit called with code ${code}`); },
            fd_write: () => 0,
            fd_seek: () => 0,
            fd_close: () => 0,
            fd_read: () => 0,
            clock_time_get: () => 0,
            environ_sizes_get: () => 0,
            environ_get: () => 0,
            random_get: () => 0,
        }
    };
    if (module && typeof WebAssembly.Module.imports === "function") {
        for (const imp of WebAssembly.Module.imports(module)) {
            if (!imports[imp.module]) imports[imp.module] = {};
            if (!imports[imp.module][imp.name]) {
                if (imp.kind === "function") {
                    imports[imp.module][imp.name] = () => 0;
                }
            }
        }
    }
    return imports;
}

export class F3HleEngine {
    constructor(instance) {
        this.instance = instance;
        this.exports = instance.exports;
        if (typeof this.exports._initialize === "function") {
            this.exports._initialize();
        }
        this.memory = this.exports.memory;
        this.enginePtr = 0;
        this.soundPtr = 0;
        this.samplesPtr = 0;
        this.replayPtr = 0;
        this.replayCap = 0;
        this.renderBufPtr = 0;
        this.renderBufFrames = 0;
    }

    static async instantiate(moduleOrBuffer, importObject) {
        let module;
        if (moduleOrBuffer instanceof WebAssembly.Module) {
            module = moduleOrBuffer;
        } else {
            module = await WebAssembly.compile(moduleOrBuffer);
        }
        const imports = importObject || createWasiImports(module);
        const instance = await WebAssembly.instantiate(module, imports);
        return new F3HleEngine(instance);
    }

    static instantiateSync(moduleOrBuffer, importObject) {
        let module;
        if (moduleOrBuffer instanceof WebAssembly.Module) {
            module = moduleOrBuffer;
        } else {
            module = new WebAssembly.Module(moduleOrBuffer);
        }
        const imports = importObject || createWasiImports(module);
        const instance = new WebAssembly.Instance(module, imports);
        return new F3HleEngine(instance);
    }

    abiVersion() {
        return this.exports.f3hle_abi_version();
    }

    lastError() {
        const ptr = this.exports.f3hle_last_error();
        if (!ptr) return "";
        const u8 = new Uint8Array(this.memory.buffer);
        let end = ptr;
        while (end < u8.length && u8[end] !== 0) end++;
        return new TextDecoder().decode(u8.subarray(ptr, end));
    }

    alloc(bytes) {
        const ptr = this.exports.f3hle_alloc(bytes);
        if (!ptr) throw new Error(`f3hle_alloc failed: ${this.lastError()}`);
        return ptr;
    }

    free(ptr) {
        if (ptr) this.exports.f3hle_free(ptr);
    }

    create(soundBuf, samplesBuf) {
        if (this.enginePtr) this.destroy();
        const soundBytes = soundBuf.byteLength;
        const sampleWords = samplesBuf.byteLength / 2;
        this.soundPtr = this.alloc(soundBytes);
        this.samplesPtr = this.alloc(samplesBuf.byteLength);

        const soundSrc = soundBuf instanceof ArrayBuffer ? new Uint8Array(soundBuf) : new Uint8Array(soundBuf.buffer, soundBuf.byteOffset || 0, soundBytes);
        const samplesSrc = samplesBuf instanceof ArrayBuffer ? new Uint8Array(samplesBuf) : new Uint8Array(samplesBuf.buffer, samplesBuf.byteOffset || 0, samplesBuf.byteLength);

        new Uint8Array(this.memory.buffer, this.soundPtr, soundBytes).set(soundSrc);
        new Uint8Array(this.memory.buffer, this.samplesPtr, samplesBuf.byteLength).set(samplesSrc);

        this.enginePtr = this.exports.f3hle_create(this.soundPtr, soundBytes, this.samplesPtr, sampleWords);
        if (!this.enginePtr) {
            throw new Error(`f3hle_create failed: ${this.lastError()}`);
        }
        return this.enginePtr;
    }

    destroy() {
        if (this.enginePtr) {
            this.exports.f3hle_destroy(this.enginePtr);
            this.enginePtr = 0;
        }
        if (this.soundPtr) {
            this.free(this.soundPtr);
            this.soundPtr = 0;
        }
        if (this.samplesPtr) {
            this.free(this.samplesPtr);
            this.samplesPtr = 0;
        }
        if (this.replayPtr) {
            this.free(this.replayPtr);
            this.replayPtr = 0;
            this.replayCap = 0;
        }
        if (this.renderBufPtr) {
            this.free(this.renderBufPtr);
            this.renderBufPtr = 0;
            this.renderBufFrames = 0;
        }
    }

    start(replayBuf) {
        if (!this.enginePtr) throw new Error("Engine not initialized");
        const bytes = replayBuf.byteLength;
        if (bytes > this.replayCap) {
            if (this.replayPtr) this.free(this.replayPtr);
            this.replayPtr = this.alloc(bytes);
            this.replayCap = bytes;
        }
        const replaySrc = replayBuf instanceof ArrayBuffer ? new Uint8Array(replayBuf) : new Uint8Array(replayBuf.buffer, replayBuf.byteOffset || 0, bytes);
        new Uint8Array(this.memory.buffer, this.replayPtr, bytes).set(replaySrc);

        const rc = this.exports.f3hle_start(this.enginePtr, this.replayPtr, bytes);
        if (rc !== 0) throw new Error(`f3hle_start failed: ${this.lastError()}`);
    }

    rewind() {
        if (!this.enginePtr) throw new Error("Engine not initialized");
        const rc = this.exports.f3hle_rewind(this.enginePtr);
        if (rc !== 0) throw new Error(`f3hle_rewind failed: ${this.lastError()}`);
    }

    ensureRenderBuffer(frames) {
        if (frames > this.renderBufFrames) {
            if (this.renderBufPtr) this.free(this.renderBufPtr);
            this.renderBufPtr = this.alloc(frames * 2 * 4); // 2 channels * float32
            this.renderBufFrames = frames;
        }
        return this.renderBufPtr;
    }

    renderDirect(outPtr, frames) {
        if (!this.enginePtr) throw new Error("Engine not initialized");
        const rc = this.exports.f3hle_render(this.enginePtr, outPtr, frames);
        if (rc === 0xffffffff) throw new Error(`f3hle_render failed: ${this.lastError()}`);
        return rc;
    }

    render(frames) {
        const ptr = this.ensureRenderBuffer(frames);
        const rc = this.renderDirect(ptr, frames);
        return new Float32Array(this.memory.buffer, ptr, rc * 2);
    }

    skip(frames) {
        if (!this.enginePtr) throw new Error("Engine not initialized");
        const rc = this.exports.f3hle_skip(this.enginePtr, frames);
        if (rc === 0xffffffff) throw new Error(`f3hle_skip failed: ${this.lastError()}`);
        return rc;
    }

    positionFrames() {
        if (!this.enginePtr) return 0;
        return this.exports.f3hle_position_frames(this.enginePtr);
    }

    positionTicks() {
        if (!this.enginePtr) return 0;
        return this.exports.f3hle_position_ticks(this.enginePtr);
    }
}

export class F3PlayerProcessor extends (typeof AudioWorkletProcessor !== "undefined" ? AudioWorkletProcessor : Object) {
    constructor() {
        super();
        this.engine = null;
        this.state = "uninitialized";
        this.currentSong = null;
        this.seekTarget = 0;
        this.seekToken = 0;
        this.processCallCount = 0;
        this.renderBufPtr = 0;
        if (typeof this.port !== "undefined" && this.port) {
            this.port.onmessage = this.onMessage.bind(this);
        }
    }

    onMessage(ev) {
        const msg = ev.data;
        if (!msg || !msg.type) return;
        try {
            switch (msg.type) {
                case "init": {
                    const module = msg.module || (msg.wasm ? new WebAssembly.Module(msg.wasm) : null);
                    if (!module) throw new Error("No WebAssembly module or bytecode provided");
                    const imports = createWasiImports(module);
                    const instance = new WebAssembly.Instance(module, imports);
                    this.engine = new F3HleEngine(instance);
                    this.engine.create(msg.sound, msg.samples);
                    this.renderBufPtr = this.engine.ensureRenderBuffer(128);
                    this.state = "paused";
                    this.port.postMessage({
                        type: "ready",
                        abi: this.engine.abiVersion(),
                        memory_bytes: this.engine.memory.buffer.byteLength,
                    });
                    break;
                }
                case "load": {
                    if (!this.engine) throw new Error("Engine not initialized");
                    this.currentSong = msg.song;
                    this.state = "paused";
                    this.engine.start(msg.replay);
                    this.port.postMessage({ type: "loaded", song: this.currentSong });
                    break;
                }
                case "play": {
                    if (this.state === "paused") this.state = "playing";
                    break;
                }
                case "pause": {
                    if (this.state === "playing") this.state = "paused";
                    break;
                }
                case "stop": {
                    if (this.engine) {
                        this.state = "paused";
                        this.engine.rewind();
                    }
                    break;
                }
                case "seek": {
                    if (!this.engine) break;
                    this.seekTarget = msg.frame;
                    this.seekToken = msg.token;
                    const cur = this.engine.positionFrames();
                    if (this.seekTarget < cur) {
                        this.engine.rewind();
                    }
                    this.state = "seeking";
                    break;
                }
            }
        } catch (err) {
            const stage = msg.type === "init" ? "init" : (msg.type === "load" ? "load" : (msg.type === "seek" ? "seek" : "engine"));
            this.port.postMessage({ type: "error", stage, message: err.message || String(err) });
        }
    }

    process(inputs, outputs, parameters) {
        const output = outputs[0];
        if (!output || output.length === 0) return true;
        const outL = output[0];
        const outR = output.length > 1 ? output[1] : output[0];
        const frames = outL.length;

        this.processCallCount++;

        if (this.state === "seeking") {
            try {
                const SEEK_SLICE = 8192;
                const cur = this.engine.positionFrames();
                if (cur < this.seekTarget) {
                    const toSkip = Math.min(SEEK_SLICE, this.seekTarget - cur);
                    this.engine.skip(toSkip);
                }
                const newPos = this.engine.positionFrames();
                if (newPos >= this.seekTarget) {
                    this.state = "paused";
                    this.port.postMessage({ type: "seeked", frame: newPos, token: this.seekToken });
                } else if (this.processCallCount % 20 === 0) {
                    this.port.postMessage({
                        type: "seeking",
                        frame: newPos,
                        target: this.seekTarget,
                        token: this.seekToken,
                    });
                }
            } catch (err) {
                this.state = "paused";
                this.port.postMessage({ type: "error", stage: "seek", message: err.message || String(err) });
            }
            outL.fill(0);
            outR.fill(0);
            return true;
        }

        if (this.state === "playing" && this.engine) {
            try {
                if (this.renderBufPtr === 0 || frames > this.engine.renderBufFrames) {
                    this.renderBufPtr = this.engine.ensureRenderBuffer(frames);
                }
                this.engine.renderDirect(this.renderBufPtr, frames);
                const f32 = new Float32Array(this.engine.memory.buffer, this.renderBufPtr, frames * 2);
                for (let i = 0; i < frames; i++) {
                    outL[i] = f32[i * 2 + 0];
                    outR[i] = f32[i * 2 + 1];
                }
                // Report position about every 1488 frames (~50ms at 29762 Hz: 12 * 128 = 1536 frames)
                if (this.processCallCount % 12 === 0) {
                    this.port.postMessage({
                        type: "position",
                        frame: this.engine.positionFrames(),
                        state: this.state,
                        ticks: this.engine.positionTicks(),
                    });
                }
                return true;
            } catch (err) {
                this.state = "paused";
                this.port.postMessage({ type: "error", stage: "render", message: err.message || String(err) });
                outL.fill(0);
                outR.fill(0);
                return true;
            }
        }

        outL.fill(0);
        outR.fill(0);
        return true;
    }
}

if (typeof registerProcessor !== "undefined") {
    registerProcessor("f3-player", F3PlayerProcessor);
}
