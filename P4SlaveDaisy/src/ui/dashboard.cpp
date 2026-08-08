// THESIS: RayDrone es una ruta viva USB > captura > granos > limitador; se
// rechaza el mosaico de tarjetas KPI porque oculta dónde está el sonido.
// OWN-WORLD: escenario azul-negro, riel mineral, señal turquesa, captura
// ámbar y alerta coral; estaciones fijas, campos abiertos y números estables.
// STORY: localizar el enlace, reconocer captura/Freeze y leer macros, RAYS,
// MOTION, acorde, niveles y carga en menos de un segundo.
// FIRST VIEWPORT: cabecera de conexión, espina de señal dominante, tres lanes
// macro y pie con estéreo/voicing; RAYS/MOTION abre un plano operativo único.
// FORM: estructura propia #6, staging visible-transit, seed f24ba3f3.

#include "dashboard.h"

#include "config.h"

#include <Arduino.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <lvgl.h>

namespace
{
constexpr uint32_t kAbyss   = 0x071119;
constexpr uint32_t kField   = 0x0C1C26;
constexpr uint32_t kSlate   = 0x173443;
constexpr uint32_t kPaper   = 0xE7F3F0;
constexpr uint32_t kMist    = 0x86A8B0;
constexpr uint32_t kSignal  = 0x42CDBD;
constexpr uint32_t kCapture = 0xF2A84A;
constexpr uint32_t kAlert   = 0xEF675F;
constexpr size_t   kChordCount = 10;
constexpr size_t   kRayChoiceCount = 5;
constexpr size_t   kMotionModeCount = 6;
constexpr size_t   kMotionDestinationCount = 4;

constexpr uint8_t kRayChoices[kRayChoiceCount] = {24, 28, 32, 40, 48};

struct Dashboard
{
    lv_obj_t* root;
    lv_obj_t* link_chip;
    lv_obj_t* link_dot;
    lv_obj_t* link_text;
    lv_obj_t* mode_text;
    lv_obj_t* route;
    lv_obj_t* route_output;
    lv_obj_t* route_marker;
    lv_obj_t* usb_node;
    lv_obj_t* capture_node;
    lv_obj_t* grain_node;
    lv_obj_t* limiter_node;
    lv_obj_t* usb_value;
    lv_obj_t* capture_value;
    lv_obj_t* capture_state;
    lv_obj_t* capture_fill;
    lv_obj_t* voices_value;
    lv_obj_t* motion_touch;
    lv_obj_t* motion_summary;
    lv_obj_t* limiter_value;
    lv_obj_t* route_message;
    lv_obj_t* waveform;
    lv_obj_t* waveform_source;
    lv_obj_t* waveform_hint;
    lv_obj_t* character_value;
    lv_obj_t* intensity_value;
    lv_obj_t* focus_value;
    lv_obj_t* material_value;
    lv_obj_t* input_fill;
    lv_obj_t* output_fill;
    lv_obj_t* input_value;
    lv_obj_t* output_value;
    lv_obj_t* chord_touch;
    lv_obj_t* chord_value;
    lv_obj_t* chord_index;
    lv_obj_t* chord_hint;
    lv_obj_t* chord_overlay;
    lv_obj_t* chord_panel;
    lv_obj_t* chord_panel_state;
    lv_obj_t* chord_buttons[kChordCount];
    lv_obj_t* motion_overlay;
    lv_obj_t* motion_panel;
    lv_obj_t* motion_panel_state;
    lv_obj_t* ray_buttons[kRayChoiceCount];
    lv_obj_t* motion_mode_buttons[kMotionModeCount];
    lv_obj_t* motion_destination_buttons[kMotionDestinationCount];
    lv_obj_t* motion_depth_slider;
    lv_obj_t* motion_speed_slider;
    lv_obj_t* motion_depth_value;
    lv_obj_t* motion_speed_value;
    lv_obj_t* diagnostics;
    lv_obj_t* source_buttons[7];
    lv_obj_t* telemetry_objects[32];
    uint8_t   telemetry_object_count;
    int8_t    waveform_min[raydrone_usb::kWaveformBinCount];
    int8_t    waveform_max[raydrone_usb::kWaveformBinCount];
    uint16_t  focus_milli;
    uint16_t  character_milli;
    uint16_t  waveform_revision;
    float     source_seconds;
    bool      waveform_valid;
    bool      default_source;
    bool      live_stream;
    uint8_t   active_chord;
    uint8_t   requested_chord;
    uint8_t   active_ray_target;
    uint8_t   requested_ray_target;
    uint16_t  active_motion_packed;
    uint16_t  requested_motion_packed;
    uint16_t  live_fill_milli;
    uint32_t  audio_sample_rate_hz;
    uint32_t  chord_retry_ms;
    uint32_t  chord_timeout_ms;
    uint32_t  chord_confirm_until_ms;
    uint32_t  rays_retry_ms;
    uint32_t  rays_timeout_ms;
    uint32_t  rays_confirm_until_ms;
    uint32_t  motion_retry_ms;
    uint32_t  motion_timeout_ms;
    uint32_t  motion_confirm_until_ms;
    bool      touch_enabled;
    bool      command_enabled;
    bool      chord_command_enabled;
    bool      rays_command_enabled;
    bool      motion_command_enabled;
    bool      chord_pending;
    bool      chord_failed;
    bool      rays_pending;
    bool      rays_failed;
    bool      motion_pending;
    bool      motion_failed;
    bool      motion_dragging;
};

Dashboard ui_ = {};
std::atomic<uint16_t> pending_focus_milli_{500};
std::atomic<bool> pending_focus_dirty_{false};
std::atomic<uint8_t> pending_command_id_{0};
std::atomic<uint16_t> pending_command_value_{0};
std::atomic<bool> pending_command_dirty_{false};

void QueueCommand(raydrone_usb::CommandId id, uint16_t value)
{
    pending_command_id_.store(static_cast<uint8_t>(id),
                              std::memory_order_relaxed);
    pending_command_value_.store(value, std::memory_order_relaxed);
    pending_command_dirty_.store(true, std::memory_order_release);
}

void SetChordPanelVisible(bool visible)
{
    if(ui_.chord_overlay == nullptr)
        return;
    if(visible)
        lv_obj_clear_flag(ui_.chord_overlay, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(ui_.chord_overlay, LV_OBJ_FLAG_HIDDEN);
}

void SetMotionPanelVisible(bool visible)
{
    if(ui_.motion_overlay == nullptr)
        return;
    if(visible)
        lv_obj_clear_flag(ui_.motion_overlay, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(ui_.motion_overlay, LV_OBJ_FLAG_HIDDEN);
}

struct SourceAction
{
    raydrone_usb::CommandId id;
    uint16_t value;
};

constexpr SourceAction kSourceActions[] = {
    {raydrone_usb::CommandId::SetSource,
     static_cast<uint16_t>(raydrone_usb::SourceCommand::Default)},
    {raydrone_usb::CommandId::SetSource,
     static_cast<uint16_t>(raydrone_usb::SourceCommand::Live)},
    {raydrone_usb::CommandId::SetSource,
     static_cast<uint16_t>(raydrone_usb::SourceCommand::Freeze)},
    {raydrone_usb::CommandId::SetSource,
     static_cast<uint16_t>(raydrone_usb::SourceCommand::Memory)},
    {raydrone_usb::CommandId::SetSource,
     static_cast<uint16_t>(raydrone_usb::SourceCommand::SdNext)},
    {raydrone_usb::CommandId::SaveSampler, 3u},
    {raydrone_usb::CommandId::SetSampleRate, 0u},
};

lv_color_t Color(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

void MakePlain(lv_obj_t* object)
{
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}

lv_obj_t* Box(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
              lv_coord_t width, lv_coord_t height, uint32_t color,
              lv_coord_t radius = 0)
{
    lv_obj_t* object = lv_obj_create(parent);
    MakePlain(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, Color(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    return object;
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, lv_coord_t x, lv_coord_t y,
                const lv_font_t* font, uint32_t color)
{
    lv_obj_t* label = lv_label_create(parent);
    MakePlain(label);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, Color(color), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    return label;
}

void SetText(lv_obj_t* label, const char* text)
{
    if(strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

void SetTextColor(lv_obj_t* label, uint32_t color)
{
    const lv_color_t target = Color(color);
    if(lv_color_to32(lv_obj_get_style_text_color(label, 0))
       != lv_color_to32(target))
        lv_obj_set_style_text_color(label, target, 0);
}

void SetBgColor(lv_obj_t* object, uint32_t color)
{
    const lv_color_t target = Color(color);
    if(lv_color_to32(lv_obj_get_style_bg_color(object, 0))
       != lv_color_to32(target))
        lv_obj_set_style_bg_color(object, target, 0);
}

void SetFill(lv_obj_t* fill, uint16_t milli, lv_coord_t maximum)
{
    const lv_coord_t width = static_cast<lv_coord_t>(
        (static_cast<uint32_t>(milli > 1000 ? 1000 : milli) * maximum) / 1000u);
    if(lv_obj_get_width(fill) != width)
        lv_obj_set_width(fill, width);
}

void SetNode(lv_obj_t* node, uint32_t color)
{
    SetBgColor(node, color);
}

const char* MotionModeName(uint8_t index)
{
    static const char* const names[] = {
        "OFF", "SLOW", "SMOOTH", "S&H", "BROWNIAN", "QMC",
    };
    return index < kMotionModeCount ? names[index] : "OFF";
}

const char* MotionDestinationName(uint8_t index)
{
    static const char* const names[] = {
        "FOCUS", "SPREAD", "DENSITY", "SPACE",
    };
    return index < kMotionDestinationCount ? names[index] : "FOCUS";
}

void UpdateMotionValueLabels()
{
    char text[16];
    const uint8_t depth = raydrone_usb::MotionDepthFromPacked(
        ui_.requested_motion_packed);
    const uint8_t speed = raydrone_usb::MotionSpeedFromPacked(
        ui_.requested_motion_packed);
    snprintf(text, sizeof(text), "%u%%",
             static_cast<unsigned>((depth * 100u + 15u) / 31u));
    SetText(ui_.motion_depth_value, text);
    snprintf(text, sizeof(text), "%u%%",
             static_cast<unsigned>((speed * 100u + 31u) / 63u));
    SetText(ui_.motion_speed_value, text);
}

void QueueRays(uint8_t rays)
{
    const uint32_t now = millis();
    ui_.requested_ray_target = rays;
    ui_.rays_pending = true;
    ui_.rays_failed = false;
    ui_.rays_retry_ms = now + 250u;
    ui_.rays_timeout_ms = now + 1800u;
    QueueCommand(raydrone_usb::CommandId::SetRays, rays);
}

void QueueMotion()
{
    const uint32_t now = millis();
    ui_.motion_pending = true;
    ui_.motion_failed = false;
    ui_.motion_retry_ms = now + 250u;
    ui_.motion_timeout_ms = now + 1800u;
    QueueCommand(raydrone_usb::CommandId::SetMotion,
                 ui_.requested_motion_packed);
}

void SourceButtonEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED || !ui_.command_enabled)
        return;

    const size_t index = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event));
    if(index >= sizeof(kSourceActions) / sizeof(kSourceActions[0]))
        return;

    const uint16_t value
        = kSourceActions[index].id == raydrone_usb::CommandId::SetSampleRate
              ? (ui_.audio_sample_rate_hz >= 96000u ? 48u : 96u)
              : kSourceActions[index].value;
    QueueCommand(kSourceActions[index].id, value);
}

void ChordSummaryEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) == LV_EVENT_CLICKED
       && ui_.chord_command_enabled)
        SetChordPanelVisible(true);
}

void ChordCloseEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) == LV_EVENT_CLICKED)
        SetChordPanelVisible(false);
}

void ChordButtonEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED
       || !ui_.chord_command_enabled)
        return;

    const size_t index = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event));
    if(index >= kChordCount)
        return;

    const uint32_t now = millis();
    ui_.requested_chord = static_cast<uint8_t>(index);
    ui_.chord_pending = true;
    ui_.chord_failed = false;
    ui_.chord_retry_ms = now + 250u;
    ui_.chord_timeout_ms = now + 1800u;
    QueueCommand(raydrone_usb::CommandId::SetChord,
                 static_cast<uint16_t>(index));
    SetText(ui_.chord_hint, "ENVIANDO...");
    SetText(ui_.chord_panel_state,
            "ENVIANDO A DAISY / ESPERANDO TELEMETRIA");
}

void MotionSummaryEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) == LV_EVENT_CLICKED
       && (ui_.rays_command_enabled || ui_.motion_command_enabled))
        SetMotionPanelVisible(true);
}

void MotionCloseEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) == LV_EVENT_CLICKED)
        SetMotionPanelVisible(false);
}

void RayButtonEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED
       || !ui_.rays_command_enabled)
        return;
    const size_t index = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event));
    if(index < kRayChoiceCount)
        QueueRays(kRayChoices[index]);
}

void MotionModeEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED
       || !ui_.motion_command_enabled)
        return;
    const size_t index = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event));
    if(index >= kMotionModeCount)
        return;
    ui_.requested_motion_packed = raydrone_usb::PackMotion(
        static_cast<raydrone_usb::MotionMode>(index),
        raydrone_usb::MotionDestinationFromPacked(ui_.requested_motion_packed),
        raydrone_usb::MotionDepthFromPacked(ui_.requested_motion_packed),
        raydrone_usb::MotionSpeedFromPacked(ui_.requested_motion_packed));
    QueueMotion();
}

void MotionDestinationEvent(lv_event_t* event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED
       || !ui_.motion_command_enabled)
        return;
    const size_t index = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event));
    if(index >= kMotionDestinationCount)
        return;
    ui_.requested_motion_packed = raydrone_usb::PackMotion(
        raydrone_usb::MotionModeFromPacked(ui_.requested_motion_packed),
        static_cast<raydrone_usb::MotionDestination>(index),
        raydrone_usb::MotionDepthFromPacked(ui_.requested_motion_packed),
        raydrone_usb::MotionSpeedFromPacked(ui_.requested_motion_packed));
    QueueMotion();
}

void MotionSliderEvent(lv_event_t* event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_PRESS_LOST)
    {
        ui_.motion_dragging = false;
        return;
    }
    if(code == LV_EVENT_RELEASED)
        ui_.motion_dragging = false;
    if(!ui_.motion_command_enabled)
        return;
    if(code == LV_EVENT_PRESSED)
    {
        ui_.motion_dragging = true;
        return;
    }
    if(code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
        return;
    const bool speed = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)) != 0u;
    const uint8_t value = static_cast<uint8_t>(
        lv_slider_get_value(lv_event_get_target(event)));
    const uint8_t depth = speed
                              ? raydrone_usb::MotionDepthFromPacked(
                                    ui_.requested_motion_packed)
                              : value;
    const uint8_t speed_step = speed
                                   ? value
                                   : raydrone_usb::MotionSpeedFromPacked(
                                         ui_.requested_motion_packed);
    ui_.requested_motion_packed = raydrone_usb::PackMotion(
        raydrone_usb::MotionModeFromPacked(ui_.requested_motion_packed),
        raydrone_usb::MotionDestinationFromPacked(ui_.requested_motion_packed),
        depth, speed_step);
    UpdateMotionValueLabels();
    if(code == LV_EVENT_RELEASED)
    {
        QueueMotion();
    }
}

lv_obj_t* ActionButton(lv_obj_t* parent, const char* text, lv_coord_t x,
                       lv_coord_t width, size_t index)
{
    lv_obj_t* button = Box(parent, x, 402, width, 44, kSlate, 7);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(button, 2);
    lv_obj_add_event_cb(button, SourceButtonEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(index));
    lv_obj_t* label = Label(button, text, 0, 0, &lv_font_montserrat_12, kPaper);
    lv_obj_center(label);
    return button;
}

