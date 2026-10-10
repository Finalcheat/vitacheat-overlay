# Development guide

Use the default English README for setup and keep `README.zh-CN.md` aligned when changing user-visible behavior.

## Change boundaries

- Keep portable core code independent of Vita headers, filesystem calls, threads, and direct game-pointer access.
- Add format behavior through parser/compiler/runtime contracts and callback-driven tests before platform integration.
- Treat private protocol layouts, exported syscall signatures, settings schemas, and resource IDs as compatibility contracts.
- Keep file and blocking work outside the PAF rendering callback. UI changes must preserve input release/neutrality behavior.
- Keep historical platform regressions separate from the normal game-executor target.
- Preserve existing third-party notices. Do not add proprietary executables, firmware captures, player data, device config, or credentials.

## Validation

Run `sh scripts/test.sh`; build Vita targets with `sh scripts/build.sh` when their code or shared contracts change. For narrow iterations, select relevant CTest cases, then run the complete suite before a candidate is packaged.

Use `.clang-format` for edited C/C++ code. Tests should exercise observable behavior and meaningful failures. A mocked restore/free result is a modeled API contract, not a new firmware guarantee.

For reports and reviews, include the concrete behavior change, compatibility impact, checks performed, and any hardware validation still required. Do not increase scope to private firmware interception merely to satisfy a host model.

## Commits and review

Prepare changes and commits locally before requesting review or pushing. Keep each commit focused on one maintainable concern. A useful message has an imperative subject and a body explaining the problem, resulting behavior, compatibility considerations, and relevant validation.

Use `git commit --amend` or an interactive rebase to refine and squash unpublished commits. Preserve already shared history unless rewriting it has been explicitly agreed. Push only the reviewed commits; create and push a version tag as a separate release action.
