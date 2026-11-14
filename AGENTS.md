# Media Cast Receiver engineering rules

These rules are mandatory for every change in this repository.

## Acceptance order

1. State machines are correct, ownership is unique, and command ordering is provable.
2. Boundaries are explicit and every failure path is contained.
3. Real interaction and accessibility are correct.
4. Visual fidelity is correct.
5. Development speed is last.

The UI is always a consumer of the state machine, never its designer.

## Required repair workflow

Do not write a speculative patch to make a symptom disappear.

Before editing code:

1. Trace the complete affected path statically, including state owner,
   transitions, event entry points, queues, acknowledgements, and failure paths.
2. State the suspected root cause and the concrete evidence supporting it.
3. Identify the smallest layer that can own the fix.
4. Check whether Protocol, Playback, Shell, wire semantics, or ownership would
   change. If so, stop and treat that as a separate core-layer task.

After the static review:

1. Add or run focused tests when uncertainty remains or behavior is testable.
2. Make the smallest patch that fixes the proven root cause.
3. Keep one diagnostic variable at a time. Remove temporary probes unless the
   existing diagnostics contract explicitly needs them.
4. Re-test the original failure mode and relevant regression paths.
5. Commit each verified change separately with an explicit file list.

Never use a fast workaround that duplicates state, bypasses the owner, merges
event entry points, weakens command ordering, or changes protocol/playback
semantics merely to make the UI appear functional.
