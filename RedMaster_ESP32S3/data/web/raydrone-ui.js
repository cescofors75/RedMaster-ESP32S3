/* RED808 RayDrone panel. Kept as a small module so the AP serves it without
 * inflating app.js or adding another high-frequency WebSocket listener. */
(function () {
    'use strict';

    const FIELDS = ['character', 'motion', 'space', 'volume', 'mix', 'evolution'];
    const MAXES = { character: 35, motion: 100, space: 100, volume: 100, mix: 100, evolution: 100 };
    let requestId = 0;
    const timers = Object.create(null);

    function byId(id) { return document.getElementById(id); }
    function clamp(value, min, max) {
        const n = Number(value);
        return Number.isFinite(n) ? Math.max(min, Math.min(max, n)) : min;
    }
    function setStatus(text, good) {
        const el = byId('raydroneStatus');
        if (el) {
            el.textContent = text;
            el.classList.toggle('is-good', !!good);
        }
    }
    function sendPatch(patch) {
        if (typeof window.sendWebSocket !== 'function' || !window.isWebSocketReady || !window.isWebSocketReady()) {
            setStatus('WebSocket no conectado', false);
            return false;
        }
        const msg = Object.assign({ cmd: 'setRaydrone', requestId: ++requestId }, patch);
        const sent = window.sendWebSocket(msg);
        if (sent !== false) setStatus('RayDrone sincronizado', true);
        return sent !== false;
    }
    function updateValue(field, value) {
        const slider = byId(`raydrone${field[0].toUpperCase()}${field.slice(1)}`);
        const label = byId(`raydrone${field[0].toUpperCase()}${field.slice(1)}Value`);
        const max = MAXES[field] || 100;
        if (slider) slider.value = String(clamp(value, 0, max));
        if (label) label.textContent = `${clamp(value, 0, max)}%`;
    }
    function updateUI(config) {
        if (!config) return;
        const active = config.active !== undefined
            ? !!config.active
            : !!(Number(config.flags || 0) & 1);
        const power = byId('raydroneActive');
        if (power) power.checked = active;
        const powerLabel = byId('raydroneActiveLabel');
        if (powerLabel) powerLabel.textContent = active ? 'ON' : 'OFF';
        const material = byId('raydroneMaterial');
        if (material && config.material !== undefined) material.value = String(clamp(config.material, 0, 5));
        FIELDS.forEach((field) => {
            if (config[field] !== undefined) updateValue(field, config[field]);
        });
    }
    window.updateRaydroneUI = updateUI;

    function scheduleField(field, value, immediate) {
        if (timers[field]) {
            clearTimeout(timers[field]);
            timers[field] = null;
        }
        const patch = {}; patch[field] = clamp(value, 0, MAXES[field] || 100);
        if (immediate) {
            sendPatch(patch);
            return;
        }
        timers[field] = setTimeout(() => {
            timers[field] = null;
            sendPatch(patch);
        }, 40);
    }

    function init() {
        const power = byId('raydroneActive');
        if (power) power.addEventListener('change', () => {
            const label = byId('raydroneActiveLabel');
            if (label) label.textContent = power.checked ? 'ON' : 'OFF';
            sendPatch({ active: power.checked });
        });
        const material = byId('raydroneMaterial');
        if (material) material.addEventListener('change', () => {
            sendPatch({ material: clamp(material.value, 0, 5) });
        });
        FIELDS.forEach((field) => {
            const id = `raydrone${field[0].toUpperCase()}${field.slice(1)}`;
            const slider = byId(id);
            if (!slider) return;
            slider.addEventListener('input', () => {
                updateValue(field, slider.value);
                scheduleField(field, slider.value, false);
            });
            slider.addEventListener('change', () => {
                updateValue(field, slider.value);
                scheduleField(field, slider.value, true);
            });
        });
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init, { once: true });
    } else {
        init();
    }
})();
