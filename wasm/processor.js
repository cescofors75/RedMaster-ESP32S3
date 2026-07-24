// AudioWorkletProcessor que ejecuta el motor RayDrone (Rust→wasm). Estéreo.
// Incluye diagnóstico: CPU del hilo de audio, voces (rayos) activas y granos/seg.

class RayDroneProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super();
        // Compilar aquí dentro: Safari no permite clonar un WebAssembly.Module
        // del hilo principal al worklet, así que llegan los bytes crudos.
        const mod = new WebAssembly.Module(options.processorOptions.wasmBytes);
        this.inst = new WebAssembly.Instance(mod, {});
        this.ex = this.inst.exports;
        this.mem = this.ex.memory;
        this.outL = this.ex.out_l_ptr();
        this.outR = this.ex.out_r_ptr();
        this.blockCapacity = this.ex.block_capacity();
        this.outViewBuffer = null;
        this.outViewCount = 0;
        this.outLView = null;
        this.outRView = null;
        this.ex.set_output_sample_rate(sampleRate);
        this.ready = false;
        this.ex.seed(0x9e3779b9);
        this.lastW = 0;
        this.rayOff = [];
        this.rayBand = [];
        this.rayRatio = [];
        this.foci = [];
        this.blockCount = 0;
        // Diagnóstico de rendimiento
        this.now = (typeof performance !== 'undefined' && performance.now) ? () => performance.now() : null;
        this.cpuAcc = 0;
        this.cpuBlocks = 0;
        this.lastSpawn = 0;
        // Grabación de la salida del motor (export WAV); tope de seguridad 5 min.
        // Dos buffers reutilizables evitan slice()/arrays por quantum en el hilo de audio.
        this.recOn = false;
        this.recCapacity = Math.ceil(sampleRate);
        this.recL = new Float32Array(this.recCapacity);
        this.recR = new Float32Array(this.recCapacity);
        this.recWrite = 0;
        this.recFrames = 0;
        // Line In reutiliza el búfer de fuente del núcleo WASM como una cinta
        // circular duplicada. Las dos copias contiguas hacen que un grano pueda
        // cruzar el final del anillo sin encontrar un salto ni memoria futura a
        // cero. El foco se mantiene detrás del cabezal según apertura/duración.
        this.liveOn = false;
        this.liveCapacity = 0;
        this.liveWrite = 0;
        this.liveSeen = 0;
        this.liveSampleView = null;
        this.liveFocusSec = 0;
        this.liveReady = false;
        this.liveInputEnv = 0;
        this.liveExciters = null;
        this.lastParams = null;
        this.port.onmessage = (e) => this.onMsg(e.data);
    }

    onMsg(d) {
        try {
            this.handleMsg(d);
        } catch (err) {
            // Lo típico: raydrone.wasm está desactualizado respecto al JS (p.ej.
            // tras un `git pull` sin recompilar — el .wasm NO está en git, hay
            // que correr wasm/build.sh) y falta un export nuevo. Sin este
            // try/catch, la excepción moría en silencio aquí dentro del
            // worklet y el control simplemente "no hacía nada".
            this.port.postMessage({
                type: 'enginemismatch',
                messageType: d.type,
                error: String((err && err.message) || err),
            });
        }
    }

    handleMsg(d) {
        const ex = this.ex;
        if (d.type === 'sample') {
            const cap = ex.sample_capacity();
            const len = Math.min(d.data.length, cap);
            new Float32Array(this.mem.buffer, ex.sample_ptr(), len).set(d.data.subarray(0, len));
            ex.set_sample(len, d.sampleRate);
            this.ready = true;
            this.liveOn = false;
            // Avisar a la UI si el sample no cabe entero (truncado silencioso, no más)
            this.port.postMessage({ type: 'sampleinfo', used: len, total: d.data.length, truncated: d.data.length > cap });
        } else if (d.type === 'record') {
            if (d.on) {
                this.recOn = true;
                this.recWrite = 0;
                this.recFrames = 0;
            } else if (this.recOn) {
                this.recOn = false;
                this.flushRec(true, false);
            }
        } else if (d.type === 'window') {
            const cap = ex.window_capacity();
            const len = Math.min(d.data.length, cap);
            new Float32Array(this.mem.buffer, ex.window_ptr(), len).set(d.data.subarray(0, len));
            if (d.data.length !== cap) {
                this.port.postMessage({ type: 'windowinfo', used: len, total: d.data.length, expected: cap });
            }
        } else if (d.type === 'live') {
            this.liveOn = !!d.on;
            if (this.liveOn) {
                const seconds = Math.max(2, Math.min(8, Number(d.seconds) || 4));
                const maxCycle = Math.floor(ex.sample_capacity() / 2);
                this.liveCapacity = Math.min(maxCycle, Math.max(4096, Math.round(seconds * sampleRate)));
                this.liveWrite = 0;
                this.liveSeen = 0;
                this.liveFocusSec = 0;
                this.liveReady = false;
                this.liveInputEnv = 0;
                // Tres resonadores de peine afinados A2–E3–A3. La voz los
                // excita y su cola pasa a formar parte de la propia cinta, por
                // lo que Drone, Shimmer, materiales y FX procesan el resultado.
                this.liveExciters = [110, 164.814, 220].map((hz) => ({
                    data: new Float32Array(Math.max(32, Math.round(sampleRate / hz))),
                    write: 0,
                }));
                this.liveSampleView = new Float32Array(this.mem.buffer, ex.sample_ptr(), this.liveCapacity * 2);
                this.liveSampleView.fill(0);
                // Una única inicialización: el motor ve dos vueltas idénticas de
                // la cinta. No se reconstruye el estado DSP por bloque.
                ex.set_sample(this.liveCapacity * 2, sampleRate);
                if (this.lastParams) {
                    ex.set_params(
                        0, this.lastParams.aperture, this.lastParams.grainMs, 0,
                        this.lastParams.gain, this.lastParams.master
                    );
                }
                this.ready = true;
            } else {
                this.liveReady = false;
                this.liveSampleView = null;
                this.liveExciters = null;
            }
        } else if (d.type === 'params') {
            this.lastParams = { focus: d.focus, aperture: d.aperture, grainMs: d.grainMs, grainRate: d.grainRate, gain: d.gain, master: d.master };
            ex.set_params(
                this.liveOn ? this.liveFocusSec : d.focus,
                this.liveOn ? Math.min(d.aperture, 0.16) : d.aperture,
                this.liveOn ? Math.min(d.grainMs, 180) : d.grainMs,
                this.liveOn && !this.liveReady ? 0 : d.grainRate,
                d.gain,
                d.master
            );
        } else if (d.type === 'direct') {
            ex.set_direct(d.on >>> 0, d.offsetSec);
        } else if (d.type === 'mode') {
            ex.set_mode(d.value >>> 0);
        } else if (d.type === 'fx') {
            ex.set_fx(d.aber, d.bounces >>> 0, d.refl, d.feedback);
        } else if (d.type === 'space') {
            ex.set_space(d.width, d.oct);
        } else if (d.type === 'pitch') {
            ex.set_pitch(d.mult);
        } else if (d.type === 'scale') {
            // Tabla de ratios microtonales (vacía = pitch continuo)
            const len = Math.min(d.data.length, ex.scale_capacity());
            if (len) new Float32Array(this.mem.buffer, ex.scale_ptr(), len).set(d.data.subarray(0, len));
            ex.set_scale(len);
        } else if (d.type === 'keys') {
            // Ratios de las notas sostenidas (piano por teclado/ratón/táctil);
            // vacía = ninguna tecla pulsada, se cae a Microtonal/Voicing o pitch continuo.
            const len = Math.min(d.data.length, ex.keys_capacity());
            if (len) new Float32Array(this.mem.buffer, ex.keys_ptr(), len).set(d.data.subarray(0, len));
            ex.set_keys(len);
        } else if (d.type === 'chord') {
            // Voicing afinado de core::music (0 = unísono/continuo).
            ex.set_chord(d.preset >>> 0);
        } else if (d.type === 'filter') {
            ex.set_filter(d.cutoff, d.res);
        } else if (d.type === 'filterlfo') {
            ex.set_filter_lfo(d.rate, d.depth);
        } else if (d.type === 'reverb') {
            ex.set_reverb(d.wet);
        } else if (d.type === 'material') {
            ex.set_material(d.kind >>> 0, d.amount);
        } else if (d.type === 'modulation') {
            ex.set_modulation(d.mode >>> 0, d.target >>> 0, d.rate, d.depth, d.attack, d.release);
        } else if (d.type === 'effects') {
            ex.set_effects(d.delayWet, d.delayTime, d.delayFeedback, d.chorusWet, d.chorusRate, d.chorusDepth);
        } else if (d.type === 'advancedfx') {
            ex.set_advanced_effects(d.flangerWet, d.flangerRate, d.flangerDepth, d.phaserWet, d.phaserRate, d.phaserDepth, d.drive, d.resonatorWet, d.resonatorHz, d.resonatorDecay);
        } else if (d.type === 'smart') {
            ex.set_smart(d.on >>> 0);
        } else if (d.type === 'ambient') {
            ex.set_ambient(d.on >>> 0, d.seeds >>> 0, d.depth >>> 0, d.spread, d.drift, d.rate);
            this.ambOn = (d.on >>> 0) === 1;
        }
    }

    // Copiar una vez por segundo y transferir el chunk; no asigna por quantum.
    flushRec(done = false, limited = false) {
        if (this.recWrite > 0) {
            const L = this.recL.slice(0, this.recWrite);
            const R = this.recR.slice(0, this.recWrite);
            this.port.postMessage({ type: 'recdata', l: L, r: R, sr: sampleRate, done, limited }, [L.buffer, R.buffer]);
        } else if (done) {
            this.port.postMessage({ type: 'recdata', l: new Float32Array(0), r: new Float32Array(0), sr: sampleRate, done, limited });
        }
        this.recWrite = 0;
    }

    recordBlock(left, right) {
        let sourceOffset = 0;
        while (sourceOffset < left.length) {
            const count = Math.min(left.length - sourceOffset, this.recCapacity - this.recWrite);
            this.recL.set(left.subarray(sourceOffset, sourceOffset + count), this.recWrite);
            this.recR.set(right.subarray(sourceOffset, sourceOffset + count), this.recWrite);
            this.recWrite += count;
            this.recFrames += count;
            sourceOffset += count;
            if (this.recWrite === this.recCapacity) this.flushRec(false, false);
        }
    }

    process(inputs, outputs) {
        const out = outputs[0];
        const frames = out[0].length;
        if (this.liveOn && this.liveSampleView) {
            const input = inputs[0] || [];
            const left = input[0];
            const right = input[1];
            let inputSq = 0;
            for (let i = 0; i < frames; i++) {
                const l = left ? left[i] : 0;
                const r = right ? right[i] : l;
                const mono = (l + r) * 0.5;
                inputSq += mono * mono;
                let excitation = 0;
                if (this.liveExciters) {
                    for (let c = 0; c < this.liveExciters.length; c++) {
                        const comb = this.liveExciters[c];
                        const delayed = comb.data[comb.write];
                        comb.data[comb.write] = Math.max(-1, Math.min(1, mono + delayed * 0.982));
                        comb.write++;
                        if (comb.write === comb.data.length) comb.write = 0;
                        excitation += delayed;
                    }
                    excitation /= this.liveExciters.length;
                }
                const liveSource = Math.max(-1, Math.min(1, mono * 0.78 + excitation * 0.22));
                this.liveSampleView[this.liveWrite] = liveSource;
                this.liveSampleView[this.liveWrite + this.liveCapacity] = liveSource;
                this.liveWrite = (this.liveWrite + 1) % this.liveCapacity;
                this.liveSeen = Math.min(this.liveCapacity, this.liveSeen + 1);
            }
            const inputRms = Math.sqrt(inputSq / Math.max(1, frames));
            this.liveInputEnv += (inputRms - this.liveInputEnv) * (inputRms > this.liveInputEnv ? 0.32 : 0.055);
            if (this.lastParams) {
                // En directo limitamos la dispersión extrema: conserva el gesto
                // Tonal→Drone, pero evita latencias de varios segundos. El margen
                // 2.1× cubre también los granos de Shimmer a una octava.
                const aperture = Math.min(this.lastParams.aperture, 0.16);
                const grainMs = Math.min(this.lastParams.grainMs, 180);
                const grainSec = grainMs * 0.001;
                const historySec = this.liveCapacity / sampleRate;
                const lookBehind = Math.min(
                    historySec * 0.45,
                    Math.max(0.12, aperture + grainSec * 2.1 + 0.035)
                );
                const behindFrames = Math.ceil(lookBehind * sampleRate);
                const ringFocus = (this.liveWrite - behindFrames + this.liveCapacity) % this.liveCapacity;
                // Si el margen izquierdo del grano cruzaría el inicio, usamos
                // la copia gemela del mismo punto una vuelta más adelante.
                const leftMargin = Math.ceil((aperture + 0.01) * sampleRate);
                const focusFrame = ringFocus < leftMargin ? ringFocus + this.liveCapacity : ringFocus;
                this.liveFocusSec = focusFrame / sampleRate;
                this.liveReady = this.liveSeen >= behindFrames + frames;
                this.ex.set_params(
                    this.liveFocusSec,
                    aperture,
                    grainMs,
                    this.liveReady ? this.lastParams.grainRate : 0,
                    this.lastParams.gain,
                    this.lastParams.master
                );
            }
        }
        if (this.ready) {
            // Cronometrar el render del motor (carga real del hilo de audio).
            const t0 = this.now ? this.now() : 0;
            for (let offset = 0; offset < frames; offset += this.blockCapacity) {
                const count = Math.min(this.blockCapacity, frames - offset);
                this.ex.process(count);
                // La memoria WASM es estable en ejecución normal. Reutilizar
                // las vistas elimina dos objetos por quantum (~750/seg a 48 kHz).
                if (this.outViewBuffer !== this.mem.buffer || this.outViewCount !== count) {
                    this.outViewBuffer = this.mem.buffer;
                    this.outViewCount = count;
                    this.outLView = new Float32Array(this.mem.buffer, this.outL, count);
                    this.outRView = new Float32Array(this.mem.buffer, this.outR, count);
                }
                out[0].set(this.outLView, offset);
                if (out[1]) out[1].set(this.outRView, offset);
            }
            if (this.now) { this.cpuAcc += this.now() - t0; this.cpuBlocks++; }

            if (this.recOn) {
                this.recordBlock(out[0], out[1] || out[0]);
                if (this.recFrames >= sampleRate * 300) { // tope 5 min
                    this.recOn = false;
                    this.flushRec(true, true);
                }
            }

            // Recoger rayos (offset + banda) para la visualización.
            const w = this.ex.slog_w() >>> 0;
            if (w !== this.lastW) {
                const cap = this.ex.slog_cap();
                const off = new Float32Array(this.mem.buffer, this.ex.slog_ptr(), cap);
                const bnd = new Float32Array(this.mem.buffer, this.ex.slog_b_ptr(), cap);
                const rat = new Float32Array(this.mem.buffer, this.ex.slog_s_ptr(), cap);
                let count = (w - this.lastW) >>> 0;
                if (count > cap) count = cap;
                for (let k = 0; k < count; k++) {
                    const idx = (this.lastW + k) % cap;
                    this.rayOff.push(off[idx]);
                    this.rayBand.push(bnd[idx]);
                    this.rayRatio.push(rat[idx]);
                }
                this.lastW = w;
            }

            if (++this.blockCount >= 16) {
                const blocks = this.blockCount;
                this.blockCount = 0;
                const level = this.ex.out_level();
                // ── Diagnóstico ──
                const blockMs = frames / sampleRate * 1000;
                const cpu = (this.now && this.cpuBlocks > 0) ? (this.cpuAcc / this.cpuBlocks) / blockMs * 100 : -1;
                const voices = this.ex.active_voices();
                const sc = this.ex.spawn_count() >>> 0;
                const spawnsDelta = (sc - this.lastSpawn) >>> 0;
                this.lastSpawn = sc;
                const spawnsPerSec = spawnsDelta / (blocks * frames / sampleRate);
                this.cpuAcc = 0; this.cpuBlocks = 0;
                const perf = { cpu, voices, spawnsPerSec };
                const live = this.liveOn ? {
                    focus: this.liveFocusSec,
                    span: this.liveCapacity / sampleRate,
                    ready: this.liveReady,
                    inputLevel: this.liveInputEnv,
                } : null;

                // Constelación de focos (solo en ambient): posición + peso de cada foco vivo.
                let foci = null;
                if (this.ambOn) {
                    const cap = this.ex.foci_cap();
                    const fp = new Float32Array(this.mem.buffer, this.ex.foci_ptr(), cap);
                    const fw = new Float32Array(this.mem.buffer, this.ex.foci_w_ptr(), cap);
                    foci = this.foci;
                    foci.length = 0;
                    for (let i = 0; i < cap; i++) if (fw[i] > 0.004) foci.push(fp[i], fw[i]);
                }

                if (this.rayOff.length) {
                    this.port.postMessage({ type: 'rays', offsets: this.rayOff, bands: this.rayBand, ratios: this.rayRatio, foci, level, perf, live });
                    // structured clone ya ha capturado el mensaje al volver de
                    // postMessage: conservar la capacidad evita tres arrays y
                    // su posterior GC unas 20 veces por segundo en audio real.
                    this.rayOff.length = 0;
                    this.rayBand.length = 0;
                    this.rayRatio.length = 0;
                } else {
                    this.port.postMessage({ type: 'level', foci, level, perf, live });
                }
            }
        } else {
            out[0].fill(0);
            if (out[1]) out[1].fill(0);
        }
        return true;
    }
}

registerProcessor('raydrone', RayDroneProcessor);
