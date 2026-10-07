FROM atmosphere-sdk:0.43
USER root
RUN apt-get update && apt-get install -y --no-install-recommends curl python3 cmake pkg-config autoconf automake libtool perl libcurl4-openssl-dev libmicrohttpd-dev libssl-dev zstd && rm -rf /var/lib/apt/lists/*
COPY tools/build-deps.sh /tmp/build-deps.sh
RUN apt-get update && apt-get install -y --no-install-recommends xz-utils && rm -rf /var/lib/apt/lists/*
RUN --mount=type=cache,target=/tmp/atmosphere-deps bash /tmp/build-deps.sh
RUN apt-get update && apt-get install -y --no-install-recommends clang-format-19 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
ENTRYPOINT ["make"]
CMD ["host"]
