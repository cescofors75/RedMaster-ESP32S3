#ifndef RED808_RAYDRONE_PROTOCOL_H
#define RED808_RAYDRONE_PROTOCOL_H

#include <stdint.h>

/*
 * Atomic Raydrone control shared by the ESP32-S3 master and Daisy.
 *
 * The user-facing controls mirror the web application's Basic workspace:
 * Material, Character, Motion, Space, Volume and recursive Autoevolution.
 * Mix is intentionally kept separate because the Daisy version is a live-input
 * master insert.
 */
#ifndef CMD_RAYDRONE_CONFIG
#define CMD_RAYDRONE_CONFIG          0x28
#endif
#define MASTER_FX_ROUTE_RAYDRONE     12

#define RAYDRONE_CONFIG_VERSION      2
#define RAYDRONE_FLAG_ACTIVE         0x01

#define RAYDRONE_MATERIAL_COUNT      6
#define RAYDRONE_DEFAULT_MATERIAL    0
#define RAYDRONE_DEFAULT_CHARACTER   35
#define RAYDRONE_DEFAULT_MOTION      0
#define RAYDRONE_DEFAULT_SPACE       15
#define RAYDRONE_DEFAULT_VOLUME      80
#define RAYDRONE_DEFAULT_MIX         70
#define RAYDRONE_DEFAULT_EVOLUTION   0
/* Character follows the WASM 0..100 macro, but the embedded renderer keeps
 * the safe operating window explicit.  The DSP clamps again at the boundary. */
#define RAYDRONE_CHARACTER_SAFE_MAX  35

/*
 * Local merge masks used by controllers. They are not transmitted to Daisy:
 * CMD_RAYDRONE_CONFIG always carries the complete nine-byte snapshot.
 */
#define RAYDRONE_UPDATE_ACTIVE       (1u << 0)
#define RAYDRONE_UPDATE_MATERIAL     (1u << 1)
#define RAYDRONE_UPDATE_CHARACTER    (1u << 2)
#define RAYDRONE_UPDATE_MOTION       (1u << 3)
#define RAYDRONE_UPDATE_SPACE        (1u << 4)
#define RAYDRONE_UPDATE_VOLUME       (1u << 5)
#define RAYDRONE_UPDATE_MIX          (1u << 6)
#define RAYDRONE_UPDATE_EVOLUTION    (1u << 7)
#define RAYDRONE_UPDATE_ALL          0xFFu

#if defined(__GNUC__)
#define RAYDRONE_PACKED __attribute__((packed))
#else
#define RAYDRONE_PACKED
#pragma pack(push, 1)
#endif

typedef struct RAYDRONE_PACKED RaydroneConfigPayload {
    uint8_t version;
    uint8_t flags;
    uint8_t material;   /* 0=Empty, 1=Metal, 2=Wood, 3=Glass, 4=Water, 5=Plasma */
    uint8_t character;  /* 0..100 wire range; embedded renderer clamps to 35 */
    uint8_t motion;     /* 0..100 */
    uint8_t space;      /* 0..100 */
    uint8_t volume;     /* 0..100, Raydrone wet-engine output */
    uint8_t mix;        /* 0..100, dry/wet insert blend */
    uint8_t evolution;  /* 0..100, recursive output -> focus/aperture */
} RaydroneConfigPayload;

#if !defined(__GNUC__)
#pragma pack(pop)
#endif

#ifdef __cplusplus
static_assert(sizeof(RaydroneConfigPayload) == 9,
              "RaydroneConfigPayload wire size changed");
#endif

#undef RAYDRONE_PACKED

#endif
