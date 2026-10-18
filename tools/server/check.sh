#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten
#
# Checks the reference server (tools/server/Caddyfile): serves a copy of the sample packs with
# Caddy, checks that it answers range requests and refuses everything else, and loads the
# packs over HTTP with the headless sample, whose heights must match those read from disk.
#
#     tools/server/check.sh <caddy> <vgl-headless> [port]
#
# The headless sample must be built with VGL_DEFAULTS_HTTP. Needs curl.

set -u
caddy=$1
headless=$2
port=${3:-18080}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mktemp -d)
failed=0

cleanup()
{
    if [ -n "${server:-}" ]; then
        kill "$server" 2>/dev/null
        wait "$server" 2>/dev/null
    fi
    rm -rf "$work"
}
trap cleanup EXIT

fail()
{
    echo "FAILED: $*"
    failed=1
}

# The packs, with files the server must not hand out next to them.
cp -R "$repo/samples/data/packs" "$work/packs"
echo secret > "$work/packs/.hidden.vgls"
echo secret > "$work/packs/gebco/notes.txt"
echo secret > "$work/outside.vgls"

VGL_SITE=":$port" VGL_PACKS="$work/packs" "$caddy" run --config "$here/Caddyfile" \
    > "$work/caddy.log" 2>&1 &
server=$!
url="http://127.0.0.1:$port"
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$url/" && break
    sleep 0.2
done

shard=$(cd "$work/packs/gebco" && ls base-*.vgls)
size=$(wc -c < "$work/packs/gebco/$shard" | tr -d ' ')

# expect <status> <curl arguments>...: the request's status code.
expect()
{
    local want=$1
    shift
    local got
    got=$(curl -s -o /dev/null -w '%{http_code}' --path-as-is "$@")
    [ "$got" = "$want" ] || fail "$* answered $got, not $want"
}

# header <name> <value> <curl arguments>...: a response header, in any case.
header()
{
    local name=$1 value=$2
    shift 2
    curl -s -D - -o /dev/null --path-as-is "$@" | tr -d '\r' | grep -qi "^$name: $value\$" ||
        fail "$* has no '$name: $value' header"
}

# What a pack client needs.
expect 200 "$url/gebco/pack.vglm"
expect 200 -I "$url/gebco/pack.vglm"
expect 206 -H "Range: bytes=0-15" "$url/gebco/$shard"
expect 206 -H "Range: bytes=16-" "$url/gebco/$shard"
expect 416 -H "Range: bytes=$size-" "$url/gebco/$shard"
header Content-Range "bytes 0-15/$size" -H "Range: bytes=0-15" "$url/gebco/$shard"
header Content-Length 16 -H "Range: bytes=0-15" "$url/gebco/$shard"
header Accept-Ranges bytes "$url/gebco/$shard"
header Content-Type application/octet-stream "$url/gebco/$shard"
header X-Content-Type-Options nosniff "$url/gebco/$shard"
header Cache-Control "public, max-age=31536000, immutable" "$url/gebco/$shard"
header Cache-Control "public, max-age=60" "$url/gebco/pack.vglm"
header Access-Control-Allow-Origin '\*' "$url/gebco/pack.vglm"
etag=$(curl -s -D - -o /dev/null "$url/gebco/$shard" | tr -d '\r' | grep -i '^etag:' | cut -d' ' -f2-)
[ -n "$etag" ] || fail "a shard has no ETag"
expect 304 -H "If-None-Match: $etag" "$url/gebco/$shard"
expect 204 -X OPTIONS -H "Origin: https://example.org" \
    -H "Access-Control-Request-Headers: range" "$url/gebco/$shard"
header Access-Control-Allow-Headers "Range, If-None-Match, If-Range" -X OPTIONS "$url/gebco/$shard"
curl -s -D - -o /dev/null "$url/gebco/pack.vglm" | grep -qi '^server:' && fail "the server names itself"

# What it refuses.
expect 405 -X POST "$url/gebco/pack.vglm"
expect 405 -X PUT --data x "$url/gebco/pack.vglm"
expect 405 -X DELETE "$url/gebco/pack.vglm"
expect 416 -H "Range: bytes=0-1,4-5" "$url/gebco/$shard"
expect 404 "$url/"
expect 404 "$url/gebco"
expect 404 "$url/gebco/"
expect 404 "$url/gebco/notes.txt"
expect 404 "$url/.hidden.vgls"
expect 404 "$url/../outside.vgls"
expect 404 "$url/gebco/../../outside.vgls"
expect 404 "$url/%2e%2e/outside.vgls"
expect 404 "$url/gebco/..%2f..%2foutside.vgls"
expect 404 "$url/gebco/missing.vgls"

# The packs over HTTP give the heights the packs on disk do.
places()
{
    "$headless" "$@" 2>/dev/null | sed -n '/^place /,$p'
}
names="gebco glo30-sognefjord egm2008 bluemarble"
from_disk=()
over_http=()
for n in $names; do
    from_disk+=(--pack "$work/packs/$n")
    over_http+=(--pack "$url/$n")
done
disk=$(places "${from_disk[@]}")
http=$(places "${over_http[@]}")
[ -n "$disk" ] || fail "the headless sample printed no heights from disk"
[ "$disk" = "$http" ] || fail "the heights over HTTP differ from those on disk"
echo "$http"

if [ $failed -ne 0 ]; then
    echo "Caddy's log:"
    tail -20 "$work/caddy.log"
    exit 1
fi
echo "The reference server passed its checks."
