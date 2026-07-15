// ================================================
// RED808 Chat AI Agent - SSE Client v2.0
// Bridges 63 agent tools → ESP32 WebSocket commands
// Maps instrument names → track indices automatically
// ================================================
console.log('%c[chat-agent.js] SCRIPT LOADED', 'color:#0ff;font-weight:bold;font-size:14px');

(function () {
    'use strict';
    console.log('%c[chat-agent.js] IIFE STARTING', 'color:#0ff;font-weight:bold');

    // ---- State ----
    let chatSessionId = null;
    let chatServerBase = '';
    let chatConnected = false;
    let chatStreaming = false;
    let chatAbortController = null;
    let chatHistory = [];
    let bridgedCmds = 0;
    let stateBridged = false; // prevent duplicate state bridges per stream

    // ════════════════════════════════════════════════════════════
    // Instrument name → ESP32 Track Index (0-15)
    // ════════════════════════════════════════════════════════════
    const INSTRUMENT_MAP = {
        kick: 0, bd: 0, bass_drum: 0,
        snare: 1, sd: 1,
        hihat_closed: 2, ch: 2, hihat: 2, closed_hihat: 2, hat: 2,
        hihat_open: 3, oh: 3, open_hihat: 3, open_hat: 3,
        cymbal: 4, cy: 4, ride: 4,
        clap: 5, cp: 5, handclap: 5,
        rim: 6, rs: 6, rimshot: 6,
        cowbell: 7, cb: 7,
        tom_low: 8, lt: 8, low_tom: 8,
        tom_mid: 9, mt: 9, mid_tom: 9,
        tom_hi: 10, ht: 10, hi_tom: 10, high_tom: 10,
        crash: 11, ma: 11, maraca: 11,
        clave: 12, cl: 12, claves: 12,
        hi_conga: 13, hc: 13, conga: 13, bongo: 13,
        mid_conga: 14, mc: 14,
        lo_conga: 15, lc: 15, low_conga: 15,
        shaker: 11, tambourine: 11,
        perc1: 12, perc2: 14,
        sub_bass: 0, '808_bass': 0,
        fx1: 11, fx2: 14
    };
    const TRACK_NAMES = ['BD','SD','CH','OH','CY','CP','RS','CB','LT','MT','HT','MA','CL','HC','MC','LC'];
    const TOOL_ICONS = {
        set_pattern: '🎹', set_pattern_with_velocity: '🎹', generate_genre_pattern: '🎶',
        generate_euclidean_rhythm: '🔵', randomize_pattern: '🎲', generate_fill: '🥁',
        create_variation: '🔀', add_ghost_notes: '👻', get_pattern: '📋',
        get_instrument_pattern: '📋', analyze_pattern: '📊', clear_pattern: '🧹',
        clear_all: '🧹', set_bpm: '⏱️', nudge_bpm: '⏱️', tap_tempo: '⏱️',
        set_swing: '🔄', set_steps: '📏', set_time_signature: '🎵',
        transport_control: '▶️', rotate_pattern: '🔃', reverse_pattern: '↩️',
        mirror_pattern: '🪞', invert_pattern: '🔁', shift_pattern: '➡️',
        humanize_pattern: '🤲', quantize_pattern: '📐', double_pattern: '✖️',
        halve_pattern: '➗', thin_pattern: '✂️', copy_instrument: '📋',
        swap_instruments: '🔄', set_accent_pattern: '💥', set_flam: '🔥',
        set_roll: '🥁', set_probability: '🎰', set_instrument_volume: '🔊',
        set_instrument_pan: '🎛️', mute_instrument: '🔇', solo_instrument: '🎯',
        set_master_volume: '🔊', set_send_levels: '📡', get_mixer: '🎚️',
        set_effect: '✨', get_effects: '✨', save_pattern: '💾',
        list_saved_patterns: '📂', load_saved_pattern: '📂', delete_saved_pattern: '🗑️',
        rename_saved_pattern: '✏️', add_to_song: '📝', remove_from_song: '❌',
        get_song: '📝', clear_song: '🧹', toggle_song_mode: '🎼',
        undo: '↩️', redo: '↪️', get_genre_info: 'ℹ️', list_genres: '📚',
        list_instruments: '🎸', export_midi_json: '📤', set_project_info: '📝',
        get_project_info: 'ℹ️'
    };

    function instrumentToTrack(name) {
        if (typeof name === 'number') return (name >= 0 && name < 16) ? name : -1;
        const key = String(name).toLowerCase().replace(/[\s-]/g, '_').trim();
        return INSTRUMENT_MAP.hasOwnProperty(key) ? INSTRUMENT_MAP[key] : -1;
    }
    function trackToName(idx) {
        return (idx >= 0 && idx < 16) ? TRACK_NAMES[idx] : '??';
    }

    // ---- DOM ----
    const $ = (id) => document.getElementById(id);

    // ---- Init ----
    function initChatAgent() {
        console.log('%c[chat-agent] initChatAgent()', 'color:#0f0;font-weight:bold',
            'chatServerUrl el=' + !!$('chatServerUrl'),
            'chatInput el=' + !!$('chatInput'),
            'seqChatFeed el=' + !!$('seqChatFeed'),
            'seqChatInput el=' + !!$('seqChatInput'),
            'ws=' + (window.ws ? 'readyState=' + window.ws.readyState : 'NONE'));
        const saved = localStorage.getItem('chatServerUrl');
        console.log('[chat-agent] saved server URL:', saved);
        if (saved) {
            const el = $('chatServerUrl');
            if (el) el.value = saved;
        }
        chatSessionId = localStorage.getItem('chatSessionId') || generateSessionId();
        localStorage.setItem('chatSessionId', chatSessionId);

        const input = $('chatInput');
        if (input) {
            input.addEventListener('input', () => {
                const btn = $('chatSendBtn');
                if (btn) btn.disabled = !input.value.trim();
            });
        }
        if (saved) {
            // Only auto-connect if AI panel is enabled
            const panel = document.getElementById('seqChatPanel');
            if (panel && !panel.classList.contains('ai-disabled')) {
                setTimeout(() => chatConnect(), 500);
            }
        }
    }

    // ---- Helpers ----
    function generateSessionId() {
        return 'red808_' + Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 8);
    }
    function escapeHtml(text) {
        const d = document.createElement('div');
        d.textContent = text;
        return d.innerHTML;
    }
    function scrollToBottom() {
        const c = $('chatMessages');
        if (c) requestAnimationFrame(() => { c.scrollTop = c.scrollHeight; });
    }

    // ════════════════════════════════════════════════════════════
    // Connection
    // ════════════════════════════════════════════════════════════
    window.chatConnect = function () {
        const el = $('chatServerUrl');
        if (!el) return;
        chatServerBase = el.value.trim().replace(/\/+$/, '');
        if (!chatServerBase) return;
        localStorage.setItem('chatServerUrl', chatServerBase);

        const badge = $('chatStatusBadge');
        const text = $('chatStatusText');
        const btn = $('chatConnectBtn');
        if (text) text.textContent = 'Conectando...';
        if (btn) { btn.textContent = '...'; btn.classList.remove('connected'); }

        fetch(chatServerBase + '/api/health', { method: 'GET', signal: AbortSignal.timeout(5000) })
            .then(r => { if (!r.ok) throw new Error('Not OK'); return r.json().catch(() => ({})); })
            .then(data => {
                chatConnected = true;
                if (badge) badge.classList.add('connected');
                if (text) text.textContent = 'Online';
                if (btn) { btn.textContent = '● ONLINE'; btn.classList.add('connected'); }
                const tools = data.tools || data.toolCount || '63';
                console.log('%c[CHAT] Connected to agent', 'color:#0f0;font-weight:bold', 'tools=' + tools, 'wsReady=' + (window.ws ? window.ws.readyState : 'NO_WS'));
                updateSeqChatStatus();
                if (window.showToast) window.showToast('🤖 Agente conectado (' + tools + ' tools)', window.TOAST_TYPES?.SUCCESS, 2500);
            })
            .catch(() => {
                fetch(chatServerBase + '/api/chat', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ message: '', sessionId: chatSessionId }),
                    signal: AbortSignal.timeout(5000)
                }).then(r => {
                    chatConnected = true;
                    if (badge) badge.classList.add('connected');
                    if (text) text.textContent = 'Online';
                    if (btn) { btn.textContent = '● ONLINE'; btn.classList.add('connected'); }
                }).catch(() => {
                    chatConnected = false;
                    if (badge) badge.classList.remove('connected');
                    if (text) text.textContent = 'Sin conexión';
                    if (btn) { btn.textContent = 'RECONECTAR'; btn.classList.remove('connected'); }
                    if (window.showToast) window.showToast('❌ No se pudo conectar al agente', window.TOAST_TYPES?.ERROR, 3000);
                });
            });
    };

    // ════════════════════════════════════════════════════════════
    // Send Message
    // ════════════════════════════════════════════════════════════
    window.chatSend = function () {
        const input = $('chatInput');
        if (!input) return;
        const message = input.value.trim();
        if (!message) return;

        input.value = '';
        input.style.height = 'auto';
        const btn = $('chatSendBtn');
        if (btn) btn.disabled = true;

        const welcome = document.querySelector('.chat-welcome');
        if (welcome) welcome.remove();

        appendMessage('user', message);
        chatHistory.push({ role: 'user', content: message });
        bridgedCmds = 0;

        if (chatConnected) {
            chatStreamRequest(message);
        } else {
            appendSystemMessage('⚠️ No conectado al agente. Pulsa CONECTAR primero.');
        }
    };

    window.chatSendSuggestion = function (btnEl) {
        const input = $('chatInput');
        if (input && btnEl) {
            input.value = btnEl.textContent.replace(/^[^\s]+\s/, '');
            input.dispatchEvent(new Event('input'));
            window.chatSend();
        }
    };

    window.chatHandleKeydown = function (e) {
        if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); window.chatSend(); }
    };

    window.chatAutoResize = function (el) {
        el.style.height = 'auto';
        el.style.height = Math.min(el.scrollHeight, 120) + 'px';
    };

    // ════════════════════════════════════════════════════════════
    // SSE Streaming Request
    // Server sends standard SSE format:
    //   event: round|tool|token|done|state|error
    //   data: {JSON}
    // ════════════════════════════════════════════════════════════
    function chatStreamRequest(message) {
        console.log('%c[SSE] ════ NEW STREAM REQUEST ════', 'color:#0af;font-weight:bold',
            'msg=' + message.slice(0, 50), 'server=' + chatServerBase,
            'wsReady=' + (window.ws ? window.ws.readyState : 'NO_WS'));
        chatStreaming = true;
        stateBridged = false; // reset per-stream dedup flag
        showProgress(true, 'Pensando...', 5);

        const assistantMsgId = 'chat-assistant-' + Date.now();
        appendMessage('assistant', '', assistantMsgId);
        showTypingInMessage(assistantMsgId);

        chatAbortController = new AbortController();

        fetch(chatServerBase + '/api/chat/stream', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ message: message, sessionId: chatSessionId }),
            signal: chatAbortController.signal
        }).then(response => {
            if (!response.ok) throw new Error('Stream failed: ' + response.status);
            if (!response.body) throw new Error('No stream body');

            const reader = response.body.getReader();
            const decoder = new TextDecoder();
            let buffer = '';
            let assistantText = '';
            let currentEventType = ''; // tracks the "event:" line

            function processChunk() {
                reader.read().then(({ done, value }) => {
                    if (done) {
                        console.log('[SSE] ════ STREAM CLOSED ════ stateBridged=' + stateBridged + ' bridgedCmds=' + bridgedCmds);
                        finishStream(assistantMsgId, assistantText);
                        return;
                    }

                    buffer += decoder.decode(value, { stream: true });
                    const lines = buffer.split('\n');
                    buffer = lines.pop() || '';

                    for (const line of lines) {
                        const trimmed = line.trim();

                        // Empty line = end of SSE event block
                        if (!trimmed) { currentEventType = ''; continue; }

                        // Capture event type
                        if (trimmed.startsWith('event:')) {
                            currentEventType = trimmed.slice(6).trim();
                            continue;
                        }

                        // Process data line
                        if (!trimmed.startsWith('data:')) continue;
                        const dataStr = trimmed.slice(5).trim();
                        if (!dataStr || dataStr === '[DONE]') continue;

                        let data;
                        try { data = JSON.parse(dataStr); }
                        catch (e) { continue; } // partial JSON, skip

                        // Determine event type: use SSE "event:" line, fallback to data.type
                        const evType = currentEventType || data.type || '';

                        console.log('%c[SSE] event=' + evType, 'color:#0af;font-weight:bold',
                            'keys=' + (typeof data === 'object' ? Object.keys(data).join(',') : typeof data),
                            evType === 'tool' ? 'tool=' + (data.name || '?') : '',
                            evType === 'state' ? 'pattern_keys=' + (data.pattern ? Object.keys(data.pattern).length : 'NO') : '');

                        switch (evType) {
                            case 'start':
                                // Session ack, nothing to render
                                break;

                            case 'round':
                                appendRoundIndicator(data.round, data.maxRounds || 8);
                                showProgress(true, 'Ronda ' + data.round + '/' + (data.maxRounds || 8),
                                    (data.round / (data.maxRounds || 8)) * 100);
                                break;

                            case 'tool': {
                                // Server sends combined: {name, args, result}
                                const toolName = data.name || data.tool || '';
                                const toolArgs = data.args || data.arguments || {};
                                const toolResult = data.result || null;
                                appendToolCall(toolName, toolArgs);
                                // Bridge commands AND pass result for multi-bar pattern extraction
                                bridgeToolCall(toolName, toolArgs, toolResult);
                                if (toolResult) {
                                    appendToolResult(toolName, toolResult, data.state);
                                }
                                break;
                            }

                            case 'tool_call':
                                // Alternative format: separate tool_call event
                                appendToolCall(data.name, data.arguments || data.args);
                                bridgeToolCall(data.name, data.arguments || data.args, null);
                                break;

                            case 'tool_result':
                                appendToolResult(data.name, data.result, data.state);
                                if (data.state) bridgeStateToESP32(data.state);
                                break;

                            case 'token':
                            case 'text':
                                removeTypingIndicator(assistantMsgId);
                                assistantText += (data.content || data.text || '');
                                updateAssistantMessage(assistantMsgId, assistantText);
                                showProgress(true, 'Respondiendo...', 80);
                                break;

                            case 'done': {
                                console.log('%c[SSE] ▶ DONE event', 'color:#ff0;font-weight:bold',
                                    'hasResponse=' + !!(data.response || data.text),
                                    'responseLen=' + (data.response || data.text || '').length,
                                    'hasState=' + !!data.state,
                                    'toolCallsCount=' + (Array.isArray(data.toolCalls) ? data.toolCalls.length : 0),
                                    'stateBridged=' + stateBridged);
                                removeTypingIndicator(assistantMsgId);
                                // Final response text
                                const finalText = data.response || data.text || '';
                                if (finalText) {
                                    assistantText = finalText;
                                    updateAssistantMessage(assistantMsgId, assistantText);
                                    console.log('[SSE] done: text updated, len=' + assistantText.length);
                                }
                                // If done includes state, bridge it now
                                if (data.state && !stateBridged) {
                                    console.log('[SSE] done: bridging embedded state');
                                    bridgeStateToESP32(data.state);
                                    stateBridged = true;
                                }
                                showProgress(true, 'Aplicando cambios...', 95);
                                // DON'T return here! The "state" event comes AFTER "done"
                                // and contains the full pattern/mixer/velocity data.
                                // We must keep reading the stream to process it.
                                console.log('[SSE] done: continuing to read stream for state event...');
                                break;
                            }

                            case 'state':
                                // Standalone state event with full pattern/mixer/velocity/effects
                                // This is THE authoritative source — bridge everything
                                console.log('%c[SSE] ▶ STATE event', 'color:#0f0;font-weight:bold',
                                    'stateBridged=' + stateBridged,
                                    'bpm=' + data.bpm,
                                    'patternKeys=' + (data.pattern ? Object.keys(data.pattern).length : 'NONE'),
                                    'velocityKeys=' + (data.velocity ? Object.keys(data.velocity).length : 'NONE'),
                                    'hasMixer=' + !!data.mixer,
                                    'hasEffects=' + !!data.effects,
                                    'wsReady=' + (window.ws ? window.ws.readyState : 'NO_WS'));
                                if (!stateBridged) {
                                    bridgeStateToESP32(data);
                                    stateBridged = true;
                                } else {
                                    console.warn('[SSE] STATE skipped - already bridged');
                                }
                                break;

                            case 'error':
                                console.error('%c[SSE] ▶ ERROR event', 'color:red;font-weight:bold',
                                    'FULL DATA:', JSON.stringify(data));
                                appendSystemMessage('❌ ' + (data.message || data.error || 'Error desconocido'));
                                finishStream(assistantMsgId, assistantText);
                                return;

                            default:
                                // Unknown event type: check if data has useful fields
                                if (data.content || data.text) {
                                    removeTypingIndicator(assistantMsgId);
                                    assistantText += (data.content || data.text || '');
                                    updateAssistantMessage(assistantMsgId, assistantText);
                                }
                                break;
                        }
                    }
                    processChunk();
                }).catch(err => {
                    if (err.name !== 'AbortError') {
                        appendSystemMessage('⚠️ Error en stream: ' + err.message);
                    }
                    finishStream(assistantMsgId, assistantText);
                });
            }
            processChunk();
        }).catch(err => {
            if (err.name === 'AbortError') return;
            chatNonStreamRequest(message, assistantMsgId);
        });
    }

    // ---- Non-streaming fallback ----
    function chatNonStreamRequest(message, assistantMsgId) {
        showProgress(true, 'Procesando...', 50);
        fetch(chatServerBase + '/api/chat', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ message: message, sessionId: chatSessionId })
        }).then(r => r.json())
            .then(data => {
                const text = data.response || data.text || data.message || '';
                updateAssistantMessage(assistantMsgId, text);
                if (data.state) bridgeStateToESP32(data.state);
                if (Array.isArray(data.toolCalls)) {
                    data.toolCalls.forEach(tc => {
                        appendToolCall(tc.name, tc.arguments);
                        bridgeToolCall(tc.name, tc.arguments, tc.result || null);
                        if (tc.result) appendToolResult(tc.name, tc.result);
                    });
                }
                finishStream(assistantMsgId, text);
            })
            .catch(err => {
                appendSystemMessage('❌ Error: ' + err.message);
                finishStream(assistantMsgId, '');
            });
    }

    function finishStream(msgId, text) {
        console.log('%c[SSE] ════ finishStream ════', 'color:#f80;font-weight:bold',
            'textLen=' + (text ? text.length : 0),
            'bridgedCmds=' + bridgedCmds,
            'stateBridged=' + stateBridged);
        chatStreaming = false;
        chatAbortController = null;
        showProgress(false);
        removeTypingIndicator(msgId);
        if (text) chatHistory.push({ role: 'assistant', content: text });

        // Song mode is now handled entirely by bridgeStateToESP32
        // via the songChain array in the state event.

        if (bridgedCmds > 0) {
            // Show progress while queue drains
            const totalCmds = bridgedCmds;
            seqAppendBridge('🤖 Agent listo — enviando ' + totalCmds + ' comandos al ESP32...');
            
            // Poll queue until empty, then show done
            const checkQueue = setInterval(() => {
                const pending = wsQueue.length;
                if (pending <= 0) {
                    clearInterval(checkQueue);
                    seqAppendBridge('✅ ESP32 cargado — ' + totalCmds + ' comandos aplicados');
                }
            }, 200);
            
            appendBridgeStatus(totalCmds);
            bridgedCmds = 0;
        }
    }

    // ════════════════════════════════════════════════════════════
    // BRIDGE: Direct tool_call → ESP32 WebSocket
    // Parses each tool and sends immediate WS commands.
    // Handles BOTH server formats:
    //   A) {instrument:"kick", pattern:[1,0,...]}   (single)
    //   B) {kick:[1,0,...], snare:[0,1,...]}         (multi)
    // ════════════════════════════════════════════════════════════
    function bridgeToolCall(toolName, toolArgs, toolResult) {
        const ws = window.ws;
        if (!ws || ws.readyState !== WebSocket.OPEN) {
            console.warn('[bridge-tool] WS not open! ws=' + !!ws + ' readyState=' + (ws ? ws.readyState : 'N/A') + ' tool=' + toolName);
            return;
        }
        console.log('[bridge-tool]', toolName,
            typeof toolArgs === 'string' ? toolArgs.slice(0, 100) : JSON.stringify(toolArgs).slice(0, 100),
            toolResult ? 'result_keys=' + Object.keys(toolResult).join(',') : 'no_result');

        let args;
        try { args = typeof toolArgs === 'string' ? JSON.parse(toolArgs) : (toolArgs || {}); }
        catch (e) { console.warn('[bridge] bad args', toolArgs); return; }

        let result;
        try { result = typeof toolResult === 'string' ? JSON.parse(toolResult) : (toolResult || null); }
        catch (e) { result = null; }

        let cmds = 0;
        const send = (obj) => { wsQueueSend(obj); cmds++; };

        // Helper: apply 16-step pattern array for one track
        function applyTrackSteps(t, steps) {
            if (t < 0 || !Array.isArray(steps)) return;
            steps.forEach((val, step) => {
                if (step >= 16) return;
                send({ cmd: 'setStep', track: t, step: step, active: val > 0, silent: true });
                if (val > 1) {
                    send({ cmd: 'setStepVelocity', track: t, step: step, velocity: Math.min(127, val), silent: true });
                }
            });
        }

        switch (toolName) {

            // ── BPM / Transport ──
            case 'set_bpm':
                if (args.bpm !== undefined) send({ cmd: 'tempo', value: args.bpm });
                break;

            case 'nudge_bpm':
                if (args.amount !== undefined && window.currentBPM) {
                    send({ cmd: 'tempo', value: window.currentBPM + args.amount });
                }
                break;

            case 'set_swing':
                if (args.swing !== undefined) send({ cmd: 'setSwing', value: args.swing });
                else if (args.amount !== undefined) send({ cmd: 'setSwing', value: args.amount });
                break;

            case 'transport_control':
                if (args.action === 'play' || args.action === 'start') send({ cmd: 'start' });
                else if (args.action === 'stop') send({ cmd: 'stop' });
                break;

            // ── Pattern (single or multi instrument — always 16 steps) ──
            case 'set_pattern':
            case 'set_pattern_with_velocity': {
                // Format A: {instrument:"kick", pattern:[1,0,...16 steps...]}
                if (args.instrument !== undefined && args.pattern !== undefined) {
                    const t = instrumentToTrack(args.instrument);
                    applyTrackSteps(t, args.pattern);
                }
                // Format B: {kick:[1,0,...], snare:[0,1,...]} — multiple instruments
                else {
                    for (const [key, steps] of Object.entries(args)) {
                        if (key === 'velocity' || key === 'bpm') continue;
                        const t = instrumentToTrack(key);
                        if (t >= 0 && Array.isArray(steps)) applyTrackSteps(t, steps);
                    }
                }
                break;
            }

            // ── Generate pattern (bridge BPM only — pattern comes in state event) ──
            case 'generate_genre_pattern':
                if (args.bpm) send({ cmd: 'tempo', value: args.bpm });
                // Pattern data comes in the state event (definitive source)
                break;

            // ── Euclidean → set steps directly ──
            case 'generate_euclidean_rhythm': {
                const t = instrumentToTrack(args.instrument);
                if (t >= 0 && args.pulses) {
                    // Build euclidean pattern locally
                    const total = Math.min(args.steps || 16, 16);
                    const pulses = Math.min(args.pulses, total);
                    const rot = args.rotation || 0;
                    const pattern = new Array(total).fill(0);
                    for (let i = 0; i < pulses; i++) {
                        const pos = (Math.floor(i * total / pulses) + rot) % total;
                        pattern[pos] = 100;
                    }
                    applyTrackSteps(t, pattern);
                }
                break;
            }

            // ── Rolls / flams → translate to step activations + velocity ──
            case 'set_roll':
            case 'set_flam':
            case 'set_accent_pattern':
            case 'add_ghost_notes': {
                const t = instrumentToTrack(args.instrument);
                if (t >= 0 && Array.isArray(args.pattern)) {
                    applyTrackSteps(t, args.pattern);
                }
                break;
            }

            // ── Server-side pattern modifiers → state event has the final result ──
            case 'humanize_pattern':
            case 'randomize_pattern':
            case 'generate_fill':
            case 'create_variation':
                // These only modify on the server. The final consolidated pattern
                // (with ALL modifications applied) comes in the state event.
                break;

            // ── Clear ──
            case 'clear_pattern':
            case 'clear_all':
                send({ cmd: 'clearPattern' });
                break;

            // ── Mute ──
            case 'mute_instrument': {
                const t = instrumentToTrack(args.instrument);
                if (t >= 0) send({ cmd: 'mute', track: t, value: args.muted !== false });
                break;
            }

            // ── Solo (emulate: mute all EXCEPT target) ──
            case 'solo_instrument': {
                const t = instrumentToTrack(args.instrument);
                if (t >= 0) {
                    const solo = args.solo !== false;
                    for (let i = 0; i < 16; i++) {
                        send({ cmd: 'mute', track: i, value: solo ? (i !== t) : false });
                    }
                }
                break;
            }

            // ── Volume ──
            case 'set_instrument_volume': {
                const t = instrumentToTrack(args.instrument);
                if (t >= 0) send({ cmd: 'setTrackVolume', track: t, volume: Math.round(args.volume != null ? args.volume : 100) });
                break;
            }
            case 'set_master_volume':
                if (args.volume !== undefined) send({ cmd: 'setVolume', value: Math.round(args.volume) });
                break;

            // ── Pan (ESP32 has no pan, skip silently) ──
            case 'set_instrument_pan':
            case 'set_send_levels':
                break;

            // ── Pattern transforms that return steps ──
            case 'rotate_pattern':
            case 'reverse_pattern':
            case 'mirror_pattern':
            case 'invert_pattern':
            case 'shift_pattern':
            case 'double_pattern':
            case 'halve_pattern':
            case 'thin_pattern':
            case 'copy_instrument':
            case 'swap_instruments':
            case 'quantize_pattern':
                // Server-side transforms — final pattern comes in state event
                break;

            // ── Effects ──
            case 'set_effect': {
                const fx = (args.effect || '').toLowerCase();
                const p = args.params || {};
                const on = args.enabled !== false;

                if (fx === 'compressor' || fx === 'sidechain') {
                    send({ cmd: 'setCompressorActive', active: on });
                    if (p.threshold !== undefined) send({ cmd: 'setCompressorThreshold', value: p.threshold });
                    if (p.ratio !== undefined) send({ cmd: 'setCompressorRatio', value: p.ratio });
                    if (p.attack !== undefined) send({ cmd: 'setCompressorAttack', value: p.attack });
                    if (p.release !== undefined) send({ cmd: 'setCompressorRelease', value: p.release });
                    if (p.makeupGain !== undefined) send({ cmd: 'setCompressorMakeupGain', value: p.makeupGain });
                }
                else if (fx === 'delay' || fx === 'echo') {
                    send({ cmd: 'setDelayActive', active: on });
                    if (p.time !== undefined) send({ cmd: 'setDelayTime', value: p.time });
                    if (p.feedback !== undefined) send({ cmd: 'setDelayFeedback', value: p.feedback });
                    if (p.mix !== undefined) send({ cmd: 'setDelayMix', value: p.mix });
                }
                else if (fx === 'distortion' || fx === 'overdrive' || fx === 'saturation') {
                    if (p.amount !== undefined) send({ cmd: 'setDistortion', value: p.amount });
                    if (p.drive !== undefined) send({ cmd: 'setDistortion', value: p.drive });
                    if (p.mode !== undefined) send({ cmd: 'setDistortionMode', value: p.mode });
                }
                else if (fx === 'filter' || fx === 'lowpass' || fx === 'highpass') {
                    if (p.type !== undefined) send({ cmd: 'setFilter', value: p.type });
                    if (p.cutoff !== undefined) send({ cmd: 'setFilterCutoff', value: p.cutoff });
                    if (p.resonance !== undefined) send({ cmd: 'setFilterResonance', value: p.resonance });
                }
                else if (fx === 'bitcrusher' || fx === 'bitcrush' || fx === 'lofi') {
                    if (p.bits !== undefined) send({ cmd: 'setBitCrush', value: p.bits });
                    if (p.sampleRate !== undefined) send({ cmd: 'setSampleRate', value: p.sampleRate });
                }
                else if (fx === 'phaser') {
                    send({ cmd: 'setPhaserActive', active: on });
                    if (p.rate !== undefined) send({ cmd: 'setPhaserRate', value: p.rate });
                    if (p.depth !== undefined) send({ cmd: 'setPhaserDepth', value: p.depth });
                    if (p.feedback !== undefined) send({ cmd: 'setPhaserFeedback', value: p.feedback });
                }
                else if (fx === 'flanger') {
                    send({ cmd: 'setFlangerActive', active: on });
                    if (p.rate !== undefined) send({ cmd: 'setFlangerRate', value: p.rate });
                    if (p.depth !== undefined) send({ cmd: 'setFlangerDepth', value: p.depth });
                    if (p.feedback !== undefined) send({ cmd: 'setFlangerFeedback', value: p.feedback });
                    if (p.mix !== undefined) send({ cmd: 'setFlangerMix', value: p.mix });
                }
                else if (fx === 'reverse') {
                    const t = instrumentToTrack(p.instrument || args.instrument);
                    if (t >= 0) send({ cmd: 'setReverse', track: t, value: on });
                }
                else if (fx === 'stutter') {
                    const t = instrumentToTrack(p.instrument || args.instrument);
                    if (t >= 0) send({ cmd: 'setStutter', track: t, value: on, interval: p.interval || 100 });
                }
                else if (fx === 'scratch') {
                    // 'setScratch' no existe en el firmware (verificado contra
                    // processCommand): se enviaba y se descartaba en silencio,
                    // y el agente reportaba un exito falso. Avisar en el chat
                    // hasta que el comando exista en el servidor.
                    console.warn('[chat-agent] scratch no soportado por el firmware');
                    appendMessage('assistant', '⚠️ El efecto "scratch" aún no está soportado por el firmware.');
                }
                else if (fx === 'pitchshift' || fx === 'pitch') {
                    const t = instrumentToTrack(p.instrument || args.instrument);
                    if (t >= 0 && p.value !== undefined) send({ cmd: 'setPitchShift', track: t, value: p.value });
                }
                break;
            }

            // ── Song mode (handled by bridgeStateToESP32 via songChain) ──
            case 'toggle_song_mode':
            case 'add_to_song':
            case 'remove_from_song':
            case 'clear_song':
            case 'reorder_song':
            case 'queue_pattern':
                // Song arrangement is managed entirely via the songChain
                // in the state event. No direct bridge needed.
                break;

            case 'set_steps':
                // ESP32 only supports 16 steps per pattern — ignore
                break;

            // ── Read-only / server-side tools (no bridge needed) ──
            case 'get_pattern':
            case 'get_instrument_pattern':
            case 'analyze_pattern':
            case 'get_mixer':
            case 'get_effects':
            case 'get_song':
            case 'list_saved_patterns':
            case 'list_genres':
            case 'list_instruments':
            case 'get_genre_info':
            case 'get_project_info':
            case 'export_midi_json':
            case 'save_pattern':
            case 'load_saved_pattern':
            case 'delete_saved_pattern':
            case 'rename_saved_pattern':
            case 'duplicate_saved_pattern':
            case 'set_project_info':
            case 'undo':
            case 'redo':
            case 'tap_tempo':
            case 'set_probability':
            case 'set_time_signature':
            // ── Server-side expression (result comes in state event pattern/velocity) ──
            case 'set_step_condition':
            case 'set_step_conditions':
            case 'set_micro_timing':
            case 'nudge_step':
            case 'set_ratchet':
            case 'scale_velocity':
            case 'velocity_ramp':
            case 'merge_patterns':
            case 'interpolate_patterns':
            case 'save_mute_group':
            case 'load_mute_group':
            case 'list_mute_groups':
                break;

            default:
                console.log('[bridge] unhandled tool:', toolName);
                break;
        }

        if (cmds > 0) {
            console.log('[bridge-tool] ✓', toolName, '→', cmds, 'cmds queued');
            bridgedCmds += cmds;
            markToolCallApplied(cmds);
            wsQueueStart(); // flush the queue
        } else {
            console.log('[bridge-tool] ○', toolName, '→ 0 cmds (no direct bridge for this tool)');
        }
    }

    // ════════════════════════════════════════════════════════════
    // Throttled WebSocket sender — sends commands in batches
    // to avoid overwhelming the ESP32 (max ~6 cmds per batch)
    // ════════════════════════════════════════════════════════════
    let wsQueue = [];
    let wsQueueTimer = null;
    let wsQueueRefreshPending = false;
    const WS_BATCH_SIZE = 6;      // commands per batch
    const WS_BATCH_DELAY = 80;    // ms between batches

    function wsQueueSend(obj) {
        wsQueue.push(JSON.stringify(obj));
        // Don't auto-start flush — caller must call wsQueueStart() when done adding
    }

    function wsQueueStart() {
        wsQueueRefreshPending = true;
        if (!wsQueueTimer) wsQueueFlush();
    }

    function wsQueueFlush() {
        const ws = window.ws;
        if (!ws || ws.readyState !== WebSocket.OPEN) {
            console.warn('[wsQueue] WS not open, dropping', wsQueue.length, 'commands');
            wsQueue = [];
            wsQueueTimer = null;
            wsQueueRefreshPending = false;
            return;
        }

        const batch = wsQueue.splice(0, WS_BATCH_SIZE);
        for (const msg of batch) ws.send(msg);

        if (wsQueue.length > 0) {
            wsQueueTimer = setTimeout(wsQueueFlush, WS_BATCH_DELAY);
        } else {
            wsQueueTimer = null;
            // All done — request ONE pattern refresh
            if (wsQueueRefreshPending) {
                wsQueueRefreshPending = false;
                console.log('[wsQueue] all ' + batch.length + ' final batch sent, requesting getPattern');
                setTimeout(() => {
                    if (ws.readyState === WebSocket.OPEN) {
                        ws.send(JSON.stringify({ cmd: 'getPattern' }));
                    }
                }, 300);
            }
        }
    }

    // ════════════════════════════════════════════════════════════
    // BRIDGE: Full state object → ESP32 (from tool_result/done/state)
    // Handles BOTH array and object-keyed formats.
    // Server sends: {bpm, swing, pattern:{kick:[...]}, velocity:{kick:[...]},
    //   mixer:{master, channels:{kick:{volume,mute,...}}}, effects:{...}, playing}
    // ════════════════════════════════════════════════════════════
    function bridgeStateToESP32(state) {
        if (!state) { console.warn('[bridge-state] called with null/undefined state'); return; }
        if (!window.ws) { console.warn('[bridge-state] NO window.ws!'); return; }
        if (window.ws.readyState !== WebSocket.OPEN) {
            console.warn('[bridge-state] ws not OPEN, readyState=' + window.ws.readyState);
            return;
        }
        const ws = window.ws;
        let cmds = 0;
        let firstCmds = [];
        const send = (obj) => { 
            wsQueueSend(obj); 
            cmds++; 
            if (firstCmds.length < 5) firstCmds.push(obj.cmd + (obj.track !== undefined ? ':t' + obj.track : '') + (obj.step !== undefined ? ':s' + obj.step : '') + (obj.active !== undefined ? ':' + (obj.active?1:0) : '') + (obj.value !== undefined ? '=' + obj.value : ''));
        };

        console.log('%c[bridge-state] START', 'color:#0f0;font-weight:bold',
            'keys=' + Object.keys(state).join(','),
            'bpm=' + state.bpm,
            'songChain=' + (state.songChain ? state.songChain.length + ' entries' : 'none'),
            'patternTracks=' + (state.pattern ? Object.keys(state.pattern).filter(k => {
                var s = state.pattern[k]; return Array.isArray(s) && s.some(v => v > 0);
            }).length : 0) + ' active',
            'mixer=' + (state.mixer ? 'yes(' + (state.mixer.channels ? Object.keys(state.mixer.channels).length : '?') + 'ch)' : 'no'));

        // BPM
        if (state.bpm !== undefined) send({ cmd: 'tempo', value: state.bpm });

        // Swing
        if (state.swing !== undefined) send({ cmd: 'setSwing', value: state.swing });

        // Transport
        if (state.playing === true) send({ cmd: 'start' });
        else if (state.playing === false) send({ cmd: 'stop' });

        // Pattern + Velocity + Song Chain
        // Server guarantees all patterns are exactly 16 steps.
        // The state event is the DEFINITIVE source.
        //
        // songChain is ONLY processed when songMode === true.
        // The server always includes songChain/savedPatterns in its state,
        // but those are server-side concepts — we only bridge them to
        // ESP32 slots when the user explicitly requested song mode.
        //
        // For normal beat requests, we just write state.pattern to the
        // currently active ESP32 slot (no selectPattern, no slot changes).

        const useSongChain = state.songMode === true &&
            state.songChain && Array.isArray(state.songChain) && state.songChain.length > 0;

        if (useSongChain) {
            // ── SONG CHAIN MODE (only when songMode explicitly ON) ──
            // Calculate total ESP32 slots needed: sum of all bars
            let totalSlots = 0;
            state.songChain.forEach(entry => { totalSlots += (entry.bars || 1); });
            console.log('[bridge-state] SONG CHAIN: ' + state.songChain.length + ' sections → ' + totalSlots + ' slots');

            let slotIdx = 0;
            for (const entry of state.songChain) {
                const bars = entry.bars || 1;
                const pat = entry.patternData || entry.pattern || {};
                const vel = entry.velocity || {};

                // Write this pattern to 'bars' consecutive ESP32 slots
                for (let b = 0; b < bars; b++) {
                    // Select the ESP32 slot
                    send({ cmd: 'selectPattern', index: slotIdx });

                    // Write pattern steps (16 per instrument)
                    for (const [inst, steps] of Object.entries(pat)) {
                        const t = instrumentToTrack(inst);
                        if (t < 0 || !Array.isArray(steps)) continue;
                        for (let s = 0; s < 16; s++) {
                            const val = s < steps.length ? steps[s] : 0;
                            send({ cmd: 'setStep', track: t, step: s, active: val > 0, silent: true });
                        }
                    }

                    // Write velocity (16 per instrument)
                    for (const [inst, vels] of Object.entries(vel)) {
                        const t = instrumentToTrack(inst);
                        if (t < 0 || !Array.isArray(vels)) continue;
                        for (let s = 0; s < 16; s++) {
                            const v = s < vels.length ? vels[s] : 0;
                            if (v > 0) send({ cmd: 'setStepVelocity', track: t, step: s, velocity: Math.min(127, v), silent: true });
                        }
                    }

                    console.log('[bridge-state] slot ' + slotIdx + ': ' + (entry.patternName || 'pattern') + ' (bar ' + (b + 1) + '/' + bars + ')');
                    slotIdx++;
                }
            }

            // Enable song mode with total slot count, start at pattern 0
            send({ cmd: 'setSongMode', enabled: true, length: totalSlots });
            send({ cmd: 'selectPattern', index: 0 });
            console.log('[bridge-state] song mode ON, length=' + totalSlots);

        } else if (state.pattern) {
            // ── SINGLE PATTERN MODE ──
            // Write pattern to whatever slot the ESP32 currently has active.
            // NO selectPattern call — don't touch other slots.
            // Disable song mode so ESP32 stays on this one slot.
            send({ cmd: 'setSongMode', enabled: false, length: 1 });

            let patternCmdsBefore = cmds;
            const isObj = typeof state.pattern === 'object' && !Array.isArray(state.pattern);

            if (Array.isArray(state.pattern)) {
                state.pattern.forEach((trackSteps, idx) => {
                    if (!Array.isArray(trackSteps) || idx >= 16) return;
                    trackSteps.forEach((val, step) => {
                        if (step < 16) send({ cmd: 'setStep', track: idx, step: step, active: val > 0, silent: true });
                    });
                });
            } else if (isObj) {
                const entries = Object.entries(state.pattern);
                let mapped = 0, unmapped = [];
                entries.forEach(([inst, steps]) => {
                    const t = instrumentToTrack(inst);
                    if (t >= 0 && Array.isArray(steps)) {
                        mapped++;
                        steps.forEach((val, step) => {
                            if (step < 16) send({ cmd: 'setStep', track: t, step: step, active: val > 0, silent: true });
                        });
                    } else if (Array.isArray(steps)) {
                        unmapped.push(inst);
                    }
                });
                console.log('[bridge-state] pattern: ' + entries.length + ' instruments, ' + mapped + ' mapped, ' + (cmds - patternCmdsBefore) + ' setStep cmds',
                    unmapped.length > 0 ? 'UNMAPPED: ' + unmapped.join(',') : '');
            }

            // Velocity (single pattern)
            if (state.velocity) {
                let velCmdsBefore = cmds;
                if (Array.isArray(state.velocity)) {
                    state.velocity.forEach((vels, idx) => {
                        if (!Array.isArray(vels) || idx >= 16) return;
                        vels.forEach((v, s) => {
                            if (s < 16 && v > 0) send({ cmd: 'setStepVelocity', track: idx, step: s, velocity: v, silent: true });
                        });
                    });
                } else if (typeof state.velocity === 'object') {
                    for (const [inst, vels] of Object.entries(state.velocity)) {
                        const t = instrumentToTrack(inst);
                        if (t >= 0 && Array.isArray(vels)) {
                            vels.forEach((v, s) => {
                                if (s < 16 && v > 0) send({ cmd: 'setStepVelocity', track: t, step: s, velocity: v, silent: true });
                            });
                        }
                    }
                }
                console.log('[bridge-state] velocity: sent ' + (cmds - velCmdsBefore) + ' setStepVelocity cmds');
            }
        } else {
            console.log('[bridge-state] NO pattern in state');
        }

        // Mixer: server format {master:{volume:100}, channels:{kick:{volume,muted,...},...}}
        //   OR legacy: {master:100, channels:{...}} or flat formats
        if (state.mixer) {
            let mixCmdsBefore = cmds;
            const mx = state.mixer;
            console.log('[bridge-state] mixer keys=' + Object.keys(mx).join(','),
                'hasMaster=' + (mx.master !== undefined), 'hasChannels=' + !!mx.channels);
            // Master volume: handle both {volume:N} and plain N
            if (mx.master !== undefined) {
                const masterVol = (typeof mx.master === 'object' && mx.master.volume !== undefined)
                    ? mx.master.volume : mx.master;
                if (typeof masterVol === 'number') send({ cmd: 'setVolume', value: Math.round(masterVol) });
            }
            
            // Channels object: {kick:{volume,mute,...},...}
            const channels = mx.channels || mx;
            if (typeof channels === 'object' && !Array.isArray(channels)) {
                for (const [inst, cfg] of Object.entries(channels)) {
                    if (inst === 'master') continue; // skip master key
                    const t = instrumentToTrack(inst);
                    if (t < 0) continue;
                    if (typeof cfg === 'number') {
                        send({ cmd: 'setTrackVolume', track: t, volume: Math.round(cfg) });
                    } else if (typeof cfg === 'object') {
                        if (cfg.volume !== undefined) send({ cmd: 'setTrackVolume', track: t, volume: Math.round(cfg.volume) });
                        if (cfg.mute === true || cfg.muted === true) send({ cmd: 'mute', track: t, value: true });
                    }
                }
            } else if (Array.isArray(mx)) {
                mx.forEach((vol, t) => {
                    if (t < 16 && vol !== undefined) send({ cmd: 'setTrackVolume', track: t, volume: Math.round(vol) });
                });
            }
            console.log('[bridge-state] mixer: sent ' + (cmds - mixCmdsBefore) + ' mixer cmds');
        } else {
            console.log('[bridge-state] NO mixer in state');
        }

        // Mutes: array [bool,...] or object {kick:true,...}
        if (state.muted) {
            if (Array.isArray(state.muted)) {
                state.muted.forEach((m, t) => {
                    if (t < 16) send({ cmd: 'mute', track: t, value: !!m });
                });
            } else if (typeof state.muted === 'object') {
                for (const [inst, m] of Object.entries(state.muted)) {
                    const t = instrumentToTrack(inst);
                    if (t >= 0) send({ cmd: 'mute', track: t, value: !!m });
                }
            }
        }

        // Effects
        if (state.effects && typeof state.effects === 'object') {
            let fxCmdsBefore = cmds;
            const fx = state.effects;
            console.log('[bridge-state] effects keys=' + Object.keys(fx).join(','));
            if (fx.compressor) {
                if (fx.compressor.enabled !== undefined) send({ cmd: 'setCompressorActive', active: !!fx.compressor.enabled });
                if (fx.compressor.threshold !== undefined) send({ cmd: 'setCompressorThreshold', value: fx.compressor.threshold });
                if (fx.compressor.ratio !== undefined) send({ cmd: 'setCompressorRatio', value: fx.compressor.ratio });
                if (fx.compressor.attack !== undefined) send({ cmd: 'setCompressorAttack', value: fx.compressor.attack });
                if (fx.compressor.release !== undefined) send({ cmd: 'setCompressorRelease', value: fx.compressor.release });
            }
            if (fx.delay) {
                if (fx.delay.enabled !== undefined) send({ cmd: 'setDelayActive', active: !!fx.delay.enabled });
                if (fx.delay.time !== undefined) send({ cmd: 'setDelayTime', value: fx.delay.time });
                if (fx.delay.feedback !== undefined) send({ cmd: 'setDelayFeedback', value: fx.delay.feedback });
                if (fx.delay.mix !== undefined) send({ cmd: 'setDelayMix', value: fx.delay.mix });
            }
            if (fx.distortion) {
                if (fx.distortion.amount !== undefined) send({ cmd: 'setDistortion', value: fx.distortion.amount });
                if (fx.distortion.drive !== undefined) send({ cmd: 'setDistortion', value: fx.distortion.drive / 100 });
            }
            if (fx.filter) {
                if (fx.filter.frequency !== undefined) send({ cmd: 'setFilterCutoff', value: fx.filter.frequency });
                if (fx.filter.resonance !== undefined) send({ cmd: 'setFilterResonance', value: fx.filter.resonance });
            }
            if (fx.bitcrusher) {
                if (fx.bitcrusher.bits !== undefined) send({ cmd: 'setBitCrush', value: fx.bitcrusher.bits });
                if (fx.bitcrusher.sampleRate !== undefined) send({ cmd: 'setSampleRate', value: fx.bitcrusher.sampleRate });
            }
            if (fx.phaser) {
                if (fx.phaser.enabled !== undefined) send({ cmd: 'setPhaserActive', active: !!fx.phaser.enabled });
                if (fx.phaser.rate !== undefined) send({ cmd: 'setPhaserRate', value: fx.phaser.rate });
                if (fx.phaser.depth !== undefined) send({ cmd: 'setPhaserDepth', value: fx.phaser.depth });
            }
            if (fx.flanger) {
                if (fx.flanger.enabled !== undefined) send({ cmd: 'setFlangerActive', active: !!fx.flanger.enabled });
                if (fx.flanger.rate !== undefined) send({ cmd: 'setFlangerRate', value: fx.flanger.rate });
                if (fx.flanger.depth !== undefined) send({ cmd: 'setFlangerDepth', value: fx.flanger.depth });
            }
            console.log('[bridge-state] effects: sent ' + (cmds - fxCmdsBefore) + ' fx cmds');
        } else {
            console.log('[bridge-state] NO effects in state');
        }

        // Refresh ESP32 UI after applying
        if (cmds > 0) {
            console.log('%c[bridge-state] DONE: queued ' + cmds + ' commands to ESP32', 'color:#0f0;font-weight:bold',
                'total bridgedCmds=' + (bridgedCmds + cmds),
                'first5=' + firstCmds.join(' | '));
            bridgedCmds += cmds;
            wsQueueStart(); // flush the queue — getPattern fires once when done
        } else {
            console.warn('[bridge-state] DONE: 0 commands queued! State may not have had bridgeable data.');
        }
    }

    // ════════════════════════════════════════════════════════════
    // UI: Messages, Tool Cards, Pattern Preview
    // ════════════════════════════════════════════════════════════
    function appendMessage(role, content, id) {
        const container = $('chatMessages');
        if (!container) return;
        const msg = document.createElement('div');
        msg.className = 'chat-msg ' + role;
        if (id) msg.id = id;
        const avatar = role === 'user' ? '👤' : '🤖';
        msg.innerHTML =
            '<div class="chat-msg-avatar">' + avatar + '</div>' +
            '<div class="chat-msg-content">' + (content ? formatMarkdown(escapeHtml(content)) : '') + '</div>';
        container.appendChild(msg);
        scrollToBottom();
    }

    function updateAssistantMessage(id, text) {
        const msg = document.getElementById(id);
        if (!msg) return;
        const el = msg.querySelector('.chat-msg-content');
        if (!el) return;
        el.innerHTML = formatMarkdown(escapeHtml(text));
        scrollToBottom();
    }

    function showTypingInMessage(id) {
        const msg = document.getElementById(id);
        if (!msg) return;
        const el = msg.querySelector('.chat-msg-content');
        if (!el) return;
        el.innerHTML = '<div class="chat-typing"><div class="chat-typing-dot"></div><div class="chat-typing-dot"></div><div class="chat-typing-dot"></div></div>';
    }

    function removeTypingIndicator(id) {
        const msg = document.getElementById(id);
        if (!msg) return;
        const typing = msg.querySelector('.chat-typing');
        if (typing) typing.remove();
    }

    function appendSystemMessage(text) {
        const container = $('chatMessages');
        if (!container) return;
        const div = document.createElement('div');
        div.className = 'chat-system-msg';
        div.textContent = text;
        container.appendChild(div);
        scrollToBottom();
    }

    function appendRoundIndicator(round, maxRounds) {
        const container = $('chatMessages');
        if (!container) return;
        const div = document.createElement('div');
        div.className = 'chat-round-indicator';
        div.innerHTML = '<span class="round-line"></span><span class="round-label">Ronda ' + round + '/' + maxRounds + '</span><span class="round-line"></span>';
        container.appendChild(div);
        scrollToBottom();
    }

    // ── Tool Call Card (enhanced) ──
    function appendToolCall(name, args) {
        const container = $('chatMessages');
        if (!container) return;

        const div = document.createElement('div');
        div.className = 'chat-tool-card';
        div.id = 'tc-' + Date.now() + '-' + Math.random().toString(36).slice(2, 5);

        const icon = TOOL_ICONS[name] || '🔧';
        let argsHtml = '';
        let argsParsed = {};
        if (args) {
            try {
                argsParsed = typeof args === 'string' ? JSON.parse(args) : args;
                argsHtml = Object.entries(argsParsed)
                    .map(function(entry) {
                        var k = entry[0], v = entry[1];
                        var val = Array.isArray(v) ? '[' + v.length + ' steps]' : JSON.stringify(v);
                        return '<span class="tc-key">' + escapeHtml(k) + '</span>=<span class="tc-val">' + escapeHtml(String(val).slice(0, 60)) + '</span>';
                    }).join(' <span class="tc-sep">&middot;</span> ');
            } catch (e) {
                argsHtml = '<span class="tc-val">' + escapeHtml(typeof args === 'string' ? args : JSON.stringify(args)).slice(0, 120) + '</span>';
            }
        }

        // Instrument badge
        var instBadge = '';
        if (argsParsed.instrument) {
            var t = instrumentToTrack(argsParsed.instrument);
            if (t >= 0) {
                instBadge = '<span class="tc-track-badge">' + trackToName(t) + '</span>';
            }
        }

        // Genre badge
        var genreBadge = '';
        if (argsParsed.genre) {
            genreBadge = '<span class="tc-genre-badge">' + escapeHtml(argsParsed.genre) + '</span>';
        }

        // BPM badge
        var bpmBadge = '';
        if (argsParsed.bpm) {
            bpmBadge = '<span class="tc-bpm-badge">' + argsParsed.bpm + ' BPM</span>';
        }

        // Mini pattern preview
        var miniPattern = '';
        if (name === 'set_pattern' || name === 'set_pattern_with_velocity') {
            miniPattern = buildPatternPreviewHtml(argsParsed);
        }

        div.innerHTML =
            '<div class="tc-header">' +
                '<span class="tc-icon">' + icon + '</span>' +
                '<span class="tc-name">' + escapeHtml(name) + '</span>' +
                instBadge + genreBadge + bpmBadge +
                '<span class="tc-status"></span>' +
            '</div>' +
            '<div class="tc-args">' + argsHtml + '</div>' +
            miniPattern;

        container.appendChild(div);
        scrollToBottom();
    }

    function markToolCallApplied(cmdCount) {
        var cards = document.querySelectorAll('.chat-tool-card');
        if (cards.length === 0) return;
        var last = cards[cards.length - 1];
        var statusEl = last.querySelector('.tc-status');
        if (statusEl) {
            statusEl.innerHTML = '<span class="tc-applied">✓ ESP32 (' + cmdCount + ' cmds)</span>';
        }
        last.classList.add('applied');
    }

    // ── Tool Result ──
    function appendToolResult(name, result, state) {
        const container = $('chatMessages');
        if (!container) return;

        const div = document.createElement('div');
        const isError = result && (result.error || result.success === false);
        div.className = 'chat-tool-result ' + (isError ? 'error' : 'success');

        var summary = '';
        if (typeof result === 'string') {
            summary = result;
        } else if (result) {
            if (result.message) summary = result.message;
            else if (result.success !== undefined) summary = result.success ? 'OK' : 'Error: ' + (result.error || '');
            else summary = JSON.stringify(result).slice(0, 150);
        }

        var rIcon = isError ? '❌' : '✅';
        div.innerHTML = '<span class="tr-icon">' + rIcon + '</span> ' +
            '<span class="tr-name">' + escapeHtml(name) + '</span>: ' +
            '<span class="tr-summary">' + escapeHtml(summary).slice(0, 150) + '</span>';

        // Pattern preview from state or result itself
        var patternData = (state && state.pattern) || (result && result.pattern);
        if (patternData) {
            var preview = buildPatternPreviewFromState(patternData);
            if (preview) div.appendChild(preview);
        }

        container.appendChild(div);
        scrollToBottom();
    }

    function appendBridgeStatus(totalCmds) {
        const container = $('chatMessages');
        if (!container) return;
        const div = document.createElement('div');
        div.className = 'chat-bridge-status';
        div.innerHTML = '<span class="bridge-icon">📡</span> <strong>' + totalCmds + '</strong> comandos WS enviados al ESP32';
        container.appendChild(div);
        scrollToBottom();
    }

    // ── Pattern Preview Builders ──
    function buildPatternPreviewHtml(patternArgs) {
        var tracks = [];
        for (var key in patternArgs) {
            if (!patternArgs.hasOwnProperty(key)) continue;
            var t = instrumentToTrack(key);
            var steps = patternArgs[key];
            if (t >= 0 && Array.isArray(steps)) {
                tracks.push({ name: trackToName(t), steps: steps });
            }
        }
        if (tracks.length === 0) return '';

        var html = '<div class="tc-pattern-preview">';
        tracks.forEach(function(tr) {
            html += '<div class="tc-pattern-row"><span class="tc-pattern-label">' + tr.name + '</span>';
            for (var s = 0; s < Math.min(tr.steps.length, 16); s++) {
                var on = tr.steps[s] > 0;
                html += '<div class="sp-cell' + (on ? ' on' : '') + '"></div>';
            }
            html += '</div>';
        });
        html += '</div>';
        return html;
    }

    function buildPatternPreviewFromState(pattern) {
        var grid = document.createElement('div');
        grid.className = 'tc-pattern-preview';
        var entries = [];

        if (Array.isArray(pattern)) {
            pattern.forEach(function(steps, idx) {
                if (Array.isArray(steps) && idx < 16) entries.push({ name: TRACK_NAMES[idx], steps: steps });
            });
        } else if (typeof pattern === 'object') {
            for (var inst in pattern) {
                if (!pattern.hasOwnProperty(inst)) continue;
                var t = instrumentToTrack(inst);
                var steps = pattern[inst];
                if (t >= 0 && Array.isArray(steps)) entries.push({ name: trackToName(t), steps: steps });
            }
        }

        // Show only active tracks, max 8
        entries = entries.filter(function(e) { return e.steps.some(function(v) { return v > 0; }); }).slice(0, 8);
        if (entries.length === 0) return null;

        entries.forEach(function(tr) {
            var row = document.createElement('div');
            row.className = 'tc-pattern-row';
            row.innerHTML = '<span class="tc-pattern-label">' + tr.name + '</span>';
            for (var s = 0; s < Math.min(tr.steps.length, 16); s++) {
                var cell = document.createElement('div');
                cell.className = tr.steps[s] > 0 ? 'sp-cell on' : 'sp-cell';
                row.appendChild(cell);
            }
            grid.appendChild(row);
        });
        return grid;
    }

    // ---- Progress ----
    function showProgress(show, text, pct) {
        const el = $('chatProgress');
        const fill = $('chatProgressFill');
        const label = $('chatProgressText');
        if (!el) return;
        el.style.display = show ? 'flex' : 'none';
        if (fill && pct !== undefined) fill.style.width = pct + '%';
        if (label && text) label.textContent = text;
    }

    // ---- Markdown ----
    function formatMarkdown(html) {
        html = html.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>');
        html = html.replace(/\*(.+?)\*/g, '<em>$1</em>');
        html = html.replace(/`([^`]+)`/g, '<code>$1</code>');
        html = html.replace(/\n/g, '<br>');
        return html;
    }

    // ---- Clear / Reset ----
    window.chatClearHistory = function () {
        const container = $('chatMessages');
        if (container) {
            container.innerHTML =
                '<div class="chat-welcome">' +
                    '<div class="chat-welcome-icon">🥁</div>' +
                    '<h3>RED808 AI Agent</h3>' +
                    '<p>Habla en lenguaje natural para crear y editar patrones de batería.</p>' +
                    '<p class="chat-welcome-sub">63 tools &middot; 23 instrumentos &middot; patrón, FX, mixer, song</p>' +
                    '<div class="chat-suggestions">' +
                        '<button class="chat-suggestion" onclick="chatSendSuggestion(this)">🎵 Hazme un beat de trap a 140 BPM</button>' +
                        '<button class="chat-suggestion" onclick="chatSendSuggestion(this)">🏠 Pon un patrón house minimalista</button>' +
                        '<button class="chat-suggestion" onclick="chatSendSuggestion(this)">👻 Añade ghost notes al hi-hat</button>' +
                        '<button class="chat-suggestion" onclick="chatSendSuggestion(this)">🔇 Quita el snare y sube el BPM a 160</button>' +
                        '<button class="chat-suggestion" onclick="chatSendSuggestion(this)">🤲 Humaniza el patrón actual</button>' +
                        '<button class="chat-suggestion" onclick="chatSendSuggestion(this)">🥁 Dame un breakbeat estilo Amen</button>' +
                    '</div>' +
                '</div>';
        }
        chatHistory = [];
    };

    window.chatResetSession = function () {
        if (chatStreaming && chatAbortController) chatAbortController.abort();
        if (chatConnected && chatServerBase) {
            fetch(chatServerBase + '/api/chat/reset', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ sessionId: chatSessionId })
            }).catch(function() {});
        }
        chatSessionId = generateSessionId();
        localStorage.setItem('chatSessionId', chatSessionId);
        window.chatClearHistory();
        if (window.showToast) window.showToast('🔄 Sesión reseteada', window.TOAST_TYPES?.INFO, 1500);
    };

    // ════════════════════════════════════════════════════════════
    // SEQUENCER INLINE CHAT PANEL
    // Mirrors all chat events to the compact feed inside the
    // sequencer tab so you can watch changes in real-time.
    // ════════════════════════════════════════════════════════════
    let seqChatExpanded = false;
    let seqMsgCount = 0;

    function initSeqChatPanel() {
        const toggle = $('seqChatToggle');
        if (toggle) {
            toggle.addEventListener('click', function () {
                seqChatExpanded = !seqChatExpanded;
                var panel = $('seqChatPanel');
                if (panel) panel.classList.toggle('expanded', seqChatExpanded);
                if (seqChatExpanded) {
                    seqMsgCount = 0;
                    var badge = $('seqChatBadge');
                    if (badge) badge.style.display = 'none';
                    seqScrollToBottom();
                }
            });
        }
        var input = $('seqChatInput');
        if (input) {
            input.addEventListener('input', function () {
                var btn = $('seqChatSendBtn');
                if (btn) btn.disabled = !input.value.trim();
            });
        }
        // Update status dot when connection state changes
        updateSeqChatStatus();
    }

    function updateSeqChatStatus() {
        var panel = $('seqChatPanel');
        var dot = $('seqChatStatus');
        if (panel) panel.classList.toggle('active', chatConnected);
        if (dot) dot.style.color = chatConnected ? '#4caf50' : '#555';
    }

    function seqScrollToBottom() {
        var feed = $('seqChatFeed');
        if (feed) requestAnimationFrame(function () { feed.scrollTop = feed.scrollHeight; });
    }

    function seqIncrementBadge() {
        if (seqChatExpanded) return;
        seqMsgCount++;
        var badge = $('seqChatBadge');
        if (badge) {
            badge.textContent = seqMsgCount;
            badge.style.display = 'inline-flex';
        }
    }

    // ── Seq Feed items ──
    function seqAppendUser(text) {
        var feed = $('seqChatFeed');
        if (!feed) return;
        clearSeqHint();
        var div = document.createElement('div');
        div.className = 'scf-user';
        div.textContent = text;
        feed.appendChild(div);
        seqScrollToBottom();
    }

    function seqAppendAssistant(text, id) {
        var feed = $('seqChatFeed');
        if (!feed) return;
        var div = document.createElement('div');
        div.className = 'scf-assistant';
        if (id) div.id = 'scf-' + id;
        div.innerHTML = text ? formatMarkdown(escapeHtml(text)) : '';
        feed.appendChild(div);
        seqScrollToBottom();
        seqIncrementBadge();
    }

    function seqUpdateAssistant(id, text) {
        var el = document.getElementById('scf-' + id);
        if (!el) return;
        el.innerHTML = formatMarkdown(escapeHtml(text));
        seqScrollToBottom();
    }

    function seqShowTyping(id) {
        var feed = $('seqChatFeed');
        if (!feed) return;
        var div = document.createElement('div');
        div.className = 'scf-typing';
        div.id = 'scf-typing-' + id;
        div.innerHTML = '<div class="scf-typing-dot"></div><div class="scf-typing-dot"></div><div class="scf-typing-dot"></div>';
        feed.appendChild(div);
        seqScrollToBottom();
    }

    function seqRemoveTyping(id) {
        var el = document.getElementById('scf-typing-' + id);
        if (el) el.remove();
    }

    function seqAppendTool(name, args, applied, cmdCount) {
        var feed = $('seqChatFeed');
        if (!feed) return;
        var div = document.createElement('div');
        div.className = 'scf-tool' + (applied ? ' applied' : '');

        var icon = TOOL_ICONS[name] || '🔧';
        var argsParsed = {};
        try { argsParsed = typeof args === 'string' ? JSON.parse(args) : (args || {}); } catch(e) {}

        var badge = '';
        if (argsParsed.instrument) {
            var t = instrumentToTrack(argsParsed.instrument);
            if (t >= 0) badge = '<span class="scf-tool-badge">' + trackToName(t) + '</span>';
        }
        if (argsParsed.genre) badge += '<span class="scf-tool-badge" style="color:#bb88ff;border-color:rgba(150,80,255,0.3);background:rgba(150,80,255,0.1)">' + escapeHtml(argsParsed.genre) + '</span>';

        var appliedHtml = applied ? '<span class="scf-tool-applied">✓ ESP32</span>' : '';

        div.innerHTML = icon + ' <span class="scf-tool-name">' + escapeHtml(name) + '</span> ' + badge + appliedHtml;
        feed.appendChild(div);
        seqScrollToBottom();
        seqIncrementBadge();
    }

    function seqMarkLastToolApplied(cmdCount) {
        var tools = document.querySelectorAll('.scf-tool:not(.applied)');
        if (tools.length === 0) return;
        var last = tools[tools.length - 1];
        last.classList.add('applied');
        var existing = last.querySelector('.scf-tool-applied');
        if (!existing) {
            var span = document.createElement('span');
            span.className = 'scf-tool-applied';
            span.textContent = '✓ ESP32';
            last.appendChild(span);
        }
    }

    function seqAppendResult(name, result) {
        var feed = $('seqChatFeed');
        if (!feed) return;
        var isError = result && (result.error || result.success === false);
        var div = document.createElement('div');
        div.className = 'scf-result' + (isError ? ' error' : '');
        var summary = '';
        if (typeof result === 'string') summary = result;
        else if (result) {
            if (result.message) summary = result.message;
            else if (result.success !== undefined) summary = result.success ? 'OK' : (result.error || 'Error');
            else summary = JSON.stringify(result).slice(0, 80);
        }
        div.textContent = (isError ? '❌ ' : '✅ ') + name + ': ' + summary.slice(0, 80);
        feed.appendChild(div);
        seqScrollToBottom();
    }

    function seqAppendRound(round, max) {
        var feed = $('seqChatFeed');
        if (!feed) return;
        var div = document.createElement('div');
        div.className = 'scf-round';
        div.innerHTML = '<span class="scf-round-line"></span><span class="scf-round-label">R' + round + '/' + max + '</span><span class="scf-round-line"></span>';
        feed.appendChild(div);
        seqScrollToBottom();
    }

    function seqAppendBridge(msgOrCmds) {
        var feed = $('seqChatFeed');
        if (!feed) return;
        var div = document.createElement('div');
        div.className = 'scf-bridge';
        if (typeof msgOrCmds === 'number') {
            div.innerHTML = '📡 <strong>' + msgOrCmds + '</strong> cmds → ESP32';
        } else {
            div.innerHTML = msgOrCmds;
        }
        feed.appendChild(div);
        seqScrollToBottom();
    }

    function clearSeqHint() {
        var hint = document.querySelector('.seq-chat-hint');
        if (hint) hint.remove();
    }

    // ── Seq input handlers (global) ──
    window.seqSuggestion = function (btnEl) {
        var input = $('seqChatInput');
        if (!input || !btnEl) return;
        // Strip leading emoji
        input.value = btnEl.textContent.replace(/^[^\s]+\s/, '');
        input.dispatchEvent(new Event('input'));
        window.seqChatSend();
    };

    window.seqChatKeydown = function (e) {
        if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); window.seqChatSend(); }
    };
    window.seqChatAutoResize = function (el) {
        el.style.height = 'auto';
        el.style.height = Math.min(el.scrollHeight, 80) + 'px';
    };
    window.seqChatSend = function () {
        var input = $('seqChatInput');
        if (!input) return;
        var message = input.value.trim();
        if (!message) return;
        input.value = '';
        input.style.height = 'auto';
        var sendBtn = $('seqChatSendBtn');
        if (sendBtn) sendBtn.disabled = true;

        // Auto-expand panel
        if (!seqChatExpanded) {
            seqChatExpanded = true;
            var panel = $('seqChatPanel');
            if (panel) panel.classList.add('expanded');
        }

        // Helper to inject message into main chatSend
        function doSend() {
            var mainInput = $('chatInput');
            if (mainInput) {
                mainInput.value = message;
                mainInput.dispatchEvent(new Event('input'));
            }
            window.chatSend();
        }

        // Auto-connect if needed, then send after connection resolves
        if (!chatConnected) {
            if (!chatServerBase) {
                var urlEl = $('chatServerUrl');
                if (urlEl && urlEl.value) {
                    chatServerBase = urlEl.value.trim().replace(/\/+$/, '');
                    localStorage.setItem('chatServerUrl', chatServerBase);
                } else {
                    var saved = localStorage.getItem('chatServerUrl');
                    if (saved) chatServerBase = saved;
                }
            }
            if (chatServerBase) {
                // Connect and wait before sending
                var origConnect = window.chatConnect;
                window.chatConnect = function () {
                    window.chatConnect = origConnect;
                    origConnect();
                };
                origConnect();
                // Poll for connection then send (max 6s)
                var attempts = 0;
                var connectWait = setInterval(function () {
                    attempts++;
                    if (chatConnected || attempts > 30) {
                        clearInterval(connectWait);
                        doSend();
                    }
                }, 200);
            } else {
                doSend(); // Will show "not connected" message
            }
        } else {
            doSend();
        }
    };

    // ════════════════════════════════════════════════════════════
    // HOOKS: Patch all main UI functions to mirror to seq feed
    // ════════════════════════════════════════════════════════════
    var _origAppendMessage = appendMessage;
    appendMessage = function (role, content, id) {
        _origAppendMessage(role, content, id);
        if (role === 'user') seqAppendUser(content);
        else if (role === 'assistant') {
            seqAppendAssistant(content, id);
        }
    };

    var _origUpdateAssistantMessage = updateAssistantMessage;
    updateAssistantMessage = function (id, text) {
        _origUpdateAssistantMessage(id, text);
        seqUpdateAssistant(id, text);
    };

    var _origShowTypingInMessage = showTypingInMessage;
    showTypingInMessage = function (id) {
        _origShowTypingInMessage(id);
        seqShowTyping(id);
    };

    var _origRemoveTypingIndicator = removeTypingIndicator;
    removeTypingIndicator = function (id) {
        _origRemoveTypingIndicator(id);
        seqRemoveTyping(id);
    };

    var _origAppendToolCall = appendToolCall;
    appendToolCall = function (name, args) {
        _origAppendToolCall(name, args);
        seqAppendTool(name, args, false);
    };

    var _origMarkToolCallApplied = markToolCallApplied;
    markToolCallApplied = function (cmdCount) {
        _origMarkToolCallApplied(cmdCount);
        seqMarkLastToolApplied(cmdCount);
    };

    var _origAppendToolResult = appendToolResult;
    appendToolResult = function (name, result, state) {
        _origAppendToolResult(name, result, state);
        seqAppendResult(name, result);
    };

    var _origAppendRoundIndicator = appendRoundIndicator;
    appendRoundIndicator = function (round, max) {
        _origAppendRoundIndicator(round, max);
        seqAppendRound(round, max);
    };

    var _origAppendBridgeStatus = appendBridgeStatus;
    appendBridgeStatus = function (totalCmds) {
        _origAppendBridgeStatus(totalCmds);
        seqAppendBridge(totalCmds);
    };

    // Also patch chatConnect to update seq status
    var _origChatConnect = window.chatConnect;
    window.chatConnect = function () {
        _origChatConnect();
        setTimeout(updateSeqChatStatus, 2000);
    };

    // ---- Bootstrap ----
    console.log('[chat-agent] readyState=' + document.readyState);
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', function () {
            console.log('[chat-agent] DOMContentLoaded fired');
            initChatAgent();
            initSeqChatPanel();
        });
    } else {
        console.log('[chat-agent] DOM already ready, init now');
        initChatAgent();
        initSeqChatPanel();
    }
})();
