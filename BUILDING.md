# Building atmosphere.elf

This checkout includes the SMB fork described in [SMB.md](SMB.md). Rebuild the
dependency image: it now includes the pinned libsmb2 client library.

This source builds **Atmosphere 0.3.0**. Publisher metadata and catalogue link checks are saved in the included JSON snapshot; a build does not fetch game metadata or artwork.

Requirements: Node.js 22+, Python 3 and Docker.

```sh
python3 -m venv .venv
.venv/bin/python tools/bootstrap.py      # pinned cJSON files and the CA bundle
npm ci
npm run build                            # interface, embedded into the payload
docker build -f tools/Dockerfile.sdk -t atmosphere-sdk:0.43 .
docker build -t atmosphere-build:0.1 .        # builds curl, OpenSSL and libmicrohttpd for the SDK
docker run --rm -v "$PWD:/work" atmosphere-build:0.1 payload
```

The result is `build/atmosphere.elf`. It carries `build/atmosphere_runtime.elf`, the copy it saves on the console.

To use a modified libmicrohttpd, change its download and build steps in `tools/build-deps.sh`, rebuild the `atmosphere-build` image, then run the last command again. `npm run build` must run before `make payload` whenever the interface changes; the payload embeds the built interface.
