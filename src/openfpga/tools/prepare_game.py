#!/usr/bin/env python3
"""Prepare privately owned game files for Butterscotch Pocket (Python 3.9+)."""
import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


class PreparationError(Exception):
    """An actionable input, conversion or installation error."""


def host_tag(system=None, machine=None):
    system = system or platform.system()
    machine = (machine or platform.machine()).lower()
    systems = {'Darwin': 'macos', 'Windows': 'windows', 'Linux': 'linux'}
    arches = {'amd64': 'x86_64', 'x86_64': 'x86_64', 'arm64': 'arm64', 'aarch64': 'arm64'}
    if system not in systems or machine not in arches or sys.byteorder != 'little':
        raise PreparationError(f'No converter bundle for {system} / {machine}. Supported hosts are little-endian macOS, Windows and Linux on x86_64 or arm64.')
    return f'{systems[system]}-{arches[machine]}'


def run(command, cwd=None):
    try:
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True, errors='replace', cwd=cwd)
    except OSError as error:
        raise PreparationError(f'Cannot run {command[0]}: {error}. Use the converter bundle for {host_tag()}.') from error
    if result.returncode:
        raise PreparationError(f'{Path(command[0]).name} failed:\n{result.stderr[-6000:]}')
    return result.stdout, result.stderr


def build_tools(destination):
    """Developer option: compile only the two host tools, without the SDK."""
    ancestors = Path(__file__).resolve().parents
    if len(ancestors) < 4:
        raise PreparationError('--build-tools requires the full source checkout.')
    repo = ancestors[3]
    src, vendor = repo / 'src', repo / 'vendor'
    if not (src / 'image/image_decoder.c').is_file():
        raise PreparationError('--build-tools requires the full source checkout. Release users should download their platform converter bundle.')
    compiler = shlex.split(os.environ.get('CC', 'gcc' if platform.system() == 'Windows' else 'cc'))
    if not compiler or not shutil.which(compiler[0]):
        raise PreparationError('C compiler not found. Use a prebuilt converter bundle, or set CC to a GCC-compatible compiler (MinGW GCC on Windows).')
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    includes = [src, src/'sw', src/'image', src/'video', src/'debug_font', src/'openfpga/platform',
                vendor/'stb/ds', vendor/'stb/image', vendor/'stb/vorbis', vendor/'bzip2']
    flags = ['-O2', '-std=gnu99', '-DOF_HOST_TOOL', '-DPIXEL_SIZE=16', '-DENABLE_SW_RENDERER',
             '-DENABLE_WAD16', '-DENABLE_WAD17', '-DUSE_FLOAT_REALS', '-DNO_RVALUE_INT64',
             '-DBUTTERSCOTCH_VIDEO_NULL', '-DUSE_STB_IMGDEC', '-DOF_GAME_NAME="undertale"', '-DOF_LOG_SLOT_FILE="undertale_1.sav"']
    flags += [f'-I{p}' for p in includes]
    if platform.system() == 'Windows':
        flags += ['-static', '-static-libgcc']
    if platform.system() == 'Darwin':
        flags += ['-arch', 'arm64' if host_tag().endswith('-arm64') else 'x86_64']
    sources = {
        'mktexpack': [src/'openfpga/tools/mktexpack.c', src/'openfpga/platform/of_log.c',
                      src/'log.c', src/'gettime.c', src/'image/image_decoder.c', src/'embedded/stb_impl.c',
                      *sorted((vendor/'bzip2').glob('*.c'))],
        'mkmusic': [src/'openfpga/tools/mkmusic.c', vendor/'stb/vorbis/stb_vorbis.c'],
    }
    suffix = '.exe' if platform.system() == 'Windows' else ''
    # Compile in a temporary directory: a failed build cannot replace working tools.
    with tempfile.TemporaryDirectory(prefix='build-', dir=destination) as temp:
        for name, files in sources.items():
            run([*compiler, *flags, *files, '-lm', '-o', Path(temp)/(name+suffix)])
        for name in sources:
            os.replace(Path(temp)/(name+suffix), destination/(name+suffix))
    return destination


