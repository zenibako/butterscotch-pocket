#!/usr/bin/env python3
"""Package an isolated Undertale bullet-hell benchmark core."""
import argparse
import copy
import json
from pathlib import Path
import zipfile

MODES = (
    ('Bullet Hell - Accuracy', 'accuracy', '--bench-bullet-hell'),
    ('Bullet Hell - Speed', 'speed', '--bench-bullet-hell-smooth'),
    ('Bullet Hell - No Render', 'no_render', '--bench-bullet-hell --draw-every 100000'),
    ('Bullet Hell - No Audio', 'no_audio', '--bench-bullet-hell --mute'),
    ('Bullet Hell - Core Only', 'core_only', '--bench-bullet-hell --draw-every 100000 --mute'),
    ('Bullet Hell - Profile', 'profile', '--bench-bullet-hell --scripts'),
    ('Chaos Saber - Accuracy', 'cs_acc', '--bench-chaos-saber'),
    ('Chaos Saber - Speed', 'cs_speed', '--bench-chaos-saber-smooth'),
    ('Flowey - Accuracy', 'fx_acc', '--bench-flowey'),
    ('Flowey - Speed', 'fx_speed', '--bench-flowey-smooth'),
    ('Flowey - Speed (Live Pacing)', 'fx_live', '--bench-flowey-live'),
    ('Flowey - No Render', 'fx_norend', '--bench-flowey --draw-every 100000'),
    ('Flowey - No Audio', 'fx_noaud', '--bench-flowey --mute'),
    ('Flowey - Core Only', 'fx_core', '--bench-flowey --draw-every 100000 --mute'),
    ('Flowey - Profile', 'fx_prof', '--bench-flowey --scripts'),
)
PORT = Path(__file__).resolve().parents[1]
DIST = PORT / 'dist'
RUNTIME = PORT / 'release/runtime'
CORE_ID = 'zenibako.BsBullet'
CORE_TEMPLATE = DIST / 'Cores/zenibako.Butterscotch'
INSTANCE_TEMPLATE = DIST / 'Assets/butterscotch/zenibako.Butterscotch/Undertale.json'
CORE_FILES = ('audio.json', 'core.json', 'data.json', 'icon.bin', 'info.txt',
              'input.json', 'interact.json', 'variants.json', 'video.json')


def package(elf: Path, output: Path, version: str) -> None:
    payload = elf.read_bytes()
    if len(payload) < 52 or payload[:6] != b'\x7fELF\x01\x01' or payload[18:20] != b'\xf3\x00':
        raise ValueError('benchmark input must be a little-endian 32-bit RISC-V ELF')
    # Renaming the ELF does not change the filenames it opens at runtime.
    if b'\0bs_bullet_0.sav\0' not in payload or b'\0bs_bullet_1.sav\0' not in payload:
        raise ValueError('benchmark ELF must use the bs_bullet save and log slots (build with APP=bs_bullet)')
    if b'\0--bench-flowey\0' not in payload or b'\0--bench-flowey-smooth\0' not in payload:
        raise ValueError('benchmark ELF must include the Flowey tour (rebuild from this revision)')
    if b'\0--bench-flowey-live\0' not in payload:
        raise ValueError('benchmark ELF must include Flowey live pacing (rebuild from this revision)')
    if b'\0--bench-chaos-saber\0' not in payload or b'\0--bench-chaos-saber-smooth\0' not in payload:
        raise ValueError('benchmark ELF must include the Chaos Saber benchmark (rebuild from this revision)')
    if output.exists():
        raise FileExistsError(output)

    entries = {
        f'Cores/{CORE_ID}/{name}': (CORE_TEMPLATE / name).read_bytes()
        for name in CORE_FILES if name != 'core.json'
    }
    core = json.loads((CORE_TEMPLATE / 'core.json').read_text())
    core['core']['metadata'].update(
        shortname='BsBullet', description='Undertale Bullet Hell Tour', version=version)
    core['core']['framework']['sleep_supported'] = False
    entries[f'Cores/{CORE_ID}/core.json'] = (json.dumps(core, indent=4) + '\n').encode()
    for name in ('os25.rbf_r', 'loader.bin'):
        entries[f'Cores/{CORE_ID}/{name}'] = (RUNTIME / name).read_bytes()
    for name in ('butterscotch.json', '_images/butterscotch.bin'):
        entries[f'Platforms/{name}'] = (DIST / 'Platforms' / name).read_bytes()
    entries['Assets/butterscotch/common/bs_bullet_os.bin'] = (RUNTIME / 'os.bin').read_bytes()
    entries['Assets/butterscotch/common/bs_bullet.elf'] = payload

    instance = json.loads(INSTANCE_TEMPLATE.read_text())
    for label, mode, arguments in MODES:
        ini = f'bs_bullet_{mode}.ini'
        entries[f'Assets/butterscotch/common/{ini}'] = (
            f'[os]\nELF=bs_bullet.elf\nARGS={arguments}\nVARIANT=os25\n').encode()
        item = copy.deepcopy(instance)
        for slot in item['instance']['data_slots']:
            number = slot['id']
            if number == 1:
                slot['filename'] = 'bs_bullet_os.bin'
            elif number == 2:
                slot['filename'] = ini
            elif number == 3:
                slot['filename'] = 'bs_bullet.elf'
            elif 10 <= number <= 19:
                slot['filename'] = f'bs_bullet_{number - 10}.sav'
            if len(slot['filename'].encode('utf-8')) > 23:
                raise ValueError(f"data-slot filename exceeds APF's 23-byte limit: {slot['filename']}")
        entries[f'Assets/butterscotch/{CORE_ID}/{label}.json'] = (
            json.dumps(item, indent=4) + '\n').encode()

    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, content in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, content)
    with zipfile.ZipFile(output) as archive:
        if (archive.testzip() is not None or len(archive.namelist()) != len(entries)
                or any(archive.read(name) != content for name, content in entries.items())):
            raise ValueError('ZIP readback mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args.elf, args.output, args.version)
    print(args.output)


if __name__ == '__main__':
    main()
