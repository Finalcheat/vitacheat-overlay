# Repository working rules

- Maintain this checkout as the single development source. Do not create another maintained copy for publishing. Temporary test checkouts are allowed.
- Prepare changes and descriptive commits locally. Amend or squash unpublished commits for review. Push only after the user approves the source and commit history.
- Treat version-tag pushes, Release publication, and repository-visibility changes as separate user-approved actions. Do not rewrite shared history without agreement. Temporary tags in isolated test fixtures are allowed.
- Keep README.md in English and README.zh-CN.md aligned. These are user-facing documents: describe the product, installation, controls, and licensing; keep build details in docs/BUILDING.md and test procedures in docs/TESTING.md. Do not add workstation/device preparation records to the landing pages.
- Project-authored code uses MIT. Preserve independent third-party notices in source and packages.
- Keep generated outputs under build/ and dist/. Never commit dependency caches, credentials, device backups, or diagnostic captures.
- Run checks appropriate to the change. CI shares .github/workflows/build.yml with Releases; preserve version/commit/hash checks and draft-only release creation.
