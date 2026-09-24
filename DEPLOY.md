# VaptVupt Codec — Release Procedure

Current release: v2.65.12. Copyright 2026 Cristian Cezar Moisés. First-party
work is Apache-2.0; the XXH64-derived file remains BSD-2-Clause. See `LICENSE`
and `NOTICE`.

New release packages use `.zupt`. Existing `.tar.gz` assets from older
releases remain published and must not be deleted, replaced, or retagged.

## 1. Validate the release commit

Start from a clean worktree and run the complete release gate:

```sh
git diff --check
make clean
make
make check-debug
make test
make amalg
make amalg-verify
make scalar-test
```

Confirm that the current version markers are 2.65.12. Older numbers in
`CHANGELOG.md` and explicitly historical benchmark sections must remain.

## 2. Sign the commit and tag

The maintainer's configured, valid OpenPGP key must sign both objects. Do not
add a DCO `Signed-off-by` trailer unless the maintainer is deliberately making
that certification.

```sh
git commit -S -m "docs: release VaptVupt Codec 2.65.12"
git verify-commit HEAD
git tag -s -a v2.65.12 -m "VaptVupt Codec v2.65.12"
git verify-tag v2.65.12
```

## 3. Build and verify the `.zupt` package

Export the signed tag into a fresh staging directory. The release archive is
unencrypted and uses Zupt's VaptVupt backend at level 9, its maximum compression
setting:

```sh
release_tmp=$(mktemp -d)
git archive --format=tar --prefix=vaptvupt-codec-2.65.12/ v2.65.12 |
  tar -xf - -C "$release_tmp"
zupt compress --vv -l 9 vaptvupt-codec-2.65.12.zupt \
  "$release_tmp/vaptvupt-codec-2.65.12"
zupt test vaptvupt-codec-2.65.12.zupt
zupt list vaptvupt-codec-2.65.12.zupt
sha256sum vaptvupt-codec-2.65.12.zupt > SHA256SUMS
```

Extract into another empty directory, compare its paths and SHA-256 hashes with
the tagged tree, then run the release gate from the extracted source. Zupt v1.6
normalizes extracted regular files to mode `0600` and therefore does not
preserve Git executable bits; release validation must exercise scripts through
their declared interpreter or restore executable bits where direct execution
is required. The archive must not contain `.git`, build output, credentials,
prompt text, private reports, or unrelated files.

Users extract the package with:

```sh
zupt test vaptvupt-codec-2.65.12.zupt
zupt extract -o ./vaptvupt-codec-2.65.12 \
  vaptvupt-codec-2.65.12.zupt
```

This package is a Zupt archive container, not a single raw VaptVupt frame.

## 4. Publish without rewriting history

Push `HEAD` to `v260-master` and push `v2.65.12` explicitly to Codeberg,
GitHub, git.securityops.co, and git.securityops.com.br. Never use `--force` or
`--mirror`. Verify that each branch and peeled tag resolve to the same commit.

Create one release record at each forge and upload only:

- `vaptvupt-codec-2.65.12.zupt`
- `SHA256SUMS`

Forge-generated source snapshots may still appear in the interface; they are
not manually uploaded package assets. Release notes must identify the signed
tag, archive SHA-256, Apache-2.0/BSD-2-Clause licensing split, and the exact
Zupt extraction command. Download every uploaded asset again and verify its
size and SHA-256 before declaring publication complete.

## 5. Smoke test

```sh
printf '%s\n' "In Code We Trust" > sample.txt
./vaptvupt -c -m extreme -o sample.zupt sample.txt
./vaptvupt -d -o sample.out sample.zupt
cmp sample.txt sample.out
```
