# Full iOS runtime: public compute, private input and output

The canonical public repository is `lyszq/NFSU2Recomp-iOS` (repository ID
`1426968128`), renamed on 2026-10-22. The private destination remains
`lyszq/NFSU2Recomp-ios-private` with its existing immutable ID and purpose marker.
Repository guards and credential-free clone URLs use the new public name.

This additional lane builds the existing complete runtime on a standard public
`macos-15-intel` runner. It preserves the public smoke tests, the private workflows,
the private repository visibility, and the default branch. The app remains unsigned:
the owner signs and installs it. Compile/link/package evidence is not device gameplay.

## Security assessment

The workflow, commit SHA, runner metadata, timings and fixed stage messages are
public. The authenticated EXE, generated C, compiler output, link map, symbol list,
provenance, IPA and detailed receipt stay in temporary runner storage and/or the
private repository. No artifact upload, cache, public release or generated-code
commit is used. No game input is fetched by the existing public test jobs.

Public runners are GitHub-hosted VMs; they are not confidential-computing enclaves.
GitHub, the runner image, Apple toolchain, XcodeGen, pinned Python wheels, and all
source at the approved commit are trusted. Malicious approved source or a compromised
toolchain can read and exfiltrate the EXE. Environment review and source review are
the boundary against that threat; log masking alone cannot provide secrecy. Review
the whole commit, especially workflows, generator/build scripts, runtime and tests,
before approving the environment. Restrict branch write access to trusted people.
The subprocess environment allowlist is defense against accidental credential and
Actions command-file inheritance, not isolation from hostile same-user processes.

The PAT is passed to one controller step only. Compiler/generator/test subprocesses
receive a fresh environment with no PAT, Actions cache/artifact/OIDC tokens, workflow
command files, shell startup hooks or Python import hooks. Their stdout/stderr go
to private log files, including failure paths. The controller prints fixed messages;
it never formats exceptions, HTTP bodies or tracebacks into public logs. Debug runs
are refused before fetching private input. Setup output is also redirected.

Python dependencies use exact versions, wheel hashes, `--require-hashes`,
`--only-binary` and `--no-deps`. Xcode/XcodeGen are trusted host tools (the image and
Homebrew are not hermetically reproducible); tools are prepared before private input
is downloaded. No third-party Actions execute in this job. Derived data, caches,
temporary files and logs are rooted in a mode-0700 runner-temp directory, removed
in Python's `finally` and the workflow's `always()` cleanup. Forced VM termination
relies on GitHub's ephemeral-runner disposal; this is not a claim of secure erasure.

Only the fixed `private-input-v1/SPEED2.EXE` asset is accepted: 4,788,224 bytes,
SHA-256 `8f6a9ae2c4cfa01181d2fd755d99c6f4b7103f60273908ea1de1e719dfe2a297`.
The release asset metadata AND downloaded bytes must agree. Other game files are
not needed to compile this IPA and are not downloaded or bundled. Additional private
inputs require a reviewed, explicit hash/size allowlist; never broad-download assets.
Signed asset redirects use HTTPS and an exact GitHub asset-host allowlist, without
forwarding Authorization. Private repo name, immutable ID, privacy and purpose marker
are checked before input access and before every confidential output upload.

The private output is a unique **draft release** `ios-public-RUN_ID-ATTEMPT`.
It never overwrites `private-input-v1`. Failure releases may contain private diagnostic
logs; they never receive a downloadable IPA unless the link/package gates passed.
`receipt.json` records completion, exact source/run/attempt and SHA-256 values for the
IPA and diagnostics. GitHub's returned asset digests are also checked. Generated C
is deleted, not archived. The current full target is 0.27.0/build35.

## One-time setup

1. Run locally from this checkout:

   ```powershell
   python tools/ios_private/setup_public_private.py --configure
   ```

   This uses existing local Git auth only to configure public environment metadata.
   It never copies that credential into Actions. It creates `private-ios-build` with
   required reviewer `lyszq` and selected **branch** `feat/ios-guest-runtime-smoke`
   only (no tag or wildcard rule). Existing unexpected policies are rejected.
   Self-review is allowed so a sole repository owner can approve their own build.

2. In GitHub Settings → Developer settings → Personal access tokens → **Fine-grained
   tokens**, create a short-lived token (for example 7 days):

   - Resource owner: `lyszq`.
   - Repository access: **Only select repositories → NFSU2Recomp-ios-private**.
   - Repository permissions: **Contents → Read and write**. Metadata read is automatic.
   - All other permissions: no access. No public repo access, Actions, Administration,
     Workflows, organization permission or classic broad `repo` scope is needed.

   A separate GitHub App installed only on that private repository with Contents
   read/write and a short-lived installation token is an alternative. The controller
   accepts either credential in the same environment secret; App token minting is
   not implemented in this minimal lane. Long builds must finish before token expiry.

3. Public repo → Settings → Environments → **private-ios-build** → Environment
   secrets: add **PRIVATE_IOS_TOKEN** with that token. Do not paste it into chat,
   code, a workflow input, a repository variable or a repository-wide secret.

4. Enable and queue a build locally:

   ```powershell
   python tools/ios_private/setup_public_private.py --enable --request
   ```

   This checks the environment and secret *name*, sets public repository variable
   **PRIVATE_IOS_ENABLED=true**, then commits only a timestamp request file to the
   existing feature branch. The helper cannot inspect a stored token's scope: verify
   the selected repo/permissions in the token UI. The public workflow is opt-in;
   without that variable it runs synthetic preflight tests and skips the secret job.

5. In the new public Actions run, review the exact source commit and approve the
   **private-ios-build** environment. Do not enable Actions runner/step debug logging.
   The run uses the event's exact SHA, never a user-supplied ref or PR artifact.

6. Download the IPA and receipt from **Releases → Drafts** in the PRIVATE repository.
   Require a successful public run plus `receipt.complete=true`, matching source,
   run/attempt and asset digests; inspect private link and package verification JSON
   inside `private-diagnostics.tar.gz`. Sign/install the unsigned IPA yourself.

To request another build, run `setup_public_private.py --request`. Normal runtime
commits do not automatically spend a full build; changing the request file selects
the current branch SHA. `workflow_dispatch` is included for later use, but GitHub
requires its workflow file on the default branch to expose manual dispatch. The
request-file push path works now without changing `main`. PR/fork/workflow_run/
pull_request_target events cannot enter this lane. Concurrency queues builds instead
of canceling an active confidential transfer. Re-runs use a distinct attempt tag.

Disable by setting `PRIVATE_IOS_ENABLED=false` and revoke/rotate the scoped token.
Never change private repository visibility. Retain/delete old private draft releases
according to your needs; there is no automatic deletion of existing work.

## GitHub references

- [Standard public runners are free](https://docs.github.com/en/billing/concepts/product-billing/github-actions).
- [GITHUB_TOKEN is scoped to its workflow repository](https://docs.github.com/en/actions/concepts/security/github_token).
- [Environment gates and secrets](https://docs.github.com/en/actions/concepts/workflows-and-actions/deployment-environments).
- [Secure use and log redaction limitations](https://docs.github.com/en/actions/reference/security/secure-use).
- [Release API permissions](https://docs.github.com/en/rest/releases/releases).
- [Workflow dispatch default-branch requirement](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#onworkflow_dispatch).
