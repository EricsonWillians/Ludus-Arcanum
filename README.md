<p align="center">
  <img src="docs/assets/ludus-arcanum-logo.png" width="320" alt="Ludus Arcanum emblem">
</p>

<h1 align="center">Ludus Arcanum</h1>

<p align="center"><em>Deterministic worlds. Programmable rules. Native performance.</em></p>

Ludus Arcanum is an open-source C++23 framework for deterministic, programmable board
games. C++ owns authoritative state and performance-critical mechanisms; game packages
define their rules in Python.

The repository is built through tested vertical slices. Milestones 0–8 provide a
buildable deterministic kernel, a trusted embedded-Python authoring boundary, a
complete dark-fantasy orthodox-chess package, a resilient native visual player, and a
GTK studio that creates, edits, validates, and playtests unpacked `.ludus` packages.
The current slice includes strong IDs, typed graph topology, reversible transactions,
canonical saves/hashes and replay, a typed Python SDK, native movement IR, immutable
value-only player views, desktop GL/GLES/Cairo rendering, bounded package-native PNG
themes, safe live reload, and a six-unit deterministic tactical skirmish with terrain,
objectives, fog, statuses, and recorded AI actions.

## Ubuntu 24.04 quick start

Install the baseline dependencies:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config python3-dev python3-venv \
    catch2 clang-format libgtkmm-4.0-dev libepoxy-dev libpng-dev
```

Catch2 3 and pybind11 are fetched at pinned releases and verified by SHA-256 when they
are not available as system packages. To require system dependencies, configure with
`-DLUDUS_FETCH_DEPENDENCIES=OFF`.

Configure, build, and test:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/ludus-server --self-check
./build/dev/ludus-server --demo
```

The GUI is optional. CMake builds `ludus-player` and `ludus-studio` when gtkmm 4 and
libepoxy development files are available and otherwise reports that those targets are
disabled; use `-DLUDUS_BUILD_GUI=OFF` for an explicit headless configuration. CI and
release builders should use the fail-fast `gui` preset so a missing optional dependency
cannot silently remove the applications:

```bash
cmake --preset gui
cmake --build --preset gui
ctest --preset gui
./build/gui/ludus-player --renderer auto
./build/gui/ludus-player --game tactical --renderer auto
./build/gui/ludus-player --game tactical --hot-seat --renderer auto
./build/gui/ludus-studio
```

The player opens chess by default; pass `--game tactical` for the 3v3 objective
skirmish. `--renderer auto` explicitly negotiates desktop OpenGL 3.3, GLES 3.0, then
Cairo/Pango software. Force a path with `gl`, `gles`, or `software`, and add
`--renderer-info` for the chosen API, vendor, renderer, version, and fallback reason.
No `GDK_DEBUG=gl-glx` workaround is required.

Chess opens in the dedicated Ivory Reliquary client. Click or drag pieces, right-drag
board annotations, use arrow keys and Enter for board focus, mouse-wheel zoom, and
middle-button pan. The header and More menu expose New/Open/Save, undo/redo, replay,
fit, flip, PGN/FEN clipboard and file workflows, resign/draw actions, high contrast,
reduced motion, and UI scale. The responsive move sidebar becomes a drawer at compact
widths; selecting a SAN row previews history without changing the live match. New Match
supports standard/custom FEN, orientation, unclocked play, presets from 1+0 through
30+0, and bounded custom controls. Native matches autosave after committed commands and
can be restored at startup.

Tactical retains its existing panel and interaction behavior, defaults to deterministic
Raiders AI, and can be switched to hot-seat mode. A physical stress capture can be run
with `./build/gui-release/ludus-player --stress-sprites 10000 --renderer auto`.

## Studio workflow

Start `ludus-studio`, enter an unused package directory such as
`/tmp/my-variation.ludus`, and choose **New**. The Studio writes a complete package,
including:

```text
my-variation.ludus/
├── game.toml
├── assets/
├── visuals/
│   └── theme.toml
├── boards/primary.board.toml
└── scripts/
    ├── __init__.py
    └── game.py
```

The left inspector edits package metadata, board dimensions, and chess-like entities.
The board tab draws logical topology links as presentation data; the source tab edits
the trusted Python rules. **Save** validates the whole in-memory document and replaces
each file through a temporary sibling. **Playtest** starts an authoritative session on
a worker thread. The lower tabs expose diagnostics, committed/redo event batches, and
the canonical native state. After editing `game.py`, **Reload rules** compiles and
validates the complete candidate movement/action registry at a safe boundary before
swapping it in. A syntax error or missing rule leaves both the active programs and
playtest state untouched.

