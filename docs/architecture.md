# Architecture

Ludus Arcanum separates game meaning from engine mechanisms. Stable identifiers,
topology, state, transactions, deterministic random streams, and serialization belong
to native modules. Python constructs rules and may provide advanced callbacks, but it
does not own authoritative state. GTK input produces intents; a simulation thread
validates and commits them; rendering consumes immutable snapshots and presentation
events.

The intended dependency direction is:

```text
GTK input -> ActionIntent -> deterministic GameSession <-> Python / native rule IR
                                      |
                                      v
                         committed immutable events
                                      |
                                      v
                           immutable RenderSnapshot
```

Dependencies point inward. `ludus-core`, `ludus-topology`, `ludus-rules`, and
`ludus-rule-ir` have no GUI or Python dependency. `ludus-python` depends on those native
APIs; the server can be configured without it. `ludus-render` accepts value-only
snapshots and has no GTK, OpenGL, or Python dependency. `ludus-gtk` exposes one
`BoardCanvas` and owns either its accelerated `Gtk::GLArea` implementation or its
Cairo/Pango implementation, without placing one widget per logical object.
`ludus-studio-core` remains headless and owns validated package documents and textual
state/event inspection. The `ludus-studio` executable composes it with GTK, the Python
runtime, and a package playtest adapter.

Each new module is added with executable behavior and tests; empty architectural
facades are deliberately avoided.

## Deterministic kernel

`GameState` aggregates a symbol registry, immutable logical topology, native entity
store, and a bounded value-only effect stack. Runtime identity is a pair of 32-bit slot
and generation values; package-facing names are interned before play and represented
by domain-specific 32-bit IDs. Space and
entity properties use sorted typed storage, so canonical iteration never depends on a
hash-table order.

`GameSession` is the only public owner of mutable authoritative state. Registered
actions validate an `ActionIntent` and resolve through a controlled `Transaction`.
Mutations immediately update native storage while recording compact before/after
patches. Commit publishes typed events; failure or an exception applies patches in
reverse and restores every deterministic random stream. Ordinary actions therefore do
not copy the complete state.

Undo and redo apply the same patches. Replays apply immutable event batches from the
initial state or the latest 32-transaction checkpoint and verify the hash recorded after
every batch. Save archives contain the initial state, seed, current history cursor,
event log, random-stream continuation states, checkpoints, and final hash. Callback
registrations are runtime/package behavior and are intentionally rebound after load;
they are never serialized as code or stack frames.

The loader recognizes retained version-1 archives. It validates their recorded hashes
against the legacy state encoding, then normalizes initial state, checkpoints, and
history hashes to the current canonical format. Resaving consequently emits a fully
current archive rather than carrying nested legacy state blobs forward.

Effects retain only interned continuation actions, stable source/target handles, and
typed arguments. A top effect may expose one bounded `ChoiceWindow` with player-owned
options. Push, pop, request, and resolve operations produce ordinary patches and typed
events, so a paused decision participates in canonical hashes, save/load, replay,
rollback, undo, and redo. No Python callable, generator, frame, or package-specific
effect type enters authoritative state. Transactions reject a commit that would leave
an effect pointing at a destroyed entity.

Transactions are bounded to 65,536 committed events, legal-action enumeration returns
at most 65,536 intents, and a session retains at most 1,000,000 history entries. These
limits are versioned engine policy rather than allocations that can grow without bound.

## Native rule IR

`MovementRuleGraph` is an immutable authoring product, while `RuleProgram` is a
versioned, canonical native bytecode. Lowering validates structure, interns directions
before evaluation, removes duplicate directions, and normalizes operation order so
equivalent graphs have identical bytes and hashes. The first evaluator performs rays
and jumps as native batch operations over topology and a native occupancy index. It
handles collision, friendly blockers, and enemy captures without callbacks inside the
traversal loop.

The initial opcodes intentionally cover only the Milestone 2 movement slice. Flood
fill, pathfinding, predicates, expressions, effects, and triggers should extend the
validated IR when a working game slice needs them; they are not represented by empty
interfaces today.

## Trusted Python boundary

`PythonRuntime` owns one embedded CPython interpreter and is confined to the simulation
thread that created it. pybind11 exposes value-only entity, space, player, and action
handles; Python never receives storage addresses. A callback receives a capability-
checked read-only `RuleContext` plus a controlled `Transaction`. Those proxy objects
expire when the callback exits, including exceptional exits. All mutations and engine-
owned random draws therefore remain inside the same native rollback boundary as C++
rules.

