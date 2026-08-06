# P4SlaveDaisy Design System

## Direction

`Signal Spine` treats RayDrone as a live route, not a collection of widgets. USB, capture, grain field and limiter occupy fixed stations on one continuous horizontal path. State travels along that route; the musician can locate a problem or transition without reading every value.

The scene is a dark stage or studio. The surface must remain quiet in peripheral vision, then become explicit when capture, limiting or link loss needs attention.

## Color

- `Abyss` `#071119`: full-screen ground.
- `Field` `#0C1C26`: large operating zones, used without decorative borders.
- `Slate` `#173443`: tracks, inactive stations and separators.
- `Paper` `#E7F3F0`: primary text and live values.
- `Mist` `#86A8B0`: secondary labels, always on Abyss or Field.
- `Signal` `#42CDBD`: healthy USB path, input state and normal live motion.
- `Capture` `#F2A84A`: freeze/capture authority and the only warm operational accent.
- `Alert` `#EF675F`: disconnection, stale data and strong limiting only.

The strategy is restrained: neutral blue-black fields plus Signal; Capture and Alert appear only when their semantics are active. No gradients, glow halos or decorative glass.

## Typography

Use LVGL Montserrat because it is already compiled and verified on the target. It is a workhorse here, not a “tech” costume.

- 40 px semibold: primary live value at a station.
- 28–32 px semibold: chord and capture state.
- 18–20 px semibold: macro value.
- 14–16 px medium: operational labels.
- 12 px medium: units and diagnostics.

Numbers use fixed-width containers and right alignment so telemetry updates do not move adjacent content. Uppercase is reserved for station names and terse state words.

## Composition

- 24 px outer safe area; 8 px base spacing unit.
- Header: 64 px.
- Signal route: upper 190 px, four fixed stations joined by 2 px rails.
- Macro field: three equal lanes separated by open space and one-pixel rules, never three floating cards.
- Footer: stereo meters at left and chord identity at right.
- Radius 12 px for state chips; macro tracks and meters use 4 px or square ends.

## Controls and State

Version 1 is a trustworthy display, not a remote controller. Touch is initialized for future use but no surface pretends to edit Daisy values.

- `BUSCANDO`: Slate route, animated restrained scan marker.
- `CONECTADO`: Signal route and moving packet marker.
- `SEÑAL PERDIDA`: last values remain visible but dim; status turns Alert and names the recovery (“REVISAR USB-C”).
- `CAPTURANDO`: Capture station fills from left to right.
- `FREEZE`: Capture station locks in warm Capture color.
- `BYPASS`: route remains visible, output segment becomes Mist and header names BYPASS.
- `LIMIT`: only the limiter station becomes Alert; no full-screen alarm.

Motion consists of one packet marker travelling through the signal path. Numeric values update without entrance animations.

## Performance Rules

- UI model updates at 20 Hz; LVGL render runs at the physical 60 Hz ceiling.
- Dirty-region, direct-mode double buffering and VSYNC synchronization remain enabled.
- USB parsing and UI rendering never run on Daisy and can never enter its audio callback.
- Repeated telemetry values do not trigger unnecessary LVGL property writes.
