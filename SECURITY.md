# Security policy

## Supported versions

Security fixes go into the latest release of the current major version, 1.x.

## Reporting a vulnerability

Please do not report security problems in public issues. Report them privately through
GitHub: open the repository's **Security** tab and choose **Report a vulnerability**. Include
what is affected, how to reproduce it (a pack, a configuration or a small program), and what
you think the impact is. The maintainer will acknowledge the report, agree a fix and a date for
disclosure with you, and credit you in the advisory unless you prefer otherwise.

## Scope

vgl reads data that may come from untrusted sources, so these matter most:

- The pack readers and decoders in the runtime (manifests, shards, tile encodings, imagery
  decoded with stb_image, Zstandard frames), which must reject malformed packs without crashing
  or reading out of bounds.
- The defaults module's HTTP reader and disk cache.
- `vgl-bake check`, which reads packs from any URL.
- The reference server configuration in `tools/server`.

Problems in a dependency (Zstandard, stb_image, libcurl, Ogre, GDAL) are best reported to that
project as well; tell us too if vgl needs a change or an update because of it.
