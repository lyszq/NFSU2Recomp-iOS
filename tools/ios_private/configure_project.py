#!/usr/bin/env python3
"""Create a build-host-only XcodeGen overlay for private iOS builds."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

import yaml

ROOT = Path(__file__).resolve().parents[2]
MANIFEST_NAME = "ios-full-manifest.json"
EXPECTED_EXE_SHA256 = "8f6a9ae2c4cfa01181d2fd755d99c6f4b7103f60273908ea1de1e719dfe2a297"
FULL_RUNTIME_SHARDS = 128

# Keep this inventory in sync with the desktop runtime recipe in link.sh.
# bundle.m and tests/boot.c are deliberately platform-host pieces: the iOS app
# session supplies the host entry and lifecycle while UIKit owns presentation.
FULL_RUNTIME_C_SOURCES = (
    "mem",
    "guest_memory",
    "guest_pe",
    "guest_dispatch",
    "guest_bootstrap",
    "dispatch",
    "genexec",
    "win32",
    "win32_ext",
    "com",
    "debug",
    "res",
    "fileio",
    "overrides",
    "d3dx_hle",
    "vsemu",
    "enhance",
    "perflog",
)
FULL_RUNTIME_OBJC_SOURCES = ("metal", "audio")
FULL_RUNTIME_EXTRA_C_SOURCES = ("guest_cursor",)
FULL_RUNTIME_EXCLUDED_APP_SOURCES = (
    "Native/GuestImageProbe.c",
    "Native/GuestSmoke.c",
    "Native/NFSU2MetalBridge.c",
    "MetalPreview.swift",
)
FULL_RUNTIME_FRAMEWORKS = (
    "CoreGraphics.framework",
    "ImageIO.framework",
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _expected_generated_names() -> list[str]:
    return (
        [f"shard_{index:04d}.c" for index in range(FULL_RUNTIME_SHARDS)]
        + ["stubs.c", "fntab.c", "decls.h", "impargs.c"]
    )


def _validate_full_generation(root: Path) -> dict[str, Any]:
    generated_dir = root / "gen" / "all"
    manifest_path = generated_dir / MANIFEST_NAME
    if not manifest_path.is_file():
        raise SystemExit(
            f"Full-runtime generation is required; missing gen/all/{MANIFEST_NAME}. "
            "Run tools/ios_private/generate_shards.py first."
        )
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"Invalid full-runtime generation manifest: {exc}") from exc
    if manifest.get("schema_version") != 1:
        raise SystemExit("Unsupported full-runtime manifest schema")
    source = manifest.get("source", {})
    if source.get("size_bytes") != 4788224 or source.get("sha256") != EXPECTED_EXE_SHA256:
        raise SystemExit("Full-runtime manifest does not identify the authenticated SPEED2.EXE")
    if not re.fullmatch(r"[0-9a-f]{40}", str(manifest.get("source_commit", ""))):
        raise SystemExit("Full-runtime manifest is missing its exact source_commit SHA")

    config = manifest.get("config", {})
    summary = manifest.get("summary", {})
    if config.get("shard_count") != FULL_RUNTIME_SHARDS or summary.get("shards") != FULL_RUNTIME_SHARDS:
        raise SystemExit(f"Full runtime requires exactly {FULL_RUNTIME_SHARDS} generated shards")
    if config.get("environment") != {"MEMARG": "1", "RETEIP": "0", "X87OPT": "1"}:
        raise SystemExit("Full-runtime manifest has unexpected v2 generation settings")
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from generate_shards import _read_override_addresses, _tool_provenance
        expected_overrides = _read_override_addresses(root)
        expected_provenance = _tool_provenance(root)
    finally:
        try:
            sys.path.remove(str(Path(__file__).resolve().parent))
        except ValueError:
            pass
    if config.get("override_source") != "tools/uret_v2.sh:OVR":
        raise SystemExit("Full-runtime manifest does not identify the canonical override list")
    if config.get("override_addresses") != expected_overrides:
        raise SystemExit("Full-runtime manifest override addresses differ from tools/uret_v2.sh")
    recorded_provenance = manifest.get("provenance", {})
    for relative, expected in expected_provenance.items():
        if recorded_provenance.get(relative) != expected:
            raise SystemExit(f"Full-runtime generation provenance differs from current source: {relative}")

    files = manifest.get("files")
    expected_names = _expected_generated_names()
    if not isinstance(files, dict) or set(files) != set(expected_names):
        raise SystemExit("Full-runtime manifest file list does not contain exactly 128 shards and support files")
    for filename in expected_names:
        record = files.get(filename)
        path = generated_dir / filename
        if not isinstance(record, dict) or not path.is_file():
            raise SystemExit(f"Missing generated full-runtime source: gen/all/{filename}")
        if path.stat().st_size == 0 or record.get("size_bytes") != path.stat().st_size:
            raise SystemExit(f"Generated source size does not match manifest: gen/all/{filename}")
        if record.get("sha256") != _sha256(path):
            raise SystemExit(f"Generated source SHA-256 does not match manifest: gen/all/{filename}")

    function_count = summary.get("function_count")
    defined_count = summary.get("defined_count")
    missing_count = summary.get("missing_count")
    if not all(isinstance(value, int) and value >= 0
               for value in (function_count, defined_count, missing_count)):
        raise SystemExit("Full-runtime manifest has invalid function counts")
    if defined_count + missing_count != function_count:
        raise SystemExit("Full-runtime manifest function counts do not add up")
    fntab = (generated_dir / "fntab.c").read_text(encoding="utf-8")
    count_match = re.search(r"\bg_fn_count\s*=\s*(\d+)\s*;", fntab)
    if not count_match or int(count_match.group(1)) != function_count:
        raise SystemExit("fntab.c count does not match the manifest function count")
    stubs = (generated_dir / "stubs.c").read_text(encoding="utf-8")
    stub_count = len(re.findall(r"\bvoid\s+sub_[0-9a-fA-F]{8}\s*\(", stubs))
    if stub_count != missing_count:
        raise SystemExit("stubs.c fallback count does not match the manifest")
    impargs = (generated_dir / "impargs.c").read_text(encoding="utf-8")
    import_count_match = re.search(r"\bg_imp_count\s*=\s*(\d+)\s*;", impargs)
    if not import_count_match:
        raise SystemExit("impargs.c is missing its generated import count")
    if int(import_count_match.group(1)) != summary.get("import_count"):
        raise SystemExit("impargs.c import count does not match the generation manifest")
    return manifest


def _full_runtime_sources(root: Path) -> list[dict[str, Any]]:
    native_dir = root / "ios" / "NFSU2iOS" / "Native"
    session_source = native_dir / "NFSU2GameSession.m"
    session_header = native_dir / "NFSU2GameSession.h"
    missing = [path.relative_to(root).as_posix() for path in (session_source, session_header) if not path.is_file()]
    if missing:
        raise SystemExit(
            "Full-runtime iOS session adapter is required; missing " + ", ".join(missing)
        )

    app_group = {
        "path": "NFSU2iOS",
        "excludes": list(FULL_RUNTIME_EXCLUDED_APP_SOURCES),
    }
    sources: list[dict[str, Any]] = [app_group]
    runtime_sources = [
        *(f"../runtime/{name}.c" for name in FULL_RUNTIME_C_SOURCES),
        *(f"../runtime/{name}.c" for name in FULL_RUNTIME_EXTRA_C_SOURCES),
        *(f"../runtime/{name}.m" for name in FULL_RUNTIME_OBJC_SOURCES),
    ]
    for relative in runtime_sources:
        absolute = (root / "ios" / relative).resolve()
        if not absolute.is_file():
            raise SystemExit(f"Required full-runtime source is missing: {relative}")
        sources.append({"path": relative})

    for filename in _expected_generated_names():
        sources.append({"path": f"../gen/all/{filename}"})
    return sources


def _add_values(setting: dict[str, Any], key: str, values: list[str]) -> None:
    current = setting.get(key, ["$(inherited)"])
    if isinstance(current, str):
        current = [current]
    setting[key] = list(dict.fromkeys(list(current) + values))


def _read_version(root: Path) -> str:
    version_path = root / "SURUM"
    if not version_path.is_file():
        return "0"
    return "".join(re.findall(r"\d", version_path.read_text(encoding="utf-8"))) or "0"


def _configure_full_mode(root: Path, spec: dict[str, Any]) -> None:
    manifest = _validate_full_generation(root)
    target = spec["targets"]["NFSU2iOS"]
    target["info"]["properties"]["NFSRuntimeSourceSHA"] = manifest["source_commit"]
    target["sources"] = _full_runtime_sources(root)
    base = target["settings"]["base"]
    _add_values(base, "GCC_PREPROCESSOR_DEFINITIONS", [
        "NFS_ALL_SHARDS=1",
        "NFS_FULL_RUNTIME=1",
        "NFSU2_LEGACY_METAL_INPUT=1",
        "NFS_ENHANCED=0",
        f"NFS_SURUM={_read_version(root)}",
    ])
    _add_values(base, "SWIFT_ACTIVE_COMPILATION_CONDITIONS", ["NFS_FULL_RUNTIME"])
    _add_values(base, "HEADER_SEARCH_PATHS", [
        "$(SRCROOT)/../runtime",
        "$(SRCROOT)/../gen/all",
        "$(SRCROOT)/NFSU2iOS/Native",
    ])
    base["CLANG_ENABLE_OBJC_ARC"] = "YES"
    base["GCC_STRICT_ALIASING"] = "NO"
    target["dependencies"] = list(target.get("dependencies", []))
    for framework in FULL_RUNTIME_FRAMEWORKS:
        item = {"sdk": framework}
        if item not in target["dependencies"]:
            target["dependencies"].append(item)


def configure_project(mode: str = "auto", out: str | Path | None = None,
                      root: Path = ROOT) -> Path:
    root = Path(root).resolve()
    if mode == "auto":
        if (root / "gen/ios-private/startup_probe.c").is_file():
            mode = "startup"
        elif (root / "gen/all/ios-full-manifest.json").is_file():
            mode = "all"
        else:
            mode = "startup"

    spec = yaml.safe_load((root / "ios/project.yml").read_text(encoding="utf-8"))
    target = spec["targets"]["NFSU2iOS"]
    base = target["settings"]["base"]

    if mode == "startup":
        required = ["lifted.c", "private_probe.c", "startup_lifted.c", "startup_probe.c", "manifest.json"]
        for filename in required:
            if not (root / "gen/ios-private" / filename).is_file():
                raise SystemExit(
                    f"Private generation is required; no stub fallback "
                    f"(missing gen/ios-private/{filename})"
                )
        target["sources"].extend(
            {"path": "../gen/ios-private/" + name}
            for name in ["lifted.c", "private_probe.c", "startup_lifted.c", "startup_probe.c"]
        )
        _add_values(base, "GCC_PREPROCESSOR_DEFINITIONS", [
            "NFS_PRIVATE_PROBE=1", "NFS_PRIVATE_STARTUP=1",
        ])
    elif mode == "all":
        _configure_full_mode(root, spec)
    else:
        raise ValueError(f"Unknown project overlay mode: {mode}")

    current_hsp = base.get("HEADER_SEARCH_PATHS", ["$(inherited)", "$(SRCROOT)/../runtime"])
    if isinstance(current_hsp, str):
        current_hsp = [current_hsp]
    extra_hsp = ["$(SRCROOT)/../runtime", "$(SRCROOT)/NFSU2iOS/Native"]
    if mode == "all":
        extra_hsp.append("$(SRCROOT)/../gen/all")
    base["HEADER_SEARCH_PATHS"] = list(dict.fromkeys(list(current_hsp) + extra_hsp))

    version, build = ("0.27.1", "36") if mode == "all" else ("0.19.0", "26")
    target["info"]["properties"]["UIFileSharingEnabled"] = True
    target["info"]["properties"]["LSSupportsOpeningDocumentsInPlace"] = True
    spec["targets"]["NFSU2iOS"]["info"]["properties"]["CFBundleShortVersionString"] = version
    spec["targets"]["NFSU2iOS"]["info"]["properties"]["CFBundleVersion"] = build
    out_path = Path(out) if out is not None else root / "ios" / "project-private.yml"
    if not out_path.is_absolute():
        out_path = root / out_path
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(yaml.safe_dump(spec, sort_keys=False), encoding="utf-8", newline="\n")
    print(f"Configured {out_path.name} in '{mode}' mode (v{version}, build {build})")
    return out_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Configure an XcodeGen project overlay for iOS")
    parser.add_argument(
        "--mode", choices=["auto", "startup", "all"], default="auto",
        help="'startup' adds the bounded entry probe; 'all' builds the full 128-shard runtime",
    )
    parser.add_argument(
        "--out", type=str, default=None,
        help="Output YAML file path (default: ios/project-private.yml)",
    )
    args = parser.parse_args(argv)
    configure_project(args.mode, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
