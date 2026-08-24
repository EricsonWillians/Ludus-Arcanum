# ADR 0006: Value-only effect pauses and viewer-specific projections

- Status: accepted
- Date: 2026-08-23

## Context

The tactical proof needs an attack to pause for player input, survive save/load, then
resume deterministically through Python. It also needs hidden cards and fog without
placing RPG terms or viewer-dependent omissions in authoritative kernel state. Storing
a Python generator, closure, frame, or callable would make serialization, reload,
replay, and rollback depend on interpreter internals.

## Decision

Add one bounded generic `EffectStack` to `GameState`. An `EffectRecord` contains only a
stable identifier, an interned continuation action, optional source/target handles,
and typed interned arguments. A `ChoiceWindow` contains a player, prompt, bounded
options, and typed option arguments. Transactions push/pop effects and request/resolve
choices through typed patches and events. The complete structure participates in
canonical state bytes, hashes, save/load, replay, rollback, undo, redo, and Studio
inspection. Choice identifiers must match the top effect, and commit rejects dangling
effect references.

Keep interpretation outside the kernel. The tactical adapter validates range and line
of sight, opens the choice, reconstructs a callback intent from value data, invokes one
trusted Python resolver, and pops the effect atomically. Health, damage, initiative,
poison, cards, and triggers remain package symbols and rules.

Build hidden-information views as package-owned `RenderSnapshot` projections for an
explicit viewer. Authoritative state retains every entity; opponent cards and units
outside fog range are simply absent from the published viewer snapshot.

## Consequences

A paused decision is portable native data rather than interpreter state. It can cross
process, archive, replay, and reload boundaries as long as the package re-registers a
compatible continuation action after loading. Nested effects are supported to a
bounded depth, with at most one open choice at the top of the stack. Packages remain
responsible for mapping option data to their domain rules.

Viewer projections prevent accidental rendering of hidden entities, but they are not
an access-control boundary for trusted in-process package code. A network server must
send only the intended projection and permitted actions to each client.
