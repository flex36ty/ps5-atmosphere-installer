#!/usr/bin/env bash
set -euo pipefail
mkdir -p /tmp/atmosphere-deps
cd /tmp/atmosphere-deps
fetch() {
  if ! test -f "$2"; then curl --silent --show-error --fail --location --retry 3 "$1" -o "$2"; fi
  printf '%s  %s\n' "$3" "$2" | sha256sum -c -
  tar xf "$2"
}
fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz openssl.tar.gz c53a47e5e441c930c3928cf7bf6fb00e5d129b630e0aa873b08258656e7345ec
fetch https://curl.se/download/curl-8.18.0.tar.xz curl.tar.xz 40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946
fetch https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz microhttpd.tar.gz a89e09fc9b4de34dde19f4fcb4faaa1ce10299b9908db1132bbfa1de47882b94
fetch https://zlib.net/fossils/zlib-1.3.1.tar.gz zlib.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
fetch https://codeload.github.com/sahlberg/libsmb2/tar.gz/51c5910da240a3b60bc4ad1b472185c2815628d4 libsmb2.tar.gz ab5398da368940bf78efb3e1949668d4f05d727d3a044db77aaeec5c3fcef671
cmake -S /tmp/atmosphere-deps/libsmb2-51c5910da240a3b60bc4ad1b472185c2815628d4 -B /tmp/atmosphere-deps/smb2-build \
  -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_LIBDCERPC=OFF \
  -DENABLE_EXAMPLES=OFF -DENABLE_UTILS=OFF
cmake --build /tmp/atmosphere-deps/smb2-build -j4
cmake --install /tmp/atmosphere-deps/smb2-build
export CPPFLAGS="-I$PS5_SYSROOT/user/homebrew/include"
export LDFLAGS="-L$PS5_SYSROOT/user/homebrew/lib"
cd /tmp/atmosphere-deps/zlib-1.3.1
CHOST=x86_64-pc-freebsd ./configure --static --prefix="$PREFIX"
make -j4
make install DESTDIR="$PS5_SYSROOT"
cd /tmp/atmosphere-deps/openssl-3.5.2
./Configure BSD-x86_64 no-tests no-apps no-shared --prefix="$PREFIX"
make -j4 build_sw
make install_sw DESTDIR="$PS5_SYSROOT"
cd /tmp/atmosphere-deps/curl-8.18.0
ac_cv_func_fnmatch=no ./configure --prefix="$PREFIX" --host=x86_64-pc-freebsd --enable-static --disable-shared --with-openssl="$PS5_SYSROOT/user/homebrew" --without-zlib --without-brotli --without-zstd --without-libpsl --without-libidn2 --without-libssh2 --disable-ldap --disable-ldaps --disable-docs --disable-manual --enable-ftp --disable-file --disable-dict --disable-gopher --disable-imap --disable-mqtt --disable-pop3 --disable-rtsp --disable-smb --disable-smtp --disable-telnet --disable-tftp
make -j4
make install DESTDIR="$PS5_SYSROOT"
cd /tmp/atmosphere-deps/libmicrohttpd-1.0.1
./configure --prefix="$PREFIX" --host=x86_64-pc-freebsd --enable-static --disable-shared --disable-doc --disable-curl --disable-examples --disable-https
make -j4
make install DESTDIR="$PS5_SYSROOT"