Python exceptions become structured native diagnostics containing exception type,
message, source path, line, and formatted traceback. Reload requests are applied only
at an explicit session boundary. The action and movement registries are rebuilt and
validated before the new module view is accepted; registry state is restored if reload
fails.

Embedded packages are trusted code. This architecture protects state ownership and
determinism when packages use the SDK contract, but it is not a sandbox and does not
claim to restrict filesystem, network, or process access. Untrusted packages require a
future isolated worker process with operating-system resource controls.

## External game packages

Orthodox chess is the first complete package and lives entirely under
`games/orthodox_chess`. Its package-local C++ position type owns chess-specific
legality, FEN handling, and the perft oracle; none of those types or concepts enter a
generic engine module. Python declares rook, bishop, queen, king, and knight traversal
and supplies the controlled transaction callback that moves, captures, promotes, and
updates package metadata.

The package adapter maps 64 chess squares onto ordinary topology spaces and pieces
onto tagged, owned entities. It reconstructs a position from the authoritative native
state, intersects ordinary legal moves with the Python-authored native movement
programs, and emits canonical `ActionIntent` values. Accepted moves resolve through a
single transaction and produce normal entity/property events, so kernel undo, redo,
hashing, and replay need no chess-specific branches. Pawns, attacks, and special moves
remain in the package legality layer because the initial IR intentionally does not yet
encode those richer predicates.

`ChessMatch` is a package-owned deterministic policy layer around the unchanged
`ChessGame`. It records bounded player metadata, an optional base-plus-increment time
control, committed integer milliseconds, timed plies, draw state, and match outcomes in
a versioned native archive. The GTK controller uses `steady_clock` only to propose an
elapsed value; the match commits that value before a move and applies increment after
the move. Undo restores committed clocks, redo reuses the recorded elapsed value, and a
new branch discards both move and clock redo. Flag fall is a recorded deterministic
match command with undo/redo and archive semantics of its own. Position hashes,
legality, perft, and existing kernel archives remain `ChessGame` responsibilities.

Automatic results cover mate, stalemate, fivefold repetition, the 75-move rule, and
dead material, with mate checked first. Threefold and 50-move claims may describe the
current position or a qualifying intended legal move. Resignation, agreed draw, and
timeout are explicit match commands. The timeout material test asks whether the
opponent can possibly mate rather than applying the stricter dead-position shortcut.

The package also owns a bounded mainline-only PGN layer. Parsing validates all input
before replacing a live match, resolves SAN against legal moves, retains comments,
numeric NAGs, and clock annotations, accepts `SetUp`/`FEN`, and rejects variations with
line/column diagnostics. Normalized export emits the seven-tag roster, time control,
termination, custom-position tags, and per-ply clock annotations. PGN is capped at
1 MiB and 20,000 plies.

The second package under `games/tactical_rpg` maps a radius-three, 37-space axial hex
field and four logical containers onto the same generic topology. Version 2 adds
blocking ruins, cover, difficult terrain, a central shrine, three Vanguard and three
Raider units, two-action initiative activations, deterministic abilities and status
expiry, shrine/elimination/round-limit victory rules, and default deterministic Raider
AI. Package symbols and rules still own every one of those concepts. AI observes the
same viewer projection as its faction, uses stable-ID tie breaking, and submits
ordinary recorded action tokens. Retained version-1 saves use their original action
and presentation path without rewriting history or hashes.

## Presentation and accelerated player

`RenderSnapshot` is native presentation data: stable entity/space handles, separate
static/dynamic revisions, shape-aware spaces, layered entity and decoration sprites,
typed interaction markers, text, bars, effects, rich animation records, and labeled
action hints whose authoritative payload remains opaque. `PlayerView` pairs a snapshot
with generic value-only participant, clock, captured-item, timeline, result, match
control, draw-claim, action, initiative, score, and log records. Game-package adapters
build those values on the simulation
thread from a consistent authoritative state. Publication moves an immutable shared
view through an atomic exchange; the GTK thread never locks the simulation or calls
Python. A bounded command queue carries opaque action/choice tokens and player-control
requests in the opposite direction.

`SpriteBatch` converts a snapshot, interaction overlays, atlas regions, and the current
presentation time into backend-neutral static, dynamic, and effect instances. It
caches immutable layers by revision, retains capacity across frames, and sorts records
deterministically. Camera transforms, bounded PNG/legacy PPM decoding, multi-page atlas
packing, shape-aware picking, theme catalogs, and event-to-animation adaptation are
also headless `ludus-render` facilities and are covered by native tests and benchmarks.

