# em2coop

Local two-player co-op for the Steam PC build of Disney Epic Mickey 2, as a
`binkw32.dll` proxy. This is the production counterpart of
`runtime_probe/`: the probe is where things were found and measured; this DLL
contains only what the probe proved, organised the way the game would have
had it.

## Design rule

Use the engine for everything it still does. Implement only what the PC build
removed.

The PC build (Vita lineage) keeps almost all of the co-op machinery. At
startup SceneApp still runs "Initializing Camera Controller" (`0x00ed738b`):

```
FUN_00ed7920()                    [0x0143b904] = PlayerCamera(1)   Oswald
FUN_00ed7980()                    [0x0143b900] = PlayerCamera(0)   Mickey
PlayerManager::SetPlayerCamera(0, [0x0143b900])   FUN_00976da0
PlayerManager::SetPlayerCamera(1, [0x0143b904])
```

and from then on the engine creates, resets (`0x00f18470`), updates (every
frame while player 2's record+0x25 is set) and device-resets both players'
cameras by itself. Player records, control modes, pad assignment, spawning,
the `...2` input actions, the HUD's Oswald panel and its publisher all
survive. SceneApp even still computes left/right/top/bottom split viewports
(`app+0x2090c..`), though nothing reads them any more.

What is gone, and what this DLL supplies:

| Removed from PC | Source of truth | Here |
|---|---|---|
| PlayerManager's `ToggleSecondPlayer` listener (join/leave) | Wii `0x80afab28` | `coop.cpp` `task_toggle`, `join`, `leave` |
| Bodies of `EnableSplitScreen`, `DisableToggleSecondPlayer`, `ActivateSplitScreen`, `DeactivateSplitScreen` (script bindings still registered and called by level scripts) | Wii `0x80afaac4`, `0x80afab20`, `0x80afa35c`, `0x80afa680` | `coop.cpp` script bindings |
| Split-state fields (PlayerManager `+0x234..0x240` on Wii; other fields on PC) | Wii | `coop.cpp` session state |
| The renderer's second viewport (listener of Wii event `0xAB`) | never in this branch | `split_screen.cpp` |

And fixes for engine bugs co-op exposes (`fixes.cpp`, `players.cpp`,
`world_views.cpp`, `split_screen.cpp`): the animation-playlist null clip, the
HUD cursor feed's hardcoded mouse index, Oswald's cursor bound to player 1's
actions (PC data), residual AI behaviours on a replaced player-2 entity, and
engine state that assumes one camera per frame:

- billboards face each half's camera while that half renders;
- world button prompts (the Pre HUD "on top" list) are drawn in each half
  with that half's camera, instead of once over the whole screen with the
  last one;
- the HUD's `UseHorizontalSplitScreen` reports the real split, so the
  reticles are offset and Oswald's is shown only while split.

## Files

| File | What |
|---|---|
| `engine.h` | every address and offset, as Ghidra VAs |
| `main.cpp` | proxy entry, bootstrap, upkeep loop |
| `coop.cpp` | session state machine and script bindings |
| `players.cpp` | PlayerManager/PadManager access, per-player cameras, cursor |
| `input.cpp` | join button via the game's own XInputGetState pointer |
| `split_screen.cpp` | vertical split rendering |
| `world_views.cpp` | billboards and world prompts per half |
| `fixes.cpp` | playlist wrap, cursor HUD feed |
| `build_check.cpp` | refuses any `DEM2.exe` but the one `engine.h` was read from |
| `game_thread.cpp` | runs engine calls on the game thread |
| `util.cpp` | log, safe memory access, patch helpers |

## Playing

Player 2 presses **Start** on the second controller to join once Oswald is in
the level, and again to leave (Oswald returns to the AI). The split follows:
Mickey left, Oswald right; level scripts turn it off and back on around
transitions, as on consoles. Options are in `em2coop.ini`.

Besides the DLL, co-op needs two of the game's data files changed:
`InputMappings.xml` (the player-2 action set, including `MouseCursor2Push`)
and `packfiles/UIhud.pak` (player 2's reticle, the split-aware HUD script and
Oswald's status panel). Those files are Disney's, so they are never shipped:
`playforward/patches/` holds VCDIFF deltas that are applied to the player's
own copies.

## Supported build

Only the Steam release of `DEM2.exe` (SHA-256 in `engine::kGameSha256`).
Every address in `engine.h` was read from that file. On any other build the
DLL logs the mismatch and does nothing but forward Bink, and PlayForward
refuses to install the package in the first place.

## Install (PlayForward)

Players switch the mod on in PlayForward. The client checks the game is the
supported build, keeps the game's `binkw32.dll` as `binkw32_real.dll`, drops
in the proxy and `em2coop.ini`, and patches the two data files, verifying each
one's SHA-256 before and after. Switching the mod off puts every file back.
The instructions it follows are `playforward/playforward-mod.json`.

## Package

```bash
./package.sh [version]
```

builds the DLL and writes `dist/em2coop-<version>.zip`: the archive uploaded
to PlayForward. CI (`.github/workflows/build.yml`) builds and checks it on
every push.

When the data patches change (the HUD pak or the input mappings are rebuilt
with the research tools), regenerate the deltas and instructions from an
untouched install:

```bash
tools/make_patches.py <stock game folder> <modded InputMappings.xml> <modded UIhud.pak>
```

It needs `xdelta3`, and refuses a game folder whose `DEM2.exe` is not the
supported build.

## Manual install

In the game directory: rename the original `binkw32.dll` to
`binkw32_real.dll`, copy `build/binkw32.dll` in, optionally `em2coop.ini`,
and apply the two deltas with
`xdelta3 -d -s <original> playforward/patches/<file>.vcdiff <output>`.
Log: `em2coop.log` next to the DLL.

## Known open items

- Reflections/refraction (steam, water): fix built 2026-10-04, untested (duplicates now carry the scene target group).
- Shops may need the split turned off and on again after leaving.
