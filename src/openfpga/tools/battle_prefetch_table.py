#!/usr/bin/env python3
"""Writes platform/game_undertale_prefetch.h, the battle prefetch table.

    tools/battle_prefetch_items.sh ./undertale_pc games/undertale out/items
    tools/battle_prefetch_table.py out/items platform/game_undertale_prefetch.h

From the runs tools/battle_prefetch_items.sh leaves: for each battlegroup, the
texture items (TPAG indices) its battle draws in its first UPDATES updates in
room_battle, and those of its first enemy speech bubble however late that comes:
the bubble sprites (spr_blcon*) and fonts first drawn in the BUBBLE updates from
the first bubble sprite's first draw. Those at least 90% of battles draw are
listed once, as common; each battle's own list leaves them out. Both are in
order of first use. Then, for each battlegroup, the sounds writer objects
start from that bubble on (the voices; some speakers pick one at random for
each letter, so the rest of the visit is taken too), by name, from the
BS_AUDIO_LOG lines in each run's output. Last, the common battle sounds: those
at least SOUND_SHARE of battles start in room_battle in the visit, whoever
starts them (the menu, the slash, the hits), in order of first use; and after
each battle's voices, the other sounds it starts in its first UPDATES updates
(its monster's hurt sound) that are not common. The tour
moves the menu cursor in only about half the battles, a player in nearly every
one, so the share is lower than the items' 90%. Each entry has the update of its
first use: the battle's own for its items and sounds, the median over the
battles that use it for a common one, which every list is in order of. The game
loads a battle's items and sounds in that order, merged, so that what the
battle needs first is read first. The battle tour's layout
(tourForList in platform/game_undertale.c): the first visit starts at update
1000, each visit lasts 600.
"""
import argparse
import collections
import pathlib
import re
import statistics

FIRST_VISIT, VISIT = 1000, 600
SOUND_SHARE = 0.4
SOUND = re.compile(r'Audio: sfx \S+ gain \S+ pitch \S+ at (\d+) in (\S+) by (\S+) sound (\S+)$')


def bubble_item(asset):
    return asset.startswith('spr_blcon') or asset.startswith('fnt_')


