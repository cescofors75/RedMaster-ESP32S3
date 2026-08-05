# Diagnóstico limpio de Daisy Pod

Este firmware copia `Line In` a `Line Out/Headphones` muestra por muestra. No
contiene filtros, memoria, reverb, limitador ni DaisySP en el callback de audio.

## Cargar

```powershell
cd C:\Users\cesco\Documents\Arduino\XboxBLE\DaisyPod_Aurora
.\flash.ps1 -Diagnostic -Direct
```

Al arrancar muestra tres alternancias rojo/verde.

## Controles

- Knob 1: brillo de LED 1.
- Knob 2: brillo verde de LED 2.
- Encoder: cambia el color de LED 1.
- Button 1: añade rojo a LED 2.
- Button 2: añade azul a LED 2.

Los controles no modifican el audio en este firmware.

## Prueba de masa

Usa auriculares directamente en Headphones y una fuente a batería, por ejemplo
un móvil que no esté cargando.

1. Arranca sin nada conectado a Line In y escucha el ruido base.
2. Conecta el móvil a Line In y escucha con la reproducción pausada.
3. Reproduce audio y confirma el passthrough limpio.
4. Desconecta el USB del ordenador y alimenta el Pod desde una batería USB o un
   cargador aislado. Repite la escucha.
5. Finalmente vuelve a conectar el USB al ordenador. Si el zumbido aparece solo
   ahora, existe un bucle de masa por USB.

## Interpretación

| Resultado | Origen probable |
|---|---|
| Silencioso aislado; zumba con USB del PC | Bucle de masa USB |
| Silencioso sin Line In; ruido al conectar fuente | Fuente, cable o masa de Line In |
| Ruido sin Line In, con batería USB y auriculares | Hardware/alimentación del Pod |
| Passthrough limpio y controles LED correctos | El problema era el firmware FX |
