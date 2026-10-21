#!/usr/bin/env python3
"""Package the Windows executable, its DLL closure, and embedded Python."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PYTHON_SHA256 = '3de305b550bdc582f7c31a0f286f5b08c453ae5628ef2800a1bb1f86a42b746c'
SYSTEM_DLLS = {
    'advapi32.dll', 'avrt.dll', 'bcrypt.dll', 'bcryptprimitives.dll', 'comdlg32.dll', 'crypt32.dll', 'd3d12.dll',
    'dbghelp.dll', 'dxgi.dll', 'gdi32.dll', 'imm32.dll', 'iphlpapi.dll', 'kernel32.dll',
    'msvcrt.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'rpcrt4.dll', 'secur32.dll',
    'setupapi.dll', 'shell32.dll', 'shlwapi.dll', 'user32.dll', 'userenv.dll', 'version.dll',
    'winmm.dll', 'ws2_32.dll', 'cfgmgr32.dll', 'dinput8.dll', 'dwmapi.dll',
    'dxcore.dll', 'hid.dll', 'normaliz.dll', 'propsys.dll', 'psapi.dll',
    'shcore.dll', 'uxtheme.dll', 'winhttp.dll', 'wininet.dll', 'wtsapi32.dll',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', required=True, type=Path)
    parser.add_argument('--python-zip', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--binary', type=Path, default=ROOT / 'out/windows-clang/Aegis.exe')
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error('--out must be empty or new')
    if hashlib.sha256(args.python_zip.read_bytes()).hexdigest() != PYTHON_SHA256:
        parser.error('Unexpected Python archive; need python-3.13.8-embed-amd64.zip')
    libgcc = subprocess.check_output(['x86_64-w64-mingw32-gcc-posix', '-print-libgcc-file-name'], text=True).strip()
    roots = [Path(libgcc).parent, args.prefix.resolve() / 'bin', Path('/usr/x86_64-w64-mingw32/lib')]
    dlls = {}
    for root in roots:
        for path in root.glob('*.dll'):
            dlls.setdefault(path.name.lower(), path)
    binary_dir = out / 'out'
    binary_dir.mkdir(parents=True)
    shutil.copy2(args.binary, binary_dir / 'Aegis.exe')
    queue = [binary_dir / 'Aegis.exe']
    launcher = binary_dir / 'AegisLauncher.exe'
    shutil.copy2(args.binary.parent / 'AegisLauncher.exe', launcher)
    queue.append(launcher)
    importer = binary_dir / 'AegisPkg.exe'
    shutil.copy2(args.binary.parent / 'AegisPkg.exe', importer)
    queue.append(importer)
    entry = out / 'AegisLauncher.exe'
    shutil.copy2(args.binary.parent / 'AegisLauncherStart.exe', entry)
    queue.append(entry)
    subprocess.run([sys.executable, str(ROOT / 'scripts/aegis_assets.py')], check=True)
    (out / 'assets').mkdir()
    shutil.copy2(ROOT / 'out/aegis.bmp', out / 'assets/aegis.bmp')
    shutil.copy2(ROOT / 'out/launcher-background.bmp', out / 'assets/launcher-background.bmp')
    seen = set()
    os_imports = set()
    while queue:
        path = queue.pop()
        headers = subprocess.check_output(['x86_64-w64-mingw32-objdump', '-p', str(path)], text=True)
        for name in re.findall(r'DLL Name: (\S+)', headers):
            lower = name.lower()
            if lower in seen:
                continue
            seen.add(lower)
            if lower in dlls:
                source = dlls[lower]
                target = binary_dir / source.name
                shutil.copy2(source, target)
                queue.append(target)
            elif lower in SYSTEM_DLLS or lower.startswith(('api-ms-win-', 'ext-ms-win-')):
                os_imports.add(name)
            else:
                raise ValueError('Unresolved Windows DLL: ' + name)
    for path in binary_dir.iterdir():
        subprocess.run(['x86_64-w64-mingw32-strip', '--strip-unneeded', str(path)], check=True)
    subprocess.run(['x86_64-w64-mingw32-strip', '--strip-unneeded', str(entry)], check=True)
    with zipfile.ZipFile(args.python_zip) as archive:
        archive.extractall(out / 'python')
    (out / 'python/python313._pth').write_text('python313.zip\n.\n../scripts\n')
    scripts = ['aegis_prepare.py', 'aegis_run.py', 'aegis_verify.py', 'prepare.py', 'content_profile.py',
               'link_modules.py', 'import_scopes.py', 'link_libc.py', 'game_check.py']
    (out / 'scripts').mkdir()
    for name in scripts:
        shutil.copy2(ROOT / 'scripts' / name, out / 'scripts' / name)
    shutil.copytree(ROOT / 'profiles', out / 'profiles')
    for name in ['README.en.txt', 'README.zh-CN.txt']:
        shutil.copy2(ROOT / 'packaging/windows' / name, out / name)
    shutil.copy2(ROOT / 'NOTICE.md', out / 'NOTICE.md')
    shutil.copy2(ROOT / 'LICENSE', out / 'LICENSE')
    revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    (out / 'REVISION').write_text(revision + '\n')
    license_dir = out / 'licenses'
    license_dir.mkdir()
    shutil.copytree(ROOT / 'out/pkg-licenses', license_dir / 'PKG-importer')
    shutil.copy2(ROOT / 'gpu/third_party/imgui/LICENSE.txt', license_dir / 'Dear-ImGui-LICENSE.txt')
    for root in (args.prefix / 'share/licenses', ROOT / 'third_party/LibAtrac9'):
        if root.exists():
            for path in root.rglob('*'):
                if path.is_file() and ('license' in path.name.lower() or 'copying' in path.name.lower()):
                    relative = path.relative_to(root)
                    target = license_dir / root.name / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, target)
    (license_dir / 'SOURCES.txt').write_text(
        'Runtime source / 运行时源码: https://github.com/kxn/13_sentinels_aegis_rim_pc\n'
        'FFmpeg 7.1.2 (LGPL): https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz\n'
        'Build options / 构建选项: scripts/aegis_build_windows.sh\n'
        'SDL3 / xxhash versions and hashes / 版本与哈希: packaging/windows/msys2.lock.json\n'
        'GCC runtime 14-posix: Debian gcc-mingw-w64-x86-64-posix-runtime 14.2.0; '
        'GCC runtime library exception / GCC 运行库例外: https://gcc.gnu.org/gcc-14/\n'
        'Python 3.13.8: https://www.python.org/ftp/python/3.13.8/\n')
    # Static source dependencies retain their original notices in binary packages.
    source_roots = [ROOT / 'gpu/third_party', ROOT / 'gpu/dlss_bridge',
                    args.binary.parent / '_deps']
    for index, source_root in enumerate(source_roots):
        if not source_root.is_dir(): continue
        for source in source_root.rglob('*'):
            if source.is_file() and source.name.lower().startswith(('license', 'copying', 'copyright', 'notice')):
                target = license_dir / ('source-' + str(index)) / source.relative_to(source_root)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
    ffmpeg_source = args.prefix.parent / 'ffmpeg-7.1.2'
    for name in ['COPYING.LGPLv2.1', 'COPYING.LGPLv3', 'LICENSE.md']:
        if (ffmpeg_source / name).exists():
            shutil.copy2(ffmpeg_source / name, license_dir / ('FFmpeg-' + name))
    gcc_copyright = Path('/usr/share/doc/gcc-mingw-w64-x86-64-posix-runtime/copyright')
    if gcc_copyright.exists():
        shutil.copy2(gcc_copyright, license_dir / 'GCC-runtime-copyright.txt')
    manifest = dict(commit=revision, platform='Windows x64',
                    python_sha256=PYTHON_SHA256, system_dlls=sorted(os_imports), files={})
    for path in sorted(out.rglob('*')):
        if path.is_file():
            manifest['files'][str(path.relative_to(out))] = hashlib.sha256(path.read_bytes()).hexdigest()
    (out / 'package-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    # Some upstream license notices have Unix-epoch mtimes. ZIP starts at 1980;
    # clamp those timestamps instead of leaving a partially written release.
    archive = str(out) + '.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, strict_timestamps=False) as zipped:
        for path in sorted(out.rglob('*')):
            if path.is_file():
                zipped.write(path, str(path.relative_to(out.parent)))
    print('Packaged', archive)


if __name__ == '__main__':
    main()
