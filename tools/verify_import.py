"""Verify a staged import without downloading games or saving artwork bytes.

Previously recorded exact metadata/HEAD evidence can be reused explicitly;
a fresh bounded range request still has to match the expected total size.
"""
import argparse
import json
import urllib.error
from pathlib import Path
from urllib.parse import urlsplit
from catalog import Client, archive_identity, now, verify_release


def save(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2)+"\n")
    temporary.replace(path)


def verify_import(client, release):
    e = release.get("importEvidence", {})
    identifier, filename = archive_identity(release["url"])
    if (e.get("url") != release["url"] or e.get("filename") != release["filename"]
            or filename != release["filename"]
            or e.get("metadataUrl") != f"https://archive.org/metadata/{identifier}"
            or e.get("httpStatus") != 200 or not e.get("metadataCheckedAt") or not e.get("headersCheckedAt")
            or e.get("headerContentLength") != release["sizeBytes"] or e.get("sizeBytes") != release["sizeBytes"]):
        return verify_release(client, release)
    status, headers, data, final = client.request(release["url"], headers={"Range":"bytes=0-15"}, limit=16)
    h = {k.lower():v for k,v in headers.items()}
    if (status != 206 or h.get("content-range") != f'bytes 0-15/{release["sizeBytes"]}'
            or len(data) != 16 or "text/" in h.get("content-type", "") or "json" in h.get("content-type", "")):
        raise ValueError("Fresh bounded range or total-size check failed")
    return {"checkedAt":now(), "metadataCheckedAt":e["metadataCheckedAt"], "headersCheckedAt":e["headersCheckedAt"],
        "rangeCheckedAt":now(), "url":release["url"], "metadataUrl":e["metadataUrl"],
        "method":"recorded exact metadata + matching HEAD; fresh 16-byte range", "httpStatus":200,
        "rangeStatus":206, "sizeBytes":release["sizeBytes"], "filename":release["filename"],
        "contentType":h.get("content-type", ""), "etag":h.get("etag", ""),
        "downloadHost":urlsplit(final).hostname, "sourceReportedHashes":e.get("sourceReportedHashes", {}),
        "consoleVerified":False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args=parser.parse_args(); root=args.directory
    games=json.loads((root/"games.json").read_text()); releases=json.loads((root/"releases.json").read_text())
    by_id={g["id"]:g for g in games}; client=Client(); progress={"phase":"checking", "failures":[]}
    try:
        for r in releases:
            progress.update(current=r["id"],updatedAt=now());save(root/"verification-status.json",progress)
            if not r.get("verification"):
                try:
                    r["verification"]=verify_import(client,r)
                except (ValueError,urllib.error.HTTPError) as error:
                    progress["failures"].append({"releaseId":r["id"],"error":str(error)})
                    print(json.dumps({"failed":r["id"],"error":str(error)}),flush=True)
                    continue
                save(root/"releases.json",releases)
            g=by_id[r["gameId"]]
            if not g["metadata"].get("artworkChecks"):
                status,headers,prefix,_=client.request(g["cover"],headers={"Range":"bytes=0-31"},limit=32)
                mime=next((v for k,v in headers.items() if k.lower()=="content-type"),"")
                image_signature=(prefix.startswith((b'\x89PNG\r\n\x1a\n',b'\xff\xd8\xff',b'GIF87a',b'GIF89a'))
                    or (prefix[:4]==b'RIFF' and prefix[8:12]==b'WEBP')
                    or (prefix[4:8]==b'ftyp' and prefix[8:12] in (b'avif',b'avis')))
                if status!=206 or not image_signature:
                    raise ValueError(f'Artwork is not an image: {g["id"]}')
                g["metadata"]["artworkChecks"]=[{"url":g["cover"],"contentType":mime or None,
                    "method":"32-byte image signature probe", "httpStatus":status,"checkedAt":now()}]
                save(root/"games.json",games)
            progress["verified"]=sum(bool(x.get("verification")) for x in releases)
            progress["artworkVerified"]=sum(bool(x["metadata"].get("artworkChecks")) for x in games)
            print(json.dumps({"verified":r["id"],"total":progress["verified"],"artwork":progress["artworkVerified"]}),flush=True)
        progress.update(phase="complete",updatedAt=now());save(root/"verification-status.json",progress)
    except BaseException as error:
        progress.update(phase="stopped",error=str(error),updatedAt=now());save(root/"verification-status.json",progress)
        raise


if __name__=="__main__": main()
