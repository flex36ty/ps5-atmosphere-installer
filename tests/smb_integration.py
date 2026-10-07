"""Run as Linux root with Samba installed. Uses only synthetic local test data.
The private Samba instance binds loopback on a random high port and is stopped
after the test. The application must be built at build/atmosphere-host first.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BINARY = os.environ.get("SMB_TEST_BINARY", str(ROOT / "build/atmosphere-host"))


def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1048576), b""):
            h.update(chunk)
    return h.digest()


def main():
    assert os.geteuid() == 0, "Samba requires root for its private test session"
    with tempfile.TemporaryDirectory(prefix="atmosphere-smb-") as tmp:
        base = Path(tmp)
        base.chmod(0o755)
        share, dest, state = [base / n for n in ("share", "destination", "state")]
        for p in (share, dest, state): p.mkdir(mode=0o755)
        folder = share / "Collection/Folder Game"
        (folder / "sce_sys").mkdir(parents=True)
        (folder / "data/empty").mkdir(parents=True)
        (folder / "sce_sys/param.json").write_text(json.dumps({"titleId": "PPSA00001", "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": "Synthetic Folder Game"}}}))
        png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aZ1sAAAAASUVORK5CYII=")
        (folder / "sce_sys/icon0.png").write_bytes(png)
        (folder / "eboot.bin").write_bytes(b"synthetic executable")
        (folder / "data/content.bin").write_bytes(os.urandom(700123))
        (share / "Image.exfat").write_bytes(os.urandom(1300017))
        (share / "Image.exfat.json").write_text('{"title":"Synthetic Image","titleId":"PPSA00002"}')
        (share / "Image.exfat.png").write_bytes(png)
        (share / "Resume.ffpfsc").write_bytes(b"resume-test" * 7000000)
        (share / "ignore.txt").write_text("not a game")
        (share / "escape.exfat").symlink_to("/etc/passwd")
        smb_port, http_port = port(), port()
        conf = base / "smb.conf"
        conf.write_text(f"""[global]
