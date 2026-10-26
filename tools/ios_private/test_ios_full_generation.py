from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import configure_project
import generate_shards


def write(path: Path, data: str | bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
    return path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_generated_fixture(root: Path, override_addresses: list[str], provenance: dict) -> dict:
    generated = root / "gen" / "all"
    generated.mkdir(parents=True, exist_ok=True)
    files = {}
    for index in range(128):
        name = f"shard_{index:04d}.c"
        path = write(generated / name, f"/* shard {index} */\n")
        files[name] = {"size_bytes": path.stat().st_size, "sha256": sha256(path)}
    generated_payloads = {
        "stubs.c": "void sub_00401000(CPU *c) {}\n",
        "fntab.c": "const uint32_t g_fn_count = 3;\n",
        "decls.h": "#ifndef DECLS_H\n#define DECLS_H\n#endif\n",
        "impargs.c": "const uint32_t g_imp_count = 4;\n",
    }
    for name, data in generated_payloads.items():
        path = write(generated / name, data)
        files[name] = {"size_bytes": path.stat().st_size, "sha256": sha256(path)}
    manifest = {
        "schema_version": 1,
        "source_commit": "a" * 40,
        "source_dirty": False,
        "source": {
            "path": "out/private/SPEED2.EXE",
            "size_bytes": 4788224,
            "sha256": configure_project.EXPECTED_EXE_SHA256,
        },
        "config": {
            "shard_count": 128,
            "override_addresses": override_addresses,
            "environment": {"MEMARG": "1", "RETEIP": "0", "X87OPT": "1"},
            "override_source": "tools/uret_v2.sh:OVR",
        },
        "summary": {
            "function_count": 3,
            "defined_count": 2,
            "missing_count": 1,
            "shards": 128,
            "import_count": 4,
        },
        "provenance": provenance,
        "files": files,
    }
    write(generated / configure_project.MANIFEST_NAME, json.dumps(manifest))
    return manifest


class GenerateShardsTests(unittest.TestCase):
    def test_reads_the_authoritative_override_list_from_v2_recipe(self):
        addresses = generate_shards._read_override_addresses(generate_shards.ROOT)
        self.assertEqual(len(addresses), 36)
        self.assertEqual(addresses[0], "0060fab0")
        self.assertEqual(addresses[-1], "00406460")
        self.assertEqual(len(set(addresses)), len(addresses))

    def test_rejects_non_128_runtime_configuration(self):
        with self.assertRaisesRegex(ValueError, "exactly 128"):
            generate_shards._check_generated_outputs(Path("."), 64, {})

    def test_generation_environment_clears_optional_instrumentation(self):
        with mock.patch.dict(
            "os.environ",
            {"HOOKS": "1234", "WATCH": "1", "COVERAGE": "1", "INLINE": "4"},
            clear=True,
        ):
            env = generate_shards._generation_environment(
                Path("C:/repo/gen/all"), ["0060fab0"], Path("C:/repo/out/flagabi.json")
            )
        self.assertEqual(env["OVERRIDE"], "0060fab0")
        self.assertEqual(env["MEMARG"], "1")
        self.assertEqual(env["RETEIP"], "0")
        self.assertEqual(env["X87OPT"], "1")
        self.assertNotIn("HOOKS", env)
        self.assertNotIn("WATCH", env)
        self.assertNotIn("COVERAGE", env)
        self.assertNotIn("INLINE", env)

    def test_checks_counts_and_hashes_for_every_generated_input(self):
        with tempfile.TemporaryDirectory() as temp:
            generated = Path(temp)
            for index in range(128):
                write(generated / f"shard_{index:04d}.c", f"/* {index} */")
            write(generated / "stubs.c", "void sub_00401000(CPU *c) {}\n")
            write(generated / "fntab.c", "const uint32_t g_fn_count = 3;\n")
            write(generated / "decls.h", "#define DECLS_H\n")
            write(generated / "impargs.c", "const uint32_t g_imp_count = 4;\n")
            summary = {
                "shards": 128,
                "function_count": 3,
                "defined_count": 2,
                "missing_count": 1,
            }
            files = generate_shards._check_generated_outputs(generated, 128, summary)
            self.assertEqual(len(files), 132)
            self.assertEqual(files["shard_0127.c"]["sha256"], sha256(generated / "shard_0127.c"))

    def test_manifest_binds_source_revision_config_counts_and_file_hashes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            generated = root / "gen" / "all"
            generated.mkdir(parents=True)
            write(generated / "impargs.c", "const uint32_t g_imp_count = 7;\n")
            summary = {
                "shards": 128,
                "function_count": 9,
                "defined_count": 7,
                "missing_count": 2,
                "bytes": 1234,
                "traps": 5,
                "truncated": 0,
            }
            source = {"path": "out/private/SPEED2.EXE", "size_bytes": 4788224, "sha256": "b" * 64}
            files = {"impargs.c": {"size_bytes": 32, "sha256": sha256(generated / "impargs.c")}}
            env = {"X87OPT": "1", "RETEIP": "0", "MEMARG": "1"}
            provenance = {"tools/genall.py": {"size_bytes": 10, "sha256": "c" * 64}}
            intermediates = {"out/relocs.txt": {"size_bytes": 12, "sha256": "e" * 64}}
            with mock.patch.object(generate_shards, "_source_revision", return_value=("d" * 40, True)):
                path = generate_shards._write_manifest(
                    generated, source, ["0060fab0"], env, summary, files, provenance,
                    intermediates, root
                )
            manifest = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(path.name, "ios-full-manifest.json")
            self.assertEqual(manifest["source_commit"], "d" * 40)
            self.assertTrue(manifest["source_dirty"])
            self.assertEqual(manifest["source"], source)
            self.assertEqual(manifest["config"]["override_addresses"], ["0060fab0"])
            self.assertEqual(manifest["summary"]["function_count"], 9)
            self.assertEqual(manifest["summary"]["import_count"], 7)
            self.assertEqual(manifest["files"], files)
            self.assertEqual(manifest["provenance"]["generated_inputs"], intermediates)


class ConfigureProjectTests(unittest.TestCase):
    def _fixture(self, root: Path) -> tuple[dict, list[str]]:
        addresses = ["00112233", "0060fab0"]
        recipe = "#!/bin/bash\nOVR=" + ",".join(addresses) + "\n"
        write(root / "tools" / "uret_v2.sh", recipe)
        provenance_paths = (
            "tools/ios_private/generate_shards.py",
            "tools/ios_private/configure_project.py",
            "tools/genall.py",
            "tools/lift.py",
            "tools/reloc2.py",
            "tools/flagabi.py",
            "tools/argcount.py",
            "runtime/overrides.c",
            "runtime/d3dx_hle.c",
        )
        for relative in provenance_paths:
            write(root / relative, f"/* provenance: {relative} */\n")
        write(root / "SURUM", "27\n")
        for name in (
            *configure_project.FULL_RUNTIME_C_SOURCES,
            *configure_project.FULL_RUNTIME_EXTRA_C_SOURCES,
        ):
            write(root / "runtime" / f"{name}.c", "/* source fixture */\n")
        for name in configure_project.FULL_RUNTIME_OBJC_SOURCES:
            write(root / "runtime" / f"{name}.m", "/* source fixture */\n")
        write(root / "ios" / "NFSU2iOS" / "Native" / "NFSU2GameSession.m", "/* session */\n")
        write(root / "ios" / "NFSU2iOS" / "Native" / "NFSU2GameSession.h", "/* session */\n")
        provenance = generate_shards._tool_provenance(root)
        make_generated_fixture(root, addresses, provenance)
        project = {
            "name": "NFSU2iOS",
            "targets": {
                "NFSU2iOS": {
                    "sources": [{"path": "NFSU2iOS"}, {"path": "../runtime/guest_execution.c"}],
                    "dependencies": [{"sdk": "Metal.framework"}],
                    "settings": {
                        "base": {
                            "PRODUCT_BUNDLE_IDENTIFIER": "dev.lyszq.nfsu2ios",
                            "GCC_STRICT_ALIASING": "NO",
                            "GCC_PREPROCESSOR_DEFINITIONS": ["$(inherited)"],
                            "HEADER_SEARCH_PATHS": ["$(inherited)", "$(SRCROOT)/../runtime"],
                        }
                    },
                    "info": {"properties": {"CFBundleShortVersionString": "old", "CFBundleVersion": "old"}},
                }
            },
        }
        write(root / "ios" / "project.yml", yaml.safe_dump(project))
        return project, addresses

    def test_all_mode_uses_full_runtime_and_excludes_bounded_probe_sources(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            original, _ = self._fixture(root)
            output = configure_project.configure_project("all", "ios/project-full.yml", root)
            configured = yaml.safe_load(output.read_text(encoding="utf-8"))
            target = configured["targets"]["NFSU2iOS"]
            sources = target["sources"]
            paths = {source["path"] for source in sources if "path" in source}
            self.assertIn("../runtime/dispatch.c", paths)
            self.assertIn("../runtime/genexec.c", paths)
            self.assertIn("../runtime/metal.m", paths)
            self.assertIn("../runtime/audio.m", paths)
            self.assertIn("../runtime/guest_cursor.c", paths)
            self.assertIn("../gen/all/shard_0127.c", paths)
            self.assertIn("../gen/all/impargs.c", paths)
            self.assertNotIn("../runtime/guest_execution.c", paths)
            self.assertNotIn("../runtime/guest_windows.c", paths)
            self.assertNotIn("../runtime/bundle.m", paths)
            self.assertNotIn("tests/boot.c", paths)
            self.assertNotIn({"sdk": "AudioUnit.framework"}, target["dependencies"])
            self.assertEqual(len([p for p in paths if re_fullmatch_shard(p)]), 128)

            app_group = next(source for source in sources if source.get("path") == "NFSU2iOS")
            self.assertIn("Native/GuestImageProbe.c", app_group["excludes"])
            self.assertIn("Native/GuestSmoke.c", app_group["excludes"])
            self.assertIn("Native/NFSU2MetalBridge.c", app_group["excludes"])
            self.assertIn("MetalPreview.swift", app_group["excludes"])

            base = target["settings"]["base"]
            cdefs = base["GCC_PREPROCESSOR_DEFINITIONS"]
            self.assertIn("NFS_FULL_RUNTIME=1", cdefs)
            self.assertIn("NFS_ALL_SHARDS=1", cdefs)
            self.assertIn("NFSU2_LEGACY_METAL_INPUT=1", cdefs)
            self.assertIn("NFS_ENHANCED=0", cdefs)
            self.assertEqual(base["GCC_STRICT_ALIASING"], "NO")
            self.assertEqual(base["CLANG_ENABLE_OBJC_ARC"], "YES")
            self.assertIn("NFS_FULL_RUNTIME", base["SWIFT_ACTIVE_COMPILATION_CONDITIONS"])
            self.assertEqual(base["PRODUCT_BUNDLE_IDENTIFIER"], "dev.lyszq.nfsu2ios")
            self.assertEqual(target["info"]["properties"]["CFBundleShortVersionString"], "0.27.1")
            self.assertEqual(target["info"]["properties"]["CFBundleVersion"], "36")
            self.assertIs(target["info"]["properties"]["UIFileSharingEnabled"], True)
            self.assertIs(target["info"]["properties"]["LSSupportsOpeningDocumentsInPlace"], True)
            self.assertEqual(original["targets"]["NFSU2iOS"]["sources"][1]["path"], "../runtime/guest_execution.c")

    def test_all_mode_rejects_tampered_shard(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self._fixture(root)
            write(root / "gen" / "all" / "shard_0053.c", "tampered\n")
            with self.assertRaisesRegex(SystemExit, "does not match manifest"):
                configure_project.configure_project("all", "ios/project-full.yml", root)

    def test_all_mode_requires_persistent_game_session_adapter(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self._fixture(root)
            (root / "ios" / "NFSU2iOS" / "Native" / "NFSU2GameSession.m").unlink()
            with self.assertRaisesRegex(SystemExit, "NFSU2GameSession.m"):
                configure_project.configure_project("all", "ios/project-full.yml", root)


def re_fullmatch_shard(path: str) -> bool:
    import re
    return re.fullmatch(r"\.\./gen/all/shard_\d{4}\.c", path) is not None


if __name__ == "__main__":
    unittest.main()
