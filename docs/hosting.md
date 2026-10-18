# Hosting packs

Packs are plain files that are read with HTTP range requests ([pack format](pack-format.md)), so hosting them needs no server code of vgl's own (design document, section 10.2). This page covers the two recommended ways to publish them: an object store, and the reference server in `tools/server`, a configuration for [Caddy](https://caddyserver.com) that is safe to expose on a public endpoint. Hosts read the packs with the defaults module's HTTP reader (`vgl_http_reader`) or with an I/O interface of their own.

## Choosing a host

| | Object store | Reference server |
|---|---|---|
| Examples | Cloudflare R2, Amazon S3, Backblaze B2 | Caddy with `tools/server/Caddyfile`, on a machine you run |
| Cost | Storage, and egress unless the store waives it | The machine and its traffic |
| HTTPS | Provided | Automatic, from Let's Encrypt or ZeroSSL |
| Maintenance | None | Updates to Caddy and the operating system |
| Suits | Public datasets, most installations | Local networks, private or offline sites, development, full control |

An object store without egress fees, such as R2, is the recommended host for the global dataset (design document, section 10.2). The reference server suits everything else, and is also how to try a pack over HTTP locally.

Any other web server works too, if it:

- answers `Range: bytes=a-b` and `bytes=a-` with `206 Partial Content`, and a range past the end with `416`;
- does not compress pack files: compression applies to the whole body, so a range of a compressed response is not a range of the file;
- sends `Access-Control-Allow-Origin`, and answers preflight requests, if browsers read the packs. vgl's web build runs in pages that are cross-origin isolated, which load packs from other servers only through CORS or with `Cross-Origin-Resource-Policy: cross-origin` ([web sample](../samples/gl/web/README.md)).

## The reference server

`tools/server/Caddyfile` makes Caddy a read-only server of pack files. It needs Caddy 2.11 or later and no plugins.

### Running it

Put the packs in one directory, each in a subdirectory of its own (`packs/gebco/pack.vglm`, `packs/gebco/base-1cd9fe31.vgls`, and so on), and start Caddy:

```sh
# Locally, over plain HTTP on port 8080.
VGL_PACKS=/srv/packs caddy run --config tools/server/Caddyfile

# In public, over HTTPS: point the domain's DNS records at the machine and open ports 80 and 443.
VGL_SITE=packs.example.org VGL_PACKS=/srv/packs caddy run --config tools/server/Caddyfile
```

A pack is then at `https://packs.example.org/gebco`, which is the URI to pass to `vgl_pack_create` and to the samples' `--pack`.

The settings come from the environment:

| Variable | Default | Meaning |
|---|---|---|
| `VGL_SITE` | `:8080` | A domain name serves HTTPS with a certificate Caddy obtains and renews, and redirects HTTP to it. A port serves plain HTTP, for local use or behind a proxy or CDN that terminates TLS. |
| `VGL_PACKS` | `packs` | The directory of packs, relative to the working directory. |
| `VGL_CORS_ORIGIN` | `*` | The origin that browsers may read packs from. Packs are public data, so `*` is usually right; name an origin to keep other sites from using your bandwidth from browsers. |

With Docker, Caddy's official image runs the same configuration. The `caddy-data` volume keeps the certificates across restarts:

```sh
docker run -d --name vgl-packs --restart unless-stopped \
    -p 80:80 -p 443:443 -p 443:443/udp \
    -e VGL_SITE=packs.example.org -e VGL_PACKS=/srv/packs \
    -v /srv/packs:/srv/packs:ro \
    -v "$PWD/tools/server/Caddyfile:/etc/caddy/Caddyfile:ro" \
    -v caddy-data:/data \
    caddy:2.11.7
```

On Linux without containers, a systemd unit runs Caddy as an unprivileged user with read-only access to the system:

```ini
[Unit]
Description=vgl pack server
After=network-online.target
Wants=network-online.target

[Service]
User=caddy
Group=caddy
Environment=VGL_SITE=packs.example.org VGL_PACKS=/srv/packs
ExecStart=/usr/bin/caddy run --config /etc/caddy/Caddyfile
AmbientCapabilities=CAP_NET_BIND_SERVICE
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/var/lib/caddy
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

The configuration has no admin endpoint, so restart Caddy to change it.

### What it does

- Serves only pack files: manifests (`pack.vglm`) and shards (`*.vgls`). Every other path, directory listings and hidden files among them, gets `404`, and paths cannot leave the pack directory.
- Allows `GET` and `HEAD`, and `OPTIONS` for browsers' preflight requests; other methods get `405`.
- Answers one range per request. A request for several ranges gets `416`: vgl never asks for them, and they let a client make the server do much work for one small request.
- Never compresses, sends `application/octet-stream` with `X-Content-Type-Options: nosniff`, and does not name itself in a `Server` header.
- Lets browsers read packs from pages on other sites, cross-origin isolated ones too: CORS for `VGL_CORS_ORIGIN`, and `Cross-Origin-Resource-Policy: cross-origin`.
- Lets caches keep shards forever (`immutable`), since a shard's name holds a digest of its content, and recheck manifests after a minute, since they change in place when a pack is rebaked. Responses carry an `ETag`, so a recheck costs a `304`.
- Limits slow clients with timeouts on reading headers and bodies, writing responses and idle connections, and limits headers to 16 KB.
- Over HTTPS, sends `Strict-Transport-Security`, and redirects HTTP to HTTPS.
- Logs every request to standard output.

### What the operator does

The configuration covers what a web server can enforce on its own. On a public endpoint, also:

- **Limit request rates.** Caddy has no rate limiting without plugins. Put a CDN in front (Cloudflare's free plan caches shards at its edge and absorbs floods), or limit connections per address in the firewall, so that one client cannot take all the bandwidth.
- **Keep Caddy updated**, and the operating system with it. Watch [Caddy's releases](https://github.com/caddyserver/caddy/releases) for security fixes.
- **Give Caddy read-only access to the packs**, and run it as an unprivileged user, as in the unit above. Caddy follows symbolic links, so keep links that point outside the pack directory out of it.
- **Open only ports 80 and 443** (and UDP 443 for HTTP/3). Port 80 is needed to obtain certificates and to redirect to HTTPS.
- **Mind the logs.** Access logs hold clients' IP addresses, which are personal data in the EU. Keep them only as long as needed, or remove the `log` block.
- **Publish the attribution** that each dataset's license requires (design document, section 9). vgl reads it from the manifests (`vgl_context_attribution_get`), so hosts can show it.

## Updating packs

A rebaked shard gets a new name, and the manifest is the only file that changes in place ([pack format](pack-format.md), section 5.2). To update a pack without readers ever seeing a mix of old and new:

1. Upload the new shards. Readers of the old manifest do not see them.
2. Replace the manifest at once: upload it under a temporary name and rename it, or rely on the object store's atomic writes.
3. After caches have moved on (a minute for the reference server's manifests, longer behind a CDN), delete the shards that the new manifest no longer names.

## Checking a server

`tools/server/check.sh` checks the reference server: it serves a copy of the sample packs with Caddy, checks that range requests work and that everything listed above is refused, and loads the packs over HTTP with the headless sample, which must report the same heights as from disk. CI runs it on Linux.

```sh
tools/server/check.sh "$(command -v caddy)" build/linux-gcc-shared/bin/Release/vgl-headless
```

The headless sample must be built with `VGL_DEFAULTS_HTTP`, as the shared presets do.
