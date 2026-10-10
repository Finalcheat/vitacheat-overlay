# Release procedure

CI and Releases share `.github/workflows/build.yml`. Release jobs build the exact validated tag commit and prepare a draft containing the installation ZIP, its SHA-256 checksum, and reviewed bilingual notes.

## Review commits before pushing

Prepare and validate changes locally. Keep commit subjects focused; use the body to explain behavior, compatibility, and checks. Amend or squash unpublished commits before review. Push after the final source and commit history are approved. Version tags are a separate release action.

## CI

`.github/workflows/ci.yml` runs on main pushes, pull requests, and manual dispatch:

1. Linux and macOS compile the host suite with strict warnings, ASan, and UBSan.
2. The generated encoding table is checked and the complete host suite runs.
3. After both host jobs succeed, Ubuntu installs the pinned VitaSDK archive, verifying SHA-256 before extraction.
4. Plugins are built with the pinned PAF declarations; Vita import, syscall, palette, and SELF checks run.
5. A complete installation ZIP is created and verified against its manifest, source commit, build stamp, licenses, and checksum.
6. The ZIP/checksum are retained as the `vitacheat-overlay-package` Actions artifact. Test logs have a separate seven-day retention period.

No release is created by an ordinary main push or pull request.

## Prepare a version

- Update `src/version.h`.
- Add the user-facing changes to `CHANGELOG.md`.
- Add reviewed English and Chinese notes to `docs/releases/<version>.md`.
- Review settings-schema or protocol changes and document upgrade/downgrade instructions.
- Complete source/build checks and applicable device validation.

After the reviewed commit is ready, create and push an annotated tag:

```sh
git tag -a v0.8.50 -m "Release VitaCheat Overlay 0.8.50"
git push origin main
git push origin v0.8.50
```

Use `v0.8.50-rc.1`, `-beta.1`, or `-alpha.1` for a pre-release; its base version must still match `src/version.h`.

## Release workflow

`.github/workflows/release.yml` runs on `v*` tag pushes. It can also be dispatched with an existing tag.

The workflow checks that the tag resolves to the checkout commit, the version matches the source, and reviewed notes exist. It then runs the shared build workflow. The release job downloads that run's package and checks its commit, version, manifest, licenses, and checksum again before creating a **draft**.

Only the draft job has `contents: write`; build jobs retain read-only repository access. GitHub's built-in token is sufficient. No personal token, device address, FTP credentials, or device connection is needed.

The draft contains:

```text
VitaCheatOverlay-<version>.zip
VitaCheatOverlay-<version>.zip.sha256
```

GitHub supplies the source archives automatically. The installation ZIP contains four plugins, `vco-base.rco`, documentation, MIT/third-party licenses, a manifest, and build provenance. It does not include the optional device harness unless packaged separately.

## Publish the draft

Inspect the notes and exact assets, validate the candidate on a Vita, then publish the draft in GitHub Releases. CI success is a build/software contract check; the manifest does not infer hardware acceptance.

Existing releases are never overwritten automatically. A rerun encountering an existing release stops for review; inspect the existing draft or intentionally remove an incomplete draft before rerunning. Keep published assets and their qualification records tied to the exact component hashes.

## Optional manual package and FTP installation

```sh
sh scripts/setup.sh
sh scripts/test.sh
sh scripts/build.sh
python3 scripts/package.py
python3 scripts/package.py --verify dist/VitaCheatOverlay-0.8.50.zip
```

`scripts/deploy.py` supports offline configuration planning, verified installation, and journal-based rollback:

```sh
python3 scripts/deploy.py plan --config /path/to/config.txt --output /path/to/config.review.txt
python3 scripts/deploy.py install \
  --host "$VITA_FTP_HOST" --port 1337 \
  --package dist/VitaCheatOverlay-0.8.50.zip \
  --config-path ur0:tai/config.txt --backup backups/install-001
python3 scripts/deploy.py rollback \
  --host "$VITA_FTP_HOST" --port 1337 --backup backups/install-001
```

Select the actually active taiHEN config path. Close games before replacing plugins and fully restart afterward. The installer does not close applications or reboot for you; it preserves the original VitaCheat configuration and database.
