#!/usr/bin/env python3
"""Host tests: production callbacks with explicit SDK mocks, math and IANA DST.

python tests/run_sync_v3_tests.py --cc C:/msys64/ucrt64/bin/gcc.exe
On hosts providing ASan/UBSan, add --sanitize.
"""
import argparse
import ctypes
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import re
import subprocess
import sys
from zoneinfo import ZoneInfo


def function(source, name):
    # Top-level firmware functions end with an unindented closing brace.
    match = re.search(r'(?m)^(?:_attribute_ram_code_\s+)?(?:void|int) ' + name + r'\([^\n]*\)\s*\{', source)
    if match is None:
        raise ValueError(name)
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--tzfile', type=Path, help='IANA TZif Europe/Berlin file if Python has no tzdata')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = root / 'out/sync3-tests'
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['PATH'] = str(Path(args.cc).resolve().parent) + os.pathsep + env['PATH']
    base = [args.cc, '-std=c99', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        base += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    def run(command):
        print(' '.join(map(str,command)), flush=True)
        subprocess.run(list(map(str,command)), env=env, check=True, cwd=root)

    run([sys.executable, root / 'tests/test_sync_v3_client.py'])
    run(base + [root / 'tests/sync_v3_host_test.c', '-o', out / 'core.exe'])
    run([out / 'core.exe'])
    source = (root / 'src/scanning.c').read_text(encoding='utf-8')
    def replace_include(match):
        name = match[1]
        if name in ('scanning.h', 'sync_v3_logic.h'):
            return f'#include "{(root / "src" / name).as_posix()}"'
        return f'/* mocked SDK/application include: {name} */'
    source = re.sub(r'^#include "([^"]+)"', replace_include, source, flags=re.M)
    (out / 'scanning_under_test.c').write_text(source, encoding='utf-8')
    ble = (root / 'src/ble.c').read_text(encoding='utf-8')
    app = (root / 'src/app.c').read_text(encoding='utf-8')
    callbacks = ''.join(function(ble,name) for name in (
        'ev_adv_timeout', 'app_advertise_prepare_handler', 'ble_connect_callback'))
    callbacks += function(app,'user_init_deepRetn')
    (out / 'ble_callbacks_under_test.c').write_text(callbacks, encoding='utf-8')
    for hard_clock in (0,1):
        exe = out / f'radio-{hard_clock}.exe'
        run(base + [f'-DTEST_HARD_CLOCK={hard_clock}', '-I' + str(out),
                    root / 'tests/sync_v3_radio_test.c', '-o', exe])
        run([exe])

    # A shared wrapper allows comparison to independent IANA calendar data.
    wrapper = out / 'timezone.c'
    wrapper.write_text(f'#include "{(root / "src/eu_timezone.h").as_posix()}"\n'
                       'uint32_t local_epoch(uint32_t utc) { return eu_tz_local_epoch(utc); }\n')
    library = out / ('timezone.dll' if os.name == 'nt' else 'timezone.so')
    # The standalone core/radio tests above use sanitizers when requested.
    run([args.cc, '-std=c99', '-O2', '-shared', '-fPIC', wrapper, '-o', library])
    convert = ctypes.CDLL(str(library)).local_epoch
    convert.argtypes = [ctypes.c_uint32]
    convert.restype = ctypes.c_uint32
    if args.tzfile:
        with args.tzfile.open('rb') as source:
            zone = ZoneInfo.from_file(source, key='Europe/Berlin')
    else:
        zone = ZoneInfo('Europe/Berlin')
    samples = 0
    for year in range(2024,2100):
        current = datetime(year,1,1,tzinfo=timezone.utc)
        end = datetime(year+1,1,1,tzinfo=timezone.utc)
        while current < end:
            # Daily checks cover local midnight, month, weekday and leap year changes.
            for hour in (0,1,12,23):
                utc = int(current.replace(hour=hour).timestamp())
                expected = utc + int(datetime.fromtimestamp(utc,zone).utcoffset().total_seconds())
                assert convert(utc) == expected, (utc,year,hour)
                samples += 1
            current += timedelta(days=1)
        for month in (3,10):
            for day in range(25,32):
                transition = int(datetime(year,month,day,1,tzinfo=timezone.utc).timestamp())
                for delta in (-1,0,1):
                    utc = transition + delta
                    assert convert(utc) == utc + int(datetime.fromtimestamp(utc,zone).utcoffset().total_seconds())
                    samples += 1
    print(f'Europe/Berlin IANA comparison: {samples} samples (2024..2099) OK')


if __name__ == '__main__':
    main()
