# VaptVupt — Release Procedure

Release v2.53.4. Commit `b02db86`. GPL-3.0-or-later.

The build environment produces and verifies the artifacts. Steps that
require GitHub or registry credentials are marked and run on a machine where
you are authenticated; they are written to paste directly. The kit embeds no
secrets.

## 1. Gate

A fresh clone from the bundle must build, reproduce the binary, and pass the
suite:

```sh
git clone vaptvupt-2.53.4.bundle repo
cd repo && git checkout v2.53.4
make
make test     # 20 C suites, differential fuzzer 5200/5200, ratio gate +/- 0,
              # safezone 55/55, DoS 12/12, competitive + cli_window pass
```

`make test` must exit 0. If it does not on the target machine, stop and
check the toolchain (gcc 13+, AVX2).

## 2. Integrity

```sh
sha256sum -c SHA256SUMS
git bundle verify vaptvupt-2.53.4.bundle
```

## 3. Push (credentials)

```sh
# git remote add origin git@github.com:<you>/vaptvupt.git   # if pushing from the bundle clone
git push origin master --tags
```

## 4. GitHub release (credentials)

```sh
gh release create v2.53.4 \
  vaptvupt-2.53.4-src.tar.gz \
  SHA256SUMS \
  COMPARISON.md \
  vaptvupt-2.53.4-linux-x86_64 \
  vaptvupt-2.53.4-linux-x86_64-mt \
  vaptvupt-2.53.4-linux-x86_64-pgo \
  --title "VaptVupt v2.53.4" \
  --notes-file RELEASE_v2.53.4.md
```

`COMPARISON.md` ships with the release; it carries the measured position
including the file classes where vv loses.

## 5. vcpkg / registries (credentials, optional)

vcpkg uses SHA512, not SHA256:

```sh
sha512sum vaptvupt-2.53.4-src.tar.gz
# vcpkg.json: "version": "2.53.4"
# portfile.cmake: REF v2.53.4, SHA512 <above>
```

## 6. Smoke test after install

```sh
echo "In Code We Trust" > t.txt
vaptvupt -c -m extreme -o t.vv t.txt && vaptvupt -d -o t.out t.vv && cmp t.txt t.out
vaptvupt -c -m extreme --bcj -o code.vv /path/to/x86-binary   # binary-ratio path
```

## Artifacts

| File | Purpose |
|---|---|
| `vaptvupt-2.53.4-src.tar.gz` | Source (`git archive v2.53.4`) |
| `vaptvupt-2.53.4.bundle` | Git history + tags (clone-able) |
| `vaptvupt-2.53.4-linux-x86_64` | Default build |
| `vaptvupt-2.53.4-linux-x86_64-mt` | Threaded build |
| `vaptvupt-2.53.4-linux-x86_64-pgo` | PGO build |
| `SHA256SUMS` | Integrity (`sha256sum -c`) |
| `CHANGELOG.md`, `COMPARISON.md`, `RELEASE_v2.53.4.md` | Docs |
