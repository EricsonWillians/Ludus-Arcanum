# ADR 0005: Validated package documents and two-phase live rule reload

- Status: accepted
- Date: 2026-08-22

## Context

The first Studio slice must create a real game package, expose topology/entities and
Python source, and playtest edits without moving authoritative state or CPython onto
the GTK thread. Saving and reloading malformed authoring data must not corrupt an
existing package or replace working rules with a partial registry.

## Decision

Represent the editable package as a headless `PackageDocument` containing a strict
manifest, rectangular board definition, and Python entry source. Use a small bounded
TOML reader for the schema currently emitted by the Studio. Reject unknown fields,
unsafe relative paths, oversized documents, unsupported engine ranges, enabled native
extensions, invalid identifiers, duplicate entities, and invalid board bounds before
mutation or save. Replace each serialized file through a temporary sibling and atomic
rename.

Keep the initial chess-like playtest conversion in `ludus-studio`, outside the generic
document and engine targets. Run its Python runtime and game session on a worker thread,
accept commands through a 64-entry bounded queue, and publish one immutable view that
contains rendering, diagnostics, event inspection, and state inspection.

Treat reload as a two-phase commit. Back up the loaded Python module dictionary, build
fresh exported registries, validate the generic module contract, and compile the
package adapter's complete required rule set into temporary native programs. Commit
the module generation and program swap only after every check succeeds; otherwise
restore the dictionary and keep the current session unchanged.

## Consequences

A generated package can be copied, versioned, and edited outside the Studio, and the
same parser/validation works in headless tests. Individual files are never observed
half-written, although a crash between separate file replacements can still leave
different valid document revisions; package-wide journaling can be added when bundled
archives or migration require it. The accepted TOML language is intentionally smaller
than general TOML and unsupported constructs fail explicitly.

GTK cannot observe partial simulation changes or call Python. Invalid live edits are
diagnosed while the prior native programs, event history, and canonical state remain
usable. The 8x8 chess-like adapter is an explicit first vertical slice, not a generic
topology restriction; future package adapters can consume the same document, command,
and immutable-view mechanisms.
