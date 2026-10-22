#!/usr/bin/env python3
"""Run a verified game image with bounded execution and reproducible logs."""
import argparse
import hashlib
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import time

from aegis_prepare import ROOT, build


def no_console_options():
    """A GUI parent must not create consoles for background command-line tools."""
    return {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}


def platform_description():
    # platform.platform() can fall back to shell commands such as "ver".
    if os.name == 'nt':
        version = sys.getwindowsversion()
        return f'Windows-{version.major}.{version.minor}.{version.build}-{os.environ.get("PROCESSOR_ARCHITECTURE", "unknown")}'
    info = os.uname()
    return f'{info.sysname}-{info.release}-{info.machine}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--user', type=Path)
    parser.add_argument('--config', type=Path, help='Persistent host settings; default: <user>/aegis.ini')
    parser.add_argument('--binary', type=Path,
                        default=ROOT / ('out/Aegis.exe' if os.name == 'nt' else 'out/aegis'))
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--cpu-only', action='store_true')
    parser.add_argument('--bind-only', action='store_true')
    parser.add_argument('--verify-full', action='store_true', help='Ignore prior content verification records')
    parser.add_argument('--np-mode', choices=['native', 'minimal'], default='native')
    window_mode = parser.add_mutually_exclusive_group()
    window_mode.add_argument('--fullscreen', dest='fullscreen', action='store_true')
    window_mode.add_argument('--windowed', dest='fullscreen', action='store_false')
    parser.set_defaults(fullscreen=None)
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error('--timeout must be positive')
    out = args.out.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error('--out must be empty or new; keep previous run evidence')
    out.mkdir(parents=True, exist_ok=True)
    user = (args.user or out / 'user').resolve()
    user.mkdir(parents=True, exist_ok=True)
    manifest = build(args.game, out / 'image', args.np_mode,
                     cache_dir=user / 'verification-cache', verify_full=args.verify_full)
    config = (args.config or user / 'aegis.ini').resolve()
    if args.config and not config.is_file():
        parser.error('--config must name an existing settings file')
    template = ROOT / 'profiles' / (manifest['title_id'] + '.ini')
    if not args.config and not config.exists() and template.is_file():
        shutil.copyfile(template, config)
    # Snapshot settings in each run; later edits must not change the recorded configuration.
    if config.is_file():
        shutil.copyfile(config, out / 'runtime.ini')
    env = os.environ.copy()
    env.update(manifest['environment'])
    if args.fullscreen is not None:
        env['BB_FULLSCREEN'] = '1' if args.fullscreen else '0'
    env['BB_CONFIG'] = str(out / 'runtime.ini')
    if config.is_file():
        values = {}
        for line in config.read_text(encoding='utf-8').splitlines():
            if '=' in line and not line.lstrip().startswith(('#', ';')):
                key, value = line.split('=', 1)
                values[key.strip()] = value.strip()
        if 'display_fps' in values:
            try:
                rate = int(values['display_fps'])
            except ValueError:
                parser.error('display_fps must be 0 (unlimited) or 30..1000')
            if rate != 0 and not 30 <= rate <= 1000:
                parser.error('display_fps must be 0 (unlimited) or 30..1000')
            env['AEGIS_PRESENT_FPS'] = str(rate)
            # Display refresh is independent of the guest's 60 Hz clock.
            env['BB_VBLANK_HZ'] = '60'
            env['BB_FPS_LIMIT'] = '0'
        if 'display_vsync' in values:
            env['BB_PRESENT_MODE'] = 'Fifo' if values['display_vsync'] != '0' else 'Immediate'
        if 'language' in values:
            try:
                language = int(values['language'])
            except ValueError:
                parser.error('language must be 9 (Korean), 10 (Traditional Chinese) '
                             'or 11 (Simplified Chinese)')
            if language not in (9, 10, 11):
                parser.error('language must be 9 (Korean), 10 (Traditional Chinese) '
                             'or 11 (Simplified Chinese)')
            env['BB_LANGUAGE'] = str(language)
    for icon in (ROOT / 'assets/aegis.bmp', ROOT / 'out/aegis.bmp'):
        if icon.is_file():
            env['BB_WINDOW_ICON'] = str(icon)
            break
    command = [str(args.binary.resolve()), str(out / 'image/boot-linked.bin'),
               '--app0', str(args.game.resolve()), '--user', str(user),
               '--content-profile', str(out / 'image/content.bin'), '--timeout', '0']
    if args.cpu_only:
        command.append('--cpu-only')
    if args.bind_only:
        command.append('--bind-only')
    revision = ROOT / 'REVISION'
    manifest.update(command=command, binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                    commit=revision.read_text().strip() if revision.is_file() else 'unavailable',
                    platform=platform_description(),
                    started_utc=datetime.now(timezone.utc).isoformat(), user=str(user),
                    config=str(config), config_sha256=(hashlib.sha256((out / 'runtime.ini').read_bytes()).hexdigest()
                                                      if (out / 'runtime.ini').exists() else None),
                    display=env.get('DISPLAY'), timeout_seconds=args.timeout,
                    effective_environment={k: v for k, v in env.items()
                                           if k.startswith(('BB_', 'SDL_', 'AEGIS_', 'MESA_', 'VK_')) or
                                           k in ('DISPLAY', 'WAYLAND_DISPLAY', 'PULSE_SERVER', 'WINEDLLOVERRIDES')})
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    started = time.monotonic()
    timed_out = False
    interrupted = False
    with (out / 'stdout.log').open('w') as stdout, (out / 'stderr.log').open('w') as stderr:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stdout, stderr=stderr,
                                   start_new_session=os.name == 'posix',
                                   **no_console_options())
        (out / 'pid').write_text(str(process.pid)+'\n')
        print(f'Running PID {process.pid}; logs: {out}', flush=True)
        try:
            process.wait(timeout=args.timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as reason:
            timed_out = isinstance(reason, subprocess.TimeoutExpired)
            interrupted = isinstance(reason, KeyboardInterrupt)
            if os.name == 'posix' and process.poll() is None:
                os.kill(process.pid, signal.SIGUSR2)
                time.sleep(0.3)
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
            elif process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                if os.name == 'posix':
                    os.killpg(process.pid, signal.SIGKILL)
                else:
                    process.kill()
                process.wait()
    output = (out / 'stdout.log').read_text(errors='replace')
    errors = (out / 'stderr.log').read_text(errors='replace')
    stages = re.findall(r'STAGE ([A-Z_]+)', output)
    trap = re.search(r'STOP: first unsupported PS4 import: (.+)', output)
    result = dict(returncode=process.returncode, timed_out=timed_out, interrupted=interrupted,
                  elapsed_seconds=round(time.monotonic()-started,3), stages=stages,
                  last_stage=stages[-1] if stages else None,
                  unsupported_import=trap[1] if trap else None)
    (out / 'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    diagnostics = [line for line in output.splitlines()
                   if line.startswith(('GPU:', 'VideoOut:', 'Runtime: audio'))]
    recent_audio = [line for line in output.splitlines() if line.startswith('Audio stats port')][-6:]
    print('\n'.join(diagnostics + recent_audio))
    print('\n'.join((output+'\n'+errors).splitlines()[-30:]))
    return 124 if timed_out else 130 if interrupted else process.returncode


if __name__ == '__main__':
    raise SystemExit(main())
