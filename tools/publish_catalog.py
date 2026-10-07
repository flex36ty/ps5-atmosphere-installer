"""Prepare the public catalogue feed. Does not commit, push, build an ELF, or fetch URLs."""
import argparse
import json
from pathlib import Path
from catalog import ROOT, compile_catalog, read, now


def prepare(public_repo):
    public_repo = Path(public_repo).resolve()
    if not (public_repo / '.git').exists():
        raise ValueError('Expected an existing checkout of the public project repository')
    games = read('games.json')
    for game in games:
        if not game.get('addedAt'):
            game['addedAt'] = now()
    rows = compile_catalog(games, read('releases.json'))
    target = public_repo / 'catalogue.json'
    revision_path = ROOT / 'catalog/revision.json'
    revision = json.loads(revision_path.read_text())['revision']
    if type(revision) is not int or not 1 <= revision < 2147483647:
        raise ValueError('Invalid local catalogue revision')
    if target.exists():
        previous = json.loads(target.read_text())
        if previous.get('schemaVersion') != 1 or type(previous.get('revision')) is not int or not 1 <= previous['revision'] < 2147483647 or not isinstance(previous.get('releases'), list):
            raise ValueError('Existing public feed is invalid; inspect it before publishing')
        revision = max(revision, previous['revision'])
        if rows != previous['releases']:
            revision += 1
    feed = {'schemaVersion': 1, 'revision': revision, 'releases': rows}
    encoded = (json.dumps(feed, ensure_ascii=False, indent=2) + '\n').encode()
    if len(encoded) > 8 * 1024 * 1024:
        raise ValueError('Catalogue exceeds the console metadata limit')
    # All validation finishes before writing. Atomic replacements leave reviewable files.
    for path, data in [(target, encoded),
                       (ROOT / 'catalog/games.json', (json.dumps(games, ensure_ascii=False, indent=2) + '\n').encode()),
                       (ROOT / 'catalog/catalog.json', (json.dumps(rows, ensure_ascii=False, indent=2) + '\n').encode()),
                       (revision_path, (json.dumps({'revision': revision}) + '\n').encode())]:
        temporary = path.with_suffix('.publish-tmp')
        temporary.write_bytes(data)
        temporary.replace(path)
    return revision, len({row['gameId'] for row in rows}), len(rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--public-repo', required=True, type=Path)
    args = parser.parse_args()
    revision, games, options = prepare(args.public_repo)
    print(f'Prepared catalogue revision {revision}: {games} games, {options} verified single-file options.')
    print('Review catalogue.json in the public repository, then commit and push main. No ELF rebuild is needed.')
