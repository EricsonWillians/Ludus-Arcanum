# Orthodox chess package

This reference package keeps every chess rule outside the generic Ludus Arcanum
kernel while using its topology, entities, validation, transactions, Python runtime,
native movement IR, saves, undo/redo, and deterministic replay.

`orthodox_chess.rules` declares ordinary non-pawn movement and the transactional move
resolver. Package-local `ludus::chess::Position` implements complete orthodox legality,
FEN, SAN, and perft; `ChessGame` connects it to an authoritative `GameSession`.

```cpp
auto game = ludus::chess::ChessGame::create(python_runtime);
auto moved = game->submit_uci("e2e4");
auto legal = game->legal_moves();
auto replayed_hash = game->replayed_state_hash();
```

The package visual catalog in `visuals/theme.toml` resolves twelve stable sprite names
to transparent bone/ivory and blackened-iron PNGs. The presentation supplies a
restrained dark-fantasy board, coordinates, click and drag movement, board flip,
move/capture hints, last-move and check marking, capture/castling/promotion effects,
SAN history, captured-material trays, and terminal overlays. Artwork prompts,
reference lineage, processing notes, and SHA-256 hashes live in
`assets/provenance.toml`; image generation is never called by configure, build, or
runtime.

The Catch2 suite covers the canonical initial perft sequence through depth 5,
reference positions, special rules, SAN, authoritative action enumeration, Python
transaction effects, presentation, history branching, undo/redo, state hashes, and
replay agreement. The pytest suite validates the portable manifest, movement DSL, and
action registration.
