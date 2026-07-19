from pathlib import Path
from shutil import copytree, rmtree, which
import gzip
import subprocess
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")


def _is_uploadfs_target() -> bool:
    targets = {str(t).lower() for t in COMMAND_LINE_TARGETS}
    return "uploadfs" in targets or "buildfs" in targets


def _minify_web(project_dir: Path, web_dir: Path) -> None:
    node = which("node")
    minifier = project_dir / "tools" / "minify_web.mjs"
    node_modules = project_dir / "node_modules"
    if (not node or not (node_modules / "esbuild").exists() or
            not (node_modules / "html-minifier-terser").exists()):
        raise RuntimeError(
            "Minificación web requerida: ejecuta 'npm install' en el proyecto "
            "antes de buildfs/uploadfs"
        )
    subprocess.run([node, str(minifier), str(web_dir)], cwd=project_dir, check=True)


def _sync_data_to_data_gz(project_dir: Path) -> None:
    src = project_dir / "data"
    dst = project_dir / "data_gz"
    staging = project_dir / "data_gz.tmp"

    if not src.exists():
        print("[prepare_data_gz] data/ no existe, nada que preparar")
        return

    if staging.exists():
        rmtree(staging)
    copytree(src, staging)

    web_dir = staging / "web"
    if not web_dir.exists():
        rmtree(staging)
        print("[prepare_data_gz] warning: no se encontró data/web en staging")
        return

    # El AP no tolera bien lecturas LittleFS grandes en competencia. La portada
    # queda pequeña y el bootstrap carga cada CSS y app.js secuencialmente;
    # los editores secundarios continúan como módulos bajo demanda.
    _minify_web(project_dir, web_dir)

    raw_bytes = sum(p.stat().st_size for p in web_dir.rglob("*") if p.is_file())
    gz_created = 0
    for path in web_dir.rglob("*"):
        if path.is_file() and path.suffix in {".js", ".css", ".html"}:
            gz_path = Path(str(path) + ".gz")
            with path.open("rb") as fin, gz_path.open("wb") as raw_out:
                with gzip.GzipFile(
                    filename="",
                    mode="wb",
                    fileobj=raw_out,
                    compresslevel=9,
                    mtime=0,
                ) as fout:
                    fout.write(fin.read())
            gz_created += 1

    removed = 0
    for path in web_dir.rglob("*"):
        if path.is_file() and path.suffix in {".js", ".css", ".html"}:
            path.unlink()
            removed += 1

    final_bytes = sum(p.stat().st_size for p in web_dir.rglob("*") if p.is_file())
    if dst.exists():
        rmtree(dst)
    try:
        staging.replace(dst)
    except PermissionError:
        # Windows Defender/Explorer puede retener brevemente un handle del
        # directorio y bloquear el rename aunque ya no exista el destino.
        # La copia conserva el staging completo y sólo se usa como fallback.
        copytree(staging, dst)
        rmtree(staging)

    saved_pct = (100.0 * (raw_bytes - final_bytes) / raw_bytes) if raw_bytes else 0.0
    print(
        f"[prepare_data_gz] data_gz listo. {gz_created} .gz, "
        f"{removed} fuentes retiradas, {raw_bytes} -> {final_bytes} bytes "
        f"(-{saved_pct:.1f}%)"
    )


if _is_uploadfs_target():
    project = Path(env.subst("$PROJECT_DIR"))
    _sync_data_to_data_gz(project)
else:
    print("[prepare_data_gz] omitido (solo actúa en uploadfs/buildfs)")