def read_runs(log_dir, updates, bubble):
    use = {}   # group -> {item: offset of first use}
    shape = {}  # item -> (page, width, height)
    voices = {}  # group -> {sound name: offset of first start}
    heard_in = {}  # group -> {sound name: offset of first start}, any object
    late = 0  # items taken in for a bubble after the first `updates`
    for listing in sorted(log_dir.glob('run*.list'), key=lambda p: int(p.stem[3:])):
        run = listing.stem
        groups = [int(g) for g in listing.read_text().strip().split(',')]
        rooms = {}
        for line in (log_dir / f'{run}.frames').read_text().splitlines():
            parts = line.split()
            rooms[int(parts[0])] = parts[1]
        starts = []
        for v in range(len(groups)):
            base = FIRST_VISIT + v * VISIT
            battle = [f for f in range(base, base + VISIT) if rooms.get(f) == 'room_battle']
            starts.append(battle[0] if battle else None)
        drawn = []  # (visit, offset, item, asset)
        for line in (log_dir / f'{run}.items').read_text().splitlines():
            frame, room, item, page, width, height, asset = line.split()
            frame, item = int(frame), int(item)
            v = (frame - FIRST_VISIT) // VISIT
            if frame < FIRST_VISIT or v >= len(groups) or room != 'room_battle' or starts[v] is None:
                continue
            offset = frame - starts[v]
            if offset >= 0:
                drawn.append((v, offset, item, asset))
            shape[item] = (int(page), int(width), int(height))
        # The first bubble of each visit: the first draw of a bubble sprite.
        first = {}
        for v, offset, item, asset in drawn:
            if asset.startswith('spr_blcon') and offset < first.get(v, VISIT):
                first[v] = offset
        for v, offset, item, asset in drawn:
            in_bubble = v in first and first[v] <= offset < first[v] + bubble and bubble_item(asset)
            if offset >= updates and not in_bubble:
                continue
            seen = use.setdefault(groups[v], {})
            if offset >= updates and item not in seen:
                late += 1
            seen[item] = min(offset, seen.get(item, offset))
        for line in (log_dir / f'{run}.out').read_text().splitlines():
            m = SOUND.search(line)
            if m is None:
                continue
            frame, room, by, name = int(m[1]), m[2], m[3], m[4]
            v = (frame - FIRST_VISIT) // VISIT
            if frame < FIRST_VISIT or v >= len(groups) or room != 'room_battle' or starts[v] is None:
                continue
            offset = frame - starts[v]
            if offset >= 0:
                any_heard = heard_in.setdefault(groups[v], {})
                any_heard[name] = min(offset, any_heard.get(name, offset))
            if v not in first:
                continue
            if 'writer' not in by.lower() or offset < first[v]:
                continue
            heard = voices.setdefault(groups[v], {})
            heard[name] = min(offset, heard.get(name, offset))
    return use, shape, voices, heard_in, late


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log_dir', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--updates', type=int, default=150)
    parser.add_argument('--bubble', type=int, default=60)
    args = parser.parse_args()
    use, shape, voices, heard_in, late = read_runs(args.log_dir, args.updates, args.bubble)
    if not use:
        raise SystemExit('no battles found in ' + str(args.log_dir))
    if not voices:
        raise SystemExit('no bubble sounds found in ' + str(args.log_dir) + ' (were the runs made with BS_AUDIO_LOG=1?)')
    count = collections.Counter(item for items in use.values() for item in items)
    first = collections.defaultdict(list)
    for items in use.values():
        for item, offset in items.items():
            first[item].append(offset)
    common_at = {i: int(statistics.median(first[i])) for i, c in count.items() if c >= 0.9 * len(use)}
    common = sorted(common_at, key=lambda i: (common_at[i], min(first[i]), i))
    # Three items of different sizes, the largest: a data file whose items
    # are these is taken to be the one the table was made from.
    check, sizes = [], set()
    for item in sorted(common, key=lambda i: (-shape[i][1] * shape[i][2], i)):
        if shape[item][1:] not in sizes:
            check.append(item)
            sizes.add(shape[item][1:])
        if len(check) == 3:
            break

    lines = [
        '/* Generated by tools/battle_prefetch_table.py from tools/battle_prefetch_items.sh;',
        ' * do not edit by hand.',
        ' *',
        f' * The texture items (TPAG indices) each Undertale battle draws in its first {args.updates} updates,',
        f' * and its first enemy speech bubble\'s sprites and fonts (drawn in the {args.bubble} updates from',
        f' * that bubble\'s first draw), recorded on desktop over {len(use)} battlegroups. g_prefetchCommon:',
        " * those at least 90% of battles draw; each battle's own list leaves them out. g_prefetchSounds:",
        ' * the sounds writers start from that bubble on (the voices), and the other sounds the battle',
        f' * starts in its first {args.updates} updates that are not common, by name. g_prefetchCommonSounds: those at',
        f' * least {round(SOUND_SHARE * 100)}% of battles start in room_battle, by anyone. Each list is in order of first use,',
        ' * and its *At list gives that update (from the battle\'s start; for a common entry, the median',
        ' * over the battles that use it), so that the game can merge them. */',
        '',
        "/* Three of the table's items in the data file it was made from: TPAG index,",
        " * texture page, width, height. Another file's indices mean other items. */",
        'static const int16_t g_prefetchCheck[][4] = { '
        + ', '.join('{{ {}, {}, {}, {} }}'.format(i, *shape[i]) for i in check) + ' };',
        '',
        'static const int16_t g_prefetchCommon[] = {',
    ]

    def rows(items):
        for k in range(0, len(items), 16):
            lines.append('    ' + ', '.join(str(i) for i in items[k:k + 16]) + ',')

    rows(common)
    lines += ['};', '', 'static const uint16_t g_prefetchCommonAt[] = {']
    rows([common_at[i] for i in common])
    lines += ['};', '', 'static const int16_t g_prefetchItems[] = {']
    starts, at, item_at = [], 0, []
    for group in sorted(use):
        own = sorted((i for i in use[group] if i not in common), key=lambda i: (use[group][i], i))
        starts.append((group, at, len(own)))
        item_at.append((group, [use[group][i] for i in own]))
        lines.append(f'    /* battlegroup {group} */')
        rows(own)
        at += len(own)
    lines += ['};', '', 'static const uint16_t g_prefetchItemAt[] = {']
    for group, offsets in item_at:
        lines.append(f'    /* battlegroup {group} */')
        rows(offsets)
    lines += ['};', '', 'static const struct { int16_t group; uint16_t start, count; } g_prefetchGroups[] = {']
    for k in range(0, len(starts), 6):
        lines.append('    ' + ' '.join(f'{{ {g}, {s}, {c} }},' for g, s, c in starts[k:k + 6]))
    lines += ['};', '']
    heard_count = collections.Counter(name for heard in heard_in.values() for name in heard)
    heard_first = collections.defaultdict(list)
    for heard in heard_in.values():
        for name, offset in heard.items():
            heard_first[name].append(offset)
    common_sound_at = {n: int(statistics.median(heard_first[n])) for n, c in heard_count.items()
                       if c >= SOUND_SHARE * len(use)}
    common_sounds = sorted(common_sound_at, key=lambda n: (common_sound_at[n], min(heard_first[n]), n))
    sounds = {}  # group -> {its voices and its other first sounds: update of first use}
    for group in sorted(set(voices) | set(heard_in)):
        own = dict(voices.get(group, {}))
        heard = heard_in.get(group, {})
        own.update({n: heard[n] for n in heard if heard[n] < args.updates and n not in common_sounds and n not in own})
        if own:
            sounds[group] = own
    names = sorted({name for own in sounds.values() for name in own} | set(common_sounds))
    if len(names) > 256:
        raise SystemExit('more bubble sounds than g_prefetchSounds can index')
    lines += ['static const char *const g_prefetchSoundNames[] = {']
    for k in range(0, len(names), 6):
        lines.append('    ' + ' '.join(f'"{n}",' for n in names[k:k + 6]))
    lines += ['};', '', 'static const uint8_t g_prefetchSounds[] = {']
    sound_starts, sound_at, sound_offsets = [], 0, []
    for group, heard in sorted(sounds.items()):
        own = sorted(heard, key=lambda n: (heard[n], n))
        sound_starts.append((group, sound_at, len(own)))
        sound_offsets.append((group, [heard[n] for n in own]))
        lines.append(f'    /* battlegroup {group} */ ' + ' '.join(f'{names.index(n)},' for n in own))
        sound_at += len(own)
    lines += ['};', '', 'static const uint16_t g_prefetchSoundAt[] = {']
    for group, offsets in sound_offsets:
        lines.append(f'    /* battlegroup {group} */ ' + ' '.join(f'{o},' for o in offsets))
    lines += ['};', '', 'static const struct { int16_t group; uint16_t start, count; } g_prefetchSoundGroups[] = {']
    for k in range(0, len(sound_starts), 6):
        lines.append('    ' + ' '.join(f'{{ {g}, {s}, {c} }},' for g, s, c in sound_starts[k:k + 6]))
    lines += ['};', '', 'static const uint8_t g_prefetchCommonSounds[] = {',
              '    ' + ' '.join(f'{names.index(n)}, /* {n} */' for n in common_sounds), '};', '',
              'static const uint16_t g_prefetchCommonSoundAt[] = {',
              '    ' + ' '.join(f'{common_sound_at[n]},' for n in common_sounds), '};', '']
    args.output.write_text('\n'.join(lines))
    print(f'{len(use)} battles, {len(common)} common items, {at} battle items ({late} for a first bubble after '
          f'update {args.updates}), {len(sounds)} battles with {sound_at} sounds ({len(names)} names), common sounds: '
          f'{" ".join(common_sounds)}')


if __name__ == '__main__':
    main()
