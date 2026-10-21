#!/usr/bin/env python3
"""Fetch the locked Windows libraries into a Linux cross-compilation prefix."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    cache = root / 'cache'
    cache.mkdir(parents=True, exist_ok=True)
    packages = json.loads((ROOT / 'packaging/windows/msys2.lock.json').read_text())

    def fetch(package):
        archive = cache / package['file']
        if not archive.exists():
            temporary = archive.with_suffix('.part')
            with urllib.request.urlopen('https://repo.msys2.org/mingw/mingw64/' + package['file']) as src:
                with temporary.open('wb') as dst:
                    while data := src.read(1024 * 1024):
                        dst.write(data)
            temporary.rename(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != package['sha256']:
            raise ValueError('SHA256 mismatch: ' + archive.name)
        print('Verified', archive.name, flush=True)
        return archive

    with ThreadPoolExecutor(max_workers=6) as pool:
        archives = list(pool.map(fetch, packages))
    for archive in archives:
        # Inspect the archive paths before extraction. Packages are SHA256 pinned above.
        process = subprocess.Popen(['zstd', '-dc', str(archive)], stdout=subprocess.PIPE)
        try:
            with tarfile.open(fileobj=process.stdout, mode='r|') as tar:
                for member in tar:
                    if member.name.startswith('mingw64/'):
                        tar.extract(member, root, filter='data')
        finally:
            process.stdout.close()
            if process.wait() != 0:
                raise RuntimeError('Cannot decompress ' + archive.name)
    prefix = root / 'mingw64'
    for path in (prefix / 'lib/pkgconfig').glob('*.pc'):
        path.write_text(path.read_text().replace('=/mingw64', '=' + str(prefix)))
    (root / 'resolved-packages.json').write_text(json.dumps(packages, indent=2) + '\n')


if __name__ == '__main__':
    main()
