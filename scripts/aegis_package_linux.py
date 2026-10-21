#!/usr/bin/env python3
"""Package Linux executables with original art; use system SDL/Vulkan/FFmpeg libraries."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ['aegis_prepare.py', 'aegis_run.py', 'aegis_verify.py', 'prepare.py',
           'content_profile.py', 'link_modules.py', 'import_scopes.py', 'link_libc.py', 'game_check.py']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    output = args.out.resolve()
    if output.exists() or Path(str(output) + '.tar.gz').exists():
        parser.error('Output already exists')
    files = ['out/aegis', 'out/gpu/AegisLauncher', 'out/gpu/libbbgpu.so',
             'out/pkg-import/release/aegis-pkg']
    for name in files:
        if not (ROOT / name).is_file():
            parser.error('Missing build output: ' + name)
    for name in files + ['LICENSE', 'NOTICE.md', 'README.md', 'README.zh-CN.md']:
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, target)
    for name in SCRIPTS:
        target = output / 'scripts' / name
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'scripts' / name, target)
    shutil.copytree(ROOT / 'profiles', output / 'profiles')
    (output / 'assets').mkdir()
    for name in ['aegis.bmp', 'launcher-background.bmp']:
        shutil.copy2(ROOT / 'out' / name, output / 'assets' / name)
    shutil.copytree(ROOT / 'out/pkg-licenses', output / 'licenses/PKG-importer')
    for source in (ROOT / 'gpu/third_party').rglob('*'):
        if source.is_file() and source.name.lower().startswith(('license', 'copying', 'copyright', 'notice')):
            target = output / 'licenses/source' / source.relative_to(ROOT / 'gpu/third_party')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    entry = output / 'AegisLauncher'
    entry.write_text('#!/bin/sh\ncd -- "$(dirname -- "$0")"\nexec ./out/gpu/AegisLauncher "$@"\n')
    entry.chmod(0o755)
    (output / 'REVISION').write_bytes(subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD']))
    with tarfile.open(str(output) + '.tar.gz', 'w:gz') as archive:
        archive.add(output, arcname=output.name)
    print(str(output) + '.tar.gz')


if __name__ == '__main__':
    main()
