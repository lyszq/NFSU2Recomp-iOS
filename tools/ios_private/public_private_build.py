#!/usr/bin/env python3
"""Public controller with confidential subprocess IO and private release storage.

No CLI accepts a token, input URL, source ref, output repository or shell command.
The public log is a small allowlist. Exceptions and subprocess output never reach
stdout/stderr. This is containment against mistakes, not a malicious-code sandbox.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

PUBLIC = "lyszq/NFSU2Recomp-iOS"
PRIVATE = "lyszq/NFSU2Recomp-ios-private"
PRIVATE_ID = 1427055553
MARKER = "Dedicated private native iOS build control for NFSU2Recomp"
BRANCH = "feat/ios-guest-runtime-smoke"
ENVIRONMENT = "private-ios-build"
EXE_SHA = "8f6a9ae2c4cfa01181d2fd755d99c6f4b7103f60273908ea1de1e719dfe2a297"
EXE_SIZE = 4788224
IPA = "NFSU2iOS-full-runtime-unsigned.ipa"
MESSAGES = {
    "preflight": "Checking trusted run and protected environment.",
    "tools": "Preparing build tools; detailed output remains private.",
    "input": "Authenticating private input.",
    "generate": "Generating the complete runtime privately.",
    "compile": "Compiling the complete ARM64 iOS runtime privately.",
    "verify": "Verifying all 128 shards, real backends and IPA package.",
    "upload": "Transferring verified IPA and evidence to the private repository.",
    "success": "PASS: verified full-runtime IPA stored only in the private repository.",
    "failure": "FAIL: private build stopped. Inspect the private draft release for diagnostics.",
    "unavailable": "FAIL: private IO unavailable; no public diagnostic fallback.",
}


def announce(key):
    print(MESSAGES[key], flush=True)


class ClosedError(Exception):
    """Details must never be formatted into public output."""


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, hdrs, newurl):
        return None


class PrivateIO:
    def __init__(self, token):
        if not token or "\n" in token or "\r" in token:
            raise ClosedError("missing credential")
        self.token = token
        self.opener = urllib.request.build_opener(NoRedirect)

    def request(self, path, method="GET", body=None, binary=None, raw=False):
        prefix = "/repos/" + PRIVATE
        if path != prefix and not path.startswith(prefix + "/"):
            raise ClosedError("private API path only")
        host = "uploads.github.com" if binary is not None else "api.github.com"
        headers = {"Authorization": "Bearer " + self.token,
                   "Accept": "application/octet-stream" if raw else "application/vnd.github+json",
                   "User-Agent": "NFSU2-private-io", "X-GitHub-Api-Version": "2022-11-28"}
        data = json.dumps(body).encode() if body is not None else binary
        if data is not None:
            headers["Content-Type"] = "application/octet-stream" if binary is not None else "application/json"
        req = urllib.request.Request("https://" + host + path, data=data, headers=headers, method=method)
        try:
            with self.opener.open(req, timeout=180) as response:
                content = response.read(EXE_SIZE + 1 if raw else 8 * 1024 * 1024)
        except urllib.error.HTTPError as error:
            if not raw or error.code != 302:
                raise ClosedError("private API request rejected") from None
            # The signed asset redirect is fetched WITHOUT the Authorization header.
            url = error.headers.get("Location", "")
            parsed = urllib.parse.urlsplit(url)
            if (parsed.scheme != "https" or parsed.hostname not in {
                    "release-assets.githubusercontent.com", "objects.githubusercontent.com"}
                    or parsed.username or parsed.password or parsed.port not in (None, 443)):
                raise ClosedError("untrusted download redirect") from None
            with self.opener.open(urllib.request.Request(url, headers={"User-Agent": "NFSU2-private-io"}), timeout=180) as response:
                content = response.read(EXE_SIZE + 1)
        return content if raw else json.loads(content)

    def private(self):
        repo = self.request("/repos/" + PRIVATE)
        if (repo.get("private") is not True or repo.get("id") != PRIVATE_ID
                or repo.get("full_name") != PRIVATE or repo.get("description") != MARKER):
            raise ClosedError("private destination identity failed")
        return repo

    def begin(self, run_id, attempt, source):
        repo = self.private()
        # Resolve the *current default branch*. No workflow-write permission or
        # arbitrary caller-controlled release target is needed.
        ref = self.request(f"/repos/{PRIVATE}/git/ref/heads/{repo['default_branch']}")
        target = ref["object"]["sha"]
        if not re.fullmatch(r"[a-f0-9]{40}", target):
            raise ClosedError("invalid private release target")
        return self.request(f"/repos/{PRIVATE}/releases", "POST", body={
            "tag_name": f"ios-public-{run_id}-{attempt}", "target_commitish": target,
            "name": f"Full ARM64 IPA / public build {run_id} attempt {attempt}",
            "body": f"Confidential build output. Public source: {source}.\n"
                    "Build pending; inspect receipt.json for completion. Unsigned; gameplay unverified.",
            "draft": True, "prerelease": True, "generate_release_notes": False})

    def input(self, destination):
        self.private()
        release = self.request(f"/repos/{PRIVATE}/releases/tags/private-input-v1")
        assets = self.request(f"/repos/{PRIVATE}/releases/{int(release['id'])}/assets?per_page=100")
        matches = [a for a in assets if a.get("name") == "SPEED2.EXE"]
        if len(matches) != 1 or matches[0].get("size") != EXE_SIZE or matches[0].get("digest") != "sha256:" + EXE_SHA:
            raise ClosedError("unsupported input metadata")
        asset = matches[0]
        data = self.request(f"/repos/{PRIVATE}/releases/assets/{int(asset['id'])}", raw=True)
        if len(data) != EXE_SIZE or hashlib.sha256(data).hexdigest() != EXE_SHA:
            raise ClosedError("unsupported input bytes")
        destination.write_bytes(data)
        return {"release_id": release["id"], "asset_id": asset["id"], "sha256": EXE_SHA}

    def upload(self, release_id, path):
        self.private()  # Recheck before EVERY write of confidential output.
        path = Path(path)
        result = self.request(f"/repos/{PRIVATE}/releases/{int(release_id)}/assets?name=" +
                              urllib.parse.quote(path.name, safe=""), "POST", binary=path.read_bytes())
        if result.get("size") != path.stat().st_size or result.get("digest") != "sha256:" + sha256(path):
            raise ClosedError("private upload digest not confirmed")
        return {"id": result["id"], "name": path.name, "size": result["size"], "sha256": sha256(path)}


def public_json(path):
    # Environment policy is public metadata. NEVER send the private credential.
    req = urllib.request.Request("https://api.github.com/repos/" + PUBLIC + path,
                                 headers={"User-Agent": "NFSU2-private-io"})
    with urllib.request.build_opener(NoRedirect).open(req, timeout=60) as response:
        return json.load(response)


def validate_policy(environment, branches):
    policy = environment.get("deployment_branch_policy") or {}
    reviewers = [r for r in environment.get("protection_rules", []) if r.get("type") == "required_reviewers"]
    if (policy != {"protected_branches": False, "custom_branch_policies": True}
            or len(reviewers) != 1
            or not any(r.get("type") == "User" and r.get("reviewer", {}).get("login") == "lyszq"
                       for r in reviewers[0].get("reviewers", []))
            or [(p.get("name"), p.get("type")) for p in branches.get("branch_policies", [])] != [(BRANCH, "branch")]):
        raise ClosedError("environment must require owner review and the exact trusted branch")


def validate_context(env):
    if (env.get("GITHUB_ACTIONS") != "true" or env.get("GITHUB_REPOSITORY") != PUBLIC
            or env.get("GITHUB_REF") != "refs/heads/" + BRANCH
            or env.get("GITHUB_EVENT_NAME") not in {"push", "workflow_dispatch"}
            or env.get("RUNNER_ENVIRONMENT") != "github-hosted"
            or env.get("RUNNER_OS") != "macOS"
            or env.get("GITHUB_WORKFLOW_REF") != PUBLIC + "/.github/workflows/ios-private-io.yml@refs/heads/" + BRANCH
            or env.get("GITHUB_SHA") != env.get("SOURCE_SHA")
            or not re.fullmatch(r"[a-f0-9]{40}", env.get("SOURCE_SHA", ""))
            or not all(re.fullmatch(r"[1-9][0-9]*", env.get(k, "")) for k in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"))):
        raise ClosedError("untrusted run context")
    if any(env.get(k, "").lower() in {"true", "1"} for k in ("ACTIONS_STEP_DEBUG", "ACTIONS_RUNNER_DEBUG", "RUNNER_DEBUG")):
        raise ClosedError("debug logging forbidden")


def child_environment(env, work):
    # Positive allowlist: no PAT, Actions artifact/cache/OIDC/command channels,
    # GITHUB_TOKEN, credentials, PYTHONPATH, shell startup hooks or git config.
    result = {k: env[k] for k in ("PATH", "USER", "LOGNAME", "DEVELOPER_DIR", "SDKROOT", "SYSTEMROOT") if k in env}
    for name in ("home", "tmp", "cache", "modules"):
        (work / name).mkdir(exist_ok=True)
    result.update({"HOME": str(work / "home"), "TMPDIR": str(work / "tmp") + "/",
                   "XDG_CACHE_HOME": str(work / "cache"), "LC_ALL": "en_US.UTF-8",
                   "LANG": "en_US.UTF-8", "PYTHONNOUSERSITE": "1", "PYTHONUNBUFFERED": "1",
                   "PIP_CONFIG_FILE": os.devnull, "PIP_DISABLE_PIP_VERSION_CHECK": "1",
                   "HOMEBREW_NO_AUTO_UPDATE": "1", "HOMEBREW_NO_ANALYTICS": "1",
                   "GIT_CONFIG_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0"})
    return result


class Builder:
    def __init__(self, work, env):
        self.work, self.env = work, env
        self.source = work / "source"
        self.evidence = work / "evidence"
        self.evidence.mkdir()

    def run(self, args, cwd=None, output="build.log"):
        with (self.evidence / output).open("ab") as log:
            # No shell, no inherited stdout, stderr, stdin or action command files.
            subprocess.run([str(a) for a in args], cwd=cwd or self.source,
                           env=self.env, stdin=subprocess.DEVNULL, stdout=log,
                           stderr=subprocess.STDOUT, check=True)

    def build(self, source):
        announce("tools")
        self.run(["git", "clone", "--no-local", "--no-hardlinks", str(Path(__file__).resolve().parents[2]), self.source], cwd=self.work)
        self.run(["git", "checkout", "--detach", source])
        python = self.work / "python/bin/python"
        self.run(["python3", "-m", "venv", self.work / "python"])
        self.run([python, "-m", "pip", "install", "--require-hashes", "--only-binary=:all:", "--no-deps",
                  "-r", "tools/ios_private/requirements-public-private.lock"])
        self.run([python, "-m", "unittest", "discover", "-s", "tools/ios_private", "-p", "test_*.py"])
        self.run(["bash", "tests/run_guest_tests.sh", self.work / "guest-tests"])
        # The reviewed runner image's XcodeGen is preferred; installation runs
        # before downloading game input and its output remains private.
        if shutil.which("xcodegen", path=self.env["PATH"]) is None:
            self.run(["brew", "install", "xcodegen"])
        # Exercise actual project generation before downloading confidential input.
        # XcodeGen needs USER/LOGNAME even with an isolated HOME. This creates only
        # ignored diagnostic project/plist files, replaced by the full overlay later.
        self.run(["xcodegen", "generate", "--spec", "project.yml"], cwd=self.source / "ios")
        self.run(["xcodebuild", "-version"], output="toolchain.txt")
        self.run(["xcrun", "--sdk", "iphoneos", "--show-sdk-version"], output="toolchain.txt")
        return python

    def compile(self, source, exe, python):
        announce("generate")
        self.run([python, "tools/ios_private/generate_shards.py", "--exe", exe, "--shards", "128"])
        manifest = self.source / "gen/all/ios-full-manifest.json"
        shutil.copyfile(manifest, self.evidence / "full-runtime-provenance.json")
        self.run([self.work / "guest-tests/full-pe-adapter-test", exe])
        self.run([python, "tools/ios_private/configure_project.py", "--mode", "all"])
        self.run(["xcodegen", "generate", "--spec", "project-private.yml"], cwd=self.source / "ios")
        announce("compile")
        derived = self.work / "DerivedData"
        self.run(["xcodebuild", "-project", "NFSU2iOS.xcodeproj", "-scheme", "NFSU2iOS", "-configuration", "Release",
                  "-sdk", "iphoneos", "-destination", "generic/platform=iOS", "-jobs", "3", "-derivedDataPath", derived,
                  "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO", "CODE_SIGN_IDENTITY=",
                  "CLANG_MODULE_CACHE_PATH=" + str(self.work / "modules"),
                  "SWIFT_MODULE_CACHE_PATH=" + str(self.work / "modules"), "COMPILATION_CACHE_ENABLE_CACHING=NO", "build"],
                 cwd=self.source / "ios", output="full-build.log")
        announce("verify")
        app = derived / "Build/Products/Release-iphoneos/NFSU2iOS.app"
        binary = app / "NFSU2iOS"
        linkmap = derived / "Build/Intermediates.noindex/NFSU2iOS.build/Release-iphoneos/NFSU2iOS.build/NFSU2iOS-LinkMap-normal-arm64.txt"
        shutil.copyfile(linkmap, self.evidence / "full-runtime-link-map.txt")
        self.run(["xcrun", "nm", "-m", binary], output="full-runtime-nm.txt")
        self.run([python, "tools/ios_private/verify_full_link.py", "--map", linkmap, "--nm", self.evidence / "full-runtime-nm.txt",
                  "--binary", binary, "--manifest", manifest, "--source", source,
                  "--out", self.evidence / "full-runtime-verification.json"])
        ipa = self.work / IPA
        with zipfile.ZipFile(ipa, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(app.rglob("*")):
                if path.is_symlink():
                    raise ClosedError("unexpected app symlink")
                if path.is_file():
                    archive.write(path, "Payload/NFSU2iOS.app/" + path.relative_to(app).as_posix())
        # Use the existing package inspector unchanged; never weaken historical
        # private-Actions provenance verification to accept this new lane.
        from verify_full_package import inspect_package
        info, binary_bytes, segments = inspect_package(ipa, source, version="0.27.2", build="37")
        report = {"source_sha": source, "ipa_sha256": sha256(ipa), "binary_sha256": hashlib.sha256(binary_bytes).hexdigest(),
                  "version": info["CFBundleShortVersionString"], "build": info["CFBundleVersion"], "segments": segments}
        (self.evidence / "package-verification.json").write_text(json.dumps(report, indent=2) + "\n")
        return ipa


def main():
    os.umask(0o077)
    work = None
    release = None
    client = None
    receipt = {"schema": 1, "lane": "public-runner-private-io", "complete": False,
               "scope": "unsigned native ARM64 compile/link/package; gameplay and physical iPad unverified"}
    builder = None
    success = False
    try:
        announce("preflight")
        validate_context(os.environ)
        validate_policy(public_json("/environments/" + ENVIRONMENT),
                        public_json("/environments/" + ENVIRONMENT + "/deployment-branch-policies?per_page=100"))
        client = PrivateIO(os.environ.pop("PRIVATE_IOS_TOKEN", ""))
        source = os.environ["SOURCE_SHA"]
        run_id, attempt = os.environ["GITHUB_RUN_ID"], os.environ["GITHUB_RUN_ATTEMPT"]
        release = client.begin(run_id, attempt, source)
        receipt.update({"source_sha": source, "public_repository": PUBLIC, "run_id": int(run_id), "run_attempt": int(attempt),
                        "workflow_ref": os.environ["GITHUB_WORKFLOW_REF"], "private_repository_id": PRIVATE_ID, "release_id": release["id"]})
        work = Path(os.environ["RUNNER_TEMP"]).resolve() / "nfs-private-build"
        work.mkdir(mode=0o700, exist_ok=False)
        builder = Builder(work, child_environment(os.environ, work))
        python = builder.build(source)
        announce("input")
        receipt["input"] = client.input(work / "SPEED2.EXE")
        ipa = builder.compile(source, work / "SPEED2.EXE", python)
        announce("upload")
        receipt["ipa"] = client.upload(release["id"], ipa)
        receipt["complete"] = True
        success = True
    except BaseException as error:
        # Never serialize str(error), repr(error), HTTP bodies or tracebacks:
        # they may embed headers, signed URLs, source lines or compiler output.
        receipt["error_type"] = type(error).__name__
    finally:
        try:
            if release and builder:
                archive = work / "private-diagnostics.tar.gz"
                with tarfile.open(archive, "w:gz") as tar:
                    tar.add(builder.evidence, arcname="evidence")
                receipt["diagnostics"] = client.upload(release["id"], archive)
                receipt_path = work / "receipt.json"
                receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
                client.upload(release["id"], receipt_path)
        except BaseException:
            success = False
        finally:
            if work is not None:
                shutil.rmtree(work)  # A failure here must also fail the job.
    announce("success" if success else "failure" if receipt.get("diagnostics") else "unavailable")
    return 0 if success else 1


if __name__ == "__main__":
    def interrupted(signum, frame):
        raise ClosedError("interrupted")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    # Even setup/cleanup exceptions must never print a Python traceback publicly.
    try:
        sys.exit(main())
    except Exception:
        announce("unavailable")
        sys.exit(1)
