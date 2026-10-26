#!/usr/bin/env python3
"""Verify a PRIVATE all-shard ARM64 link; never infer gameplay from linkage."""
import argparse
import hashlib
import json
from pathlib import Path
import re

# Map object ownership prevents weak/no-op application bridges masking real backends.
REQUIRED = {
    "dispatch_init": "dispatch.o", "dispatch_indirect": "dispatch.o",
    "rdtsc_shim": "dispatch.o",
    "win32_init": "win32.o", "win32_call": "win32.o", "com_call": "com.o",
    "win32_exit_scope_enter": "win32.o", "win32_exit_scope_leave": "win32.o",
    "win32_exit_checkpoint": "win32.o",
    "run_generated": "genexec.o", "overrides_install": "overrides.o",
    "sub_0041e2c0": "overrides.o", "sub_0041e2c0_orig": None,
    "sub_00406460": "overrides.o", "sub_00406460_orig": None,
    "nfsu2_input_report_rumble": "NFSU2InputBridge.o",
    "nfsu2_input_trigger_impact": "NFSU2InputBridge.o",
    "nfsu2_input_poll_rumble": "NFSU2InputBridge.o",
    "nfsu2_input_note_rumble_hook": "NFSU2InputBridge.o",
    "nfsu2_input_get_rumble_diagnostics": "NFSU2InputBridge.o",
    "metal_early_init": "metal.o", "metal_init_ios": "metal.o",
    "metal_render_frame": "metal.o", "metal_available": "metal.o",
    "metal_present": "metal.o", "nfs_game_session_start": "NFSU2GameSession.o",
    "audio_init": "audio.o", "audio_play": "audio.o",
    "audio_set_position": "audio.o", "audio_status": "audio.o",
    "audio_snapshot": "audio.o", "audio_get_freq": "audio.o",
    "metal_pipeline_cache_snapshot": "metal.o",
    "metal_runtime_perf_snapshot": "metal.o",
    "guest_host_wait_begin": "win32_ext.o",
    "guest_host_wait_end": "win32_ext.o",
    "guest_lock_snapshot": "win32_ext.o",
    "guest_frame_checkpoint": "win32_ext.o",
    "audio_publish_write": "audio.o",
    "audio_registration_available": "audio.o",
    "fileio_perf_snapshot": "fileio.o",
    "guest_pe_load": "guest_pe.o",
    "vs_to_msl": "vsemu.o", "fileio_call": "fileio.o",
    "g_fn_count": "fntab.o", "g_fn_ptr": "fntab.o", "g_imp_count": "impargs.o",
    "sub_0075bcc7": None,
}

def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def verify(map_text, nm_text):
    live = map_text.split("# Dead Stripped Symbols", 1)[0]
    objects = {int(index): path.strip() for index, path in
               re.findall(r"^\[\s*(\d+)\]\s+(.+)$", live, re.M)}
    symbols = {name: int(index) for index, name in
               re.findall(r"^0x[0-9A-Fa-f]+\s+0x[0-9A-Fa-f]+\s+\[\s*(\d+)\]\s+_(\w+)\s*$", live, re.M)}
    shard_ids = set()
    for path in objects.values():
        match = re.search(r"(?:^|/)shard_(\d{4})\.o$", path)
        if match:
            shard_ids.add(int(match[1]))
    if shard_ids != set(range(128)):
        raise ValueError(f"expected all 128 shard objects; got {len(shard_ids)}")
    evidence = {}
    for name, expected_object in REQUIRED.items():
        if name not in symbols:
            raise ValueError(f"missing live symbol: {name}")
        owner = objects.get(symbols[name], "")
        if expected_object and Path(owner).name != expected_object:
            raise ValueError(f"wrong owner for {name}: {owner}")
        rows = [line for line in nm_text.splitlines() if re.search(r"(?:^|\s)_" + re.escape(name) + r"$", line)]
        if len(rows) != 1 or "weak" in rows[0].lower() or "undefined" in rows[0].lower():
            raise ValueError(f"symbol is absent, weak or undefined: {name}")
        evidence[name] = owner
    unresolved = [line for line in nm_text.splitlines() if "(undefined)" in line]
    # Framework/dylib imports are expected. Guest/runtime undefineds are not.
    for line in unresolved:
        if re.search(r"_(?:sub_[0-9a-fA-F]+|dispatch_(?:init|indirect|report|patch)|g_fn_\w+|g_imp_\w+|metal_\w+|nfs_game_\w+)(?:\s|$)", line):
            raise ValueError("unresolved runtime symbol: " + line)
    return {"shard_object_count": len(shard_ids), "strong_symbols": evidence,
            "dynamic_undefined_count": len(unresolved), "dynamic_undefined_symbols": unresolved,
            "scope": "compile/link only; game boot, rendering, sound and device performance unverified"}

def main():
    p = argparse.ArgumentParser()
    for name in ("map", "nm", "binary", "manifest", "out"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--source", required=True)
    args = p.parse_args()
    if not re.fullmatch(r"[a-f0-9]{40}", args.source):
        raise ValueError("exact source commit required")
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("source_commit") != args.source or manifest.get("source_dirty") is not False:
        raise ValueError("generation must come from the exact clean pinned source commit")
    result = verify(args.map.read_text(), args.nm.read_text())
    result.update({"source_sha": args.source, "binary_sha256": sha256(args.binary),
                   "map_sha256": sha256(args.map), "nm_sha256": sha256(args.nm),
                   "generation_manifest_sha256": sha256(args.manifest), "generation": manifest})
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("PASS | all 128 shards and strong real runtime backends linked; gameplay unverified")

if __name__ == "__main__":
    main()
