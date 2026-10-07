"""Stage compatible single-file releases from a researched catalogue.

Imports metadata/evidence only. Network verification and publishing are separate.
Existing game metadata and release IDs are preserved. Artwork remains remote.
"""
import argparse
import copy
import hashlib
import json
import re
import unicodedata
from pathlib import Path
from urllib.parse import urlsplit
from catalog import archive_identity, https, now


def game_key(title):
    title = unicodedata.normalize("NFKD", title).casefold().replace("&", "and")
    title = "".join(c for c in title if not unicodedata.combining(c))
    title = re.sub(r"\b(?:digital deluxe|deluxe|complete|ultimate|definitive|standard|premium|gold) edition\b", "", title)
    roman = {"ii":"2", "iii":"3", "iv":"4", "v":"5", "vi":"6", "vii":"7", "viii":"8", "ix":"9", "x":"10", "xv":"15", "xvi":"16"}
    title = re.sub(r"\b(?:ii|iii|iv|v|vi|vii|viii|ix|x|xv|xvi)\b", lambda m: roman[m[0]], title)
    return re.sub(r"[^a-z0-9]", "", title)


def stage(master, games, releases, pippo, pegasus, *, source_hash):
    games, releases = copy.deepcopy(games), copy.deepcopy(releases)
    by_title = {game_key(g["title"]): g for g in games}
    by_id = {g["id"]: g for g in games}
    by_tid = {r["titleId"]: by_id[r["gameId"]] for r in releases}
    urls = {r["url"] for r in releases}
    identities = {r["id"] for r in releases}
    candidates, ignored = [], []
    for e in master["entries"]:
        url, filename = e.get("file_url", ""), e.get("filename", "")
        if url in urls:
            continue
        if e.get("host", "Vikingfile") == "Vikingfile" and filename.lower().endswith((".exfat", ".ffpfsc")):
            candidates.append({"title": e["game"], "titleId": e.get("title_id"), "filePage": url,
                "filename": filename, "sizeBytes": e.get("size_bytes"), "format": "exFAT" if filename.lower().endswith(".exfat") else "FFPFSC",
                "downloadReady": False, "status": "resolver-required", "checkedAt": e.get("file_page_checked_at"),
                "reason": "File metadata is available; the console download resolver is not implemented.",
                "catalogueEntry": e["entry"]})
            continue
        if e.get("host") != "Internet Archive" or not filename.lower().endswith((".ffpfsc", ".exfat")):
            ignored.append({"entry": e["entry"], "reason": "Not a supported direct single-file source"})
            continue
        identifier, exact = archive_identity(url)
        if exact != filename or "/" in filename or ".." in filename or not e.get("size_bytes"):
            ignored.append({"entry": e["entry"], "reason": "Missing or unsafe exact filename/size"})
            continue
        tid = e.get("title_id", "")
        if not re.fullmatch(r"PPSA\d{5}", tid):
            ignored.append({"entry": e["entry"], "reason": "PS5 title identity missing"})
            continue
        game = by_tid.get(tid) or by_title.get(game_key(e["game"]))
        if game is None:
            matches = [p for p in pegasus if p.get("titleId") == tid]
            pippo_matches = [p for p in pippo if tid in p.get("tags", [])]
            art = [(p.get("posterUrl"), p["_catalogSource"], p["title"]) for p in matches]
            art += [(p.get("image"), "https://pippo26442999.github.io/.exFAT/exFAT.json", p["title"]) for p in pippo_matches]
            art = [x for x in art if x[0] and https(x[0])]
            art.sort(key=lambda x: "playstation.com" not in urlsplit(x[0]).hostname)
            if not art:
                ignored.append({"entry": e["entry"], "reason": "No title-ID-matched external artwork"})
                continue
            title = e["game"]
            title = {"PPSA16738": "Transformers: Galactic Trials", "PPSA18010": "Tony Hawk’s Pro Skater 3 + 4"}.get(tid, title)
            slug = re.sub(r"[^a-z0-9]+", "-", unicodedata.normalize("NFKD", title).encode("ascii", "ignore").decode().lower()).strip("-")[:54].rstrip("-")
            if not slug or slug in by_id:
                slug = (slug or "game")[:53] + "-" + tid.lower()
            game = {"id": slug, "title": title, "genre": None, "tagline": None, "description": None,
                "publisher": None, "releaseDate": None, "cover": art[0][0], "hero": art[0][0], "artworkLayout": "ambient",
                "metadata": {"provider": "Title-ID-matched catalogue", "url": art[0][1], "sourceTitle": art[0][2],
                    "titleId": tid, "fetchedAt": now(), "artworkChecks": [],
                    "note": "Only identity and remote artwork imported. Publisher, genre, date and description remain unset until sourced."},
                "artworkCandidates": list(dict.fromkeys(a[0] for a in art))}
            games.append(game); by_id[slug] = game; by_title[game_key(title)] = game
        by_tid[tid] = game
        fmt = "FFPFSC" if filename.lower().endswith(".ffpfsc") else "exFAT"
        rid = tid.lower() + "-" + fmt.lower()
        if rid in identities:
            rid = tid.lower() + "-" + hashlib.sha256(url.encode()).hexdigest()[:8]
        release = {"id": rid, "gameId": game["id"], "titleId": tid, "sizeBytes": int(e["size_bytes"]),
            "provider": "Archive.org", "sourceId": "archive", "format": fmt, "version": e.get("version"),
            "filename": filename, "url": url, "sha256": "",
            "importEvidence": {"catalogueEntry": e["entry"], "sourceSnapshotSha256": source_hash,
                "metadataUrl": e.get("identity_source_url"), "metadataCheckedAt": e.get("metadata_checked_at"),
                "headersCheckedAt": e.get("download_headers_checked_at"), "httpStatus": e.get("http_status"),
                "headerContentLength": e.get("header_content_length"), "sizeBytes": int(e["size_bytes"]),
                "filename": filename, "url": url, "sourceReportedHashes": e.get("source_reported_hashes", {})}}
        releases.append(release); urls.add(url); identities.add(rid)
    return games, releases, candidates, ignored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalogue", type=Path, required=True)
    parser.add_argument("--pippo", type=Path, required=True)
    parser.add_argument("--pegasus", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    raw = args.catalogue.read_bytes()
    master = json.loads(raw)
    feeds = []
    for source in args.pegasus:
        feed_name = "dlps" if "dlps" in source.name else "pfs"
        feeds.extend({**record, "_catalogSource": f"https://pegasus-catalog.fly.dev/catalogs/{feed_name}.json"}
                     for record in json.loads(source.read_text())["packages"])
    result = stage(master, json.loads((repo/"catalog/games.json").read_text()),
        json.loads((repo/"catalog/releases.json").read_text()), json.loads(args.pippo.read_text()), feeds,
        source_hash=hashlib.sha256(raw).hexdigest())
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in zip(("games.json", "releases.json", "source-candidates.json", "excluded.json"), result):
        (args.output/name).write_text(json.dumps(data, ensure_ascii=False, indent=2)+"\n")
    print(json.dumps(dict(games=len(result[0]), releases=len(result[1]), provider_candidates=len(result[2]), excluded=len(result[3]))))


if __name__ == "__main__":
    main()
