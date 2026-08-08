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
- Wave field: one uninterrupted 976 px source surface. The min/max envelope,
  aperture window and Focus needle share the same coordinate system; compact
  Character, Intensity and Material values sit above it.
- Footer: stereo meters at left and a large tactile chord summary at right.
- Radius 12 px for state chips; macro tracks and meters use 4 px or square ends.

## Controls and State

Touch has two deliberate modes. Focus is manipulated directly on the waveform;
the gesture uses the full 118 px-high target and responds on press/drag.
Discrete operations use explicit buttons. Tapping the chord summary opens a
focused 5 × 2 selector whose ten 176 × 76 px targets show the confirmed voicing.
Tapping the grain station opens one full-width operating plane: five RAYS
targets, six MOTION modes, four destinations and two long touch sliders. Pending
changes use Capture, confirmed state uses Signal and an unconfirmed command uses
Alert. Source and sample-rate actions remain in their compact command strip.

- `BUSCANDO`: Slate route, animated restrained scan marker.
- `CONECTADO`: Signal route and moving packet marker.
- `SEÑAL PERDIDA`: last values remain visible but dim; status turns Alert and
  names the active recovery (“RECUPERANDO USB-C”). Focus and command touch are
  disabled until valid telemetry returns; open chord or motion selectors close.
- `CAPTURANDO`: Capture station fills from left to right.
- `FREEZE`: Capture station locks in warm Capture color.
- `BYPASS`: route remains visible, output segment becomes Mist and header names BYPASS.
- `LIMIT`: only the limiter station becomes Alert; no full-screen alarm.

Motion consists of one packet marker travelling through the signal path. Numeric values update without entrance animations.
The waveform itself does not scroll or pulse: new min/max chunks replace it as
a stable instrument surface. Touch moves the Focus needle immediately, with
the telemetry echo confirming the final position.

## Performance Rules

- UI model updates at 20 Hz; LVGL render runs at the physical 60 Hz ceiling.
- Waveform data is reduced to 96 min/max columns on Daisy and drawn as 96
  vertical strokes; no full audio buffer is copied to P4.
- Dirty-region, direct-mode double buffering and VSYNC synchronization remain enabled.
- USB parsing and UI rendering never run on Daisy and can never enter its audio callback.
- Repeated telemetry values do not trigger unnecessary LVGL property writes.
