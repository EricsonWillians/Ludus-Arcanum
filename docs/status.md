# Implementation status

## Completed

- Milestones 0–3: a C++23 deterministic kernel, reversible transactions, typed events,
  canonical saves/hashes, replay, trusted embedded Python, validated native movement
  IR, and the external orthodox-chess package with complete legality and perft coverage.
- Milestones 4–6: immutable presentation snapshots, threaded GTK player and Studio,
  atomic playtest views, safe two-phase Python reload, a serializable value-only
  effect/choice boundary, legacy archive migration, and the first tactical package.
- Milestone 7: `BoardCanvas` now negotiates desktop OpenGL 3.3, OpenGL ES 3.0, then a
  Cairo/Pango software backend. The player exposes `--renderer`, `--renderer-info`, and
  a physical-GPU stress mode; failures report the requested API and native diagnostic.
- The render model now separates static and dynamic revisions and supports rectangles,
  rounded rectangles, circles, true hexagons, thick lines, sprites, outlines, health
  bars, UTF-8 text, effects, and event-driven move/capture/fade/scale/projectile/impact/
  poison/check/promotion animation. Camera rotation, board flip, transformed picking,
  drag pan, cursor-anchored zoom, fit/reset, and device-scale changes work in both
  accelerated and software modes.
- Package visuals are headless and bounded: manifests may name a `visuals` theme,
  libpng validates RGBA assets, catalogs use stable string keys, and deterministic
  multi-page atlases extrude padding and premultiply alpha. Traversal, symlink escape,
  undeclared files, corrupt PNGs, non-finite values, duplicate keys, absent frames,
  decoded-pixel limits, and atlas overflow are rejected with source diagnostics.
  `AssetCatalog` preserves the last valid catalog after a failed hot reload.
- Studio imports PNGs with collision-safe atomic writes, maintains the package allowlist
  and theme, previews atlas pages/pivots/animation frames/alpha, edits named sprites,
  maps legacy numeric chess sprites, and refreshes a valid preview immediately.
- Orthodox chess ships package-native transparent bone/ivory and blackened-iron sets,
  a dark-fantasy board, drag or click movement, focus/hover feedback, coordinates,
  flipping, distinct move/capture hints, last-move/check/end-state presentation,
  capture/promotion/castling effects, captured-material trays, and SAN move history.
  Legality, state hashing, save behavior, perft, replay, and authoritative history are
  unchanged.
- Tactical scenario version 2 uses a radius-three 37-hex battlefield with ruins, cover,
  difficult terrain, and a shrine. Six distinct units implement initiative, two-action
  activations, deterministic abilities, armor/poison/ward statuses, shrine scoring,
  elimination and round-ten victory rules, default deterministic fog-safe Raiders AI,
  hot-seat mode, grouped player undo, a generic value-only HUD, and rich range/effect
  presentation. Version-1 saves retain their legacy action and presentation path.
- Generated bitmap art is committed data, never a build/runtime dependency. Each
  package records prompts, tool, date, reference lineage, processing, and SHA-256
  provenance beside its assets.
- Milestone 8 adds the package-owned `ChessMatch` layer without changing orthodox
  legality or position hashes. Optional deterministic clocks, timed-ply archives,
  undo/redo/branching, flag fall, resignation, agreed draws, claimable threefold and
  50-move draws, automatic fivefold/75-move/dead-position results, and result precedence
  are represented as bounded native values.
- Mainline PGN/FEN workflows validate complete replacements, resolve SAN against legal
  moves, retain comments/NAGs/clock annotations, reject variations with source-located
  diagnostics, and export normalized seven-tag PGN. Native match files and autosaves
  share the versioned archive format.
- The Ivory Reliquary is now a dedicated responsive chess client: header controls,
  player cards, captured material, optional live clocks, two-column SAN history with
  non-mutating preview, New Match settings, promotion imagery, result actions,
  accessibility labels, persistent contrast/motion/scale preferences, and compact
  drawer behavior coexist with the unchanged tactical layout.
- Chess board interaction now uses typed move/capture/castle/promotion/claim markers,
  restrained last-move/check treatments, drag ghosts, package decorations, named color
  roles, and presentation-only right-drag annotations. Generated crest, frame, and board
  material assets are package-native, validated, optimized, and provenance-recorded.

## Verification status

The repository contains context, asset, renderer, chess, tactical, Studio, migration,
and deterministic replay tests. The Milestone 8 gate passes all 63 C++/package tests
in GUI development, release, GUI release, explicit headless-with-Python, and ASan/UBSan
configurations, plus all 11 Python SDK/package tests. Xvfb/Mesa smoke tests initialize
forced desktop GL, forced GLES, forced Cairo, and automatic selection; the physical
NVIDIA RTX 3060 Ti X11 smoke selects GLES 3.2 automatically after the unavailable
desktop context, without a `GDK_DEBUG` override. Responsive software rendering was
also inspected at 800×600. The physical NVIDIA stress capture is recorded in
`docs/performance.md`; its renderer CPU and GPU targets pass, while the observed X11
presented-frame p95 narrowly exceeds the target and is reported as a measured
limitation rather than being hidden behind a CI timing assertion.

## Deferred scope

Milestone 8 remains a local two-player 2D client. Meshes, 3D materials, Vulkan, audio,
package-embedded fonts, networking, chess AI, engine analysis, and opening databases
remain deliberately outside this slice. Package-wide journaling and a dedicated
glyph-level text atlas are also later hardening work; current text rendering uses
bounded high-DPI Pango layout caches.
