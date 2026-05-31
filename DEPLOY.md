# VaptVupt v2.53.2 — Deployment Runbook

This is the deployment procedure for the release packaged in this kit.
Everything here has been verified in the build environment **except** the
steps that require your GitHub/registry credentials (clearly marked
"CREDENTIALS"). Those must run on your machine where you are authenticated;
they are written so you can paste them directly.

**Release under deployment:** `v2.53.2` · commit `5fc6ce0` · GPL-3.0-or-later

---

## 0. Deployment gate (already passed in-environment, re-run anywhere)

A fresh clone from the bundle must build, reproduce the binary, and pass
the full suite. This was run and passed before packaging:

```sh
git clone vaptvupt-2.53.2.bundle repo
cd repo && git checkout v2.53.2
make                     # → binary builds
make test                # → 19/19 C suites, differential 5200/5200,
                         #    ratio gate pass, safezone 55/55, DoS 12/12,
                         #    competitive + cli_window PASS
```
Expected: `make test` exits 0. If it does not on the target machine, **stop
and do not deploy** — investigate the toolchain (gcc 13+, AVX2 assumed).

## 1. Verify artifact integrity

```sh
sha256sum -c SHA256SUMS        # all artifacts: OK
git bundle verify vaptvupt-2.53.2.bundle   # records a complete history
```

## 2. Push source + tags  [CREDENTIALS]

From your authenticated clone of the canonical repo:

```sh
# if pushing from the bundle clone, add your remote first:
#   git remote add origin git@github.com:<you>/vaptvupt.git
git push origin master --tags
```

This publishes commits up to `5fc6ce0` and all tags `v2.52.2 … v2.53.2`.

## 3. Cut the GitHub release  [CREDENTIALS]

```sh
gh release create v2.53.2 \
  vaptvupt-2.53.2-src.tar.gz \
  SHA256SUMS \
  COMPARISON.md \
  vaptvupt-2.53.2-linux-x86_64 \
  vaptvupt-2.53.2-linux-x86_64-mt \
  vaptvupt-2.53.2-linux-x86_64-pgo \
  --title "VaptVupt v2.53.2" \
  --notes-file RELEASE_v2.53.2.md
```

Attach `COMPARISON.md` deliberately: the honest competitive position ships
with the release. Do not edit it to look better than the measurements.

## 4. vcpkg / package registries  [CREDENTIALS, optional]

If maintaining a vcpkg port, update the port version and the source tarball
SHA512 (vcpkg uses SHA512, not SHA256):

```sh
sha512sum vaptvupt-2.53.2-src.tar.gz    # → REF for portfile.cmake
# edit ports/vaptvupt/vcpkg.json  -> "version": "2.53.2"
# edit ports/vaptvupt/portfile.cmake -> SHA512 <above>, REF v2.53.2
git commit -am "vaptvupt 2.53.2" && gh pr create ...
```

## 5. Post-deploy smoke test (any machine, after install)

```sh
echo "In Code We Trust" > t.txt
vaptvupt -c -m extreme -o t.vv t.txt
vaptvupt -d -o t.out t.vv
cmp t.txt t.out && echo "roundtrip OK"
vaptvupt -c -m balanced -w 24 -o big.vv <large-redundant-file>   # long-range win
```

---

## What is NOT auto-deployed from the build environment

The build/CI environment here has **no GitHub credentials and no network
auth to your registries**, so it cannot perform the actual `git push`,
`gh release create`, or vcpkg PR. It produces and verifies the artifacts;
you run steps 2–4 where you are authenticated. This boundary is honest by
design — the kit never embeds or assumes secrets.

## Artifacts in this kit

| File | Purpose |
|---|---|
| `vaptvupt-2.53.2-src.tar.gz` | Canonical source (`git archive v2.53.2`) |
| `vaptvupt-2.53.2.bundle` | Full git history + all tags (clone-able) |
| `vaptvupt-2.53.2-linux-x86_64` | Default build (`-O3 -flto`) |
| `vaptvupt-2.53.2-linux-x86_64-mt` | Threaded build |
| `vaptvupt-2.53.2-linux-x86_64-pgo` | PGO build |
| `SHA256SUMS` | Integrity (verify with `sha256sum -c`) |
| `CHANGELOG.md` | Full version history |
| `COMPARISON.md` | Measured competitive position (ships with release) |
| `RELEASE_v2.53.2.md` | Release notes |
| `VAPTVUPT_PROGRAM_CHARTER.md` | Charter for continued development |
| `DEPLOY.md` | This runbook |
