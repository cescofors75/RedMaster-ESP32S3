# Implementacion MIDI

Aurora escucha en modo omni por el **MIDI In TRS** de la Daisy Pod. Los numeros
de canal no se filtran. MIDI Clock tiene prioridad practica porque cada pulso
actualiza el tempo; un nuevo tap en Button 2 vuelve a fijar el tempo interno.

## Control Change

| CC | Funcion | Valores |
|---:|---|---|
| 20 | Character | 0–127 |
| 21 | Intensity | 0–127, limpio a inmersivo |
| 22 | Escena | 0–31 Ocean, 32–63 Pulse, 64–95 Halo, 96–127 Ritual |
| 23 | Longitud | 0–31: 1 pulso, 32–63: 2, 64–95: 4, 96–127: 8 |
| 64 | Capture gate | 0–63 libera, 64–127 captura |
| 65 | Morph | 0–63 off, 64–127 on |
| 66 | Bypass | 0–63 activo, 64–127 bypass |

Character e Intensity tienen **soft takeover**: tras recibir un CC, el valor MIDI sigue
mandando hasta que el knob fisico se mueve mas de un 2,5 % desde su posicion.

## Notas

| Nota | Funcion |
|---:|---|
| 60 (C4) | Note On alterna Capture |
| 62 (D4) | Morph momentaneo; Note Off lo libera |
| 64 (E4) | Borra logicamente el historial y reinicia la memoria rodante |

La nota de borrado no recorre ni pone a cero la SDRAM: invalida el historial en
tiempo constante, por lo que no interrumpe el callback de audio.

## Program Change

Los programas se repiten modulo 4:

- 0: Ocean
- 1: Pulse
- 2: Halo
- 3: Ritual

Por ejemplo, Program Change 4 vuelve a Ocean.

## MIDI Clock

Aurora calcula el tempo a partir de los mensajes `F8` a 24 PPQN y suaviza el
intervalo para reducir jitter. La longitud se decide al hacer Capture; cambiar
el tempo despues no estira destructivamente una memoria ya capturada.
