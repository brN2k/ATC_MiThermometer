#!/usr/bin/env python3
"""Reproducible TC32 build matrix, with isolated objects and retained logs.

Windows example: python utils/build_sync3.py --modes 3 0 1 2
No flashing. A failed build, compiler warning or memory overflow is an error.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modes', nargs='+', type=int, choices=range(4), default=[3])
    parser.add_argument('--devices', nargs='+', default=[
        'MJWSD05MMC', 'MJWSD05MMC_EN', 'LYWSD03MMC'])
    parser.add_argument('--make', default='C:/TelinkSDK/bin/make.exe' if os.name == 'nt' else 'make')
    parser.add_argument('--tc32-path', default='C:/TelinkSDK/opt/tc32/bin' if os.name == 'nt' else '')
    parser.add_argument('--output', default='out/sync3')
    parser.add_argument('--bn', action='store_true', help='Append bn to file names and BLE software revision')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    os.chdir(root)
    version_match = re.search(r'^#define VERSION\s+0x([0-9a-fA-F]+)',
                              (root / 'src/app_config.h').read_text(encoding='utf-8'), re.M)
    version = int(version_match[1],16)
    revision = f'V{version >> 4}.{version & 15}' + ('_bn' if args.bn else '')
    out = Path(args.output).resolve()
    out.relative_to(root)  # keep all artifacts in this checkout
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = 'C:/TelinkSDK/jre/bin;C:/TelinkSDK/bin;' + args.tc32_path + ';' + env['PATH']
    tc = Path(args.tc32_path) if args.tc32_path else None
    gcc = str(tc / 'tc32-elf-gcc') if tc else 'tc32-elf-gcc'
    compiler = subprocess.check_output([gcc, '--version'], env=env, text=True).splitlines()[0]
    results = []
    failed = False
    for mode in args.modes:
        for device in args.devices:
            if not re.fullmatch(r'[A-Z0-9_]+', device):
                parser.error('invalid device name')
            name = f'{device}' + (f'_v{version:02x}bn' if args.bn else '') + f'_sync{mode}'
            build_dir = out / name
            build_dir.mkdir(exist_ok=True)
            binary = build_dir / f'{name}.bin'
            # Remove only this run's stale firmware; -B rebuilds every object.
            binary.unlink(missing_ok=True)
            rel = build_dir.relative_to(root).as_posix()
            command = [args.make, '-s', '-B', 'build', f'PROJECT_NAME={name}',
                       f'OUT_PATH=./{rel}/obj', f'BIN_FILE=./{rel}/{name}.bin',
                       f'ZBO_PATH=./{rel}/ota',
                       f'POJECT_DEF=-DDEVICE_TYPE=DEVICE_{device} -DUSE_SYNC_SCAN={mode} '
                       '-DTIME_SYNC_SERVER_ID=0xA1B2C3D4' + (' -DFIRMWARE_BUILD_BN=1' if args.bn else '')]
            if tc:
                command.append(f'TC32_PATH={tc.as_posix()}/')
            print(f'Building {name} ...', flush=True)
            log_path = build_dir / 'build.log'
            with log_path.open('w', encoding='utf-8') as log:
                log.write(json.dumps(command) + '\n')
                log.flush()
                completed = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            log = log_path.read_text(encoding='utf-8', errors='replace')
            warnings = [line for line in log.splitlines() if ': warning:' in line.lower()]
            memory_warnings = [line for line in log.splitlines() if line.startswith('Warning:')]
            result = dict(name=name, device=device, mode=mode, software_revision=revision,
                          exit_code=completed.returncode, command=command,
                          compiler_warnings=warnings, memory_tool_warnings=memory_warnings)
            ok = completed.returncode == 0 and binary.exists() and not warnings
            if ok:
                data = binary.read_bytes()
                result.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
                # BLE OTA occupies a 128 KiB slot; include the appended CRC.
                ok = len(data) <= 0x20000
                for field in ('Total Used RRAM', 'Total Used SRAM'):
                    match = re.search(field + r' : (\d+) from (\d+)', log)
                    if not match or int(match[1]) > int(match[2]):
                        ok = False
                    if match:
                        result[field] = [int(match[1]), int(match[2])]
            result['ok'] = ok
            results.append(result)
            failed |= not ok
            print(f'{name}: {"OK" if ok else "FAILED"}; {log_path}', flush=True)
    (out / 'manifest.json').write_text(json.dumps(dict(compiler=compiler, results=results), indent=2) + '\n', encoding='utf-8')
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
