#!/usr/bin/env python3
"""Use the same content verifier for PKG import and startup; never execute guest code here."""
import json
import os
from pathlib import Path
import subprocess


def verify_game(game, progress, root, cache_dir=None, force=False):
    root, progress = Path(root), Path(progress)
    tool = root / ('out/AegisPkg.exe' if os.name == 'nt' else 'out/pkg-import/release/aegis-pkg')
    if not tool.is_file():
        raise ValueError('文件校验组件缺失：' + str(tool))
    progress.parent.mkdir(parents=True, exist_ok=True)
    command = [str(tool), '--verify-full' if force else '--verify', str(game), str(progress),
               str(root / 'profiles/CUSA18093.json')]
    if cache_dir is not None:
        command.append(str(cache_dir))
    options = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}
    with progress.with_suffix('.log').open('w', encoding='utf-8') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, **options)
    if result.returncode:
        state = {}
        if progress.is_file():
            state = dict(line.split('=', 1) for line in progress.read_text(encoding='utf-8').splitlines() if '=' in line)
        raise ValueError(state.get('error', '游戏文件校验失败，详见：' + str(progress.with_suffix('.log'))))
    report = json.loads(progress.with_suffix('.json').read_text(encoding='utf-8'))
    if not report.get('passed'):
        raise ValueError('游戏文件校验未通过')
    return report
