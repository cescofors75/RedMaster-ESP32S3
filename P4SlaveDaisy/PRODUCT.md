# Product

<!-- impeccable:product-schema 1 -->

## Platform

adaptive

## Users

El usuario principal es el músico que maneja RayDrone en directo desde una Daisy Pod y necesita leer su estado de un vistazo sin apartarse del instrumento.

## Product Purpose

P4SlaveDaisy convierte una Guition ESP32-P4 JC1060P470C de 7 pulgadas en la superficie visual dedicada de RayDrone. Recibe por USB-C la telemetría del DSP —captura, parámetros, acorde, voces, niveles y limitador— y la presenta con latencia visual baja. El audio y los controles físicos siguen perteneciendo a Daisy Pod.

## Positioning

No es un monitor serie genérico: conoce el estado musical real del motor RayDrone y traduce sus parámetros y transiciones de captura a una superficie escénica legible.

## Operating Context

- Escenario o estudio con luz ambiental baja y atención dividida entre instrumento, Daisy Pod y pantalla.
- Daisy Pod actúa como dispositivo USB CDC; ESP32-P4 actúa como host USB y alimenta/enumera el enlace.
- Primera versión orientada a visualización segura. El protocolo reserva mensajes de comando para una evolución táctil posterior, sin prometer aún control remoto.

## Capabilities and Constraints

- Hardware confirmado: Guition JC1060P470C, ESP32-P4, LCD MIPI-DSI JD9165BA 1024×600, táctil GT911, 16 MB flash y 32 MB PSRAM.
- Transporte confirmado: USB CDC `0483:5740`, paquetes binarios versionados de 44 bytes con CRC-16 a 20 Hz.
- El firmware Daisy normal BOOT_SRAM/QSPI ofrece telemetría. El perfil directo BOOT_NONE queda deliberadamente sin USB por su límite de 128 KB.
- Debe reconectar automáticamente y diferenciar con claridad BUSCANDO, CONECTADO y SEÑAL PERDIDA.
- El refresco de interfaz o USB nunca puede afectar al audio de Daisy.

## Brand Commitments

Los nombres `RayDrone` y `P4SlaveDaisy` se mantienen. El usuario pidió una ejecución “súper moderna y profesional”; la voz debe ser técnica, breve y musical, sin apariencia de consola de depuración.

## Evidence on Hand

- Motor y firmware reales en `../main.cpp`, `../rust/` y `../usb/raydrone_usb_protocol.h`.
- Configuración y drivers probados de la placa en `C:/Users/cesco/Documents/Arduino/XboxBLE/BlueSlaveP4`.
- No hay logotipo, tipografía de marca ni recursos comerciales confirmados; no deben inventarse claims ni identidad corporativa externa.

## Product Principles

- Estado crítico legible en menos de un segundo.
- Daisy conserva autoridad sobre audio y control físico.
- La desconexión se muestra con honestidad y se recupera sin intervención.
- Densidad informativa musical, no telemetría cruda.
- Rendimiento y estabilidad por encima de decoración.
