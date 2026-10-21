#!/usr/bin/env python3
"""Validate the audited game files and prepare a native linked image."""
import argparse
import json
import os
from pathlib import Path

import content_profile
import link_modules
import prepare
from aegis_verify import verify_game

ROOT = Path(__file__).resolve().parents[1]


def build(game, out, np_mode='native', cache_dir=None, verify_full=False):
    game, out = Path(game).resolve(), Path(out).resolve()
    profile = json.loads((ROOT / 'profiles/CUSA18093.json').read_text())
    out.mkdir(parents=True, exist_ok=True)
    verified = verify_game(game, out / 'verification.ini', ROOT, cache_dir, verify_full)
    info = prepare.sfo((game / 'sce_sys/param.sfo').read_bytes())
    if info.get('TITLE_ID') != profile['title_id']:
        raise ValueError('Unexpected game title')
    out.mkdir(parents=True, exist_ok=True)
    previous = os.environ.get('BB_SKIP_GAME_CHECK')
    try:
        # The exact Aegis inputs were verified above; the upstream check only accepts Bloodborne.
        os.environ['BB_SKIP_GAME_CHECK'] = '1'
        prepare.prepare(game, out)
    finally:
        if previous is None:
            os.environ.pop('BB_SKIP_GAME_CHECK', None)
        else:
            os.environ['BB_SKIP_GAME_CHECK'] = previous
    modules = profile['native_modules'] if np_mode == 'native' else ['libc.prx', 'libSceFios2.prx']
    link_modules.link(game, out, modules)
    content_profile.prepare(game, out, 'full')
    manifest = dict(title_id=info['TITLE_ID'], app_ver=info.get('APP_VER'), version=info.get('VERSION'),
                    files=profile['files'], native_modules=modules, np_mode=np_mode,
                    environment=profile['environment'], game=str(game),
                    verification=dict(report=str(out / "verification.json"), mode=verified["mode"],
                                      files=verified["total_files"], cached=verified["cached_files"]))
    (out / 'aegis-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('STAGE INPUT_VERIFIED: CUSA18093 audited files; no guest code executed')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--np-mode', choices=['native', 'minimal'], default='native')
    args = parser.parse_args()
    build(args.game, args.out, args.np_mode)