server role = standalone server
interfaces = 127.0.0.1
bind interfaces only = yes
smb ports = {smb_port}
server min protocol = SMB2
map to guest = Bad User
guest account = nobody
pid directory = {base}
lock directory = {base}
state directory = {base}
cache directory = {base}
private dir = {base}
log file = {base}/samba.log
log level = 1
[games]
path = {share}
guest ok = yes
read only = yes
wide links = no
follow symlinks = no
""")
        subprocess.run(["smbpasswd", "-c", str(conf), "-s", "-a", "root"], input="test-smb-password\ntest-smb-password\n", text=True, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        samba_log = open(base / "smbd.log", "w")
        samba = subprocess.Popen(["smbd", "--foreground", "--no-process-group", "--configfile", str(conf)], stdin=subprocess.PIPE, stdout=samba_log, stderr=subprocess.STDOUT, start_new_session=True)
        app, token = None, ""
        url = f"http://127.0.0.1:{http_port}/api/v1"

        def request(path="/smb", body=None, expected=None, auth=True):
            headers = {"Content-Type": "application/json", "Origin": f"http://127.0.0.1:{http_port}"}
            if auth and token: headers["Authorization"] = "Bearer " + token
            req = urllib.request.Request(url + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=20) as r: status, data = r.status, json.load(r)
            except urllib.error.HTTPError as e: status, data = e.code, json.load(e)
            assert status == expected if expected else status < 400, (status, data)
            return data

        def action(operation, **kw): return request(body={"action": operation, **({"allowDuplicate":True} if operation=="copy" else {}), **kw})

        def idle():
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                s = request()
                if not s["busy"]: return s
                time.sleep(.03)
            raise AssertionError("Worker timed out")

        def start():
            nonlocal app, token
            with open(base / "atmosphere.log", "w") as log:
                app = subprocess.Popen([BINARY, "--state", str(state), "--storage", str(dest), "--port", str(http_port)], stdout=log, stderr=subprocess.STDOUT)
            for _ in range(400):
                text = (base / "atmosphere.log").read_text()
                match = re.search(r"Pairing code: (\d{6})", text)
                if match:
                    try:
                        token = request("/pair", {"code": match[1]}, auth=False)["token"]
                        return
                    except OSError: pass
                assert app.poll() is None, text
                time.sleep(.05)
            raise AssertionError(text)

        try:
            for _ in range(100):
                try:
                    with socket.create_connection(("127.0.0.1", smb_port), .1): break
                except OSError: time.sleep(.05)
            else:
                print("Samba exit:", samba.poll(), flush=True)
                for p in base.glob("*.log"): print(p.name, p.read_text(), flush=True)
                raise AssertionError("Local test SMB listener did not start")
            start()
            request(auth=False, expected=401)
            cfg = dict(server=f"127.0.0.1:{smb_port}", share="games", folder="", username="", domain="", password="", remember=False)
            request(body={"action": "configure", **cfg, "folder": "../escape"}, expected=400)
            action("configure", **cfg)
            action("scan")
            scanned = idle()
            if len(scanned["games"]) != 3:
                print((base / "samba.log").read_text(), flush=True)
                print((base / "atmosphere.log").read_text(), flush=True)
            assert len(scanned["games"]) == 3, scanned
            by_name = {g["filename"]: g for g in scanned["games"]}
            assert by_name["Folder Game"]["title"] == "Synthetic Folder Game"
            assert by_name["Image.exfat"]["cover"].startswith("data:image/png;base64,")
            assert "password" not in scanned["settings"]
            print("PASS authentication, validation, recursive scan, metadata and covers", flush=True)
            action("copy", gameId=by_name["Folder Game"]["id"], storageId="desktop")
            copied = idle()
            assert copied["job"]["status"] == "complete", copied
            assert copied["job"]["verification"] == "sha256"
            for p in folder.rglob("*"):
                target = dest / "homebrew/Folder Game" / p.relative_to(folder)
                assert target.is_dir() if p.is_dir() else sha(target) == sha(p)
            print("PASS recursive copy, empty directories, SHA-256 verification", flush=True)
            action("copy", gameId=by_name["Image.exfat"]["id"], storageId="desktop")
            assert idle()["job"]["status"] == "complete"
            assert sha(dest / "homebrew/Image.exfat") == sha(share / "Image.exfat")
            action("copy", gameId=by_name["Image.exfat"]["id"], storageId="desktop")
            assert idle()["job"]["status"] == "error"
            assert sha(dest / "homebrew/Image.exfat") == sha(share / "Image.exfat")
            print("PASS image copy and overwrite protection", flush=True)
            for filename in ("Image.exfat", "Folder Game"):
                action("copy", gameId=by_name[filename]["id"], storageId="desktop", usbRoot=True)
                assert idle()["job"]["status"] == "complete"
            assert sha(dest / "Image.exfat") == sha(share / "Image.exfat")
            assert sha(dest / "Folder Game/data/content.bin") == sha(folder / "data/content.bin")
            action("copy", gameId=by_name["Image.exfat"]["id"], storageId="desktop", usbRoot=True)
            assert idle()["job"]["status"] == "error"
            assert sha(dest / "Image.exfat") == sha(share / "Image.exfat")
            print("PASS root placement for images/folders and overwrite protection", flush=True)

            def partial():
                action("copy", gameId=by_name["Resume.ffpfsc"]["id"], storageId="desktop")
                for _ in range(2000):
                    snap = request()
                    assert snap["job"]["speedBytesPerSecond"] >= 0
                    assert snap["job"]["phase"] in ("Preparing", "Transferring", "Checking partial copy", "Verifying")
                    if snap.get("job", {}).get("received", 0) > 0: break
                    assert snap["busy"], snap
                    time.sleep(.005)
                action("pause")
                snap = idle()
                assert snap["job"]["status"] == "paused", snap
                assert snap["job"]["speedBytesPerSecond"] == 0
                assert not (dest / "homebrew/Resume.ffpfsc").exists()
                return dest / ".atmosphere-smb-staging" / snap["job"]["id"] / "content"

            assert partial().stat().st_size > 0
            app.terminate(); app.wait(timeout=45)
            start()
            assert request()["job"]["status"] == "paused"
            action("resume")
            result = idle()
            assert result["job"]["status"] == "complete", result
            assert sha(dest / "homebrew/Resume.ffpfsc") == sha(share / "Resume.ffpfsc")
            print("PASS pause, staged invisibility, restart recovery and resume", flush=True)
            (dest / "homebrew/Resume.ffpfsc").unlink()  # Disposable test output only.
            part = partial()
            with part.open("r+b") as f: f.write(b"corrupt")
            action("resume")
            result = idle()
            assert result["job"]["status"] == "error" and "differs" in result["job"]["error"], result
            print("PASS corrupted partial rejection", flush=True)
            partial()
            source = share / "Resume.ffpfsc"
            st = source.stat(); os.utime(source, (st.st_atime, st.st_mtime + 10))
            action("resume")
            result = idle()
            assert result["job"]["status"] == "error" and "changed" in result["job"]["error"], result
            print("PASS changed source rejection", flush=True)
            action("configure", **{**cfg, "password": "synthetic-secret", "remember": False})
            assert "synthetic-secret" not in (state / "smb-state.json").read_text()
            assert "synthetic-secret" not in json.dumps(request())
            action("configure", **{**cfg, "password": "synthetic-secret", "remember": True})
            assert (state / "smb-state.json").stat().st_mode & 0o077 == 0
            assert "synthetic-secret" not in json.dumps(request())
            print("PASS credential privacy and opt-in persistence", flush=True)
            auth_cfg = {**cfg, "username": "root", "password": "test-smb-password", "destinationFolder": "custom/PS5"}
            action("configure", **auth_cfg)
            action("scan")
            result = idle()
            assert len(result["games"]) == 3, result
            selected = next(g for g in result["games"] if g["filename"] == "Image.exfat")
            action("copy", gameId=selected["id"], storageId="desktop")
            result = idle()
            assert result["job"]["status"] == "complete", result
            assert sha(dest / "custom/PS5/Image.exfat") == sha(share / "Image.exfat")
            assert "games" not in request("/smb/status")
            print("PASS authenticated SMB3, custom destination and lightweight status polling", flush=True)
            action("configure", **{**auth_cfg, "password": "wrong-password"})
            action("scan")
            rejected = idle()
            assert "Cannot connect" in rejected["message"] or "SMB connection failed" in rejected["message"], rejected
            print("PASS bad-password rejection without guest fallback", flush=True)
            action("configure", **cfg)
            first = request()["activeSourceId"]
            first_games = request()["games"]
            second = action("addSource")["activeSourceId"]
            assert second != first and not request()["games"]
            action("configure", **{**cfg, "folder": "Collection", "password": "session-secret"}, name="Second library")
            action("scan")
            assert len(idle()["games"]) == 1
            action("selectSource", sourceId=first)
            assert request()["games"] == first_games and not request()["hasPassword"]
            action("selectSource", sourceId=second)
            assert request()["hasPassword"] and request()["settings"]["name"] == "Second library"
            assert "session-secret" not in json.dumps(request())
            assert "session-secret" not in (state / "smb-state.json").read_text()
            app.terminate(); app.wait(timeout=45); start()
            assert request()["activeSourceId"] == second and not request()["hasPassword"]
            assert len(request()["games"]) == 1
            action("selectSource", sourceId=first)
            assert request()["games"] == first_games
            request(body={"action":"selectSource", "sourceId":"missing"}, expected=404)
            print("PASS multiple sources, isolated caches/credentials, switching and restart persistence", flush=True)
        finally:
            if app and app.poll() is None:
                app.terminate()
                try: app.wait(timeout=45)
                except subprocess.TimeoutExpired: app.kill(); app.wait()
            samba.terminate()
            try: samba.wait(timeout=10)
            except subprocess.TimeoutExpired: samba.kill(); samba.wait()
            samba_log.close()


if __name__ == "__main__": main()