def find_tools(directory=None):
    tag = host_tag()
    here = Path(__file__).resolve().parent
    candidates = [Path(directory)] if directory else [here/'bin'/tag, here.parent/'out/data-tools'/tag]
    suffix = '.exe' if platform.system() == 'Windows' else ''
    for candidate in candidates:
        if all((candidate/(name+suffix)).is_file() for name in ('mktexpack', 'mkmusic')):
            return [candidate.resolve()/(name+suffix) for name in ('mktexpack', 'mkmusic')]
    raise PreparationError(f'Converters for {tag} were not found. Download the matching game-data preparation bundle, or run this script with --build-tools from the source checkout. --tools-dir can select an existing converter folder.')


def chunks(data):
    if len(data) < 16 or data[:4] != b'FORM' or struct.unpack_from('<I', data, 4)[0] != len(data)-8:
        raise PreparationError('Not a complete GameMaker data file. Select data.win, game.ios or game.unx from the installed game, not its installer.')
    offset = 8
    result = {}
    while offset < len(data):
        if offset+8 > len(data):
            raise PreparationError('Truncated GameMaker chunk header.')
        name, size = struct.unpack_from('<4sI', data, offset)
        end = offset+8+size
        if end > len(data):
            raise PreparationError('Truncated GameMaker chunk. Copy the original game file again.')
        result[name] = data[offset+8:end]
        offset = end
    return result


def sound_requirements(data, parsed):
    """Track the sounds the C converter would otherwise silently skip."""
    sond = parsed.get(b'SOND', b'')
    if len(sond) < 4:
        raise PreparationError('Game data has no complete SOND audio table.')
    count = struct.unpack_from('<I', sond)[0]
    if 4+count*4 > len(sond):
        raise PreparationError('Truncated SOND audio table.')

    def text_at(pointer):
        if pointer == 0:
            return ''
        end = data.find(b'\0', pointer)
        if pointer >= len(data) or end < 0:
            raise PreparationError('Invalid sound filename in game data.')
        try:
            return data[pointer:end].decode('utf-8').lower()
        except UnicodeError as error:
            raise PreparationError('Invalid sound filename encoding.') from error

    embedded, external = set(), set()
    for i in range(count):
        offset = struct.unpack_from('<I', sond, 4+i*4)[0]
        if offset+36 > len(data):
            raise PreparationError('Invalid sound entry in game data.')
        name, flags = struct.unpack_from('<II', data, offset)
        filename = text_at(struct.unpack_from('<I', data, offset+12)[0])
        audio_index = struct.unpack_from('<i', data, offset+32)[0]
        if flags & 3 and audio_index >= 0:
            embedded.add(text_at(name))
        if not flags & 3 and filename.endswith('.ogg'):
            external.add(filename.replace('\\', '/').rsplit('/', 1)[-1])
    return embedded, external


