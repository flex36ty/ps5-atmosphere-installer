"""Maintain the embedded catalogue. Metadata and verification only; never save artwork/game bytes.

Run with .venv/bin/python tools/catalog.py {metadata,verify,build,check}.
Network operations are explicit; normal builds use the reviewed JSON snapshot offline.
"""
import argparse
import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "catalog"
MAX_RELEASES = int(re.search(r"#define ATMOSPHERE_MAX_RELEASES (\d+)",
                            (ROOT / "backend/atmosphere.h").read_text())[1])


def now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def read(name):
    return json.loads((CATALOG / name).read_text())


def save(name, value):
    path = CATALOG / name
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def https(url):
    p = urllib.parse.urlsplit(url)
    return p.scheme == "https" and bool(p.hostname) and not p.username and not p.password


class Client:
    """One request at a time, eight seconds between requests to the same host.

    A challenge or rate limit ends this run, preserving the last good snapshot.
    No credentials, retries, domain rotation, or challenge-solving.
    """
    def __init__(self):
        self.last = {}

    def request(self, url, *, method="GET", headers=None, limit=4_000_000):
        if not https(url):
            raise ValueError("Only public HTTPS URLs are accepted")
        host = urllib.parse.urlsplit(url).hostname
        time.sleep(max(0, self.last.get(host, 0) + 8 - time.monotonic()))
        self.last[host] = time.monotonic()
        request = urllib.request.Request(url, method=method, headers={
            "User-Agent": "AtmosphereStore-Catalogue/0.2 (metadata verification)",
            **(headers or {}),
        })
        try:
            with urllib.request.urlopen(request, timeout=45) as response:
                if not https(response.url):
                    raise ValueError("Non-HTTPS redirect refused")
                data = b"" if method == "HEAD" else response.read(limit + 1)
                if len(data) > limit:
                    raise ValueError("Response exceeded the metadata/probe limit")
                return response.status, dict(response.headers.items()), data, response.url
        except urllib.error.HTTPError as exc:
            if exc.code in (403, 429, 503):
                raise SystemExit(f"Stopped: HTTP {exc.code} from {host}; Retry-After: "
                                 f"{exc.headers.get('Retry-After', 'not provided')}. Try later.") from exc
            raise


