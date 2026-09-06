# VaptVupt — Release Procedure

Release v2.65.10 (tag `v2.65.10`). GPL-3.0-or-later.

The v2.65.10 publication plan has one release artifact:
`vaptvupt-2.65.10-src.tar.gz`. Binary builds, Git bundles, standalone checksum
files, and copied comparison documents are not release assets. The source
archive already contains the repository documentation.

## 1. Gate the release commit

Start from the exact commit that will be tagged and require a clean result from
the normal release checks:

```sh
git status --short
make
make check-debug
make test
make amalg
make amalg-verify
```

`make test` must exit 0. Stop on any failure and record any unavailable
optional formal tool rather than treating a skipped tool as a pass. The
v2.65.10 delta specifically includes Huffman capacity/truncation regressions,
ANS frequency validation, token extension/window bounds, streaming format-reset
roundtrips, CLI failure reporting, and reference-decoder parity. The existing
BCJ, SEQ, OOM and competitive-harness gates remain required. Run the full
generated-v1 matrix separately to produce published benchmark evidence.

Review the release diff and confirm that current version markers say 2.65.10;
older numbers in the changelog and explicitly historical benchmark sections
must remain:

```sh
git diff --check
git diff --stat v2.65.9..HEAD
rg -n '2\.65\.9|v2\.65\.9' --glob '*.md' --glob '*.[ch]'
```

## 2. Tag and build the source archive

Create the release tag only after the gate is clean. Use a signed annotated tag
where signing is configured:

```sh
git tag -s -a v2.65.10 -m "VaptVupt v2.65.10"
git archive --format=tar.gz --prefix=vaptvupt-2.65.10/ \
  -o vaptvupt-2.65.10-src.tar.gz v2.65.10
sha256sum vaptvupt-2.65.10-src.tar.gz
```

Record the printed SHA-256 in the release notes. It is release metadata, not a
second uploaded asset.

## 3. Verify the archive, not just the worktree

Extract into a fresh temporary directory and run the same source-facing gate:

```sh
release_tmp=$(mktemp -d)
tar -xzf vaptvupt-2.65.10-src.tar.gz -C "$release_tmp"
(
  cd "$release_tmp/vaptvupt-2.65.10"
  make
  make check-debug
  make test
  make amalg
  make amalg-verify
)
```

Confirm the archive root is `vaptvupt-2.65.10/`, contains no `.git` directory,
and contains only files tracked by the tagged source tree. The historical
repository scrub completed for v2.65.8 remains a separate, one-time operation;
ordinary v2.65.10 publication must not force-update old tags or branches.

## 4. Push the release commit and tag (credentials)

Push without history rewriting, then verify that every endpoint resolves the
branch and tag to the same release commit:

```sh
git push codeberg HEAD:refs/heads/v260-master
git push codeberg refs/tags/v2.65.10
git push github HEAD:refs/heads/v260-master
git push github refs/tags/v2.65.10
git push origin-https HEAD:refs/heads/v260-master
git push origin-https refs/tags/v2.65.10

release_sha=$(git rev-parse 'v2.65.10^{}')
printf 'expected release commit: %s\n' "$release_sha"
for remote in codeberg github origin-https; do
  git ls-remote "$remote" refs/heads/v260-master refs/tags/v2.65.10 \
    'refs/tags/v2.65.10^{}'
done
```

Apply the same explicit branch/tag pushes to the Forgejo `.com.br` endpoint
when it is reachable. The branch result and peeled tag (`^{}`) must equal
`$release_sha`. Do not use `--mirror`.

## 5. Create the GitHub release (credentials)

Upload only the source archive. The notes come from the matching changelog
section:

```sh
archive_sha=$(sha256sum vaptvupt-2.65.10-src.tar.gz | awk '{print $1}')
release_notes=$(mktemp)
awk '/^## v2.65.10 /{f=1;next} /^## /{f=0} f' CHANGELOG.md > "$release_notes"
printf '\nSource archive SHA-256: `%s`\n' "$archive_sha" >> "$release_notes"
gh release create v2.65.10 \
  vaptvupt-2.65.10-src.tar.gz \
  --title "VaptVupt v2.65.10" \
  --notes-file "$release_notes" \
  --verify-tag
```

Check the published page: it must list `vaptvupt-2.65.10-src.tar.gz` as the only
manually uploaded asset, show the recorded SHA-256 in the notes, and preserve
the workload-dependent benchmark wording.

## 6. vcpkg / registries (credentials, optional)

vcpkg uses SHA-512 for its source reference:

```sh
sha512sum vaptvupt-2.65.10-src.tar.gz
# vcpkg.json: "version": "2.65.10"
# portfile.cmake: REF v2.65.10, SHA512 <above>
```

## 7. Smoke test after install

```sh
echo "In Code We Trust" > t.txt
vaptvupt -c -m extreme -o t.zupt t.txt && \
  vaptvupt -d -o t.out t.zupt && cmp t.txt t.out
vaptvupt -c -m extreme --bcj -o code.zupt /path/to/x86-binary
vaptvupt -c -m extreme --bcj-arm64 -o code.zupt /path/to/arm64-binary
```

## Artifact

| File | Purpose |
|---|---|
| `vaptvupt-2.65.10-src.tar.gz` | Tagged source tree (`git archive v2.65.10`) |
