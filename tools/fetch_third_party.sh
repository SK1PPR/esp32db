#!/usr/bin/env bash
# fetch_third_party.sh — download the engines we benchmark against into
# third_party/ (gitignored). Versions are pinned and checksummed so every board
# runs exactly the same code. Only needed for the flashdb / sqlite builds.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

FLASHDB_VER=2.2.0
FLASHDB_URL="https://github.com/armink/FlashDB/archive/refs/tags/${FLASHDB_VER}.tar.gz"
FLASHDB_SHA256=7dab46c1ce8203f2ebd267e1ef0c0627cd08a4b0b7722569436994c31ae9039e

SQLITE_VER=3530400  # 3.53.4
SQLITE_URL="https://www.sqlite.org/2026/sqlite-amalgamation-${SQLITE_VER}.zip"
SQLITE_SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e

check() {  # check <algo> <file> <expected>
    local got
    got="$(python3 -c "import hashlib,sys; print(hashlib.new(sys.argv[1], open(sys.argv[2],'rb').read()).hexdigest())" "$1" "$2")"
    if [[ "$got" != "$3" ]]; then
        echo "checksum mismatch for $2 ($1): got $got" >&2
        exit 1
    fi
}

mkdir -p "$DEST"

if [[ ! -f "$DEST/FlashDB/src/fdb_kvdb.c" ]]; then
    echo "fetching FlashDB $FLASHDB_VER"
    curl -fsSL -o "$TMP/flashdb.tar.gz" "$FLASHDB_URL"
    check sha256 "$TMP/flashdb.tar.gz" "$FLASHDB_SHA256"
    tar -xzf "$TMP/flashdb.tar.gz" -C "$TMP"
    rm -rf "$DEST/FlashDB"
    mv "$TMP/FlashDB-$FLASHDB_VER" "$DEST/FlashDB"
else
    echo "FlashDB already present"
fi

if [[ ! -f "$DEST/sqlite/sqlite3.c" ]]; then
    echo "fetching SQLite amalgamation $SQLITE_VER"
    curl -fsSL -o "$TMP/sqlite.zip" "$SQLITE_URL"
    check sha3_256 "$TMP/sqlite.zip" "$SQLITE_SHA3_256"
    python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$TMP/sqlite.zip" "$TMP"
    rm -rf "$DEST/sqlite"
    mv "$TMP/sqlite-amalgamation-$SQLITE_VER" "$DEST/sqlite"
else
    echo "SQLite already present"
fi

echo "third_party/ ready"
