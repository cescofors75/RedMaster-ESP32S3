// THESIS: RayDrone es una ruta viva USB > captura > granos > limitador; se
// rechaza el mosaico de tarjetas KPI porque oculta dónde está el sonido.
// OWN-WORLD: escenario azul-negro, riel mineral, señal turquesa, captura
// ámbar y alerta coral; estaciones fijas, campos abiertos y números estables.
// STORY: localizar el enlace, reconocer captura/Freeze y leer macros, acorde,
// niveles y carga en menos de un segundo.
// FIRST VIEWPORT: cabecera de conexión, espina de señal dominante, tres lanes
// macro y pie con estéreo/voicing; todo cabe en 1024x600 sin navegación.
// FORM: estructura propia #6, staging visible-transit, seed f24ba3f3.

#include "dashboard.h"

#include "config.h"

#include <Arduino.h>
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

struct MacroLane
{
    lv_obj_t* value;
    lv_obj_t* qualifier;
    lv_obj_t* fill;
    lv_coord_t track_width;
};

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
    lv_obj_t* limiter_value;
    lv_obj_t* route_message;
    MacroLane character;
    MacroLane intensity;
    MacroLane focus;
    lv_obj_t* input_fill;
    lv_obj_t* output_fill;
    lv_obj_t* input_value;
    lv_obj_t* output_value;
    lv_obj_t* chord_value;
    lv_obj_t* chord_index;
    lv_obj_t* diagnostics;
    lv_obj_t* telemetry_objects[32];
    uint8_t   telemetry_object_count;
};

Dashboard ui_ = {};

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

MacroLane CreateMacroLane(lv_obj_t* parent, const char* name,
                          lv_coord_t x, uint32_t accent)
{
    Label(parent, name, x, 274, &lv_font_montserrat_14, kMist);
    MacroLane lane = {};
    lane.value = TrackTelemetry(
        Label(parent, "0%", x, 304, &lv_font_montserrat_40, kPaper));
    lane.qualifier = TrackTelemetry(
        Label(parent, "--", x + 4, 359, &lv_font_montserrat_14, kMist));
    Box(parent, x, 397, 280, 6, kSlate, 3);
    lane.fill = TrackTelemetry(Box(parent, x, 397, 0, 6, accent, 3));
    lane.track_width = 280;
    return lane;
}

const char* ChordName(uint8_t index)
{
    static const char* const names[] = {
        "UNISONO", "OCTAVAS", "POWER", "MAYOR", "MENOR",
        "QUINTAS", "SUS2", "PENTATONICA", "ESCALA MAYOR", "ESCALA MENOR",
    };
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "UNKNOWN";
}

const char* CharacterWord(uint16_t value)
{
    return value < 250 ? "TONAL / CERRADO"
           : value < 600 ? "APERTURA MEDIA"
                         : "ABIERTO / DIFUSO";
}

const char* IntensityWord(uint16_t value)
{
    return value < 250 ? "BAJA DENSIDAD"
           : value < 700 ? "NUBE ACTIVA"
                         : "CAMPO DENSO";
}

const char* FocusWord(uint16_t value)
{
    return value < 200 ? "INICIO CAPTURA"
           : value > 800 ? "FINAL CAPTURA"
                         : "DENTRO CAPTURA";
}
} // namespace

void dashboard_create()
{
    ui_.telemetry_object_count = 0;
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

    Label(ui_.root, "CAMPO DE GRANOS", 600, 91, &lv_font_montserrat_12, kMist);
    ui_.voices_value = TrackTelemetry(
        Label(ui_.root, "0 VOCES", 600, 111, &lv_font_montserrat_20, kPaper));

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
    Box(ui_.root, 341, 270, 1, 137, kSlate);
    Box(ui_.root, 682, 270, 1, 137, kSlate);
    ui_.character = CreateMacroLane(ui_.root, "CHARACTER", 24, kSignal);
    ui_.intensity = CreateMacroLane(ui_.root, "INTENSITY", 365, kCapture);
    ui_.focus     = CreateMacroLane(ui_.root, "FOCUS", 706, kSignal);

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

    Box(ui_.root, 584, 456, 1, 86, kSlate);
    Label(ui_.root, "ACORDE", 620, 459, &lv_font_montserrat_12, kMist);
    ui_.chord_value = TrackTelemetry(
        Label(ui_.root, "UNISONO", 620, 480, &lv_font_montserrat_32, kPaper));
    ui_.chord_index = TrackTelemetry(
        Label(ui_.root, "01 / 10", 920, 495, &lv_font_montserrat_12, kMist));

    Box(ui_.root, 24, 555, 976, 1, kSlate);
    ui_.diagnostics = Label(ui_.root, "HOST USB ACTIVO / ESPERANDO DAISY",
                            24, 568, &lv_font_montserrat_12, kMist);
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
            route_text = "REVISAR USB-C / ULTIMOS VALORES CONSERVADOS";
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

    char text[96];
    const float seconds = status.sample_rate_hz != 0
                              ? static_cast<float>(status.recorded_samples)
                                    / static_cast<float>(status.sample_rate_hz)
                              : 0.0f;
    snprintf(text, sizeof(text), "%.1f s", static_cast<double>(seconds));
    SetText(ui_.capture_value, text);

    const char* capture_state = captured ? "CONGELADO"
                                : committing ? "CONFIRMANDO"
                                : recording ? "CAPTURANDO"
                                            : "LLENO";
    SetText(ui_.capture_state, capture_state);
    SetTextColor(ui_.capture_state, captured || recording ? kCapture : kMist);
    SetNode(ui_.capture_node, captured || recording ? kCapture : kSlate);
    const uint16_t capture_milli
        = status.capacity_samples == 0
              ? 0
              : static_cast<uint16_t>((static_cast<uint64_t>(status.recorded_samples)
                                        * 1000u)
                                       / status.capacity_samples);
    SetFill(ui_.capture_fill, capture_milli, 252);

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
    SetText(ui_.character.value, text);
    SetText(ui_.character.qualifier, CharacterWord(status.character_milli));
    SetFill(ui_.character.fill, status.character_milli, ui_.character.track_width);

    snprintf(text, sizeof(text), "%u%%", status.intensity_milli / 10u);
    SetText(ui_.intensity.value, text);
    SetText(ui_.intensity.qualifier, IntensityWord(status.intensity_milli));
    SetFill(ui_.intensity.fill, status.intensity_milli, ui_.intensity.track_width);

    snprintf(text, sizeof(text), "%u%%", status.focus_milli / 10u);
    SetText(ui_.focus.value, text);
    SetText(ui_.focus.qualifier, FocusWord(status.focus_milli));
    SetFill(ui_.focus.fill, status.focus_milli, ui_.focus.track_width);

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
            bypassed ? "BYPASS" : captured ? "FREEZE" : "ENTRADA ACTIVA");
    SetTextColor(ui_.mode_text, bypassed ? kMist : captured ? kCapture : kSignal);

    if(model.link_state == RayDroneLinkState::Live)
        snprintf(text, sizeof(text), "ENLACE USB ESTABLE / TELEMETRIA 20 HZ");
    else if(model.link_state == RayDroneLinkState::Stale)
        snprintf(text, sizeof(text),
                 "SIN DATOS %lums / PERDIDOS %lu / CRC %lu",
                 static_cast<unsigned long>(model.telemetry_age_ms),
                 static_cast<unsigned long>(model.dropped_packets),
                 static_cast<unsigned long>(model.invalid_packets));
    else
        snprintf(text, sizeof(text), "HOST USB ACTIVO / ESPERANDO DAISY");
    SetText(ui_.diagnostics, text);
}
