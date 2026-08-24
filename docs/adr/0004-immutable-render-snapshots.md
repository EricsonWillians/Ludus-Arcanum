# ADR 0004: Immutable presentation snapshots and a single native board widget

- Status: accepted
- Date: 2026-08-22

## Context

The visual player must remain responsive while trusted Python rules execute, render a
large logical board without a GTK child per object, and animate committed changes
without weakening authoritative determinism. The rendering core must also remain
usable in headless tests and benchmarks.

## Decision

Build package-specific presentation data on the simulation thread and publish complete
snapshots as atomically exchanged `shared_ptr<const RenderSnapshot>` values. Send UI
commands through a bounded queue; keep CPython and `GameSession` confined to the worker
thread. Wake GTK through `Glib::Dispatcher`, never through a shared state lock.

Keep batching, atlases, camera transforms, picking, and event interpolation in the
backend-neutral `ludus-render` module. Retain batch storage across frames. Implement
the first backend as one gtkmm 4 `Gtk::GLArea` using an OpenGL 3.3 instanced-quad batch,
with GTK widgets only for window-level controls. Represent package actions as opaque
numeric tokens and derive animations from committed typed events plus the before/after
presentation snapshots.

## Consequences

Rendering cannot observe a partial transaction, access Python, or mutate authoritative
state. Simulation latency does not block GTK, and a burst of more than 64 pending UI
commands is rejected visibly instead of growing memory without bound. Visual
interpolation is nondeterministic presentation time only and cannot affect replay or
hashes. Backends can consume the same quad batch without depending on chess or Python.
The initial external texture path intentionally supports bounded P3/P6 PPM sheets;
broader asset decoding and a fallback non-OpenGL renderer remain future work.
