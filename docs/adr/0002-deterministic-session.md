# ADR 0002: Patch-based deterministic sessions

- Status: accepted
- Date: 2026-08-22

## Context

Transactions must roll back across native and future Python failures without copying
the full state for every action. The same foundation must support undo, redo, save/load,
replay, multiplayer divergence detection, and future AI simulation.

## Decision

`GameSession` owns authoritative state and versioned random streams. Each controlled
transaction records compact typed before/after patches while mutating native storage.
Commit assigns monotonic event sequence numbers and publishes immutable typed event
batches. Rollback and undo apply patches backward; redo applies them forward. Replay
applies events from a canonical initial state or a periodic checkpoint and validates a
recorded state-plus-RNG hash after every batch.

Use a versioned canonical binary representation rather than JSON for runtime state and
history. JSON/TOML remains appropriate for declarative package authoring, but parsing it
does not belong in action resolution.

## Consequences

Ordinary mutations are proportional to their changes instead of world size. Saves
retain redo history and deterministic RNG continuation. Rule callback code and call
stacks are not serialized; packages must register compatible action definitions after
loading a session. Format changes require an explicit archive or state version change.
