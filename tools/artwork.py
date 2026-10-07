"""Resolve title-ID-matched external Prosperopatches artwork, with bounded image probes.

Uses one request at a time and the catalogue client's per-host eight-second interval.
Only URLs and verification metadata are saved. No artwork bytes enter the repo.
"""
import argparse
import json
import re
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit
import urllib.error
from catalog import Client, ROOT, read, save, now

HOST = 'cdn.prosperopatches.com'


class ArtworkPage(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.meta = {}

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'meta':
            self.meta[a.get('name') or a.get('property')] = a.get('content', '')


def parse_page(text, title_id):
    page = ArtworkPage()
    page.feed(text)
    source_title = page.meta.get('twitter:title', '')
    url = page.meta.get('twitter:image', '')
    if not source_title.startswith(title_id + ':'):
        raise ValueError('Title page does not identify the requested title ID')
    p = urlsplit(url)
    if p.scheme != 'https' or p.hostname != HOST or p.username or p.password or p.query or p.fragment or not re.fullmatch(r'/titles/' + title_id + r'_[0-9a-f]{64}/icon0\.webp', p.path):
        raise ValueError('Page does not publish an exact-title Prosperopatches icon URL')
    return url, source_title.split(':', 1)[1].strip()


def image_signature(data):
    return (data.startswith(b'RIFF') and data[8:12] == b'WEBP') or data.startswith(b'\x89PNG\r\n\x1a\n') or data.startswith(b'\xff\xd8\xff')


def apply_verified(games, releases, report):
    ids = {g['id']: {r['titleId'] for r in releases if r['gameId'] == g['id']} for g in games}
    changed = 0
    for game in games:
        item = report.get(game['id'], {})
        if item.get('status') != 'verified' or item.get('titleId') not in ids[game['id']]:
            continue
        url = item['url']
        if urlsplit(url).hostname != HOST or item['finalUrl'] != url:
            raise ValueError('Artwork verification host changed')
        for key in ('cover', 'hero'):
            previous = game.get(key, '')
            if urlsplit(previous).scheme == 'https' and previous != url and not game.get(key + 'Fallback'):
                game[key + 'Fallback'] = previous
        game['artwork'] = {
            'provider': 'Prosperopatches', 'titleId': item['titleId'], 'sourceUrl': item['sourceUrl'],
            'sourceTitle': item['sourceTitle'], 'cover': url, 'hero': url, 'artworkLayout': 'ambient',
            'checkedAt': item['checkedAt'], 'httpStatus': item['httpStatus'],
            'method': 'title page identity + 32-byte image signature probe',
        }
        game.update(cover=url, hero=url, artworkLayout='ambient')
        changed += 1
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Apply verified matches to shared artwork metadata')
    parser.add_argument('--report', type=Path, default=ROOT / '.state/artwork-prospero.json')
    args = parser.parse_args()
    games, releases = read('games.json'), read('releases.json')
    wanted = [g for g in games if any(urlsplit(g[k]).hostname != HOST for k in ('cover', 'hero'))]
    report = json.loads(args.report.read_text()) if args.report.exists() else {}
    client = Client()
    def checkpoint():
        args.report.parent.mkdir(parents=True, exist_ok=True)
        tmp = args.report.with_suffix('.tmp')
        tmp.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        tmp.replace(args.report)
    for index, game in enumerate(wanted):
        if report.get(game['id'], {}).get('status') == 'verified':
            continue
        title_id = next(r['titleId'] for r in releases if r['gameId'] == game['id'])
        page_url = 'https://prosperopatches.com/' + title_id
        try:
            status, _, data, final = client.request(page_url, limit=500_000)
            if status != 200 or urlsplit(final).hostname != 'prosperopatches.com':
                raise ValueError('Unexpected title page response')
            url, title = parse_page(data.decode('utf-8'), title_id)
            status, headers, data, final = client.request(url, headers={'Range': 'bytes=0-31'}, limit=32)
            if status != 206 or final != url or not image_signature(data):
                raise ValueError('Artwork URL did not return a valid partial image response')
            report[game['id']] = dict(status='verified', titleId=title_id, sourceUrl=page_url,
                sourceTitle=title, url=url, finalUrl=final, httpStatus=status, checkedAt=now())
        except (ValueError, urllib.error.HTTPError) as exc:
            report[game['id']] = dict(status='unavailable', titleId=title_id, sourceUrl=page_url, error=str(exc), checkedAt=now())
        checkpoint()
        if (index + 1) % 10 == 0 or index + 1 == len(wanted):
            print(f"Checked {index + 1}/{len(wanted)}; {sum(r['status'] == 'verified' for r in report.values())} verified", flush=True)
    if args.apply:
        changed = apply_verified(games, releases, report)
        save('games.json', games)
        print(f'Applied {changed} verified artwork matches. Compile the catalogue before building.', flush=True)
    missing = [g['title'] for g in wanted if report.get(g['id'], {}).get('status') != 'verified']
    print(json.dumps({'remaining': missing}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
