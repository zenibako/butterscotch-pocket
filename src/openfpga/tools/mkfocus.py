#!/usr/bin/env python3
"""Package the focused Deltarune diagnostic without replacing normal installs."""
import argparse
import copy
import json
from pathlib import Path
import zipfile

MODES = [('normal', 'Normal'), ('no-render', 'No Render'),
         ('no-audio', 'No Audio'), ('core', 'Core Only'), ('profile', 'Profile')]
PORT = Path(__file__).resolve().parents[1]


def package(elf: Path, template: Path, output: Path):
    payload = elf.read_bytes()
    if not payload.startswith(b'\x7fELF'):
        raise ValueError('diagnostic input must be an ELF')
    instance = json.loads(template.read_text())
    entries = {'Assets/butterscotch/common/deltarune_tiles.elf': payload}
    for number, (mode, label) in enumerate(MODES, 1):
        ini = 'deltarune_tiles_' + mode.replace('-', '_') + '.ini'
        entries['Assets/butterscotch/common/' + ini] = (
            f'[os]\nELF=deltarune_tiles.elf\nARGS=--focus-{mode}\nVARIANT=os20\n').encode()
        item = copy.deepcopy(instance)
        slots = item['instance']['data_slots']
        for slot in slots:
            if slot['id'] == 2:
                slot['filename'] = ini
            elif slot['id'] == 3:
                slot['filename'] = 'deltarune_tiles.elf'
            elif 10 <= slot['id'] <= 19:
                slot['filename'] = f"deltarune_tiles_{slot['id']-10}.sav"
        entries[f'Assets/butterscotch/zenibako.Butterscotch/Deltarune Tiles {number} {label}.json'] = (
            json.dumps(item, indent=4) + '\n').encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, content in entries.items():
            info = zipfile.ZipInfo(name)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, content)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None or any(archive.read(n) != b for n, b in entries.items()):
            raise ValueError('ZIP readback mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--template', type=Path, default=PORT / 'dist/Assets/butterscotch/zenibako.Butterscotch/Deltarune.json')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args.elf, args.template, args.output)
    print(args.output)


if __name__ == '__main__':
    main()