class PageMetadata(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.meta = {}
        self.images = []
        self.script_type = ""
        self.script_text = ""
        self.products = {}
        self.structured = {}

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "script":
            self.script_type, self.script_text = a.get("type", ""), ""
        if tag == "meta":
            key = a.get("property") or a.get("name")
            if key and key not in self.meta:
                self.meta[key] = a.get("content", "")
        if tag in ("link", "source", "img"):
            for key in ("href", "src", "srcset", "data-src"):
                for url in re.findall(r'https://[^\s,]+', a.get(key, "")):
                    if "gmedia.playstation.com/is/image/" in url and "hero" in url.lower():
                        self.images.append(url)

    def handle_data(self, data):
        if self.script_type in ("application/json", "application/ld+json"):
            self.script_text += data

    def handle_endtag(self, tag):
        if tag != "script":
            return
        if self.script_type in ("application/json", "application/ld+json"):
            try:
                obj = json.loads(self.script_text)
                if isinstance(obj, dict):
                    if obj.get("@type") == "Product":
                        self.structured = obj
                    for key, value in obj.get("cache", {}).items():
                        if key.startswith("Product:") and isinstance(value, dict):
                            self.products.setdefault(key[8:], {}).update(value)
            except (ValueError, TypeError):
                pass
        self.script_type, self.script_text = "", ""


def parse_metadata(html, url):
    page = PageMetadata()
    page.feed(html)
    m = page.meta
    product = page.products.get(page.structured.get("sku"), {})
    if product:
        m.update({"name": product.get("name", page.structured.get("name", "")),
                  "publisher": product.get("publisherName", ""),
                  "releaseDate": product.get("releaseDate", ""),
                  "platforms": ",".join(product.get("platforms", [])),
                  "genres": ",".join(g.get("value", "") for g in product.get("localizedGenres", [])),
                  "og:image": page.structured.get("image", "")})
    title = m.get("name") or m.get("og:title", "").split(" - PS")[0]
    if not title or not https(m.get("og:image", "")):
        raise ValueError("Publisher page has no game title or HTTPS artwork")
    if m.get("platforms") and "PS5" not in m["platforms"]:
        raise ValueError("Publisher page is not for PS5")
    cover = m["og:image"]
    # Prefer the page's actual desktop hero, otherwise use cover art as ambient colour.
    heroes = [u for u in page.images if "mobile" not in u.lower() and "logo" not in u.lower()]
    heroes += [x["url"] for x in product.get("media", [])
               if x.get("type") == "IMAGE" and x.get("role") == "BACKGROUND"]
    hero = heroes[0] if heroes else cover
    if "gmedia.playstation.com/is/image/" in hero:
        hero = hero.split("?")[0] + "?$1600px$"
    elif heroes and "image.api.playstation.com/" in hero:
        hero = hero.split("?")[0] + "?w=1600"
    if "image.api.playstation.com/" in cover:
        cover = cover.split("?")[0] + "?w=600"
    return {"publisher": m.get("publisher", ""), "releaseDate": m.get("releaseDate", "")[:10],
            "cover": cover, "hero": hero, "artworkLayout": "wide" if heroes else "ambient",
            "metadata": {"provider": "PlayStation", "url": url, "sourceTitle": title,
                         "fetchedAt": now()},
            "genres": ",".join(dict.fromkeys(x.strip() for x in m.get("genres", "").split(",") if x.strip()))}


def fetch_metadata(client, games, selected):
    for g in games:
        if selected and g["id"] not in selected:
            continue
        url = g.get("metadataUrl")
        if not url:
            print(f"No reviewed publisher page yet: {g['id']}", flush=True)
            continue
        if urllib.parse.urlsplit(url).hostname not in ("www.playstation.com", "store.playstation.com"):
            raise ValueError("Metadata adapter supports official PlayStation game/store pages")
        _, _, data, _ = client.request(url)
        result = parse_metadata(data.decode(), url)
        genres = result.pop("genres")
        if genres and genres != "Unique":
            g["genre"] = genres.replace(",", " / ")
        g.update(result)
        # Reviewable exceptions for pages whose default edition has changed its release date/art.
        overrides = g.get("metadataOverrides", {})
        if overrides:
            if not overrides.get("reason") or not https(overrides.get("sourceUrl", "")):
                raise ValueError("Metadata overrides require a reason and source URL")
            for key in ("releaseDate", "cover", "hero", "artworkLayout", "genre"):
                if key in overrides:
                    g[key] = overrides[key]
        # Artwork hosting is independent of publisher metadata. Preserve reviewed
        # non-Sony URLs when a later metadata refresh parses Sony's page again.
        apply_artwork_override(g)
        # Header checks only. Artwork binaries never enter the repository or cache.
        checked = []
        for art in dict.fromkeys((g["cover"], g["hero"])):
            status, headers, _, _ = client.request(art, method="HEAD")
            mime = next((v for k, v in headers.items() if k.lower() == "content-type"), "")
            if status != 200 or not mime.startswith("image/"):
                raise ValueError(f"Artwork did not return an image: {g['id']}")
            checked.append({"url": art, "contentType": mime, "checkedAt": now()})
        g["metadata"]["artworkChecks"] = checked
        save("games.json", games)
        print(f"Metadata + artwork: {g['id']}", flush=True)


def apply_artwork_override(game):
    artwork = game.get('artwork')
    if not artwork:
        return
    if artwork.get('provider') != 'Prosperopatches' or not artwork.get('checkedAt'):
        raise ValueError('Artwork override needs a verified provider')
    title_id = artwork.get('titleId', '')
    if not re.fullmatch(r'PPSA\d{5}', title_id):
        raise ValueError('Artwork override needs a PS5 title ID')
    for key in ('cover', 'hero'):
        url = artwork.get(key, '')
        parsed = urllib.parse.urlsplit(url)
        if not https(url) or parsed.hostname != 'cdn.prosperopatches.com' or not parsed.path.startswith('/titles/' + title_id + '_'):
            raise ValueError('Artwork override must use its verified title ID and CDN')
        game[key] = url
    if artwork.get('artworkLayout') != 'ambient':
        raise ValueError('Prosperopatches icons use the ambient background layout')
    game['artworkLayout'] = artwork['artworkLayout']


def archive_identity(url):
    p = urllib.parse.urlsplit(url)
    parts = p.path.split("/", 3)
    if p.hostname != "archive.org" or not https(url) or len(parts) != 4 or parts[1] != "download" or p.query:
        raise ValueError("Expected a stable Archive.org download URL")
    return parts[2], urllib.parse.unquote(parts[3])


def verify_release(client, r):
    identifier, filename = archive_identity(r["url"])
    _, _, data, _ = client.request("https://archive.org/metadata/" + identifier)
    metadata = json.loads(data)
    if metadata.get("is_dark") or metadata.get("metadata", {}).get("access-restricted-item"):
        raise ValueError("Restricted item")
    files = [f for f in metadata.get("files", []) if f.get("name") == filename]
    if (len(files) != 1 or files[0].get("private") or filename != r["filename"]
            or not filename.lower().endswith((".ffpfsc", ".exfat"))):
        raise ValueError("Exact single-file listing missing")
    f = files[0]
    size = int(f["size"])
    if size != r["sizeBytes"]:
        raise ValueError(f"File size changed: expected {r['sizeBytes']}, found {size}")
    status, h, _, final = client.request(r["url"], method="HEAD")
    h = {k.lower(): v for k, v in h.items()}
    if status != 200 or int(h.get("content-length", "-1")) != size:
        raise ValueError("Download HEAD size mismatch")
    if "text/" in h.get("content-type", "") or "json" in h.get("content-type", ""):
        raise ValueError("Download returned a page")
    status, rh, data, _ = client.request(r["url"], headers={"Range": "bytes=0-15"}, limit=16)
    rh = {k.lower(): v for k, v in rh.items()}
    if status != 206 or rh.get("content-range") != f"bytes 0-15/{size}" or len(data) != 16:
        raise ValueError("Bounded range probe failed")
    return {"checkedAt": now(), "url": r["url"], "metadataUrl": "https://archive.org/metadata/" + identifier,
            "method": "metadata + HEAD + 16-byte range", "httpStatus": 200, "rangeStatus": 206,
            "sizeBytes": size, "filename": filename, "contentType": h.get("content-type", ""),
            "etag": h.get("etag", ""), "downloadHost": urllib.parse.urlsplit(final).hostname,
            "sourceReportedHashes": {k: f[k] for k in ("md5", "sha1", "sha256") if k in f},
            "consoleVerified": False}


def compile_catalog(games, releases):
    by_id = {g["id"]: g for g in games}
    if len(by_id) != len(games) or not 1 <= len(games) <= MAX_RELEASES:
        raise ValueError("Expected unique game identities within the console capacity")
    if not 1 <= len(releases) <= MAX_RELEASES:
        raise ValueError(f"Expected 1–{MAX_RELEASES} download options")
    rows, seen, used, urls = [], set(), set(), set()
    fields = ("title", "genre", "tagline", "description", "cover", "hero", "coverFallback", "heroFallback", "publisher", "releaseDate", "artworkLayout", "addedAt")
    for r in releases:
        g = by_id.get(r["gameId"])
        if not g or r["id"] in seen or len(r["id"].encode()) >= 24 or len(r["gameId"].encode()) >= 64:
            raise ValueError("Invalid or duplicate game/release identity")
        if any(not g.get(k) for k in ("title", "cover", "hero", "artworkLayout")):
            raise ValueError(f"Missing shared metadata: {g['id']}")
        if len(g["title"].encode()) >= 128:
            raise ValueError("Game title exceeds the console field limit")
        if g.get("releaseDate") is not None:
            datetime.strptime(g["releaseDate"], "%Y-%m-%d")
        if g.get("addedAt") is not None:
            added = datetime.fromisoformat(g["addedAt"].replace("Z", "+00:00"))
            if added.tzinfo is None:
                raise ValueError("Catalogue addition date requires a timezone")
        if any(not https(g[k]) for k in ("cover", "hero")):
            raise ValueError("Artwork must be external HTTPS URLs")
        for key in ("coverFallback", "heroFallback"):
            value = g.get(key)
            if value is not None and (not isinstance(value, str) or not https(value) or len(value.encode()) >= 2048 or any(ord(c) < 32 or ord(c) == 127 for c in value)):
                raise ValueError("Artwork fallback must be a bounded external HTTPS URL")
        if g["artworkLayout"] not in ("wide", "ambient"):
            raise ValueError("Unsupported artwork layout")
        if r["url"] in urls or r["sourceId"] != "archive" or r["format"] not in ("FFPFSC", "exFAT"):
            raise ValueError("Duplicate or unsupported download source")
        _, filename = archive_identity(r["url"])
        if (filename != r["filename"] or "/" in filename or ".." in filename
                or any(ord(c) < 32 for c in filename)
                or len(filename.encode()) >= 160 or len(r["url"].encode()) >= 2048):
            raise ValueError("Unsafe filename or oversized URL")
        expected_suffix = ".ffpfsc" if r["format"] == "FFPFSC" else ".exfat"
        if not filename.lower().endswith(expected_suffix) or type(r["sizeBytes"]) is not int or r["sizeBytes"] <= 0:
            raise ValueError("Format/size mismatch")
        evidence = r.get("verification", {})
        if (evidence.get("url") != r["url"] or evidence.get("httpStatus") != 200 or evidence.get("rangeStatus") != 206
                or evidence.get("sizeBytes") != r["sizeBytes"] or evidence.get("filename") != filename):
            raise ValueError(f"Missing matching source verification: {r['id']}")
        seen.add(r["id"]); used.add(g["id"]); urls.add(r["url"])
        row = {k: v for k, v in r.items() if k not in ("verification", "importEvidence")}
        row.update({k: g.get(k) for k in fields})
        row["sourceCheckedAt"] = evidence["checkedAt"]
        row["consoleVerified"] = evidence.get("consoleVerified", False)
        rows.append(row)
    if used != set(by_id):
        raise ValueError("Every game needs a verified download option")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("metadata", "verify", "build", "check"))
    parser.add_argument("--only", nargs="*", help="game IDs (metadata) or release IDs (verify)")
    args = parser.parse_args()
    games, releases = read("games.json"), read("releases.json")
    if args.command == "metadata":
        fetch_metadata(Client(), games, args.only)
    elif args.command == "verify":
        client = Client()
        for r in releases:
            if args.only and r["id"] not in args.only:
                continue
            r["verification"] = verify_release(client, r)
            save("releases.json", releases)
            print(f"Verified single file: {r['id']}", flush=True)
    else:
        rows = compile_catalog(games, releases)
        if args.command == "build":
            save("catalog.json", rows)
        elif rows != read("catalog.json"):
            raise ValueError("Embedded catalogue is stale; run catalog:build")
        print(f"Catalogue: {len(games)} games, {len(rows)} verified options; artwork URLs only")


if __name__ == "__main__":
    main()