def discover(game, source):
    source = Path(source).expanduser().resolve()
    if not source.exists():
        raise PreparationError(f'Game source does not exist: {source}')
    folder = source.parent if source.is_file() else source
    resources = folder/'Contents/Resources'
    if resources.is_dir():
        folder = resources
    root = folder
    if game == 'deltarune' and not source.is_file():
        chapters = [folder/f'chapter1_{system}' for system in ('mac', 'windows', 'linux')]
        found = [p for p in chapters if p.is_dir()]
        if len(found) > 1:
            raise PreparationError('Multiple Chapter 1 folders found. Select the chapter folder you intend to use.')
        if found:
            folder = found[0]
    if source.is_file():
        data_path = source
    else:
        candidates = [folder/name for name in ('data.win', 'game.ios', 'game.unx')]
        candidates += [folder/'assets'/name for name in ('data.win', 'game.unx')]
        found = [p for p in candidates if p.is_file()]
        if len(found) != 1:
            raise PreparationError('Select the installed game folder, macOS .app, Chapter 1 folder, or the exact data.win/game.ios/game.unx file; exactly one data file must be identifiable.')
        data_path = found[0]
        folder = data_path.parent
    data = data_path.read_bytes()
    parsed = chunks(data)
    expected = 16 if game == 'undertale' else 17
    if len(parsed.get(b'GEN8', b'')) < 2 or parsed[b'GEN8'][1] != expected:
        raise PreparationError(f'{game} requires bytecode version {expected}. Use Undertale v1.08 or the supported Deltarune Chapter 1 files.')
    if len(parsed.get(b'TXTR', b'')) < 4 or struct.unpack_from('<I', parsed[b'TXTR'])[0] == 0:
        raise PreparationError('Game file has no texture pages to convert.')
    extras = []
    if game == 'deltarune':
        group, language = folder/'audiogroup1.dat', folder/'lang/lang_en.json'
        if not group.is_file() or not language.is_file():
            raise PreparationError('Deltarune Chapter 1 needs audiogroup1.dat and lang/lang_en.json beside its data file. Select the full chapter folder.')
        bank = chunks(group.read_bytes())
        if len(bank.get(b'AUDO', b'')) < 4:
            raise PreparationError('Deltarune audiogroup1.dat has no complete AUDO audio chunk.')
        try:
            json.loads(language.read_text(encoding='utf-8-sig'))
        except (ValueError, UnicodeError) as error:
            raise PreparationError('Deltarune lang_en.json is not valid JSON.') from error
        extras = [('group:1:', group), ('file:', language)]
    music = {}
    # Support app Resources, chapter folders, Steam installations and a selected data file.
    directories = [folder, folder/'music', folder/'mus', root/'mus', folder.parent/'mus']
    for directory in dict.fromkeys(directories):
        if not directory.is_dir():
            continue
        for path in sorted(directory.iterdir()):
            if not path.is_file() or path.suffix.lower() != '.ogg':
                continue
            name = path.name.lower()
            if game == 'deltarune' and name.encode('utf-8') not in data.lower():
                continue
            if name in music and music[name].read_bytes() != path.read_bytes():
                raise PreparationError(f'Different music files share the name {name}; select a single complete game installation.')
            music[name] = path
    _, required_music = sound_requirements(data, parsed)
    missing = required_music - music.keys()
    if missing:
        raise PreparationError('Missing game music: '+', '.join(sorted(missing))+'. Select the complete installed game folder.')
    return data_path, data, music, extras


def validate_packs(texture, music, expected_tracks):
    tex = texture.read_bytes()
    raw = music.read_bytes()
    if len(tex) < 16 or tex[:4] != b'UTX2' or len(raw) < 8 or raw[:4] != b'UTM1':
        raise PreparationError('Converter produced an invalid texture or audio pack.')
    count = struct.unpack_from('<I', raw, 4)[0]
    if not count or 8+48*count > len(raw):
        raise PreparationError('Audio pack has no valid tracks.')
    found = set()
    for i in range(count):
        name, start, samples, rate, _ = struct.unpack_from('<32s4I', raw, 8+48*i)
        found.add(name.split(bytes([0]))[0].decode('utf-8'))
        size = samples if rate == 0 else samples//2
        if not samples or start < 8+48*count or start+size > len(raw):
            raise PreparationError(f'Incomplete audio track: {name.split(bytes([0]))[0]!r}. Restore the original game music and audio-group files.')
    missing = expected_tracks - found
    if missing:
        raise PreparationError('Missing audio tracks: '+', '.join(sorted(missing))+'. Check the original audio-group and music files.')
    # Reproduce mkcard.sh's production pack layout, without modifying source packs.
    with music.open('ab') as out:
        padding = (-len(raw)) % 4096
        out.write(bytes(padding))
        start = len(raw)+padding
        out.write(tex)
        out.write(struct.pack('<III4s', start, len(tex), 0, b'UTXT'))