lv_obj_t* ChordButton(lv_obj_t* parent, const char* text, lv_coord_t x,
                      lv_coord_t y, size_t index)
{
    lv_obj_t* button = Box(parent, x, y, 176, 76, kSlate, 12);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(button, Color(kCapture), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, ChordButtonEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(index));
    lv_obj_t* label = Label(button, text, 8, 0, &lv_font_montserrat_14, kPaper);
    lv_obj_set_width(label, 160);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t* ControlButton(lv_obj_t* parent, const char* text,
                        lv_coord_t x, lv_coord_t y,
                        lv_coord_t width, lv_coord_t height,
                        lv_event_cb_t callback, size_t index)
{
    lv_obj_t* button = Box(parent, x, y, width, height, kSlate, 12);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(button, Color(kCapture), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(index));
    lv_obj_t* label = Label(button, text, 6, 0,
                            &lv_font_montserrat_14, kPaper);
    lv_obj_set_width(label, width - 12);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t* MotionSlider(lv_obj_t* parent, lv_coord_t x, bool speed)
{
    lv_obj_t* slider = lv_slider_create(parent);
    MakePlain(slider);
    lv_obj_set_pos(slider, x, 426);
    lv_obj_set_size(slider, 430, 28);
    lv_obj_set_ext_click_area(slider, 10);
    lv_slider_set_range(slider, 0, speed ? 63 : 31);
    lv_obj_set_style_bg_color(slider, Color(kSlate), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, Color(kSignal), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, Color(kPaper), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(slider, 22, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 48, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, MotionSliderEvent, LV_EVENT_ALL,
                        reinterpret_cast<void*>(speed ? 1u : 0u));
    return slider;
}

lv_obj_t* TrackTelemetry(lv_obj_t* object)
{
    if(ui_.telemetry_object_count
       < sizeof(ui_.telemetry_objects) / sizeof(ui_.telemetry_objects[0]))
        ui_.telemetry_objects[ui_.telemetry_object_count++] = object;
    return object;
}

void SetTelemetryOpacity(lv_opa_t opacity)
{
    for(uint8_t i = 0; i < ui_.telemetry_object_count; ++i)
    {
        lv_obj_t* object = ui_.telemetry_objects[i];
        if(lv_obj_get_style_opa(object, 0) != opacity)
            lv_obj_set_style_opa(object, opacity, 0);
    }
}

void MarkerX(void* object, int32_t value)
{
    lv_obj_set_x(static_cast<lv_obj_t*>(object), static_cast<lv_coord_t>(value));
}

const char* ChordName(uint8_t index)
{
    static const char* const names[] = {
        "UNISONO", "OCTAVAS", "POWER", "MAYOR", "MENOR",
        "QUINTAS", "SUS2", "PENTATONICA", "ESCALA MAYOR", "ESCALA MENOR",
    };
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "UNKNOWN";
}

const char* ChordButtonName(uint8_t index)
{
    static const char* const names[] = {
        "UNISONO", "OCTAVAS", "POWER", "MAYOR", "MENOR",
        "QUINTAS", "SUS2", "PENTATONICA", "ESCALA\nMAYOR", "ESCALA\nMENOR",
    };
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "";
}

const char* MaterialName(uint8_t index)
{
    static const char* const names[] = {
        "VACIO", "METAL", "MADERA", "CRISTAL", "AGUA", "PLASMA",
    };
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "VACIO";
}

void DrawWaveform(lv_draw_ctx_t* draw_context, lv_obj_t* object)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const lv_coord_t left = area.x1 + 8;
    const lv_coord_t right = area.x2 - 8;
    const lv_coord_t top = area.y1 + 8;
    const lv_coord_t bottom = area.y2 - 8;
    const lv_coord_t middle = (top + bottom) / 2;
    const lv_coord_t amplitude = (bottom - top) / 2;
    const lv_coord_t width = right - left;
    lv_coord_t visible_width = width;
    if(ui_.live_stream && ui_.live_fill_milli < 1000u)
    {
        visible_width = static_cast<lv_coord_t>(
            (static_cast<uint32_t>(width) * ui_.live_fill_milli) / 1000u);
        if(visible_width < 2)
            visible_width = 2;
    }

    const lv_coord_t focus_x = left + static_cast<lv_coord_t>(
        (static_cast<uint32_t>(ui_.focus_milli) * visible_width) / 1000u);
    const float character = static_cast<float>(ui_.character_milli) * 0.001f;
    const float aperture_seconds = 0.02f + character * character * 0.60f;
    const float aperture_fraction = ui_.source_seconds > 0.001f
                                        ? aperture_seconds / ui_.source_seconds
                                        : 0.02f;
    lv_coord_t half_window = static_cast<lv_coord_t>(
        aperture_fraction * static_cast<float>(visible_width) * 0.5f);
    if(half_window < 5)
        half_window = 5;

    lv_area_t focus_area = {
        static_cast<lv_coord_t>(focus_x - half_window), top,
        static_cast<lv_coord_t>(focus_x + half_window), bottom,
    };
    if(focus_area.x1 < left)
        focus_area.x1 = left;
    if(focus_area.x2 > left + visible_width)
        focus_area.x2 = left + visible_width;
    lv_draw_rect_dsc_t focus_fill;
    lv_draw_rect_dsc_init(&focus_fill);
    focus_fill.bg_color = Color(kCapture);
    focus_fill.bg_opa = LV_OPA_20;
    focus_fill.radius = 4;
    lv_draw_rect(draw_context, &focus_fill, &focus_area);

    lv_draw_line_dsc_t baseline;
    lv_draw_line_dsc_init(&baseline);
    baseline.color = Color(kSlate);
    baseline.width = 1;
    lv_point_t base_start = {left, middle};
    lv_point_t base_end = {right, middle};
    lv_draw_line(draw_context, &baseline, &base_start, &base_end);

    lv_draw_line_dsc_t wave;
    lv_draw_line_dsc_init(&wave);
    wave.color = Color(ui_.default_source ? kSignal : kCapture);
    wave.width = 2;
    wave.opa = ui_.waveform_valid ? LV_OPA_80 : LV_OPA_20;
    for(size_t i = 0; i < raydrone_usb::kWaveformBinCount; ++i)
    {
        const lv_coord_t x = left + static_cast<lv_coord_t>(
            (i * static_cast<size_t>(visible_width))
            / (raydrone_usb::kWaveformBinCount - 1u));
        const lv_coord_t y_min = middle - static_cast<lv_coord_t>(
            (static_cast<int32_t>(ui_.waveform_min[i]) * amplitude) / 127);
        const lv_coord_t y_max = middle - static_cast<lv_coord_t>(
            (static_cast<int32_t>(ui_.waveform_max[i]) * amplitude) / 127);
        lv_point_t p1 = {x, y_max};
        lv_point_t p2 = {x, y_min};
        lv_draw_line(draw_context, &wave, &p1, &p2);
    }

    if(ui_.live_stream)
    {
        lv_draw_line_dsc_t head;
        lv_draw_line_dsc_init(&head);
        head.color = Color(kSignal);
        head.width = 2;
        head.opa = LV_OPA_80;
        const lv_coord_t head_x = left + visible_width;
        lv_point_t head_top = {head_x, top};
        lv_point_t head_bottom = {head_x, bottom};
        lv_draw_line(draw_context, &head, &head_top, &head_bottom);
    }

    lv_draw_line_dsc_t needle;
    lv_draw_line_dsc_init(&needle);
    needle.color = Color(kCapture);
    needle.width = 2;
    lv_point_t needle_top = {focus_x, top};
    lv_point_t needle_bottom = {focus_x, bottom};
    lv_draw_line(draw_context, &needle, &needle_top, &needle_bottom);

    lv_area_t handle = {static_cast<lv_coord_t>(focus_x - 4), top,
                        static_cast<lv_coord_t>(focus_x + 4),
                        static_cast<lv_coord_t>(top + 8)};
    lv_draw_rect_dsc_t handle_style;
    lv_draw_rect_dsc_init(&handle_style);
    handle_style.bg_color = Color(kCapture);
    handle_style.bg_opa = LV_OPA_COVER;
    handle_style.radius = 4;
    lv_draw_rect(draw_context, &handle_style, &handle);
}

void WaveformEvent(lv_event_t* event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t* object = lv_event_get_target(event);
    if(code == LV_EVENT_DRAW_MAIN)
    {
        DrawWaveform(lv_event_get_draw_ctx(event), object);
        return;
    }
    if((code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING)
       || !ui_.touch_enabled)
        return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const lv_coord_t left = area.x1 + 8;
    const lv_coord_t full_right = area.x2 - 8;
    lv_coord_t right = full_right;
    if(ui_.live_stream && ui_.live_fill_milli < 1000u)
    {
        right = left + static_cast<lv_coord_t>(
            (static_cast<uint32_t>(full_right - left) * ui_.live_fill_milli)
            / 1000u);
        if(right <= left)
            right = left + 1;
    }
    lv_coord_t x = point.x < left ? left : (point.x > right ? right : point.x);
    const uint16_t focus = static_cast<uint16_t>(
        (static_cast<uint32_t>(x - left) * 1000u) / (right - left));
    ui_.focus_milli = focus;
    pending_focus_milli_.store(focus, std::memory_order_relaxed);
    pending_focus_dirty_.store(true, std::memory_order_release);
    lv_obj_invalidate(object);
}
} // namespace

void dashboard_create()
{
    ui_.telemetry_object_count = 0;
    ui_.requested_ray_target = 32;
    ui_.requested_motion_packed = raydrone_usb::PackMotion(
        raydrone_usb::MotionMode::Off,
        raydrone_usb::MotionDestination::Focus, 18, 12);
    ui_.root = lv_scr_act();
    lv_obj_remove_style_all(ui_.root);
    lv_obj_clear_flag(ui_.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_.root, Color(kAbyss), 0);
    lv_obj_set_style_bg_opa(ui_.root, LV_OPA_COVER, 0);

    Label(ui_.root, "RAYDRONE", 24, 18, &lv_font_montserrat_22, kPaper);
    Label(ui_.root, "P4 VISUAL / USB-C", 182, 23,
          &lv_font_montserrat_12, kMist);
    ui_.mode_text = Label(ui_.root, "", 648, 22,
                          &lv_font_montserrat_14, kCapture);

    ui_.link_chip = Box(ui_.root, 782, 14, 218, 34, kField, 12);
    ui_.link_dot  = Box(ui_.link_chip, 12, 12, 10, 10, kSlate, 5);
    ui_.link_text = Label(ui_.link_chip, "BUSCANDO DAISY", 32, 8,
                          &lv_font_montserrat_14, kMist);
    Box(ui_.root, 24, 62, 976, 1, kSlate);

    // Signal spine: the route is the primary visual hierarchy.
    ui_.route = Box(ui_.root, 64, 148, 896, 2, kSlate);
    ui_.route_output = Box(ui_.root, 658, 148, 302, 2, kSlate);
    const lv_coord_t station_x[] = {64, 326, 658, 944};
    ui_.usb_node     = Box(ui_.root, station_x[0] - 7, 141, 16, 16, kSlate, 8);
    ui_.capture_node = Box(ui_.root, station_x[1] - 7, 141, 16, 16, kSlate, 8);
    ui_.grain_node   = Box(ui_.root, station_x[2] - 7, 141, 16, 16, kSlate, 8);
    ui_.limiter_node = Box(ui_.root, station_x[3] - 7, 141, 16, 16, kSlate, 8);

    Label(ui_.root, "USB HOST", 24, 91, &lv_font_montserrat_12, kMist);
    ui_.usb_value = TrackTelemetry(
        Label(ui_.root, "ESPERA", 24, 111, &lv_font_montserrat_20, kPaper));

    Label(ui_.root, "CAPTURE", 258, 82, &lv_font_montserrat_12, kMist);
    ui_.capture_value = TrackTelemetry(
        Label(ui_.root, "0.0 s", 258, 102, &lv_font_montserrat_32, kPaper));
    ui_.capture_state = TrackTelemetry(
        Label(ui_.root, "ARMADO", 384, 113, &lv_font_montserrat_14, kCapture));
    Box(ui_.root, 258, 175, 252, 7, kSlate, 3);
    ui_.capture_fill = TrackTelemetry(
        Box(ui_.root, 258, 175, 0, 7, kCapture, 3));

    ui_.motion_touch = Box(ui_.root, 574, 76, 230, 64, kField, 12);
    lv_obj_add_flag(ui_.motion_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ui_.motion_touch, Color(kSlate), LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_.motion_touch, MotionSummaryEvent,
                        LV_EVENT_CLICKED, nullptr);
    Label(ui_.motion_touch, "CAMPO DE GRANOS", 12, 7,
          &lv_font_montserrat_12, kMist);
    ui_.motion_summary = Label(ui_.motion_touch, "R32 / OFF", 135, 8,
                               &lv_font_montserrat_12, kSignal);
    ui_.voices_value = TrackTelemetry(
        Label(ui_.motion_touch, "0 VOCES", 12, 31,
              &lv_font_montserrat_20, kPaper));

    Label(ui_.root, "LIMITADOR", 842, 91, &lv_font_montserrat_12, kMist);
    ui_.limiter_value = TrackTelemetry(
        Label(ui_.root, "0.0 dB", 842, 111, &lv_font_montserrat_20, kPaper));

    ui_.route_marker = Box(ui_.root, 57, 144, 10, 10, kSignal, 5);
    lv_anim_t marker_animation;
    lv_anim_init(&marker_animation);
    lv_anim_set_var(&marker_animation, ui_.route_marker);
    lv_anim_set_exec_cb(&marker_animation, MarkerX);
    lv_anim_set_values(&marker_animation, 57, 951);
    lv_anim_set_time(&marker_animation, 1700);
    lv_anim_set_repeat_count(&marker_animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&marker_animation, lv_anim_path_linear);
    lv_anim_start(&marker_animation);

    ui_.route_message = Label(ui_.root, "CONECTA DAISY USB-C AL PUERTO HOST",
                              24, 205, &lv_font_montserrat_14, kMist);

    Box(ui_.root, 0, 246, 1024, 188, kField);
    Label(ui_.root, "FORMA DE ONDA", 24, 258, &lv_font_montserrat_12, kMist);
    ui_.waveform_source = TrackTelemetry(
        Label(ui_.root, "ESPERANDO FUENTE", 145, 257,
              &lv_font_montserrat_14, kSignal));
    Label(ui_.root, "MATERIAL", 370, 258, &lv_font_montserrat_12, kMist);
    ui_.material_value = TrackTelemetry(
        Label(ui_.root, "VACIO", 438, 257, &lv_font_montserrat_14, kPaper));
    Label(ui_.root, "CHAR", 575, 258, &lv_font_montserrat_12, kMist);
    ui_.character_value = TrackTelemetry(
        Label(ui_.root, "0%", 620, 257, &lv_font_montserrat_14, kPaper));
    Label(ui_.root, "INT", 710, 258, &lv_font_montserrat_12, kMist);
    ui_.intensity_value = TrackTelemetry(
        Label(ui_.root, "0%", 744, 257, &lv_font_montserrat_14, kPaper));
    Label(ui_.root, "FOCUS", 835, 258, &lv_font_montserrat_12, kMist);
    ui_.focus_value = TrackTelemetry(
        Label(ui_.root, "50%", 894, 257, &lv_font_montserrat_14, kCapture));

    ui_.waveform = TrackTelemetry(Box(ui_.root, 24, 282, 976, 118, kAbyss, 8));
    lv_obj_add_flag(ui_.waveform, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_.waveform, WaveformEvent, LV_EVENT_ALL, nullptr);
    ui_.waveform_hint = Label(ui_.root, "FOCUS TACTIL", 24, 410,
                              &lv_font_montserrat_12, kMist);
    ui_.source_buttons[0] = ActionButton(ui_.root, "DEFAULT", 484, 78, 0);
    ui_.source_buttons[1] = ActionButton(ui_.root, "LIVE", 568, 58, 1);
    ui_.source_buttons[2] = ActionButton(ui_.root, "FREEZE", 632, 72, 2);
    ui_.source_buttons[3] = ActionButton(ui_.root, "MEM", 710, 62, 3);
    ui_.source_buttons[4] = ActionButton(ui_.root, "SD NEXT", 778, 68, 4);
    ui_.source_buttons[5] = ActionButton(ui_.root, "SAVE", 852, 66, 5);
    ui_.source_buttons[6] = ActionButton(ui_.root, "48K", 924, 76, 6);

    Label(ui_.root, "INPUT", 24, 464, &lv_font_montserrat_12, kMist);
    Box(ui_.root, 82, 470, 420, 6, kSlate, 3);
    ui_.input_fill = TrackTelemetry(Box(ui_.root, 82, 470, 0, 6, kSignal, 3));
    ui_.input_value = TrackTelemetry(
        Label(ui_.root, "0%", 514, 460, &lv_font_montserrat_14, kPaper));
    Label(ui_.root, "OUTPUT", 24, 498, &lv_font_montserrat_12, kMist);
    Box(ui_.root, 82, 504, 420, 6, kSlate, 3);
    ui_.output_fill = TrackTelemetry(Box(ui_.root, 82, 504, 0, 6, kCapture, 3));
    ui_.output_value = TrackTelemetry(
        Label(ui_.root, "0%", 514, 494, &lv_font_montserrat_14, kPaper));

    ui_.chord_touch = Box(ui_.root, 604, 452, 396, 90, kField, 12);
    lv_obj_add_flag(ui_.chord_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ui_.chord_touch, Color(kSlate), LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_.chord_touch, ChordSummaryEvent, LV_EVENT_CLICKED,
                        nullptr);
    Label(ui_.chord_touch, "ACORDE", 16, 9, &lv_font_montserrat_12, kMist);
    ui_.chord_hint = Label(ui_.chord_touch, "TOCA / ELEGIR", 275, 9,
                           &lv_font_montserrat_12, kSignal);
    ui_.chord_value = TrackTelemetry(
        Label(ui_.chord_touch, "UNISONO", 16, 38,
              &lv_font_montserrat_22, kPaper));
    ui_.chord_index = TrackTelemetry(
        Label(ui_.chord_touch, "01 / 10", 320, 55,
              &lv_font_montserrat_12, kMist));

    Box(ui_.root, 24, 555, 976, 1, kSlate);
    ui_.diagnostics = Label(ui_.root, "HOST USB ACTIVO / ESPERANDO DAISY",
                            24, 568, &lv_font_montserrat_12, kMist);

    ui_.chord_overlay = Box(ui_.root, 0, 0, 1024, 600, kAbyss);
    lv_obj_set_style_bg_opa(ui_.chord_overlay, LV_OPA_90, 0);
    lv_obj_add_flag(ui_.chord_overlay, LV_OBJ_FLAG_CLICKABLE);
    ui_.chord_panel = Box(ui_.chord_overlay, 24, 208, 976, 300, kField, 14);
    Label(ui_.chord_panel, "ESCOGE ACORDE", 24, 20,
          &lv_font_montserrat_20, kPaper);
    ui_.chord_panel_state = Label(
        ui_.chord_panel, "CONTROL TACTIL / DAISY CONFIRMA POR USB-C", 238, 25,
        &lv_font_montserrat_12, kMist);
    lv_obj_t* close = Box(ui_.chord_panel, 904, 14, 48, 48, kSlate, 12);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(close, Color(kCapture), LV_STATE_PRESSED);
    lv_obj_add_event_cb(close, ChordCloseEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* close_label = Label(close, "X", 0, 0,
                                  &lv_font_montserrat_20, kPaper);
    lv_obj_center(close_label);
    for(size_t i = 0; i < kChordCount; ++i)
    {
        const lv_coord_t column = static_cast<lv_coord_t>(i % 5u);
        const lv_coord_t row = static_cast<lv_coord_t>(i / 5u);
        ui_.chord_buttons[i] = ChordButton(
            ui_.chord_panel, ChordButtonName(static_cast<uint8_t>(i)),
            24 + column * 188, 76 + row * 92, i);
    }
    SetChordPanelVisible(false);

    ui_.motion_overlay = Box(ui_.root, 0, 0, 1024, 600, kAbyss);
    lv_obj_set_style_bg_opa(ui_.motion_overlay, LV_OPA_90, 0);
    lv_obj_add_flag(ui_.motion_overlay, LV_OBJ_FLAG_CLICKABLE);
    ui_.motion_panel = Box(ui_.motion_overlay, 24, 60, 976, 480, kField, 14);
    Label(ui_.motion_panel, "RAYS / MOTION", 24, 20,
          &lv_font_montserrat_20, kPaper);
    ui_.motion_panel_state = Label(
        ui_.motion_panel, "DAISY GENERA EL MOVIMIENTO / USB-C SOLO CONFIGURA",
        214, 25, &lv_font_montserrat_12, kMist);
    lv_obj_t* motion_close = Box(ui_.motion_panel, 904, 14, 48, 48,
                                 kSlate, 12);
    lv_obj_add_flag(motion_close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(motion_close, Color(kCapture), LV_STATE_PRESSED);
    lv_obj_add_event_cb(motion_close, MotionCloseEvent,
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_t* motion_close_label = Label(motion_close, "X", 0, 0,
                                         &lv_font_montserrat_20, kPaper);
    lv_obj_center(motion_close_label);

    Label(ui_.motion_panel, "RAYS", 24, 78,
          &lv_font_montserrat_12, kMist);
    for(size_t i = 0; i < kRayChoiceCount; ++i)
    {
        char rays[8];
        snprintf(rays, sizeof(rays), "%u", kRayChoices[i]);
        ui_.ray_buttons[i] = ControlButton(
            ui_.motion_panel, rays, 24 + static_cast<lv_coord_t>(i) * 186,
            102, 176, 58, RayButtonEvent, i);
    }

    Label(ui_.motion_panel, "MODO", 24, 188,
          &lv_font_montserrat_12, kMist);
    for(size_t i = 0; i < kMotionModeCount; ++i)
        ui_.motion_mode_buttons[i] = ControlButton(
            ui_.motion_panel, MotionModeName(static_cast<uint8_t>(i)),
            24 + static_cast<lv_coord_t>(i) * 154, 212, 144, 58,
            MotionModeEvent, i);

    Label(ui_.motion_panel, "DESTINO", 24, 296,
          &lv_font_montserrat_12, kMist);
    for(size_t i = 0; i < kMotionDestinationCount; ++i)
        ui_.motion_destination_buttons[i] = ControlButton(
            ui_.motion_panel,
            MotionDestinationName(static_cast<uint8_t>(i)),
            24 + static_cast<lv_coord_t>(i) * 230, 320, 218, 58,
            MotionDestinationEvent, i);

    Label(ui_.motion_panel, "AMOUNT", 24, 398,
          &lv_font_montserrat_12, kMist);
    ui_.motion_depth_value = Label(ui_.motion_panel, "58%", 410, 398,
                                   &lv_font_montserrat_12, kSignal);
    Label(ui_.motion_panel, "SPEED", 500, 398,
          &lv_font_montserrat_12, kMist);
    ui_.motion_speed_value = Label(ui_.motion_panel, "19%", 886, 398,
                                   &lv_font_montserrat_12, kSignal);
    ui_.motion_depth_slider = MotionSlider(ui_.motion_panel, 24, false);
    ui_.motion_speed_slider = MotionSlider(ui_.motion_panel, 500, true);
    lv_slider_set_value(ui_.motion_depth_slider, 18, LV_ANIM_OFF);
    lv_slider_set_value(ui_.motion_speed_slider, 12, LV_ANIM_OFF);
    UpdateMotionValueLabels();
    SetMotionPanelVisible(false);
}

void dashboard_update(const RayDroneModel& model)
{
    uint32_t link_color = kSlate;
    const char* link_text = "BUSCANDO DAISY";
    const char* route_text = "CONECTA DAISY USB-C AL PUERTO HOST";
    bool marker_visible = true;

    switch(model.link_state)
    {
        case RayDroneLinkState::Searching:
            break;
        case RayDroneLinkState::WaitingForTelemetry:
            link_color = kCapture;
            link_text = "USB LISTO / SIN DATOS";
            route_text = "DAISY DETECTADA / ESPERANDO FIRMWARE RAYDRONE";
            break;
        case RayDroneLinkState::Live:
            link_color = kSignal;
            link_text = "CONECTADO";
            route_text = "RUTA DE SENAL ACTIVA";
            break;
        case RayDroneLinkState::Stale:
            link_color = kAlert;
            link_text = "SENAL PERDIDA";
            route_text = "RECUPERANDO USB-C / ULTIMOS VALORES CONSERVADOS";
            marker_visible = false;
            break;
    }

    SetBgColor(ui_.link_chip,
               model.link_state == RayDroneLinkState::Stale ? 0x321B20 : kField);
    SetNode(ui_.link_dot, link_color);
    SetTextColor(ui_.link_text, link_color == kSlate ? kMist : link_color);
    SetText(ui_.link_text, link_text);
    SetText(ui_.route_message, route_text);
    SetTextColor(ui_.route_message, link_color == kSlate ? kMist : link_color);
    SetBgColor(ui_.route, link_color);
    SetBgColor(ui_.route_output, link_color);
    SetNode(ui_.usb_node, link_color);
    if(marker_visible && lv_obj_has_flag(ui_.route_marker, LV_OBJ_FLAG_HIDDEN))
        lv_obj_clear_flag(ui_.route_marker, LV_OBJ_FLAG_HIDDEN);
    else if(!marker_visible
            && !lv_obj_has_flag(ui_.route_marker, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(ui_.route_marker, LV_OBJ_FLAG_HIDDEN);
    SetBgColor(ui_.route_marker, link_color);
    SetTelemetryOpacity(model.link_state == RayDroneLinkState::Stale
                            ? LV_OPA_40
                            : LV_OPA_COVER);

    SetText(ui_.usb_value,
            model.link_state == RayDroneLinkState::Searching ? "ESPERA"
            : model.link_state == RayDroneLinkState::Stale ? "SIN SENAL"
                                                           : "USB");

    if(!model.has_status)
    {
        ui_.touch_enabled = false;
        ui_.command_enabled = false;
        ui_.chord_command_enabled = false;
        ui_.rays_command_enabled = false;
        ui_.motion_command_enabled = false;
        ui_.chord_pending = false;
        ui_.chord_failed = false;
        ui_.rays_pending = false;
        ui_.rays_failed = false;
        ui_.motion_pending = false;
        ui_.motion_failed = false;
        ui_.motion_dragging = false;
        SetChordPanelVisible(false);
        SetMotionPanelVisible(false);
        SetBgColor(ui_.chord_touch, kField);
        SetBgColor(ui_.motion_touch, kField);
        SetText(ui_.motion_summary, "R-- / OFF");
        SetTextColor(ui_.motion_summary, kMist);
        SetText(ui_.chord_hint, "SIN DAISY");
        SetTextColor(ui_.chord_hint, kMist);
        for(lv_obj_t* button : ui_.chord_buttons)
        {
            SetBgColor(button, kField);
            SetTextColor(lv_obj_get_child(button, 0), kMist);
        }
        for(lv_obj_t* button : ui_.ray_buttons)
            SetBgColor(button, kField);
        for(lv_obj_t* button : ui_.motion_mode_buttons)
            SetBgColor(button, kField);
        for(lv_obj_t* button : ui_.motion_destination_buttons)
            SetBgColor(button, kField);
        for(lv_obj_t* button : ui_.source_buttons)
            SetBgColor(button, kSlate);
        SetText(ui_.waveform_source, "ESPERANDO FUENTE");
        SetText(ui_.waveform_hint, "LA ONDA APARECERA AL RECIBIR RAYDRONE");
        SetText(ui_.diagnostics,
                model.link_state == RayDroneLinkState::WaitingForTelemetry
                    ? "DISPOSITIVO USB DETECTADO / ESPERANDO RAYDRONE"
                    : "HOST USB ACTIVO / ESPERANDO DAISY");
        return;
    }

    const raydrone_usb::Status& status = model.status;
    const bool captured = (status.flags & raydrone_usb::Captured) != 0;
    const bool recording = (status.flags & raydrone_usb::Recording) != 0;
    const bool committing = (status.flags & raydrone_usb::Committing) != 0;
    const bool bypassed = (status.flags & raydrone_usb::Bypassed) != 0;
    const bool limiting = (status.flags & raydrone_usb::Limiting) != 0;
    const bool default_source = (status.flags & raydrone_usb::DefaultSample) != 0;
    const bool live_stream = (status.flags & raydrone_usb::LiveStream) != 0;
    const bool memory_source = (status.flags & raydrone_usb::MemorySample) != 0;
    const bool sd_source = (status.flags & raydrone_usb::SdSample) != 0;
    const bool storage_busy = (status.flags & raydrone_usb::StorageBusy) != 0;
    const bool sd_mounted = (status.flags & raydrone_usb::SdMounted) != 0;
    const bool chord_capable = (status.flags & raydrone_usb::ChordControl) != 0;
    const bool rays_capable = (status.flags & raydrone_usb::RaysControl) != 0;
    const bool motion_capable = (status.flags & raydrone_usb::MotionControl) != 0;
    const bool shimmer = (status.flags & raydrone_usb::ShimmerScene) != 0;
    const uint8_t material = status.reserved < 6u ? status.reserved : 0u;

    char text[96];
    const float seconds = status.source_sample_rate_hz != 0
                              ? static_cast<float>(status.recorded_samples)
                                    / static_cast<float>(status.source_sample_rate_hz)
                              : 0.0f;
    snprintf(text, sizeof(text), "%.1f s", static_cast<double>(seconds));
    SetText(ui_.capture_value, text);

    bool waveform_changed = false;
    if(model.has_waveform
       && (ui_.waveform_revision != model.waveform_revision
           || memcmp(ui_.waveform_min, model.waveform_min,
                     sizeof(ui_.waveform_min)) != 0
           || memcmp(ui_.waveform_max, model.waveform_max,
                     sizeof(ui_.waveform_max)) != 0))
    {
        memcpy(ui_.waveform_min, model.waveform_min, sizeof(ui_.waveform_min));
        memcpy(ui_.waveform_max, model.waveform_max, sizeof(ui_.waveform_max));
        ui_.waveform_revision = model.waveform_revision;
        waveform_changed = true;
    }
    const uint16_t capture_milli
        = status.capacity_samples == 0
              ? 0
              : static_cast<uint16_t>((static_cast<uint64_t>(status.recorded_samples)
                                        * 1000u)
                                       / status.capacity_samples);
    if(ui_.focus_milli != status.focus_milli
       || ui_.character_milli != status.character_milli
       || ui_.default_source != default_source
       || ui_.live_stream != live_stream
       || ui_.live_fill_milli != capture_milli
       || fabsf(ui_.source_seconds - seconds) > 0.02f)
        waveform_changed = true;
    ui_.focus_milli = status.focus_milli;
    ui_.character_milli = status.character_milli;
    ui_.default_source = default_source;
    ui_.live_stream = live_stream;
    ui_.live_fill_milli = capture_milli > 1000u ? 1000u : capture_milli;
    ui_.audio_sample_rate_hz = status.audio_sample_rate_hz;
    ui_.source_seconds = seconds;
    ui_.waveform_valid = model.has_waveform;
    ui_.command_enabled = model.link_state == RayDroneLinkState::Live;
    ui_.chord_command_enabled = ui_.command_enabled && chord_capable;
    ui_.rays_command_enabled = ui_.command_enabled && rays_capable;
    ui_.motion_command_enabled = ui_.command_enabled && motion_capable;
    ui_.active_chord = status.chord_index;
    ui_.active_ray_target = status.ray_target;
    ui_.active_motion_packed = raydrone_usb::PackMotion(
        static_cast<raydrone_usb::MotionMode>(status.motion_mode),
        static_cast<raydrone_usb::MotionDestination>(status.motion_destination),
        status.motion_depth_step, status.motion_speed_step);
    ui_.touch_enabled = model.link_state == RayDroneLinkState::Live
                        && model.has_waveform
                        && (captured || (live_stream && seconds > 0.55f));
    const uint32_t now = millis();
    bool command_queued = false;
    if(!ui_.chord_command_enabled)
    {
        ui_.chord_pending = false;
        ui_.chord_failed = false;
        SetChordPanelVisible(false);
    }
    else if(ui_.chord_pending)
    {
        if(ui_.active_chord == ui_.requested_chord)
        {
            ui_.chord_pending = false;
            ui_.chord_failed = false;
            ui_.chord_confirm_until_ms = now + 700u;
            SetChordPanelVisible(false);
        }
        else if(static_cast<int32_t>(now - ui_.chord_timeout_ms) >= 0)
        {
            ui_.chord_pending = false;
            ui_.chord_failed = true;
        }
        else if(static_cast<int32_t>(now - ui_.chord_retry_ms) >= 0)
        {
            QueueCommand(raydrone_usb::CommandId::SetChord,
                         ui_.requested_chord);
            ui_.chord_retry_ms = now + 250u;
            command_queued = true;
        }
    }

    if(!ui_.rays_command_enabled)
    {
        ui_.rays_pending = false;
        ui_.rays_failed = false;
    }
    else if((ui_.rays_pending || ui_.rays_failed)
            && ui_.active_ray_target == ui_.requested_ray_target)
    {
        ui_.rays_pending = false;
        ui_.rays_failed = false;
        ui_.rays_confirm_until_ms = now + 700u;
    }
    else if(ui_.rays_pending)
    {
        if(static_cast<int32_t>(now - ui_.rays_timeout_ms) >= 0)
        {
            ui_.rays_pending = false;
            ui_.rays_failed = true;
        }
        else if(!command_queued
                && static_cast<int32_t>(now - ui_.rays_retry_ms) >= 0)
        {
            QueueCommand(raydrone_usb::CommandId::SetRays,
                         ui_.requested_ray_target);
            ui_.rays_retry_ms = now + 250u;
            command_queued = true;
        }
    }

    if(!ui_.motion_command_enabled)
    {
        ui_.motion_pending = false;
        ui_.motion_failed = false;
        ui_.motion_dragging = false;
    }
    else if((ui_.motion_pending || ui_.motion_failed)
            && ui_.active_motion_packed == ui_.requested_motion_packed)
    {
        ui_.motion_pending = false;
        ui_.motion_failed = false;
        ui_.motion_confirm_until_ms = now + 700u;
    }
    else if(ui_.motion_pending)
    {
        if(static_cast<int32_t>(now - ui_.motion_timeout_ms) >= 0)
        {
            ui_.motion_pending = false;
            ui_.motion_failed = true;
        }
        else if(!command_queued
                && static_cast<int32_t>(now - ui_.motion_retry_ms) >= 0)
        {
            QueueCommand(raydrone_usb::CommandId::SetMotion,
                         ui_.requested_motion_packed);
            ui_.motion_retry_ms = now + 250u;
        }
    }

    if(!ui_.rays_pending && !ui_.rays_failed)
        ui_.requested_ray_target = ui_.active_ray_target;
    if(!ui_.motion_pending && !ui_.motion_failed && !ui_.motion_dragging)
    {
        ui_.requested_motion_packed = ui_.active_motion_packed;
        const int32_t depth = raydrone_usb::MotionDepthFromPacked(
            ui_.requested_motion_packed);
        const int32_t speed = raydrone_usb::MotionSpeedFromPacked(
            ui_.requested_motion_packed);
        if(lv_slider_get_value(ui_.motion_depth_slider) != depth)
            lv_slider_set_value(ui_.motion_depth_slider, depth, LV_ANIM_OFF);
        if(lv_slider_get_value(ui_.motion_speed_slider) != speed)
            lv_slider_set_value(ui_.motion_speed_slider, speed, LV_ANIM_OFF);
        UpdateMotionValueLabels();
    }

    const bool chord_confirmed
        = !ui_.chord_pending && !ui_.chord_failed
          && static_cast<int32_t>(ui_.chord_confirm_until_ms - now) > 0;
    SetBgColor(ui_.chord_touch,
               ui_.chord_command_enabled ? kField : kAbyss);
    SetText(ui_.chord_hint,
            !ui_.command_enabled ? "SIN CONEXION"
            : !chord_capable ? "ACTUALIZA DAISY"
            : ui_.chord_pending ? "ENVIANDO..."
            : ui_.chord_failed ? "SIN CONFIRMAR"
            : chord_confirmed ? "CONFIRMADO"
                              : "TOCA / ELEGIR");
    SetTextColor(ui_.chord_hint,
                 ui_.chord_failed ? kAlert
                 : ui_.chord_command_enabled ? kSignal : kMist);
    SetText(ui_.chord_panel_state,
            ui_.chord_pending
                ? "ENVIANDO A DAISY / ESPERANDO TELEMETRIA"
            : ui_.chord_failed
                ? "DAISY NO CONFIRMO / REINTENTA"
                : "CONTROL TACTIL / DAISY CONFIRMA POR USB-C");
    SetTextColor(ui_.chord_panel_state,
                 ui_.chord_failed ? kAlert : kMist);
    for(size_t i = 0; i < kChordCount; ++i)
    {
        const bool active = i == ui_.active_chord;
        const bool requested = i == ui_.requested_chord;
        SetBgColor(ui_.chord_buttons[i],
                   !ui_.chord_command_enabled ? kField
                   : ui_.chord_pending && requested ? kCapture
                   : ui_.chord_failed && requested ? kAlert
                   : active ? kSignal : kSlate);
        SetTextColor(lv_obj_get_child(ui_.chord_buttons[i], 0),
                     ui_.chord_command_enabled
                             && (active || (ui_.chord_pending && requested))
                         ? kAbyss
                     : ui_.chord_command_enabled ? kPaper : kMist);
    }

    if(!ui_.rays_command_enabled && !ui_.motion_command_enabled)
        SetMotionPanelVisible(false);

    const bool rays_confirmed
        = !ui_.rays_pending && !ui_.rays_failed
          && static_cast<int32_t>(ui_.rays_confirm_until_ms - now) > 0;
    const bool motion_confirmed
        = !ui_.motion_pending && !ui_.motion_failed
          && static_cast<int32_t>(ui_.motion_confirm_until_ms - now) > 0;
    const uint8_t shown_rays = ui_.rays_pending
                                   ? ui_.requested_ray_target
                                   : ui_.active_ray_target;
    const uint16_t shown_motion = ui_.motion_pending || ui_.motion_dragging
                                      ? ui_.requested_motion_packed
                                      : ui_.active_motion_packed;
    snprintf(text, sizeof(text), "R%u / %s", shown_rays,
             MotionModeName(static_cast<uint8_t>(
                 raydrone_usb::MotionModeFromPacked(shown_motion))));
    SetText(ui_.motion_summary, text);
    SetBgColor(ui_.motion_touch,
               ui_.rays_command_enabled || ui_.motion_command_enabled
                   ? kField : kAbyss);
    SetTextColor(ui_.motion_summary,
                 ui_.rays_failed || ui_.motion_failed ? kAlert
                 : ui_.rays_pending || ui_.motion_pending || ui_.motion_dragging
                     ? kCapture
                 : rays_confirmed || motion_confirmed ? kPaper
                                                      : kSignal);

    SetText(ui_.motion_panel_state,
            ui_.motion_dragging ? "AJUSTANDO MOTION / SUELTA PARA ENVIAR"
            : ui_.rays_pending ? "ENVIANDO RAYS / ESPERANDO TELEMETRIA"
            : ui_.motion_pending ? "ENVIANDO MOTION / ESPERANDO TELEMETRIA"
            : ui_.rays_failed ? "RAYS SIN CONFIRMAR / TOCA PARA REINTENTAR"
            : ui_.motion_failed ? "MOTION SIN CONFIRMAR / TOCA PARA REINTENTAR"
            : rays_confirmed || motion_confirmed
                ? "CONFIRMADO POR DAISY"
                : "DAISY GENERA EL MOVIMIENTO / USB-C SOLO CONFIGURA");
    SetTextColor(ui_.motion_panel_state,
                 ui_.rays_failed || ui_.motion_failed ? kAlert
                 : ui_.rays_pending || ui_.motion_pending || ui_.motion_dragging
                     ? kCapture
                                                          : kMist);

    for(size_t i = 0; i < kRayChoiceCount; ++i)
    {
        const bool active = kRayChoices[i] == ui_.active_ray_target;
        const bool requested = kRayChoices[i] == ui_.requested_ray_target;
        SetBgColor(ui_.ray_buttons[i],
                   !ui_.rays_command_enabled ? kField
                   : ui_.rays_pending && requested ? kCapture
                   : ui_.rays_failed && requested ? kAlert
                   : active ? kSignal : kSlate);
        SetTextColor(lv_obj_get_child(ui_.ray_buttons[i], 0),
                     ui_.rays_command_enabled
                             && (active || (ui_.rays_pending && requested))
                         ? kAbyss
                     : ui_.rays_command_enabled ? kPaper : kMist);
    }

    const uint8_t active_mode = static_cast<uint8_t>(
        raydrone_usb::MotionModeFromPacked(ui_.active_motion_packed));
    const uint8_t requested_mode = static_cast<uint8_t>(
        raydrone_usb::MotionModeFromPacked(ui_.requested_motion_packed));
    for(size_t i = 0; i < kMotionModeCount; ++i)
    {
        const bool active = i == active_mode;
        const bool requested = i == requested_mode;
        SetBgColor(ui_.motion_mode_buttons[i],
                   !ui_.motion_command_enabled ? kField
                   : ui_.motion_pending && requested ? kCapture
                   : ui_.motion_failed && requested ? kAlert
                   : active ? kSignal : kSlate);
        SetTextColor(lv_obj_get_child(ui_.motion_mode_buttons[i], 0),
                     ui_.motion_command_enabled
                             && (active || (ui_.motion_pending && requested))
                         ? kAbyss
                     : ui_.motion_command_enabled ? kPaper : kMist);
    }

    const uint8_t active_destination = static_cast<uint8_t>(
        raydrone_usb::MotionDestinationFromPacked(ui_.active_motion_packed));
    const uint8_t requested_destination = static_cast<uint8_t>(
        raydrone_usb::MotionDestinationFromPacked(
            ui_.requested_motion_packed));
    for(size_t i = 0; i < kMotionDestinationCount; ++i)
    {
        const bool active = i == active_destination;
        const bool requested = i == requested_destination;
        SetBgColor(ui_.motion_destination_buttons[i],
                   !ui_.motion_command_enabled ? kField
                   : ui_.motion_pending && requested ? kCapture
                   : ui_.motion_failed && requested ? kAlert
                   : active ? kSignal : kSlate);
        SetTextColor(lv_obj_get_child(ui_.motion_destination_buttons[i], 0),
                     ui_.motion_command_enabled
                             && (active || (ui_.motion_pending && requested))
                         ? kAbyss
                     : ui_.motion_command_enabled ? kPaper : kMist);
    }
    const uint32_t motion_control_color
        = !ui_.motion_command_enabled ? kMist
          : ui_.motion_failed ? kAlert
          : ui_.motion_pending || ui_.motion_dragging ? kCapture
                                                     : kSignal;
    lv_obj_set_style_bg_color(ui_.motion_depth_slider,
        Color(motion_control_color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_.motion_speed_slider,
        Color(motion_control_color), LV_PART_INDICATOR);
    SetTextColor(ui_.motion_depth_value, motion_control_color);
    SetTextColor(ui_.motion_speed_value, motion_control_color);
    if(waveform_changed)
        lv_obj_invalidate(ui_.waveform);

    const char* source_name = default_source ? "DEFAULT.MP3"
                              : memory_source ? "MEMORIA DAISY"
                              : sd_source ? "SAMPLER SD"
                              : live_stream ? "LINE IN / LIVE"
                              : captured ? "FREEZE LIVE"
                                         : "LINE IN";
    SetText(ui_.waveform_source, source_name);
    SetTextColor(ui_.waveform_source, default_source ? kSignal : kCapture);
    SetText(ui_.waveform_hint,
            storage_busy ? "MEMORIA / SD EN PROCESO"
            : ui_.touch_enabled ? "FOCUS TACTIL / ARRASTRA"
            : model.link_state == RayDroneLinkState::Stale
                ? "FOCUS BLOQUEADO / RECUPERANDO USB-C"
            : live_stream ? "LLENANDO BUFFER LIVE"
                          : "RECIBIENDO FORMA DE ONDA");
    SetText(ui_.material_value, MaterialName(material));

    const char* capture_state = storage_busy ? "GUARDANDO"
                                : default_source ? "DEFAULT.MP3"
                                : memory_source ? "FLASH"
                                : sd_source ? "MICROSD"
                                : live_stream ? "LIVE"
                                : captured ? "CONGELADO"
                                : committing ? "CONFIRMANDO"
                                : recording ? "CAPTURANDO"
                                            : "LISTO";
    SetText(ui_.capture_state, capture_state);
    SetTextColor(ui_.capture_state, captured || recording ? kCapture : kMist);
    SetNode(ui_.capture_node, captured || recording ? kCapture : kSlate);
    SetFill(ui_.capture_fill, capture_milli, 252);

    const bool active_buttons[] = {
        default_source,
        live_stream,
        captured && !default_source && !memory_source && !sd_source,
        memory_source,
        sd_source,
        storage_busy,
        true,
    };
    for(size_t i = 0; i < sizeof(active_buttons) / sizeof(active_buttons[0]); ++i)
    {
        uint32_t color = active_buttons[i] ? (i == 5 ? kCapture : kSignal)
                                           : kSlate;
        if(i == 4 && !sd_mounted)
            color = kField;
        if(i == 6 && status.audio_sample_rate_hz >= 96000u)
            color = kCapture;
        SetBgColor(ui_.source_buttons[i], color);
        if(ui_.command_enabled)
            lv_obj_clear_flag(ui_.source_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t* rate_label = lv_obj_get_child(ui_.source_buttons[6], 0);
    SetText(rate_label,
            status.audio_sample_rate_hz >= 96000u ? "96K ULTRA" : "48K");

    snprintf(text, sizeof(text), "%u VOCES", status.active_voices);
    SetText(ui_.voices_value, text);
    SetNode(ui_.grain_node, status.active_voices > 0 ? kSignal : kSlate);

    const float gain = status.limiter_gain_milli > 0
                           ? status.limiter_gain_milli / 1000.0f
                           : 0.001f;
    const float reduction_db = 20.0f * log10f(gain);
    snprintf(text, sizeof(text), "%.1f dB", static_cast<double>(reduction_db));
    SetText(ui_.limiter_value, text);
    SetTextColor(ui_.limiter_value, bypassed ? kMist : limiting ? kAlert : kPaper);
    SetNode(ui_.limiter_node, bypassed ? kMist : limiting ? kAlert : kSignal);
    SetBgColor(ui_.route_output,
               model.link_state == RayDroneLinkState::Stale
                   ? kAlert
                   : bypassed ? kMist : link_color);

    snprintf(text, sizeof(text), "%u%%", status.character_milli / 10u);
    SetText(ui_.character_value, text);
    snprintf(text, sizeof(text), "%u%%", status.intensity_milli / 10u);
    SetText(ui_.intensity_value, text);
    snprintf(text, sizeof(text), "%u%%", status.focus_milli / 10u);
    SetText(ui_.focus_value, text);

    SetFill(ui_.input_fill, status.input_level_milli, 420);
    SetFill(ui_.output_fill, status.output_level_milli, 420);
    SetBgColor(ui_.output_fill, bypassed ? kMist : kCapture);
    snprintf(text, sizeof(text), "%u%%", status.input_level_milli / 10u);
    SetText(ui_.input_value, text);
    snprintf(text, sizeof(text), "%u%%", status.output_level_milli / 10u);
    SetText(ui_.output_value, text);

    SetText(ui_.chord_value, ChordName(status.chord_index));
    snprintf(text, sizeof(text), "%02u / 10", status.chord_index + 1u);
    SetText(ui_.chord_index, text);

    SetText(ui_.mode_text,
            storage_busy ? "STORAGE"
            : bypassed ? "BYPASS"
                       : shimmer ? "SHIMMER" : "DRONE");
    SetTextColor(ui_.mode_text, bypassed ? kMist : captured ? kCapture : kSignal);

    if(model.link_state == RayDroneLinkState::Live)
    {
        snprintf(text, sizeof(text), "RUTA %s / %s / %s",
                 source_name,
                 shimmer ? "SHIMMER" : "DRONE",
                 MaterialName(material));
        SetText(ui_.route_message, text);
    }

    if(model.link_state == RayDroneLinkState::Live
       && (!chord_capable || !rays_capable || !motion_capable))
        snprintf(text, sizeof(text),
                 "ACTUALIZA FIRMWARE DAISY / PROTOCOLO DE CONTROL INCOMPLETO");
    else if(model.link_state == RayDroneLinkState::Live)
        snprintf(text, sizeof(text),
                 "%s / %s / %luK / R%u %s>%s / CPU %u%% / SD %s",
                 source_name, shimmer ? "SHIMMER" : "DRONE",
                 static_cast<unsigned long>(status.audio_sample_rate_hz / 1000u),
                 status.ray_target,
                 MotionModeName(status.motion_mode),
                 MotionDestinationName(status.motion_destination),
                 status.cpu_load_milli / 10u,
                 sd_mounted ? "OK" : "NO");
    else if(model.link_state == RayDroneLinkState::Stale)
        snprintf(text, sizeof(text),
                 "AUTORECUPERACION / SIN DATOS %lums / PERDIDOS %lu / CRC %lu",
                 static_cast<unsigned long>(model.telemetry_age_ms),
                 static_cast<unsigned long>(model.dropped_packets),
                 static_cast<unsigned long>(model.invalid_packets));
    else
        snprintf(text, sizeof(text), "HOST USB ACTIVO / ESPERANDO DAISY");
    SetText(ui_.diagnostics, text);
}

bool dashboard_take_focus_command(uint16_t* focus_milli)
{
    if(focus_milli == nullptr
       || !pending_focus_dirty_.exchange(false, std::memory_order_acq_rel))
        return false;
    *focus_milli = pending_focus_milli_.load(std::memory_order_relaxed);
    return true;
}

bool dashboard_take_command(raydrone_usb::CommandId* id, uint16_t* value)
{
    if(id == nullptr || value == nullptr
       || !pending_command_dirty_.exchange(false, std::memory_order_acq_rel))
        return false;
    *id = static_cast<raydrone_usb::CommandId>(
        pending_command_id_.load(std::memory_order_relaxed));
    *value = pending_command_value_.load(std::memory_order_relaxed);
    return true;
}
