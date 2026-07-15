#!/usr/bin/env bash
# Regenera la imagen del filesystem web a partir de las fuentes.
#
#   FUENTE DE VERDAD:  RedMaster_ESP32S3/data/web/   (archivos en claro)
#   SE FLASHEA:        RedMaster_ESP32S3/data_gz/web/ (gzip; platformio.ini -> data_dir = data_gz)
#
# Ejecutar SIEMPRE antes de `pio run -t uploadfs` tras editar la web.
# Evita el bug histórico de editar data/web y flashear un data_gz desfasado.
set -euo pipefail
cd "$(dirname "$0")/../RedMaster_ESP32S3"

SRC=data/web
DST=data_gz/web
mkdir -p "$DST"

for f in "$SRC"/*; do
  name=$(basename "$f")
  case "$name" in
    *.gz) echo "SKIP artefacto: $name" ;;
    favicon.ico) cp -f "$f" "$DST/$name" ;;            # binario: tal cual
    *) gzip -9 -c "$f" > "$DST/$name.gz" ;;
  esac
done

echo "OK: $(ls "$DST" | wc -l) archivos en $DST"
