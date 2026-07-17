from pathlib import Path
from shutil import copytree, rmtree, which
import gzip
import hashlib
import json
import re
import subprocess
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")


def _is_uploadfs_target() -> bool:
    targets = {str(t).lower() for t in COMMAND_LINE_TARGETS}
    return "uploadfs" in targets or "buildfs" in targets


def _web_route(path: Path) -> str:
    html_routes = {
        "index.html": "/index.html",
        "patchbay.html": "/patchbay",
        "multiview.html": "/multiview.html",
        "gesture.html": "/gesture.html",
        "gesture-pro.html": "/gesture-pro.html",
        "mobile.html": "/mobile.html",
        "admin.html": "/adm",
    }
    return html_routes.get(path.name, "/" + path.as_posix())


def _render_service_worker(project_dir: Path, web_dir: Path) -> None:
    template_path = web_dir / "sw.js"
    if not template_path.exists():
        raise RuntimeError("Falta data/web/sw.js")

    digest = hashlib.sha256()
    assets = []
    for path in sorted(p for p in web_dir.rglob("*") if p.is_file()):
        relative = path.relative_to(web_dir)
        digest.update(relative.as_posix().encode("utf-8"))
        digest.update(path.read_bytes())
        if (relative.as_posix() != "sw.js" and
                relative.suffix.lower() in {".html", ".js", ".css", ".ico"}):
            assets.append(_web_route(relative))

    for build_input in (
        project_dir / "tools" / "minify_web.mjs",
        project_dir / "package-lock.json",
    ):
        if build_input.exists():
            digest.update(build_input.read_bytes())

    template = template_path.read_text(encoding="utf-8")
    placeholders = (
        "__RED808_CACHE_VERSION__",
        "__RED808_PRECACHE_MANIFEST__",
        "__RED808_STATIC_MANIFEST__",
    )
    if not all(placeholder in template for placeholder in placeholders):
        raise RuntimeError("Los placeholders de data/web/sw.js no están completos")

    # La primera instalación sólo pide la pantalla principal y sus dependencias
    # directas. Los editores/páginas secundarios se cachean cuando se usan para
    # no saturar Wi-Fi + LittleFS durante el primer arranque de la interfaz.
    index_html = (web_dir / "index.html").read_text(encoding="utf-8")
    linked_urls = {
        url if url.startswith("/") else "/" + url
        for url in re.findall(
            r"<(?:script|link)\b[^>]*\b(?:src|href)=[\"']([^\"'#]+)",
            index_html,
            flags=re.IGNORECASE,
        )
        if not url.startswith(("data:", "http:", "https:"))
    }
    critical_assets = ["/index.html", *linked_urls]

    rendered = template.replace("__RED808_CACHE_VERSION__", digest.hexdigest()[:16])
    rendered = rendered.replace(
        "__RED808_PRECACHE_MANIFEST__",
        json.dumps(sorted(set(critical_assets)), separators=(",", ":")),
    )
    rendered = rendered.replace(
        "__RED808_STATIC_MANIFEST__",
        json.dumps(sorted(set(assets)), separators=(",", ":")),
    )
    template_path.write_text(rendered, encoding="utf-8", newline="\n")


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

    _minify_web(project_dir, web_dir)
    _render_service_worker(project_dir, web_dir)

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
