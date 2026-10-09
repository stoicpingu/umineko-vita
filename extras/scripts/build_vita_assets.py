#!/usr/bin/env python3
"""Build Vita-optimized Umineko assets from HD originals.

Reads from --src (HD originals), writes optimized files to --out using the
exact same relative paths the script already requests.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from native_asset_metadata import collect_dimensions, png_size, write_dimensions

VITA_MAX_TEX = 2048
SKIP_DIRS = {"save", "onscripter-ru-osx.app"}
SKIP_NAMES = {".DS_Store", "Thumbs.db"}
AUDIO_KINDS = {"voice", "bgm", "se", "pam"}


def read_png_meta(path: Path):
    try:
        with open(path, "rb") as f:
            if f.read(8) != b"\x89PNG\r\n\x1a\n":
                return None
            width = height = bit_depth = color_type = None
            has_trns = False
            while True:
                raw_len = f.read(4)
                if len(raw_len) != 4:
                    break
                length = struct.unpack(">I", raw_len)[0]
                chunk = f.read(4)
                data = f.read(length)
                f.read(4)  # CRC
                if chunk == b"IHDR" and len(data) >= 10:
                    width, height, bit_depth, color_type = struct.unpack(
                        ">IIBB", data[:10]
                    )
                elif chunk == b"tRNS":
                    has_trns = True
                elif chunk == b"IEND":
                    break
            if width is None or height is None:
                return None
            has_alpha = color_type in (4, 6) or has_trns
            return {
                "width": int(width),
                "height": int(height),
                "bit_depth": int(bit_depth) if bit_depth is not None else 0,
                "color_type": int(color_type) if color_type is not None else 0,
                "has_alpha": bool(has_alpha),
            }
    except (OSError, struct.error):
        return None


def classify(rel: Path, ext: str, png_meta):
    p = rel.as_posix().lower()
    if ext == ".png":
        w = png_meta.get("width", 0) if png_meta else 0
        h = png_meta.get("height", 0) if png_meta else 0
        if h > VITA_MAX_TEX or w > VITA_MAX_TEX:
            if re.match(r"graphics/locale(?:_[^/]+)?/", p):
                return "long_strip"
            if p.startswith("backgrounds/"):
                return "background"
            if p.startswith("graphics/cg/"):
                return "fullscreen_graphic"
            return "large_png_scale_2_3"
        if p.startswith("backgrounds/"):
            return "background"
        if re.match(r"sprites/[^/]+/2/", p):
            # 4-cell lipsync mouth strips (script lss macro: ":a/4,0,3;sprites\SEC\2\...").
            # Cells must be scaled independently: whole-strip scaling makes cell
            # boundaries fractional and bleeds neighbor-cell pixels across them,
            # leaving a grey line around the mouth.
            return "mouth_strip"
        if p.startswith("sprites/"):
            return "sprite"
        if p.startswith("graphics/big_chars/") or p.startswith("graphics/chars/"):
            return "sprite"
        if p.startswith("graphics/cg/"):
            return "fullscreen_graphic"
        if p.startswith("graphics/menu/") or p.startswith("graphics/menu_en/"):
            return "script_ui"
        if p.startswith("graphics/system/"):
            return "script_ui"
        if p.startswith("graphics/thumb/"):
            return "thumbnail"
        if p.startswith("graphics/trailer/"):
            return "trailer_image"
        return "script_ui"
    if ext == ".ogg":
        if p.startswith("sound/voice/"):
            return "voice"
        if p.startswith("sound/bgm/") or p.startswith("legacy/bgm/"):
            return "bgm"
        if p.startswith("sound/se/") or p.startswith("sound/sysse/"):
            return "se"
        if p.startswith("sound/pam/"):
            return "pam"
        return "voice"
    if ext == ".mp4":
        return "normal_video"
    if ext == ".m2v":
        return "alpha_masked_video"
    if ext == ".bmp":
        return "copy"
    if ext in {".ttf", ".otf"}:
        return "copy"
    if ext == ".ass":
        return "copy"
    if ext in {".cfg", ".sav", ".db"}:
        return "copy"
    return "skip"


def escape_filter_path(p: str) -> str:
    return (
        p.replace("\\", "\\\\")
         .replace(":", "\\:")
         .replace("'", r"\'")
         .replace("[", "\\[")
         .replace("]", "\\]")
         .replace(",", "\\,")
    )


def find_ass_for_video(rel: Path, src_root: Path):
    base = rel.stem
    candidates = [
        src_root / rel.parent / f"{base}.ass",
        src_root / "video" / "sub" / f"{base}.ass",
        src_root / "legacy" / "sub" / f"{base}.ass",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def alpha_safe_scale(scale_expr_w: str, scale_expr_h: str, alpha: bool) -> str:
    """Downscale without alpha-edge artifacts.

    Straight-alpha lanczos blends the (black) RGB of transparent pixels into
    edge texels and its wide kernel dilutes/overshoots alpha at hard edges,
    leaving visible light/dark lines along e.g. the textbox border.
    Premultiplying first and using `area` (an exact NxN average at integral
    factors) keeps edge rows at their true color and alpha.
    """
    if alpha:
        return (
            f"format=rgba,premultiply=inplace=1,"
            f"scale={scale_expr_w}:{scale_expr_h}:flags=area,"
            f"unpremultiply=inplace=1,setsar=1,format=rgba"
        )
    return f"scale={scale_expr_w}:{scale_expr_h}:flags=area,setsar=1,format=rgb24"


def build_png_command(src: Path, dst: Path, kind: str, meta, scale: str):
    alpha = meta.get("has_alpha", False) or kind == "sprite"
    fmt = "rgba" if alpha else "rgb24"
    if kind in {"long_strip", "long_strip_placeholder"}:
        # Preserve the complete scrolling strip. The native engine's
        # GPUBigImage tiles images beyond the hardware texture dimensions.
        vf = alpha_safe_scale(
            f"'max(2,trunc(iw*{scale}))'",
            f"'max(2,trunc(ih*{scale}))'",
            alpha,
        )
    elif kind in {"background", "fullscreen_graphic", "large_png_scale_2_3"} and not alpha:
        # Opaque photographic art: lanczos keeps the most detail and has no
        # alpha edges to corrupt.
        vf = (
            f"scale='min({VITA_MAX_TEX},trunc(iw*{scale}))':'min({VITA_MAX_TEX},trunc(ih*{scale}))':"
            f"force_original_aspect_ratio=decrease:force_divisible_by=2:"
            f"flags=lanczos,setsar=1,format={fmt}"
        )
    elif kind in {"background", "fullscreen_graphic", "large_png_scale_2_3", "sprite"}:
        vf = alpha_safe_scale(
            f"'min({VITA_MAX_TEX},trunc(iw*{scale}))'",
            f"'min({VITA_MAX_TEX},trunc(ih*{scale}))'",
            alpha,
        )
    elif kind in {"script_ui", "thumbnail", "trailer_image"}:
        # Small files must be scaled too: the engine sizes every image as
        # pixels / render_scale, so an unscaled copy renders 1/scale times
        # too large. Only truly degenerate images (fill/marker pixels) are
        # copied verbatim.
        if meta.get("width", 0) <= 4 or meta.get("height", 0) <= 4:
            return ["copy", str(src), str(dst)]
        vf = alpha_safe_scale(
            f"'max(2,trunc(iw*{scale}))'",
            f"'max(2,trunc(ih*{scale}))'",
            alpha,
        )
    else:
        return ["copy", str(src), str(dst)]
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(src),
        "-vf", vf,
        "-compression_level", "9",
        "-pred", "mixed",
        str(dst),
    ]


MOUTH_STRIP_CELLS = 4


def build_mouth_strip_command(src: Path, dst: Path, meta, scale: str):
    """Scale a 4-cell lipsync strip cell-by-cell.

    Whole-strip scaling produces fractional cell boundaries (652/2=326,
    326/4=81.5) and lanczos bleeds adjacent cells into each other; the
    engine's integer cell cut then shows a grey seam around the mouth.
    Cropping each cell, scaling it alone, and re-stacking keeps cells
    integral and bleed-free.
    """
    w = meta.get("width", 0)
    h = meta.get("height", 0)
    if w <= 0 or h <= 0 or w % MOUTH_STRIP_CELLS != 0:
        return None  # caller falls back to plain sprite scaling
    cell_w = w // MOUTH_STRIP_CELLS
    scale_f = float(scale)
    cw = max(2, int(cell_w * scale_f))
    ch = max(2, int(h * scale_f))
    parts = [f"[0:v]format=rgba,premultiply=inplace=1,split={MOUTH_STRIP_CELLS}" +
             "".join(f"[i{i}]" for i in range(MOUTH_STRIP_CELLS))]
    for i in range(MOUTH_STRIP_CELLS):
        parts.append(
            f"[i{i}]crop={cell_w}:{h}:{cell_w * i}:0,"
            f"scale={cw}:{ch}:flags=area[o{i}]"
        )
    parts.append(
        "".join(f"[o{i}]" for i in range(MOUTH_STRIP_CELLS)) +
        f"hstack=inputs={MOUTH_STRIP_CELLS},unpremultiply=inplace=1,"
        f"setsar=1,format=rgba[out]"
    )
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(src),
        "-filter_complex", ";".join(parts),
        "-map", "[out]",
        "-compression_level", "9",
        "-pred", "mixed",
        str(dst),
    ]


def build_normal_video_command(src: Path, dst: Path, ass):
    common_scale = (
        "fps=30000/1001,"
        "scale=960:540:force_original_aspect_ratio=decrease:"
        "force_divisible_by=2:flags=bicubic,"
        "pad=960:544:(ow-iw)/2:(oh-ih)/2,setsar=1"
    )
    if ass:
        vf = (
            f"subtitles={escape_filter_path(str(ass))}"
            f":original_size=1920x1080,{common_scale}"
        )
    else:
        vf = common_scale
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(src),
        "-map", "0:v:0", "-map", "0:a:0?",
        "-map_metadata", "-1",
        "-vf", vf,
        "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.1",
        "-pix_fmt", "yuv420p",
        "-preset", "veryfast", "-crf", "28",
        "-x264-params", "ref=1:bframes=0:keyint=60:min-keyint=60:scenecut=0",
        "-c:a", "aac", "-profile:a", "aac_low",
        "-b:a", "96k", "-ar", "48000", "-ac", "2",
        "-movflags", "+faststart",
        str(dst),
    ]


def build_alpha_masked_command(src: Path, dst: Path):
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(src),
        "-vf", "scale=640:720:flags=bicubic,setsar=1",
        "-an",
        "-c:v", "mpeg2video", "-q:v", "10",
        "-f", "mpeg2video",
        str(dst),
    ]


def build_audio_command(kind: str, src: Path, dst: Path):
    if kind == "voice":
        ar, ac, br = "22050", "1", "48k"
    elif kind == "bgm":
        ar, ac, br = "22050", "2", "96k"
    elif kind == "se":
        ar, ac, br = "22050", "1", "48k"
    elif kind == "pam":
        ar, ac, br = "22050", "2", "64k"
    else:
        raise ValueError(f"unknown audio kind: {kind}")
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(src),
        "-vn", "-map_metadata", "-1",
        "-ar", ar, "-ac", ac,
        "-c:a", "libvorbis", "-b:a", br,
        str(dst),
    ]


def needs_conversion(src: Path, dst: Path) -> bool:
    if not dst.exists():
        return True
    try:
        return src.stat().st_mtime > dst.stat().st_mtime
    except OSError:
        return True


def walk_sources(src_root: Path):
    src_root_str = str(src_root)
    for dirpath, dirnames, filenames in os.walk(src_root):
        # Only prune SKIP_DIRS at the source root, not nested "save"/etc
        if dirpath == src_root_str:
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name in SKIP_NAMES or name.startswith("._"):
                continue
            yield Path(dirpath) / name


def plan_jobs(src_root: Path, out_root: Path, *, skip_audio: bool, scale: str):
    jobs = []
    for src in walk_sources(src_root):
        rel = src.relative_to(src_root)
        ext = src.suffix.lower()

        if skip_audio and ext == ".ogg":
            continue

        png_meta = None
        meta = {}
        if ext == ".png":
            png_meta = read_png_meta(src)
            if png_meta:
                meta.update(png_meta)
        kind = classify(rel, ext, png_meta)

        if kind == "skip":
            continue
        if skip_audio and kind in AUDIO_KINDS:
            continue

        dst = out_root / rel

        if kind == "copy":
            cmd = ["copy", str(src), str(dst)]
        elif kind == "mouth_strip":
            if not png_meta:
                continue
            cmd = build_mouth_strip_command(src, dst, png_meta, scale)
            if cmd is None:
                kind = "sprite"
                cmd = build_png_command(src, dst, kind, png_meta, scale)
        elif ext == ".png":
            if not png_meta:
                continue
            cmd = build_png_command(src, dst, kind, png_meta, scale)
        elif kind == "normal_video":
            ass = find_ass_for_video(rel, src_root)
            cmd = build_normal_video_command(src, dst, ass)
            if ass:
                meta["ass"] = str(ass.relative_to(src_root))
        elif kind == "alpha_masked_video":
            cmd = build_alpha_masked_command(src, dst)
        elif kind in AUDIO_KINDS:
            cmd = build_audio_command(kind, src, dst)
        else:
            continue

        jobs.append({
            "source": rel.as_posix(),
            "output": rel.as_posix(),
            "kind": kind,
            "src_path": str(src),
            "dst_path": str(dst),
            "command": cmd,
            "meta": meta,
        })
    return jobs


def run_job(job):
    src = Path(job["src_path"])
    dst = Path(job["dst_path"])
    dst.parent.mkdir(parents=True, exist_ok=True)
    # Convert beside the final output and publish only complete successes.
    # A failed ffmpeg process must not replace an already usable asset.
    with tempfile.NamedTemporaryFile(dir=dst.parent, prefix=".native-", suffix=dst.suffix, delete=False) as stream:
        temporary = Path(stream.name)
    cmd = list(job["command"])
    try:
        if cmd and cmd[0] == "copy":
            shutil.copy2(src, temporary)
            os.replace(temporary, dst)
            return job, 0, ""
        cmd[-1] = str(temporary)
        res = subprocess.run(cmd, capture_output=True, check=False)
        if res.returncode == 0:
            os.replace(temporary, dst)
        err = res.stderr.decode("utf-8", errors="replace") if res.returncode != 0 else ""
        return job, res.returncode, err
    except OSError as e:
        return job, 1, str(e)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build Vita-optimized assets.")
    parser.add_argument("--src", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--skip-audio", action="store_true",
        help="Skip OGG audio conversion entirely.",
    )
    parser.add_argument(
        "--scale", type=float, default=0.5,
        help="Image prescale factor. MUST equal the engine's --render-scale; "
             "the value is stamped into <out>/render_scale.txt, which the "
             "native engine reads at boot (absent marker = original size). "
             "Default: 0.5 (canvas ~= the Vita's 960x544 output).",
    )
    parser.add_argument(
        "--force", action="store_true",
        help="Re-convert even if output is newer than source.",
    )
    parser.add_argument(
        "--only", action="append",
        choices=[
            "background", "sprite", "mouth_strip", "fullscreen_graphic",
            "script_ui", "thumbnail", "trailer_image",
            "long_strip", "long_strip_placeholder", "large_png_scale_2_3",
            "normal_video", "alpha_masked_video",
            "voice", "bgm", "se", "pam", "copy",
        ],
        help="Only process these kinds. Repeatable.",
    )
    args = parser.parse_args()

    src_root = args.src.resolve()
    out_root = args.out.resolve()

    if not src_root.is_dir():
        print(f"--src is not a directory: {src_root}", file=sys.stderr)
        return 2
    if src_root == out_root:
        print("refusing: --src and --out are the same directory", file=sys.stderr)
        return 2
    if src_root.name == "game":
        print(
            "refusing: --src looks like the optimized output tree (assets/game). "
            "Move HD originals to assets/original_assets first.",
            file=sys.stderr,
        )
        return 2

    if not (0.25 <= args.scale <= 1.0):
        print(f"--scale {args.scale} outside sane range [0.25, 1.0]", file=sys.stderr)
        return 2
    scale = f"{args.scale:.7f}"

    print(f"Walking {src_root} ...")
    jobs = plan_jobs(src_root, out_root, skip_audio=args.skip_audio, scale=scale)
    all_jobs = jobs
    print(f"Planned {len(jobs)} job(s).")

    if args.only:
        only = set(args.only)
        if "long_strip_placeholder" in only:
            only.add("long_strip")
        before = len(jobs)
        jobs = [j for j in jobs if j["kind"] in only]
        print(f"Filtered to {len(jobs)}/{before} via --only.")

    out_root.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest_jobs = []
    counts = {}
    for j in all_jobs:
        counts[j["kind"]] = counts.get(j["kind"], 0) + 1
        manifest_jobs.append({
            "source": j["source"],
            "output": j["output"],
            "kind": j["kind"],
            "ffmpeg": j["command"],
            "meta": j["meta"],
        })
    args.manifest.write_text(json.dumps({
        "src": str(src_root),
        "out": str(out_root),
        "skip_audio": args.skip_audio,
        "scale": scale,
        "count": len(manifest_jobs),
        "counts_per_kind": counts,
        "jobs": manifest_jobs,
    }, indent=2))
    print(f"Manifest written: {args.manifest}")
    print("Per-kind counts:")
    for k in sorted(counts):
        print(f"  {k:30s} {counts[k]}")

    if args.dry_run:
        return 0

    todo = []
    for j in jobs:
        src = Path(j["src_path"])
        dst = Path(j["dst_path"])
        # Rebuild legacy cropped strips even when their timestamp is newer.
        strip_mismatch = j["kind"] == "long_strip" and png_size(dst) != (
            max(2, int(j["meta"]["width"] * args.scale)),
            max(2, int(j["meta"]["height"] * args.scale)),
        )
        if not args.force and not strip_mismatch and not needs_conversion(src, dst):
            continue
        todo.append(j)
    print(f"To convert: {len(todo)} (skipping {len(jobs) - len(todo)} up-to-date)")

    failures = 0
    failed_outputs = set()
    start = time.time()
    completed = 0
    last_print = 0.0

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_job, j) for j in todo]
        for fut in as_completed(futures):
            job, rc, err = fut.result()
            completed += 1
            if rc != 0:
                failures += 1
                failed_outputs.add(job["output"])
                msg = err.strip()[:500] if err else ""
                print(
                    f"FAIL [{job['kind']}] {job['source']}: rc={rc}\n{msg}",
                    file=sys.stderr,
                )
            now = time.time()
            if now - last_print >= 2.0 or completed == len(todo):
                last_print = now
                rate = completed / max(0.001, now - start)
                print(
                    f"  progress: {completed}/{len(todo)}  fail={failures}  "
                    f"{rate:.1f}/s"
                )

    dimensions, metadata_warnings = collect_dimensions(all_jobs, out_root, failed_outputs)
    write_dimensions(out_root, dimensions)
    for warning in metadata_warnings:
        print(f"Warning: {warning}", file=sys.stderr)
    if failures == 0:
        marker = out_root / ".render_scale.txt.tmp"
        marker.write_text(scale + "\n")
        marker.replace(out_root / "render_scale.txt")
        print(f"Stamped {out_root / 'render_scale.txt'} = {scale}")

    elapsed = time.time() - start
    print(
        f"Done in {elapsed:.1f}s. Converted {completed}, "
        f"failed {failures}."
    )
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
