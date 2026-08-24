# Determinism contract

The Milestone 1 deterministic format and random algorithm are versioned independently.
All canonical integers use explicit little-endian encoding. Collections are serialized
in stable numeric-ID order. Authoritative fixed-point values use a scale of 10,000.
Canonical hashes use 64-bit FNV-1a over the canonical byte sequence; this is a compact
divergence detector, not a cryptographic authentication mechanism.

Version-1 archive migration never compares a legacy recorded hash to current bytes.
The loader reconstructs the old state byte form, verifies each historical and final
hash with it, then stores the equivalent current hash and canonical checkpoint bytes.
Retained empty and history-bearing fixtures prevent compatibility from depending on a
save synthesized by the current writer.

## Random algorithm version 1

Authoritative randomness uses PCG-XSH-RR 32 with a 64-bit state. A named stream derives
its initial state and odd increment from the master seed and the UTF-8 stream name using
FNV-1a plus SplitMix64. Streams are independent: drawing from `loot` cannot change the
next `combat` result. Bounded values use rejection sampling rather than modulo alone.

For master seed `42` and stream `combat`, the first eight raw 32-bit outcomes are:

```text
792947071, 114436514, 3312788359, 3403716857,
2234156308, 353647627, 841742849, 3452942987
```

These values are enforced in the native test suite. Dice grammar currently accepts
`NdM`, an optional exploding suffix (`!`), keep-highest/lowest-one (`kh1`/`kl1`), and an
integer modifier. Examples include `2d6+3`, `1d6!`, and `2d20kh1+4`. A committed roll
records the stream, original expression, every raw face, and total.

Cosmetic randomness is outside this service and must never reuse an authoritative
stream.

## Paused effects and choices

Effect records and player choices are canonical value data. Effect order, continuation
action IDs, source and target handles, prompts, option order, and typed arguments all
participate in the state hash. A choice may only refer to the top effect. Resolution
records the complete choice and selected option before the package continuation's
ordinary mutation and dice events. Saving at the pause boundary and resuming after
load therefore produces the same deterministic stream continuation and replay hash.

Python execution state is deliberately excluded. A package restores behavior by
registering a compatible continuation action against the loaded symbols; it never
serializes a Python frame or closure.

## Python rules

Python callbacks participate in the same reversible transaction and versioned random
service as native callbacks. Exceptions restore state and every RNG stream before a
diagnostic crosses the boundary. Context and transaction capabilities expire when a
callback returns so retained Python objects cannot mutate or inspect a later session
state.

Trusted package authors must not use wall-clock time, hash-order iteration, the Python
`random` module, or external I/O to decide authoritative results. The engine records
its own random outcomes and canonical state, but an in-process trusted runtime cannot
make arbitrary Python code deterministic by force. A package that uses the SDK contract
can be verified through replay hashes; a package that deliberately bypasses it is
outside the determinism contract.