Studio imports validated transparent PNGs into `assets/` with collision-safe atomic
writes, maintains the manifest allowlist and visual catalog, and previews sprites,
pivots, animation frames, alpha, and atlas pages. Entities use stable named sprites;
legacy numeric chess sprites are upgraded to known names or visible placeholders on
the next save. A valid asset/theme edit refreshes the preview immediately, while a
failed reload keeps the last valid catalog active and reports source-located errors.

The initial playtest adapter accepts an 8x8 position whose entity types are `pawn`,
`knight`, `bishop`, `rook`, `queen`, and `king`, with one king for each owner. This is
enough to create a chess-like movement variation entirely in Python. The document and
topology preview support rectangular boards up to 65,536 spaces; playing non-chess
topologies is the next package-adapter slice rather than a hidden kernel assumption.

Both reference games ship package-native visual themes and transparent PNG artwork.
Packages without a `visuals` entry continue to use procedural defaults. PPM loading is
retained only for legacy compatibility; PNG is the supported authoring format.

To include the pytest SDK suite in CTest, create its isolated environment before the
final configure (rerunning configure is safe):

```bash
python3 -m venv build/dev/pytest-venv
build/dev/pytest-venv/bin/python -m pip install -r requirements-dev.txt
cmake --preset dev
ctest --preset dev
```

Python support is enabled by default. A server-only build can omit CPython entirely
with `cmake --preset dev -DLUDUS_BUILD_PYTHON=OFF -DLUDUS_BUILD_GUI=OFF`.

## Python rule authoring

Rules declare common movement once and lower it to validated native bytecode during
package loading. Arbitrary trusted callbacks remain available for unusual mechanics:

```python
from ludus_arcanum import action, move

MOVEMENT_RULES = {
    "bishop": (
        move.rays(("north_east", "north_west", "south_east", "south_west"))
        .until_blocked()
        .allow_empty()
        .capture_enemy()
    )
}

@action("quantum_exchange")
def quantum_exchange(ctx, tx, actor, targets):
    destination = ctx.neighbors(ctx.entity(actor).location, "portal")[0]
    tx.move(actor, destination)
```

Callbacks receive expiring read-only contexts, value-only handles, and a controlled
transaction. An exception rejects the complete transaction and returns a traceback
with its source path and line. Embedded game packages are trusted code; this boundary
is an integrity boundary, not a security sandbox.

The reference package under `games/orthodox_chess` declares ordinary piece traversal
in Python, lowers it once to native IR, and resolves every accepted move through the
same authoritative transaction/event path as any other game. Castling, en passant,
promotion, check state, history, undo, and replay remain package behavior; no chess
concepts were added to the kernel.

The second reference package under `games/tactical_rpg` uses the same kernel for a
radius-three 37-hex objective encounter. Ranger, Warden, and Arcanist face Thorn
Guardian, Stalker, and Hexer with two-action initiative activations, terrain, cover,
LOS, poison, armor/ward effects, cards, and shrine scoring. Default Raider AI evaluates
only its viewer-visible projection and submits ordinary stable recorded tokens.
Version-1 saves retain their legacy action/presentation path. All tactical meaning
remains package behavior rather than entering the generic kernel.

Version-1 save migration is guarded by retained, repository-owned fixtures. Loading
verifies hashes against the legacy canonical encoding, translates history hashes and
checkpoints to the current encoding, and writes only current-version archives.

For an optimized build, use the `release` preset. To exercise AddressSanitizer and
UndefinedBehaviorSanitizer together, use the `sanitizers` preset. Leak detection is
disabled in that preset because it is incompatible with ptrace-managed build runners;
run a separate LeakSanitizer job on an unrestricted host when needed.

Build and run the optimized kernel benchmarks with:

```bash
cmake --preset benchmarks
cmake --build --preset benchmarks
./build/benchmarks/benchmarks/ludus-benchmarks
```

## Components

Working targets are `ludus-core`, `ludus-topology`, `ludus-rules`, `ludus-rule-ir`,
`ludus-python`, `ludus-render`, `ludus-studio-core`, `ludus-orthodox-chess`,
`ludus-tactical-rpg`, `ludus-gtk`, `ludus-player`, `ludus-studio`, `ludus-server`, and
`ludus-benchmarks`. The Python build also produces the
`ludus_arcanum._native` extension used by package tests and authoring tools. GUI targets
are conditionally available as described above. Server-only builds do not initialize
GTK or OpenGL, and rendering tests and benchmarks remain headless.

Architecture constraints and decisions live under [`docs/`](docs/).

## License

Ludus Arcanum is licensed under the MIT License. See [`LICENSE`](LICENSE).
