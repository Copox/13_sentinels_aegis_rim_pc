#!/usr/bin/env python3
"""Collect notices for the locked Rust importer and its dependencies."""
import json
from pathlib import Path
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[1]
metadata = json.loads(subprocess.check_output([
    'cargo', 'metadata', '--locked', '--format-version', '1',
    '--manifest-path', str(ROOT / 'tools/pkg-import/Cargo.toml')]))
out = ROOT / 'out/pkg-licenses'
out.mkdir(parents=True, exist_ok=True)
entries = []
for package in metadata['packages']:
    source = Path(package['manifest_path']).parent
    name = package['name'] + '-' + package['version']
    target = out / name
    target.mkdir(exist_ok=True)
    copied = []
    for path in source.iterdir():
        if path.is_file() and path.name.lower().startswith(('license', 'licence', 'copying', 'notice', 'copyright')):
            shutil.copy2(path, target / path.name)
            copied.append(path.name)
    if package['name'].startswith('orbis-'):
        # crates.io 0.1.0 omits the workspace license files; include upstream notices.
        for path in (ROOT / 'tools/pkg-import/vendor').glob('LICENSE-*'):
            shutil.copy2(path, target / path.name)
            copied.append(path.name)
    shutil.copy2(package['manifest_path'], target / 'Cargo.toml')
    entries.append(dict(name=name, license=package['license'], repository=package['repository'], notices=copied))
(out / 'DEPENDENCIES.json').write_text(json.dumps(entries, indent=2) + '\n')
sysroot = Path(subprocess.check_output(['rustc', '--print', 'sysroot'], text=True).strip())
rust_notices = sysroot / 'share/doc/rust'
target = out / 'Rust-standard-library'
target.mkdir(exist_ok=True)
if rust_notices.is_dir():
    for name in ('LICENSE-MIT', 'LICENSE-APACHE', 'COPYRIGHT'):
        shutil.copy2(rust_notices / name, target / name)
else:
    # Debian's Rust toolchain ships its notices in the package documentation.
    shutil.copy2(sysroot / 'share/doc/rustc/copyright', target / 'copyright')
(target / 'VERSION.txt').write_text(subprocess.check_output(['rustc', '--version'], text=True))
print('Importer notices:', out)
