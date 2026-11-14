# Computer-use test plans

Test plans that an AI session carries out by operating OrcaSlicer on a Windows test box:
it clicks through the app as a user would and takes a screenshot where a step asks for one.

## How the folder is used

1. `.github/workflows/windows-prerelease.yml` builds the Windows x64 app and publishes a
   rolling pre-release per branch, tagged `nightly-<branch-slug>`, with three assets:
   `OrcaSlicer-windows-x64.zip` (portable app), `build-info.json` (what was built) and
   `test-plans.zip` (this folder).
2. The testing session reads `testbox.yml` for where to download, how to install and
   launch, where logs are written and what to wipe between runs.
3. It runs `smoke.md` first. If that fails, the other plans are not worth running.
4. It then runs the plans it was asked for, in the order of their `id`.

Everything a plan needs is in this folder; the box does not need git.

| File | Purpose |
|---|---|
| `testbox.yml` | Machine-readable install, launch and environment settings |
| `_template.md` | Start a new plan from this |
| `smoke.md` | Under 5 minutes: starts, loads a model, slices, exits |
| `01-*.md`, `02-*.md`, ... | Full plans, one flow each |
| `data/` | Input files the plans load |

## Rules for the session running a plan

- Start every plan from a clean state: close the app and delete the folders listed under
  `settings_dirs` in `testbox.yml`, unless the plan's preconditions say otherwise.
- Carry out the steps in order. For each step record PASS, FAIL or BLOCKED and take a
  screenshot where the Screenshot column says `y`, and on every FAIL.
- On FAIL, write what was on screen instead, then continue with the next step if the app
  still allows it. Do not try to fix anything.
- A label or position that differs slightly from the plan is not a failure: use the
  closest match and note the difference.
- Hold a mouse button down for about a quarter of a second when clicking a button inside
  the 3D view. Those buttons are drawn by the 3D view and can miss a faster click.
- If the app crashes or hangs past the plan's `timeout_minutes`, record the step, collect
  the folders listed under `logs` in `testbox.yml`, and stop the plan.
- Attach the newest log file to the report for every plan, pass or fail.

## Writing a plan

- Copy `_template.md`, name it `NN-short-name.md` and give it a unique `id`.
- One flow per plan. A plan that needs more than about 25 steps should be split.
- Write each step as the user sees it: visible labels in bold, menu paths as
  **File > Import > ...**, what to type in backticks. Never refer to code.
- Give every step an expected result that can be judged from the screen.
- Ask for a screenshot where the result is visual, not on every step.
- Put input files in `data/` and list them under `test_data` in the front matter.
- Keep `preconditions` to things the session can check or arrange by itself.

## What has not been checked on screen

The labels in these plans were taken from the source code. The first-run setup wizard in
particular is a web page whose wording was not read; its steps are written loosely.
Correct the plans after the first real run.
