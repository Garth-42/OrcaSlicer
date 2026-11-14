---
id: NN-short-name
title: One line that says what the plan proves
priority: P1            # P0 = smoke, P1 = core flow, P2 = secondary
timeout_minutes: 15
preconditions:
  - App installed and settings folders wiped as described in testbox.yml
test_data:
  - data/cube_20mm.stl
---

## Setup

What to do before step 1, beyond the preconditions. Say "None" if there is nothing.

## Steps

| # | Action | Expected result | Screenshot? |
|---|---|---|---|
| 1 | What the user does, with **visible labels** and menu paths | What must be on screen afterwards | y |
| 2 | ... | ... | n |

## Pass criteria

The plan passes when every step passes. List here anything that decides the result beyond
the single steps, for example "no error dialog appeared at any point".

## Known flaky areas

Steps that are sensitive to timing, rendering or first-run state, and what to do about them.

## Cleanup

How to leave the box: what to close, what to delete.
