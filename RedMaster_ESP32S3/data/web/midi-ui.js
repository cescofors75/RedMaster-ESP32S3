// ============================================
// MIDI FUNCTIONS
// ============================================

// ============================================
// MIDI DASHBOARD - Professional Version
// ============================================

let midiTotalNotes = 0;
let midiCCMessages = 0;
let midiVelocitySum = 0;
let midiVelocityCount = 0;
let midiConnectTimestamp = null;
let midiUptimeInterval = null;
const midiMessagesQueue = [];
const MAX_MIDI_MESSAGES_DISPLAY = 50;

function handleMIDIDeviceMessage(data) {
    const badge = document.getElementById('midiConnectionBadge');
    const deviceCard = document.getElementById('midiDeviceCard');
    
    if (data.connected) {
        // Device connected
        badge.classList.add('connected');
        badge.querySelector('.badge-text').textContent = 'Conectado';
        
        deviceCard.classList.add('connected');
        document.getElementById('midiDeviceName').textContent = data.deviceName || 'USB MIDI Device';
        document.getElementById('midiVendorId').textContent = data.vendorId ? `0x${data.vendorId.toString(16).toUpperCase()}` : '—';
        document.getElementById('midiProductId').textContent = data.productId ? `0x${data.productId.toString(16).toUpperCase()}` : '—';
        
        // Start uptime counter
        midiConnectTimestamp = Date.now();
        if (midiUptimeInterval) clearInterval(midiUptimeInterval);
        midiUptimeInterval = setInterval(updateMidiUptime, 1000);
    } else {
        // Device disconnected
        badge.classList.remove('connected');
        badge.querySelector('.badge-text').textContent = 'Desconectado';
        
        deviceCard.classList.remove('connected');
        document.getElementById('midiDeviceName').textContent = 'Esperando conexión...';
        document.getElementById('midiVendorId').textContent = '—';
        document.getElementById('midiProductId').textContent = '—';
        
        // Stop uptime counter
        if (midiUptimeInterval) {
            clearInterval(midiUptimeInterval);
            midiUptimeInterval = null;
        }
        document.getElementById('midiUptime').textContent = '00:00';
    }
}

function handleMidiScanState(data) {
    const toggle = document.getElementById('midiScanToggle');
    if (toggle) toggle.checked = !!data.enabled;
}

function toggleMidiScan(enabled) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ cmd: 'setMidiScan', enabled: enabled }));
    }
}

