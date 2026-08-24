# Tactical skirmish package

Scenario version 2 is a deterministic 3v3 objective skirmish on a radius-three,
37-hex battlefield with blocking ruins, cover, difficult terrain, and a central
shrine. Four additional topology spaces hold logical inventory, deck, and discard
state; they are not rendered as terrain.

The Vanguard field Ranger, Warden, and Arcanist. Raiders field Thorn Guardian,
Stalker, and Hexer. Initiative-ordered units receive two action points, move at most
once, use at most one offensive ability, then end their activation. Package-owned
rules implement Quick/Venom/Focused Shot, Shield Bash/Guard, Arc Bolt/Ward,
Crush/Bulwark/thorns, Dash/Ambush, and Blight Bolt/Drain, including armor, poison,
push, healing, LOS, range, and deterministic status expiry.

Exactly one faction occupying the shrine scores at round end. Three points or enemy
elimination wins; round ten resolves by shrine score, then surviving health, then a
draw. Raiders are AI-controlled by default, with optional hot-seat play. The AI reads
only its viewer-visible state, prioritizes lethal attacks and shrine scoring before
safety/cover/distance, breaks ties by stable ID, and submits ordinary recorded tokens.

`TacticalPresentation` publishes only immutable value data: the filtered battlefield,
unit health/AP/status, initiative, objective score, ability tray, range/LOS previews,
combat log, effects, and terminal overlay. Hidden enemies and cards never reach that
view. In AI mode, player undo groups a human decision with the resulting AI
activations while the kernel retains transaction-level history.

Version-1 saves remain loadable through the retained legacy action and presentation
path and are not rewritten on resave. Package artwork and its bounded named theme live
under `assets/` and `visuals/`; provenance records prompts, tool, processing, and
SHA-256 hashes.

Build and run:

```bash
cmake --preset gui
cmake --build --preset gui
./build/gui/ludus-player --game tactical --renderer auto --renderer-info
./build/gui/ludus-player --game tactical --hot-seat --renderer auto
```

The package tests cover the action/token boundary, ability resolution, deterministic
AI, terrain/LOS/fog projection, status and initiative transitions, objective victory,
save/restore, replay, grouped undo, stale tokens, and the legacy scenario path.
