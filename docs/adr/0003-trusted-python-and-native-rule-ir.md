# ADR 0003: Trusted Python boundary and canonical native rule IR

- Status: accepted
- Date: 2026-08-22

## Context

Python must remain a Turing-complete authoring and escape-hatch language without owning
authoritative state or entering common traversal hot loops. Failures must roll back the
same state and random streams as native failures. Development reload must not replace
live rules in the middle of an action or decision.

## Decision

Own one embedded CPython interpreter in a simulation-thread-confined `PythonRuntime`.
Expose stable value handles, a capability-checked read-only context, and the existing
controlled native transaction through pybind11. Capabilities expire at callback exit.
Translate Python exceptions into source-located native diagnostics and let
`GameSession` reject and reverse the transaction.

Represent common Python-authored movement as immutable graphs lowered at package load
into versioned canonical `RuleProgram` bytecode. Resolve direction names once, validate
and normalize the program, and evaluate complete rays or jumps in C++. Keep arbitrary
callbacks as a supported slower path. Apply reload only at an explicit safe session
boundary and restore the previous exported registries on failure.

Treat in-process packages as trusted code. Security isolation requires a future worker
process with filesystem, network, CPU, memory, and time controls.

## Consequences

Python cannot retain a usable native state pointer, and ordinary movement crosses the
language boundary once during lowering rather than once per query step. Canonical IR
can be hashed, serialized, cached, and regression-tested independently of Python.
Callbacks pay interpreter overhead and packages remain responsible for deterministic
behavior outside engine-owned APIs. A process hosts one runtime, and rendering must
consume native snapshots without importing or calling Python.
