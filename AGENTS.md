# AGENTS.md

Operating rules for AI agents and automated tools contributing to this
repository. Human contributors follow the same rules — see
[CONTRIBUTING.md](CONTRIBUTING.md) for the full guide.

## Commits

- Messages in English with a `feat:` / `fix:` / `docs:` / `build:` / `test:`
  prefix: one line stating the behavior change and motivation.
- Every commit carries a DCO `Signed-off-by` trailer — commit with
  `git commit -s`.
- Keep messages self-contained: no references to private trackers, internal
  review numbers, or documents that do not exist in this repository.

## Branches and history

- `main` is the published default branch and the source of truth for
  history. Never rewrite it, force-push it, or reset it to remembered
  hashes.
- Commit hashes are not stable across the repository's initial publication
  cleanup. If `main` looks unfamiliar, run
  `git fetch origin && git log --oneline -3 origin/main` and compare; if it
  still looks wrong, stop and ask — do not "repair" it yourself.
- Work on a short-lived feature branch for anything non-trivial. Pushes are
  performed by the maintainer or under explicit per-task authorization only.

## Checks before handing off

- Core (model / layout / theme / contracts):
  `cmake --preset core`, `cmake --build --preset core`, `ctest --preset core`.
- Windows shell and renderer changes: build and run the affected tests
  against the shared Skia package per
  [docs/SDK_DEVELOPMENT.md](docs/SDK_DEVELOPMENT.md).
