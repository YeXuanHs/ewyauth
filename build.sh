#!/usr/bin/env bash
# Build ewyauth against the PHP currently on PATH and drop the .so into dist/.
set -euo pipefail

PHP_BIN="${PHP_BIN:-php}"
PHP_CONFIG="${PHP_CONFIG:-php-config}"

VER="$("$PHP_BIN" -r 'echo PHP_MAJOR_VERSION, ".", PHP_MINOR_VERSION;')"
TS="$("$PHP_BIN" -r 'echo PHP_ZTS ? "zts" : "nts";')"
ARCH="$(uname -m)"

echo "==> building ewyauth for PHP ${VER} (${TS}, ${ARCH})"

phpize >/dev/null
./configure --with-php-config="$(command -v "$PHP_CONFIG")" >/dev/null
make -j"$(nproc)"

mkdir -p dist
OUT="dist/ewyauth-${VER}-${TS}-linux-${ARCH}.so"
cp modules/ewyauth.so "$OUT"

"$PHP_BIN" -d extension="$(pwd)/modules/ewyauth.so" \
    -r 'exit(extension_loaded("ewyauth") ? 0 : 1);' \
    && echo "==> smoke test ok"

echo "==> $OUT"
sha256sum "$OUT" || true
