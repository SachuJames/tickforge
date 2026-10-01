# Contributing to TickForge

Thanks for considering a contribution. TickForge is early-stage, so design discussion in issues is especially welcome before large pull requests.

## Branch naming

* `feature/<short-name>` for new functionality
* `fix/<short-name>` for bug fixes
* `docs/<short-name>` for documentation only
* `chore/<short-name>` for build/tooling changes

## Commit conventions

Use Conventional Commits:

```text
feat: add price-time priority matching
fix: correct queue index after partial fill
docs: clarify timestamp ordering in SPEC.md
test: add order book invariant tests
chore: bump GoogleTest to v1.16.0
ci: run sanitizer build on pull requests
```

* Keep commits small and single-purpose.
* Write imperative summaries under 72 characters.
* If a change alters simulation results, the commit message must say so and SPEC.md must be updated.

## Formatting

* Run `clang-format` on all changed C++ files before pushing. CI enforces it:
  `clang-format --dry-run --Werror <files>`.
* Follow the existing style in `.clang-format`. Do not reformat unrelated code in the same commit.

## Static analysis

* New code should be clang-tidy clean under the project's `.clang-tidy` baseline.
* If a check produces a false positive, prefer a targeted `// NOLINT` with a reason comment over disabling the check globally.

## Testing

* Every behavior change needs a test. Bug fixes need a regression test.
* Tests run via `ctest --preset default` (or `ctest --output-on-failure` in the build directory).
* Determinism rule: tests must be fully deterministic. No wall-clock timing assertions, no unseeded randomness.

## Pull requests

* Target `main`.
* Keep the PR focused; one concern per PR.
* Fill in: what changed, why, how it was tested, and whether SPEC.md or ARCHITECTURE.md needed updates.
* CI must be green before merge.

## Issue reporting

* Bugs: include the TickForge version, compiler, OS, minimal reproduction (config + event snippet), and expected vs. actual behavior.
* Design proposals: describe the problem, the proposed change to SPEC.md/ARCHITECTURE.md, and alternatives considered.

## Code of conduct

Be professional and respectful. See CODE_OF_CONDUCT.md.
