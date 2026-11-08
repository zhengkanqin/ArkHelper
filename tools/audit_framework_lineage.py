"""Compare the supplied Framework snapshot with ArkHelper without touching it.

Counts describe mapped source files, not a percentage of dependency or authorship.
Third-party code, tests, build outputs and local archives are outside this count.
"""
import argparse
import hashlib
import json
from pathlib import Path

CODE = {'.c', '.cpp', '.h', '.hpp'}
GROUPS = [
    ('core', 'src/core', 'native/core'),
    ('loader', 'src/loader', 'native/loader'),
    ('bootstrap', 'src/bootstrap', 'native/bootstrap'),
    ('common', 'plugins/common', 'native/modules/common'),
    ('damage_meter', 'plugins/damage_meter', 'native/modules/damage_meter'),
    ('contract', 'include/ark', 'native/include/ark'),
]
RENAMES = {'framework.cpp': 'runtime.cpp', 'framework_loader.h': 'runtime_loader.h',
           'framework_loader.cpp': 'runtime_loader.cpp'}


def sources(folder):
    return sorted(p for p in folder.rglob('*') if p.is_file() and p.suffix in CODE)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare(reference, current):
    groups, records, mapped = [], [], set()
    for name, old, new in GROUPS:
        origin, target = reference / old, current / new
        counts = dict(group=name, reference=0, identical=0, changed=0, absent=0, added=0)
        targets = set()
        for source in sources(origin):
            relative = source.relative_to(origin)
            renamed = relative.with_name(RENAMES.get(relative.name, relative.name))
            destination = target / renamed
            counts['reference'] += 1
            record = dict(reference=source.relative_to(reference).as_posix(),
                          reference_sha256=digest(source))
            if not destination.is_file():
                status = 'absent'
            else:
                targets.add(destination)
                mapped.add(destination)
                # Normalize newline differences only; any other change counts as changed.
                status = 'identical' if source.read_text(encoding='utf-8-sig') == destination.read_text(encoding='utf-8-sig') else 'changed'
                record.update(current=destination.relative_to(current).as_posix(),
                              current_sha256=digest(destination))
            record['status'] = status
            counts[status] += 1
            records.append(record)
        counts['added'] = len(set(sources(target)) - targets)
        groups.append(counts)
    all_current = [p for folder in ('src', 'native', 'include') for p in sources(current / folder)]
    return dict(scope='Production source/headers only; excludes third_party, tests, archives and outputs.',
                reference=str(reference), current=str(current), groups=groups,
                current_files=len(all_current), mapped_current_files=len(mapped),
                current_without_mapped_reference=len(set(all_current) - mapped), files=records)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--current', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.reference.resolve(strict=True), args.current.resolve(strict=True))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key != 'files'}, ensure_ascii=False, indent=2))