`BoardCanvas` owns exactly one active renderer. Automatic selection explicitly tries
desktop OpenGL 3.3/GLSL 330, OpenGL ES 3.0/GLSL ES 300, then Cairo/Pango software.
Forced modes fail with the underlying `Glib::Error`; selected API, vendor, renderer,
version, and fallback reason are observable. Accelerated paths use instanced geometry,
retained buffers, multi-page premultiplied RGBA textures, sRGB-aware output, and
high-DPI Pango raster caching. Cairo follows the same shape, layout, camera, and input
semantics. Presentation clocks drive movement, capture, fade, scale, projectile,
impact, poison, check, and promotion effects but never mutate authoritative state.

The chess presentation adapter remains inside `games/orthodox_chess`: it maps generic
entities and legal moves to visual records and opaque numeric action tokens, resolves
the package's named ivory/iron piece and decoration sprites once at load, and derives
participant cards, clocks, SAN rows, captured material, last-move corners, check state,
claims, and terminal presentation. SAN, captures, and results are structured HUD data,
not screen-space board text. Packages and themes without new named color roles or
decorations receive procedural defaults. No chess type is introduced into
`ludus-render` or `ludus-gtk`.

`TacticalPresentation` creates an explicit viewer projection. Friendly units are
always visible, enemy units require a friendly observer within the package fog radius,
and opponent-owned cards are omitted. Its value-only HUD describes unit cards,
health/AP/status, initiative, objective score, abilities, combat log, and terminal
state. The authoritative state still retains all units and cards. Render and AI code
receive only their filtered immutable view and do not call Python.

The player controller hosts either reference package behind the same token submission
and snapshot publication infrastructure, while the application shell selects a
chess-specific responsive client or the unchanged tactical panel. The chess shell uses
a header bar, participant cards, structured history navigation, wide/medium sidebars,
a compact drawer, promotion popover, result overlay, import/export workflows, and
accessible controls. History preview reconstructs a separate presentation position,
pauses clocks, and refuses actions; it never moves the authoritative history cursor.
Right-drag annotations are window-local presentation records and clear after a committed
move. Preferences are atomically replaced under the user's configuration directory,
and committed match commands trigger crash-safe autosave. Tactical tokens are resolved
only against freshly enumerated legal actions or the current choice window, so a stale
UI snapshot cannot bypass package validation.

## Studio documents and safe playtesting

An initial Studio package is an unpacked `.ludus` directory with a strict `game.toml`,
a declarative board file, Python package initializers, and an editable entry module.
`PackageDocument` parses a bounded TOML schema, caps text at 1 MiB and topology at
65,536 spaces/entities, rejects path traversal and enabled native extensions, and
validates the complete in-memory model before saving. Each output file is written to a
temporary sibling and renamed into place so an interrupted individual replacement
does not expose a partial file. Package-wide journaling is intentionally deferred.

An optional manifest `visuals` path names a bounded package theme. The headless catalog
validates canonical containment, symlinks, manifest membership, PNG integrity and
dimensions, finite theme values, animation references, and atlas capacity before it
can replace the active catalog. Studio imports into `assets/` through collision-safe
atomic writes, maintains the allowlist/catalog, previews atlas pages and pivots, and
stores stable sprite names. Legacy numeric chess sprites are mapped to known names or
visible placeholders and serialize in named form on the next save.

Edit previews turn logical spaces and links into value-only render records; coordinates
remain presentation data and never become movement authority. Package and entity
inspector mutations are transactional at the document level: invalid changes restore
the prior values. The first playtest adapter maps the editable 8x8 chess-like board to
the external orthodox-chess package. This adapter belongs to the application layer;
neither `ludus-studio-core` nor the kernel knows chess piece types.

The Studio playtest controller owns CPython, `ChessGame`, and presentation construction
on one simulation thread. GTK sends numeric commands through a bounded 64-entry queue
and receives atomic `shared_ptr<const StudioView>` values through a dispatcher. Each
view contains a complete render snapshot plus derived event-log and native-state text.
The renderer therefore neither calls Python nor locks the simulation.

Live reload is a two-phase replacement. The runtime reloads into a backed-up module
dictionary, validates its generic registries, and asks the chess adapter to compile
every required movement program and action into temporary native values. Only a fully
valid candidate advances the module generation and swaps the compiled program map. On
any syntax, schema, or compile failure, the old module dictionary, active programs,
session history, and state hash remain intact.
