#!/usr/bin/env bash
# Build a static OpenSSL (pinned version, verified tarball) for release builds.
#
# Usage: tools/build-openssl.sh <prefix> [Configure args...]
#   e.g. MACOSX_DEPLOYMENT_TARGET=11.0 tools/build-openssl.sh "$HOME/openssl" -mmacosx-version-min=11.0
#
# Skips the build if <prefix>/lib/libssl.a already exists (CI cache hit).
set -euo pipefail

OPENSSL_VERSION=${OPENSSL_VERSION:-3.5.8}
OPENSSL_SHA256=${OPENSSL_SHA256:-a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2}

prefix=${1:?usage: $0 <prefix> [Configure args...]}
shift

if [ -f "$prefix/lib/libssl.a" ] && [ -f "$prefix/lib/libcrypto.a" ]; then
  echo "OpenSSL already built in $prefix"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tarball="$work/openssl-$OPENSSL_VERSION.tar.gz"

curl -fsSL -o "$tarball" \
  "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"

if command -v sha256sum >/dev/null; then
  actual=$(sha256sum "$tarball" | cut -d' ' -f1)
else
  actual=$(shasum -a 256 "$tarball" | cut -d' ' -f1)
fi
if [ "$actual" != "$OPENSSL_SHA256" ]; then
  echo "OpenSSL $OPENSSL_VERSION checksum mismatch: $actual" >&2
  exit 1
fi

tar -xzf "$tarball" -C "$work"
cd "$work/openssl-$OPENSSL_VERSION"
./Configure no-shared no-tests no-docs no-module no-engine \
  --prefix="$prefix" --libdir=lib "$@"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" build_libs
make install_dev
echo "OpenSSL $OPENSSL_VERSION installed in $prefix"
