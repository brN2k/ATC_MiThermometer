#!/usr/bin/env python3
"""Verify the build manifest, BLE CRC/header and generated Zigbee containers.
Optionally compare modes 1/2 byte-for-byte to an independently built baseline.
"""
import argparse
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('out/sync3'))
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.output / 'manifest.json').read_text())
    result = []
    for build in manifest['results']:
        assert build['ok'] and not build['compiler_warnings'], build['name']
        name = build['name']
        directory = args.output / name
        firmware = (directory / (name + '.bin')).read_bytes()
        assert len(firmware) == build['bytes'] < 0x20000, name
        assert hashlib.sha256(firmware).hexdigest() == build['sha256'], name
        assert firmware[6:8] == b'\x5d\x02', name
        assert int.from_bytes(firmware[24:28], 'little') == len(firmware), name
        assert int.from_bytes(firmware[-4:], 'little') == binascii.crc32(firmware[:-4]) ^ 0xffffffff, name
        device = build.get('device',name.rsplit('_sync',1)[0])
        device_id = {'LYWSD03MMC': 10, 'MJWSD05MMC': 9, 'MJWSD05MMC_EN': 12}[device]
        assert int.from_bytes(firmware[20:22], 'little') == 0x200 + device_id, name
        containers = list((directory / 'ota').glob('*.zigbee'))
        assert len(containers) == 1, name
        container = containers[0].read_bytes()
        assert container[:4] == b'\x1e\xf1\xee\x0b', name
        assert int.from_bytes(container[6:8],'little') == 56, name
        assert int.from_bytes(container[52:56],'little') == len(container), name
        assert int.from_bytes(container[58:62],'little') == len(firmware), name
        assert container[62:] == firmware, name
        entry = dict(name=name, bytes=len(firmware), crc_header_device_container='OK')
        if 'software_revision' in build:
            # Verify the GATT value and its advertised attribute length in the ELF.
            tc_arg = next((arg.split('=',1)[1] for arg in build['command'] if arg.startswith('TC32_PATH=')), '')
            nm = str(Path(tc_arg) / 'tc32-elf-nm') if tc_arg else 'tc32-elf-nm'
            env = os.environ.copy()
            if os.name == 'nt':
                env['PATH'] = 'C:/TelinkSDK/bin;' + tc_arg + ';' + env['PATH']
            symbols = subprocess.check_output([nm,'-S',str(directory/'obj'/(name+'.elf'))],env=env,text=True)
            match = re.search(r'^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+\w\s+my_SoftStr$',symbols,re.M)
            assert match, name
            address,size = (int(value,16) for value in match.groups())
            assert firmware[address:address+size] == build['software_revision'].encode('ascii'), name
            entry['software_revision'] = build['software_revision']
        if args.baseline and name.endswith(('_sync1','_sync2')):
            reference = (args.baseline / name / (name + '.bin')).read_bytes()
            assert reference == firmware, f'{name}: differs from baseline'
            entry['baseline_byte_identical'] = True
        result.append(entry)
        print(name, 'OK', 'baseline identical' if 'baseline_byte_identical' in entry else '')
    (args.output / 'verification.json').write_text(json.dumps(result,indent=2) + '\n')


if __name__ == '__main__':
    main()
