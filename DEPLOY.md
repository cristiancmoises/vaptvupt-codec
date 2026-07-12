# VaptVupt — Release Procedure

Release v2.65.3 (tag `v2.65.3`). GPL-3.0-or-later.

The build environment produces and verifies the artifacts. Steps that
require GitHub or registry credentials are marked and run on a machine where
you are authenticated; they are written to paste directly. The kit embeds no
secrets.

## 1. Gate

A fresh clone from the bundle must build, reproduce the binary, and pass the
suite:

```sh
git clone vaptvupt-2.61.1.bundle repo
cd repo && git checkout v2.65.3
make
make test     # 22 C suites + OOM sweep, differential 5576/5576, fuzz 5200/5200,
              # ratio gate +/- 0, safezone 58/58, exact-buffer 20136/20136,
              # DoS 12/12, competitive + cli_window pass
```

`make test` must exit 0. If it does not on the target machine, stop and
check the toolchain (gcc 13+, AVX2).

## 2. Integrity

```sh
sha256sum -c SHA256SUMS
git bundle verify vaptvupt-2.61.1.bundle
```

## 3. Push (credentials)

```sh
# git remote add origin git@github.com:<you>/vaptvupt.git   # if pushing from the bundle clone
git push origin master --tags
```

## 4. GitHub release (credentials)

```sh
gh release create v2.65.3 \
  vaptvupt-2.61.1-src.tar.gz \
  SHA256SUMS \
  COMPARISON.md \
  vaptvupt-2.61.1-linux-x86_64 \
  vaptvupt-2.61.1-linux-x86_64-mt \
  vaptvupt-2.61.1-linux-x86_64-pgo \
  --title "VaptVupt v2.65.3" \
  --notes-file <(awk '/^## v2.65.3 /{f=1;next} /^## /{f=0} f' CHANGELOG.md)
```

`COMPARISON.md` ships with the release; it carries the measured position
including the file classes where vv loses.

## 5. vcpkg / registries (credentials, optional)

vcpkg uses SHA512, not SHA256:

```sh
sha512sum vaptvupt-2.61.1-src.tar.gz
# vcpkg.json: "version": "2.61.1"
# portfile.cmake: REF v2.65.3, SHA512 <above>
```

## 6. Smoke test after install

```sh
echo "In Code We Trust" > t.txt
vaptvupt -c -m extreme -o t.zupt t.txt && vaptvupt -d -o t.out t.zupt && cmp t.txt t.out
vaptvupt -c -m extreme --bcj -o code.zupt /path/to/x86-binary       # x86 binary-ratio path
vaptvupt -c -m extreme --bcj-arm64 -o code.zupt /path/to/arm64-bin  # AArch64 binary-ratio path
```

## Artifacts

| File | Purpose |
|---|---|
| `vaptvupt-2.61.1-src.tar.gz` | Source (`git archive v2.65.3`) |
| `vaptvupt-2.61.1.bundle` | Git history + tags (clone-able) |
| `vaptvupt-2.61.1-linux-x86_64` | Default build |
| `vaptvupt-2.61.1-linux-x86_64-mt` | Threaded build |
| `vaptvupt-2.61.1-linux-x86_64-pgo` | PGO build |
| `SHA256SUMS` | Integrity (`sha256sum -c`) |
| `CHANGELOG.md`, `COMPARISON.md` | Docs |
