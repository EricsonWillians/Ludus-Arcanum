# ADR 0001: Target-oriented CMake foundation

- Status: accepted
- Date: 2026-08-22

## Context

The framework must support independently usable native modules and headless operation
without pulling GUI or scripting dependencies into every consumer.

## Decision

Use target-oriented CMake with public aliases such as `ludus::core`. Require C++23,
disable compiler extensions through presets, enable strict warnings per project target,
and keep sanitizers opt-in through a dedicated preset. Add modules only with tested
behavior. Use Catch2 3 for native tests, preferring an installed package and otherwise
fetching a pinned archive with hash verification.

## Consequences

Consumers receive only explicit include paths and transitive dependencies. Headless
targets remain small. Network-free builders can install system dependencies and set
`LUDUS_FETCH_DEPENDENCIES=OFF`.
