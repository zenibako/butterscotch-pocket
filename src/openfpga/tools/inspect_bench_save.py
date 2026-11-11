#!/usr/bin/env python3
"""Find complete, distinct Butterscotch reports in a saved log's history."""
import argparse
from collections import Counter
from hashlib import sha256
from pathlib import Path
import re


MARKER = re.compile(rb'^(?:(?P<title>=== [^\r\n]*\bbenchmark\b[^\r\n]* ===)|--- end of report ---)\r?$', re.M)


def read_reports(path: Path) -> tuple[Counter[bytes], int]:
    text = path.read_bytes().split(b'\0', 1)[0]
    reports = Counter()
    start = None
    incomplete = 0
    for marker in MARKER.finditer(text):
        if marker.group('title'):
            if start is not None:
                incomplete += 1
            start = marker.start()
        elif start is not None:
            reports[text[start:marker.end()]] += 1
            start = None
    return reports, incomplete + (start is not None)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('save', type=Path, help='save to inspect')
    parser.add_argument('earlier', type=Path, nargs='*', help='earlier saves for exact history matching')
    args = parser.parse_args()
    try:
        reports, incomplete = read_reports(args.save)
        earlier = set()
        for path in args.earlier:
            earlier.update(read_reports(path)[0])
    except OSError as error:
        parser.exit(1, f'{error}\n')

    print(f'{sum(reports.values())} complete blocks, {len(reports)} distinct, {incomplete} incomplete')
    for report, copies in reports.items():
        title = report.splitlines()[0].decode('utf-8', errors='replace')
        status = 'present'
        if args.earlier:
            status = 'seen earlier' if report in earlier else 'unseen'
        print(f'{status}\t{copies}\t{sha256(report).hexdigest()[:12]}\t{title}')


if __name__ == '__main__':
    main()
