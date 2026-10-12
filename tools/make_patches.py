#!/usr/bin/env python3
"""Regenerate em2coop's PlayForward install instructions.

em2coop changes two of the game's own data files: InputMappings.xml (the
player-2 action set) and packfiles/UIhud.pak (player 2's reticle, the
split-aware HUD script and Oswald's status panel). Those files are Disney's,
so the package never carries them. It carries VCDIFF deltas (xdelta3) that
PlayForward's client applies to the player's own copies, pinned by SHA-256 at
both ends, plus the digest of the one DEM2.exe the DLL's addresses fit.

  tools/make_patches.py <stock game folder> <modded InputMappings.xml> <modded UIhud.pak>

The stock folder must be an untouched Steam install (or a copy of its
DEM2.exe, binkw32.dll, InputMappings.xml and packfiles/UIhud.pak). Writes
playforward/patches/*.vcdiff and playforward/playforward-mod.json.
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'playforward'
PATCHES = 'patches'
MANIFEST = 'playforward-mod.json'

# Must match engine::kGameSha256 in src/engine.h.
GAME_EXE = 'DEM2.exe'
# The proxy forwards to the game's own Bink DLL, kept under this name.
BINK = 'binkw32.dll'
BINK_KEPT = 'binkw32_real.dll'

# Game file (relative, forward slashes) -> argument index of its modded copy.
PATCHED = {
    'InputMappings.xml': 2,
    'packfiles/UIhud.pak': 3,
}

# xdelta3: best compression, no secondary compressor (the client's decoder
# supports plain VCDIFF), and no application header naming local files.
XDELTA_FLAGS = ['-9', '-S', 'none', '-A', '-f', '-e']


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    stock = Path(argv[1])

    exe_digest = sha256(stock / GAME_EXE)
    engine_h = (ROOT / 'src' / 'engine.h').read_text()
    if exe_digest not in engine_h:
        sys.exit(f'{GAME_EXE} is {exe_digest}, not the build src/engine.h '
                 'was made for (kGameSha256)')

    (OUT / PATCHES).mkdir(parents=True, exist_ok=True)
    patch_files = []
    for game_file, arg in PATCHED.items():
        original = stock / game_file
        modded = Path(argv[arg])
        delta_name = f'{PATCHES}/{Path(game_file).name}.vcdiff'
        subprocess.run(['xdelta3', *XDELTA_FLAGS, '-s', str(original),
                        str(modded), str(OUT / delta_name)], check=True)
        patch_files.append({
            'file': game_file,
            'delta': delta_name,
            'from_sha256': sha256(original),
            'to_sha256': sha256(modded),
            'to_size': modded.stat().st_size,
        })

    manifest = {
        'requires': [
            {'file': GAME_EXE, 'sha256': exe_digest},
            # The kept copy must be RAD's own DLL, not another proxy.
            {'file': BINK, 'sha256': sha256(stock / BINK)},
        ],
        'keep_originals': [{'file': BINK, 'as': BINK_KEPT}],
        'patch_files': patch_files,
    }
    (OUT / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n')
    for patch in patch_files:
        size = (OUT / patch['delta']).stat().st_size
        print(f"{patch['delta']}: {size} bytes")
    print(f'wrote {OUT / MANIFEST}')


if __name__ == '__main__':
    main(sys.argv)
