# Dependencias de terceros

Aurora usa las siguientes bibliotecas ya presentes en el workspace:

- **libDaisy 8.1.0**, soporte de hardware Electrosmith. Licencia BSD 3-Clause.
- **DaisySP**, módulos Chorus, Phaser, Flanger, Overdrive, LadderFilter y SVF.
  Licencia MIT de Electrosmith y colaboradores.
- **DaisySP-LGPL / ReverbSc**, reverb estereo basada en el trabajo de Sean
  Costello, Istvan Varga, Paul Batchelor y Electrosmith. Licencia LGPL-2.1.

El `Makefile` enlaza `libdaisysp.a` y `libdaisysp-lgpl.a` desde:

```text
../RedMaster_DaisySeed64MB/DaisySeed/DaisySP/build
../RedMaster_DaisySeed64MB/DaisySeed/DaisySP/DaisySP-LGPL/build
```

El código fuente y la licencia correspondiente están junto a esa biblioteca en
`DaisySP/DaisySP-LGPL`. Si se distribuye un binario de Aurora fuera de este
workspace, deben conservarse los avisos y cumplirse las condiciones de LGPL-2.1.