function updateMidiUptime() {
    if (!midiConnectTimestamp) return;
    
    const elapsed = Math.floor((Date.now() - midiConnectTimestamp) / 1000);
    const minutes = Math.floor(elapsed / 60);
    const seconds = elapsed % 60;
    document.getElementById('midiUptime').textContent = 
        `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
}

function handleMIDIMessage(data) {
    // Update stats
    if (data.messageType === 'noteOn') {
        midiTotalNotes++;
        midiVelocitySum += data.data2 || 0;
        midiVelocityCount++;
        
        document.getElementById('midiTotalNotes').textContent = midiTotalNotes;
        const avgVel = Math.round(midiVelocitySum / midiVelocityCount);
        document.getElementById('midiAvgVelocity').textContent = avgVel;
        
        // Animate velocity bar for the note
        animateNoteVelocity(data.data1, data.data2);
        
        // Highlight mapping item
        highlightMappingItem(data.data1);
    } else if (data.messageType === 'cc') {
        midiCCMessages++;
        document.getElementById('midiCCMessages').textContent = midiCCMessages;
    }
    
    // Add to message queue
    const messageEntry = {
        ...data,
        timestamp: Date.now()
    };
    
    midiMessagesQueue.unshift(messageEntry);
    if (midiMessagesQueue.length > MAX_MIDI_MESSAGES_DISPLAY) {
        midiMessagesQueue.pop();
    }
    
    // Update monitor display
    updateMIDIMonitorDisplay();
}

function animateNoteVelocity(note, velocity) {
    const item = document.querySelector(`.mapping-item[data-note="${note}"]`);
    if (!item) return;
    const velocityFill = item.querySelector('.velocity-fill');
    if (!velocityFill) return;
    const percent = Math.round((velocity / 127) * 100);
    velocityFill.style.width = `${percent}%`;
    setTimeout(() => { velocityFill.style.width = '0%'; }, 500);
}

function highlightMappingItem(note) {
    const item = document.querySelector(`.mapping-item[data-note="${note}"]`);
    if (!item) return;
    item.classList.add('active');
    setTimeout(() => { item.classList.remove('active'); }, 300);
}

function updateMIDIMonitorDisplay() {
    const monitor = document.getElementById('midiMonitor');
    if (!monitor) return;
    
    // Remove placeholder if exists (only once)
    const placeholder = monitor.querySelector('.monitor-placeholder');
    if (placeholder) {
        placeholder.remove();
    }
    
    // OPTIMIZACIÓN: Solo agregar el mensaje más reciente en lugar de re-renderizar todo
    // Esto evita el parpadeo y duplicación
    if (midiMessagesQueue.length > 0) {
        const latestMsg = midiMessagesQueue[0];
        
        // Aplicar filtro
        if (midiMonitorFilter !== 'all' && latestMsg.messageType !== midiMonitorFilter) return;
        
        const entry = createMIDIMessageEntry(latestMsg);
        
        // Insertar al inicio (más nuevo arriba)
        monitor.insertBefore(entry, monitor.firstChild);
        
        // Limitar el número de mensajes visibles (eliminar los más antiguos)
        while (monitor.children.length > MAX_MIDI_MESSAGES_DISPLAY) {
            monitor.removeChild(monitor.lastChild);
        }
    }
}

function createMIDIMessageEntry(msg) {
    const entry = document.createElement('div');
    entry.className = `midi-message-entry ${getMIDIMessageClass(msg.messageType)}`;
    
    // Header
    const header = document.createElement('div');
    header.className = 'message-header';
    
    const type = document.createElement('div');
    type.className = `message-type ${getMIDIMessageClass(msg.messageType)}`;
    type.innerHTML = `
        <span class="message-type-icon">${getMIDIIcon(msg.messageType)}</span>
        <span>${getMIDITypeName(msg.messageType)}</span>
    `;
    
    const time = document.createElement('div');
    time.className = 'message-time';
    const elapsed = Date.now() - msg.timestamp;
    time.textContent = elapsed < 1000 ? 'ahora' : `${Math.floor(elapsed / 1000)}s ago`;
    
    header.appendChild(type);
    header.appendChild(time);
    
    // Details
    const details = document.createElement('div');
    details.className = 'message-details';
    details.innerHTML = getMIDIDetailsHTML(msg);
    
    entry.appendChild(header);
    entry.appendChild(details);
    
    return entry;
}

function getMIDIMessageClass(type) {
    const classes = {
        'noteOn': 'note-on',
        'noteOff': 'note-off',
        'cc': 'cc',
        'pitchBend': 'pitchbend',
        'program': 'program'
    };
    return classes[type] || 'other';
}

function getMIDIIcon(type) {
    const icons = {
        'noteOn': '🎹',
        'noteOff': '⬜',
        'cc': '🎛️',
        'pitchBend': '🎚️',
        'program': '📋',
        'aftertouch': '👆'
    };
    return icons[type] || '📨';
}

function getMIDITypeName(type) {
    const names = {
        'noteOn': 'Note On',
        'noteOff': 'Note Off',
        'cc': 'Control Change',
        'pitchBend': 'Pitch Bend',
        'program': 'Program Change',
        'aftertouch': 'Aftertouch'
    };
    return names[type] || type;
}

function getMIDIDetailsHTML(msg) {
    let html = `<span><span class="label">Canal:</span> <span class="value">${msg.channel}</span></span>`;
    
    if (msg.messageType === 'noteOn' || msg.messageType === 'noteOff') {
        const noteName = getNoteNameFromNumber(msg.data1);
        html += `<span><span class="label">Nota:</span> <span class="value">${msg.data1} (${noteName})</span></span>`;
        html += `<span><span class="label">Velocity:</span> <span class="value">${msg.data2}</span></span>`;
    } else if (msg.messageType === 'cc') {
        html += `<span><span class="label">CC:</span> <span class="value">${msg.data1}</span></span>`;
        html += `<span><span class="label">Valor:</span> <span class="value">${msg.data2}</span></span>`;
    } else if (msg.messageType === 'pitchBend') {
        const bendValue = (msg.data1 | (msg.data2 << 7)) - 8192;
        html += `<span><span class="label">Bend:</span> <span class="value">${bendValue}</span></span>`;
    } else {
        html += `<span><span class="label">Data1:</span> <span class="value">${msg.data1}</span></span>`;
        if (msg.data2 !== undefined) {
            html += `<span><span class="label">Data2:</span> <span class="value">${msg.data2}</span></span>`;
        }
    }
    
    return html;
}

function getNoteNameFromNumber(noteNumber) {
    const notes = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    const octave = Math.floor(noteNumber / 12) - 1;
    const noteName = notes[noteNumber % 12];
    return `${noteName}${octave}`;
}

function formatMIDIData(msg) {
    switch(msg.messageType) {
        case 'noteOn':
        case 'noteOff':
            return `Ch${msg.channel} Note:${msg.data1} Vel:${msg.data2}`;
        case 'cc':
            return `Ch${msg.channel} CC:${msg.data1} Val:${msg.data2}`;
        case 'program':
            return `Ch${msg.channel} Program:${msg.data1}`;
        case 'pitchBend':
            const bend = (msg.data2 << 7) | msg.data1;
            return `Ch${msg.channel} Bend:${bend}`;
        default:
            return `Ch${msg.channel} D1:${msg.data1} D2:${msg.data2}`;
    }
}

// Update messages per second periodically
setInterval(() => {
    // This would need backend support to send real-time stats
    // For now we can estimate based on message timestamps
}, 1000);

// ============================================
// SAMPLE UPLOAD FUNCTIONS
// ============================================

function showUploadDialog(padIndex) {
    // Crear input file oculto — abre el Sample Editor modal antes de subir
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.wav';
    input.style.display = 'none';

    input.addEventListener('change', (e) => {
        const file = e.target.files[0];
        if (!file) return;

        if (!file.name.toLowerCase().endsWith('.wav')) {
            if (window.showToast) {
                window.showToast('❌ Solo se permiten archivos WAV', window.TOAST_TYPES.ERROR, 3000);
            }
            return;
        }

        // Abrir el editor de sample (trim, fade, preview) — él se encarga del upload
        if (window.SampleEditor) {
            SampleEditor.open(padIndex, file);
        } else {
            // fallback directo si el script no cargó
            uploadSample(padIndex, file);
        }
    });

    document.body.appendChild(input);
    input.click();
    setTimeout(() => input.remove(), 1000);
}

let currentUploadPad = -1;

function uploadSample(padIndex, file) {
    currentUploadPad = padIndex;
    const padName = padNames[padIndex];
    
    // Mostrar toast de inicio
    if (window.showToast) {
        window.showToast(`📤 Subiendo ${file.name} a ${padName}...`, window.TOAST_TYPES.INFO, 2000);
    }
    
    // Deshabilitar botón de upload durante el proceso
    const btn = document.querySelector(`.pad-upload-btn[data-pad="${padIndex}"]`);
    if (btn) {
        btn.disabled = true;
        btn.textContent = '⏳';
    }
    
    // Crear FormData (sin el pad, irá en la URL)
    const formData = new FormData();
    formData.append('file', file);
    
    // Enviar via fetch con pad como query parameter
    fetch(`/api/upload?pad=${padIndex}`, {
        method: 'POST',
        body: formData
    })
    .then(response => response.json())
    .then(data => {
    })
    .catch(error => {
        console.error('[Upload] Error:', error);
        if (window.showToast) {
            window.showToast(`❌ Error al subir archivo: ${error.message}`, window.TOAST_TYPES.ERROR, 4000);
        }
        
        // Re-habilitar botón
        if (btn) {
            btn.disabled = false;
            btn.textContent = '📤';
        }
        currentUploadPad = -1;
    });
}

function handleUploadProgress(data) {
    if (data.pad !== currentUploadPad) return;
    
    const btn = document.querySelector(`.pad-upload-btn[data-pad="${data.pad}"]`);
    if (btn) {
        btn.textContent = `${data.percent}%`;
    }
}

function handleUploadComplete(data) {
    const btn = document.querySelector(`.pad-upload-btn[data-pad="${data.pad}"]`);
    if (btn) {
        btn.disabled = false;
        btn.textContent = '📤';
    }
    
    if (data.success) {
        if (window.showToast) {
            const padName = padNames[data.pad];
            window.showToast(`✅ ${padName}: ${data.message}`, window.TOAST_TYPES.SUCCESS, 3000);
        }
        
        // Actualizar info del pad
        refreshPadSampleInfo(data.pad);
        
        // Animación de éxito en el pad
        const pad = document.querySelector(`.pad[data-pad="${data.pad}"]`);
        if (pad) {
            pad.style.animation = 'padPulseSuccess 0.5s ease-out';
            setTimeout(() => {
                pad.style.animation = '';
            }, 500);
        }
    } else {
        if (window.showToast) {
            window.showToast(`❌ Error: ${data.message}`, window.TOAST_TYPES.ERROR, 4000);
        }
    }
    
    currentUploadPad = -1;
}

// ============================================
// MIDI MAPPING EDITOR
// ============================================

let isEditingMapping = false;
let originalMappings = {};
let midiMonitorFilter = 'all';

// Presets de mapeo MIDI para diferentes controladores
const MIDI_MAPPING_PRESETS = {
    gm: [
        {pad:0, note:36}, {pad:1, note:38}, {pad:2, note:42}, {pad:3, note:46},
        {pad:4, note:49}, {pad:5, note:39}, {pad:6, note:37}, {pad:7, note:56},
        {pad:8, note:41}, {pad:9, note:47}, {pad:10, note:50}, {pad:11, note:70},
        {pad:12, note:75}, {pad:13, note:62}, {pad:14, note:63}, {pad:15, note:64}
    ],
    roland: [
        // Roland TR-8S / TD pads
        {pad:0, note:36}, {pad:1, note:38}, {pad:2, note:42}, {pad:3, note:46},
        {pad:4, note:49}, {pad:5, note:39}, {pad:6, note:37}, {pad:7, note:56},
        {pad:8, note:43}, {pad:9, note:47}, {pad:10, note:48}, {pad:11, note:70},
        {pad:12, note:75}, {pad:13, note:62}, {pad:14, note:63}, {pad:15, note:64}
    ],
    mpc: [
        // Akai MPC default pad layout (A01-A16 = 60-75)
        {pad:0, note:60}, {pad:1, note:61}, {pad:2, note:62}, {pad:3, note:63},
        {pad:4, note:64}, {pad:5, note:65}, {pad:6, note:66}, {pad:7, note:67},
        {pad:8, note:68}, {pad:9, note:69}, {pad:10, note:70}, {pad:11, note:71},
        {pad:12, note:72}, {pad:13, note:73}, {pad:14, note:74}, {pad:15, note:75}
    ]
};

function setMidiMonitorFilter(filter) {
    midiMonitorFilter = filter;
}

async function loadMIDIMapping() {
    try {
        const response = await fetch('/api/midi/mapping');
        const data = await response.json();
        
        if (data.mappings) {
            // Solo actualizar los pads 0-15 (mapeos principales, no alias)
            const primaryMappings = data.mappings.filter(m => m.pad >= 0 && m.pad <= 15);
            // Crear mapa pad→note para búsqueda rápida
            const padNoteMap = {};
            // En caso de múltiples notas por pad, usar la primera
            primaryMappings.forEach(m => {
                if (padNoteMap[m.pad] === undefined) padNoteMap[m.pad] = m.note;
            });
            
            for (let pad = 0; pad <= 15; pad++) {
                const item = document.querySelector(`.mapping-item[data-pad="${pad}"]`);
                if (!item) continue;
                const note = padNoteMap[pad];
                if (note === undefined) continue;
                
                const input   = item.querySelector('.note-input');
                const valueEl = item.querySelector('.note-value');
                const nameEl  = item.querySelector('.note-name');
                try {
                    if (input)   { input.value = note; item.dataset.note = note; }
                    if (valueEl) valueEl.textContent = note;
                    if (nameEl)  nameEl.textContent  = getNoteNameFromNumber(note);
                } catch(ex) { /* elemento no visible aún */ }
            }
        }
    } catch (error) {
        console.error('[MIDI Mapping] Error loading:', error);
    }
}

// Bloquear atajos globales cuando el slider de mapping tiene el foco
function stopKeyPropForSlider(e) {
    e.stopPropagation();
}

function toggleMappingEdit() {
    isEditingMapping = !isEditingMapping;
    
    const editBtn   = document.getElementById('editMappingBtn');
    const resetBtn  = document.getElementById('resetMappingBtn');
    const saveBtn   = document.getElementById('saveMappingBtn');
    const cancelBtn = document.getElementById('cancelMappingBtn');
    const presets   = document.getElementById('mappingPresets');
    const mappingGrid = document.getElementById('mappingGrid');
    const inputs    = document.querySelectorAll('.note-input');
    
    if (isEditingMapping) {
        editBtn.style.display  = 'none';
        resetBtn.style.display = 'inline-block';
        saveBtn.style.display  = 'inline-block';
        cancelBtn.style.display = 'inline-block';
        if (presets) presets.style.display = 'flex';
        mappingGrid.classList.add('editing');
        
        // Guardar valores originales y habilitar sliders
        inputs.forEach(input => {
            const item = input.closest('.mapping-item');
            originalMappings[item.dataset.pad] = input.value;
            input.disabled = false;
            input.classList.add('editing');
            input.addEventListener('input', onNoteInputChange);
            // Evitar que el teclado global intercepte las flechas del slider
            input.addEventListener('keydown', stopKeyPropForSlider);
        });
        
        if (window.showToast) window.showToast('✏️ Modo edición — arrastra los sliders y pulsa Guardar', window.TOAST_TYPES?.INFO, 3000);
    } else {
        // Cancelar → restaurar
        inputs.forEach(input => {
            const item = input.closest('.mapping-item');
            input.value = originalMappings[item.dataset.pad] ?? input.value;
            item.dataset.note = input.value;
            input.disabled = true;
            input.classList.remove('editing');
            input.removeEventListener('input', onNoteInputChange);
            input.removeEventListener('keydown', stopKeyPropForSlider);
            const val = parseInt(input.value);
            const valueEl = item.querySelector('.note-value');
            const nameEl  = item.querySelector('.note-name');
            if (valueEl) valueEl.textContent = val;
            if (nameEl)  nameEl.textContent  = getNoteNameFromNumber(val);
        });
        editBtn.style.display   = 'inline-block';
        resetBtn.style.display  = 'none';
        saveBtn.style.display   = 'none';
        cancelBtn.style.display = 'none';
        if (presets) presets.style.display = 'none';
        mappingGrid.classList.remove('editing');
        originalMappings = {};
    }
}

function cancelMappingEdit() {
    if (isEditingMapping) toggleMappingEdit(); // restaura y sale del modo edición
}

function onNoteInputChange(e) {
    const input = e.target;
    const val = parseInt(input.value);
    const item = input.closest('.mapping-item');
    const valueEl = item.querySelector('.note-value');
    const nameEl  = item.querySelector('.note-name');
    if (valueEl) valueEl.textContent = val;
    if (nameEl)  nameEl.textContent  = getNoteNameFromNumber(val);
}

function applyMappingPreset(name) {
    const preset = MIDI_MAPPING_PRESETS[name];
    if (!preset) return;
    
    preset.forEach(({pad, note}) => {
        const item = document.querySelector(`.mapping-item[data-pad="${pad}"]`);
        if (!item) return;
        const input   = item.querySelector('.note-input');
        const valueEl = item.querySelector('.note-value');
        const nameEl  = item.querySelector('.note-name');
        if (input)   { input.value = note; input.classList.remove('error'); }
        if (valueEl) valueEl.textContent = note;
        if (nameEl)  nameEl.textContent  = getNoteNameFromNumber(note);
    });
    
    if (window.showToast) window.showToast(`✅ Preset "${name.toUpperCase()}" aplicado — pulsa Guardar para confirmar`, window.TOAST_TYPES?.INFO, 3000);
}

async function saveMIDIMapping() {
    const inputs = document.querySelectorAll('.note-input');
    const mappings = [];
    let hasErrors = false;
    
    inputs.forEach(input => {
        const item = input.closest('.mapping-item');
        const pad  = parseInt(item.dataset.pad);
        const note = parseInt(input.value);
        
        if (isNaN(note) || note < 0 || note > 127) {
            input.classList.add('error');
            hasErrors = true;
            return;
        }
        input.classList.remove('error');
        mappings.push({ note, pad });
    });
    
    if (hasErrors) {
        if (window.showToast) window.showToast('❌ Notas inválidas (0-127)', window.TOAST_TYPES?.ERROR, 3000);
        return;
    }
    
    try {
        for (const mapping of mappings) {
            const resp = await fetch('/api/midi/mapping', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(mapping)
            });
            if (!resp.ok) throw new Error(`Pad ${mapping.pad}`);
        }
        
        // Actualizar dataset y note-names
        inputs.forEach(input => {
            const item    = input.closest('.mapping-item');
            const note    = parseInt(input.value);
            const valueEl = item.querySelector('.note-value');
            const nameEl  = item.querySelector('.note-name');
            item.dataset.note = note;
            if (valueEl) valueEl.textContent = note;
            if (nameEl)  nameEl.textContent  = getNoteNameFromNumber(note);
        });
        
        // Salir modo edición
        isEditingMapping = true;  // forzar a que toggleMappingEdit lo desactive
        cancelMappingEdit();
        const editBtn = document.getElementById('editMappingBtn');
        if (editBtn) editBtn.style.display = 'inline-block';
        
        // Recargar desde ESP32 para confirmar valores guardados
        await loadMIDIMapping();
        
        if (window.showToast) window.showToast('✅ Mapeo MIDI guardado', window.TOAST_TYPES?.SUCCESS, 3000);
    } catch (error) {
        console.error('[MIDI Mapping] Error saving:', error);
        if (window.showToast) window.showToast('❌ Error al guardar mapeo', window.TOAST_TYPES?.ERROR, 3000);
    }
}

async function resetMIDIMapping() {
    if (!confirm('¿Resetear el mapeo MIDI al mapa GM estándar (16 pads)?')) return;
    
    try {
        const resp = await fetch('/api/midi/mapping', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ reset: true })
        });
        
        if (resp.ok) {
            await loadMIDIMapping();
            if (window.showToast) window.showToast('🔄 Mapeo reseteado a GM (16 pads)', window.TOAST_TYPES?.SUCCESS, 3000);
        } else {
            throw new Error('Reset failed');
        }
    } catch (error) {
        console.error('[MIDI Mapping] Error resetting:', error);
        if (window.showToast) window.showToast('❌ Error al resetear mapeo', window.TOAST_TYPES?.ERROR, 3000);
    }
}

// Cargar mapeo al abrir la tab MIDI
document.addEventListener('DOMContentLoaded', () => {
    const midiTab = document.querySelector('[data-tab="midi"]');
    if (midiTab) {
        midiTab.addEventListener('click', () => {
            setTimeout(loadMIDIMapping, 100);
        });
    }
    if (window.location.hash === '#midi' || document.getElementById('tab-midi')?.classList.contains('active')) {
        setTimeout(loadMIDIMapping, 500);
    }
});