def prepare(game, source, output, tools_dir=None):
    if game not in ('undertale', 'deltarune'):
        raise PreparationError('Choose undertale or deltarune.')
    source = Path(source).expanduser().resolve()
    output = Path(output).expanduser().resolve()
    source_folder = source.parent if source.is_file() else source
    if output == source_folder or source_folder in output.parents:
        raise PreparationError('Choose an output folder outside the original game installation.')
    if output.exists():
        raise PreparationError('Output already exists. Choose a new folder; existing installs and saves will not be overwritten.')
    if any(part.lower() == 'saves' for part in output.parts):
        raise PreparationError('Choose an output folder outside Saves.')
    data_path, data, music, extras = discover(game, source)
    texture_tool, audio_tool = find_tools(tools_dir)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.butterscotch-data-', dir=output.parent) as temp:
        work = Path(temp)
        # ASCII temporary filenames also keep arguments simple for native converters.
        staged_data = work/'data.win'
        staged_data.write_bytes(data)
        ogg = work/'music'; ogg.mkdir()
        for name, path in music.items():
            shutil.copyfile(path, ogg/name)
        arguments = []
        for prefix, path in extras:
            staged = work/path.name
            shutil.copyfile(path, staged)
            # Relative ASCII paths avoid Windows drive/encoding/basename pitfalls.
            arguments.append(prefix+staged.name)
        ready = work/'ready'
        common = ready/'Assets/butterscotch/common'
        common.mkdir(parents=True)
        names = ('data.win', 'textures.bin', 'music.bin') if game == 'undertale' else ('deltarune.win', 'dr_textures.bin', 'dr_music.bin')
        texture, audio = common/names[1], common/names[2]
        run([texture_tool, staged_data.name, texture.relative_to(work).as_posix()], cwd=work)
        _, warnings = run([audio_tool, staged_data.name, ogg.name,
                           audio.relative_to(work).as_posix(), '32000', *arguments], cwd=work)
        if 'could not decode' in warnings or 'skipping ' in warnings:
            raise PreparationError('Audio conversion was incomplete:\n'+warnings[-6000:])
        expected_tracks, _ = sound_requirements(data, chunks(data))
        expected_tracks |= {name[:-4] for name in music}
        expected_tracks |= {path.name.lower() for prefix, path in extras if prefix == 'file:'}
        validate_packs(texture, audio, expected_tracks)
        shutil.copyfile(staged_data, common/names[0])
        manifest = {'game': game, 'source_filename': data_path.name, 'data_sha256': hashlib.sha256(data).hexdigest(),
                    'host': host_tag(), 'files': {str(p.relative_to(ready)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(common.iterdir())}}
        (ready/'PREPARED-DATA.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
        # Publish only after every conversion and validation succeeds.
        if output.exists():
            raise PreparationError('Output appeared during conversion. Choose a new output folder.')
        ready.rename(output)
    return output


def main():
    # Stable Unicode output when Windows redirects the terminal to a pipe.
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description='Prepare your own game files for Butterscotch Pocket. No game data is downloaded; source files and saves are not modified.')
    parser.add_argument('--game', choices=('undertale', 'deltarune'))
    parser.add_argument('--source', type=Path, help='installed game folder, macOS .app, chapter folder or game data file')
    parser.add_argument('--output', type=Path, help='new output folder, outside the game installation')
    parser.add_argument('--tools-dir', type=Path, help='folder containing mktexpack and mkmusic for this host')
    parser.add_argument('--build-tools', action='store_true', help='developer option: build native converters from this full source checkout, without an SDK')
    args = parser.parse_args()
    try:
        tag = host_tag()
        print(f'Host: {tag}', flush=True)
        if args.build_tools:
            destination = args.tools_dir or Path(__file__).resolve().parent.parent/'out/data-tools'/tag
            args.tools_dir = build_tools(destination)
            print(f'Converters built in {args.tools_dir}')
            if not any((args.game, args.source, args.output)):
                return 0
        if not all((args.game, args.source, args.output)):
            parser.error('--game, --source and --output are required for game preparation')
        output = prepare(args.game, args.source, args.output, args.tools_dir)
        print(f'Ready: {output}\nMerge its Assets folder into the root of your Pocket SD card, then unmount the card.\nKeep these generated game files private. Your original files and saves were not changed.')
        return 0
    except (PreparationError, OSError) as error:
        print(f'Preparation failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
