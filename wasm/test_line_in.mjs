import fs from 'node:fs';

const SR = 48_000;
const BLOCK = 128;
globalThis.sampleRate = SR;

let Processor = null;
class FakeAudioWorkletProcessor {
    constructor() {
        this.messages = [];
        this.port = {
            onmessage: null,
            postMessage: (message) => this.messages.push(message),
        };
    }
}
globalThis.AudioWorkletProcessor = FakeAudioWorkletProcessor;
globalThis.registerProcessor = (_name, implementation) => { Processor = implementation; };

await import(`./processor.js?line-in-test=${Date.now()}`);
if (!Processor) throw new Error('processor.js no registró RayDroneProcessor');

const wasmBytes = fs.readFileSync(new URL('./raydrone.wasm', import.meta.url));
const processor = new Processor({ processorOptions: { wasmBytes } });

const hann = new Float32Array(2048);
for (let i = 0; i < hann.length; i++) {
    hann[i] = 0.5 * (1 - Math.cos((2 * Math.PI * i) / (hann.length - 1)));
}
processor.handleMsg({ type: 'window', data: hann });
processor.handleMsg({
    type: 'params',
    focus: 0,
    aperture: 0.095,
    grainMs: 122,
    grainRate: 230,
    gain: 0.26,
    master: 1,
});
processor.handleMsg({ type: 'space', width: 0.35, oct: 0 });
processor.handleMsg({ type: 'reverb', wet: 0.15 });
processor.handleMsg({ type: 'live', on: 1, seconds: 4 });

let speechSq = 0;
let speechFrames = 0;
let silenceSq = 0;
let silenceFrames = 0;
let peak = 0;
const totalBlocks = Math.ceil((2.6 * SR) / BLOCK);

for (let block = 0; block < totalBlocks; block++) {
    const input = new Float32Array(BLOCK);
    const blockStart = block * BLOCK;
    for (let i = 0; i < BLOCK; i++) {
        const t = (blockStart + i) / SR;
        // Señal vocal sintética: dos formantes y una envolvente articulada.
        if (t >= 0.15 && t < 1.75) {
            const local = t - 0.15;
            const syllable = 0.35 + 0.65 * Math.max(0, Math.sin(local * Math.PI * 3.1));
            input[i] = syllable * (
                0.18 * Math.sin(2 * Math.PI * 145 * t)
                + 0.08 * Math.sin(2 * Math.PI * 720 * t)
                + 0.045 * Math.sin(2 * Math.PI * 1_180 * t)
            );
        }
    }
    const left = new Float32Array(BLOCK);
    const right = new Float32Array(BLOCK);
    processor.process([[input]], [[left, right]]);
    for (let i = 0; i < BLOCK; i++) {
        const value = (left[i] + right[i]) * 0.5;
        peak = Math.max(peak, Math.abs(value));
        const t = (blockStart + i) / SR;
        if (t >= 0.65 && t < 1.75) {
            speechSq += value * value;
            speechFrames++;
        } else if (t >= 2.2) {
            silenceSq += value * value;
            silenceFrames++;
        }
    }
}

const speechRms = Math.sqrt(speechSq / Math.max(1, speechFrames));
const silenceRms = Math.sqrt(silenceSq / Math.max(1, silenceFrames));
const liveMessages = processor.messages.filter((message) => message.live);
const latestLive = liveMessages.at(-1)?.live;

if (!latestLive?.ready) throw new Error('la cinta viva no llegó al estado ready');
if (!(speechRms > 0.001)) throw new Error(`salida inaudible: RMS=${speechRms}`);
if (!Number.isFinite(peak) || peak > 1.01) throw new Error(`pico inválido: ${peak}`);

console.log('Line In DSP — OK');
console.log(`voz procesada: RMS=${speechRms.toFixed(5)} · pico=${peak.toFixed(4)}`);
console.log(`cola tras voz: RMS=${silenceRms.toFixed(5)}`);
console.log(`cinta: foco=${latestLive.focus.toFixed(3)} s · span=${latestLive.span.toFixed(1)} s · ready=${latestLive.ready}`);
