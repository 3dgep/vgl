# The OpenGL sample in a browser

The OpenGL sample (`samples/gl`) builds with [Emscripten](https://emscripten.org) into a page that draws the globe with WebGL 2, the browser's OpenGL ES 3.0. It is the same program as the native sample, with the same panel, navigation, atmosphere, stars and clouds. Its thread pool runs on Web Workers, and the HTTP reader fetches packs with the browser's own fetch.

## Building

Install [emsdk](https://emscripten.org/docs/getting_started/downloads.html) (6.0.11 is tested), activate it, and build the `emscripten` preset. It needs Ninja on the path.

```sh
emsdk install 6.0.11 && emsdk activate 6.0.11
source ./emsdk_env.sh          # emsdk_env.ps1 on Windows
cmake --workflow --preset emscripten-release
```

The workflow also runs the tests, on Node.js. The page is `build/emscripten/bin/Release/vgl-gl.html`, with `vgl-gl.js`, `vgl-gl.wasm` and `vgl-gl.data`, which holds the clouds' image.

## Running

The page runs threads, which browsers allow only in a page that is *cross-origin isolated*: served with these two headers.

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Opening the file directly, or serving it from a server without the headers, shows a message saying so. `samples/gl/web/Caddyfile` serves the page with them, and the sample set at `data/packs` next to it, as the reference server serves packs ([hosting guide](../../../docs/hosting.md)). From the repository root, with [Caddy](https://caddyserver.com) 2.11:

```sh
caddy run --config samples/gl/web/Caddyfile
```

Then open <http://localhost:8090/vgl-gl.html>. `VGL_SITE`, `VGL_WEB` and `VGL_PACKS` change the address, the directory of the page and the directory of the packs.

## Options

The page takes the native sample's options from its address: a parameter with a value passes `--name value`, and one without passes `--name`. Without `pack`, it loads the sample set from `data/packs`.

| Address | Native equivalent |
|---|---|
| `vgl-gl.html?pack=https://packs.example.org/gebco&pack=https://packs.example.org/glo30` | `--pack https://packs.example.org/gebco --pack https://packs.example.org/glo30` |
| `vgl-gl.html?depth=conventional` | `--depth conventional` |

Relative pack URIs are fetched from the page's own address. Packs on another server load if it allows the page's origin with CORS, as the reference server does. `--check` is the native sample's only, since its loops would stop the page.

## Checking

`tests/browser/page_check.mjs` checks the page in headless Chrome, on its software renderer, so that it needs no GPU. It serves the page with Caddy and the Caddyfile above, drives it over the DevTools protocol as a user would, and fails if the page is not cross-origin isolated, does not draw tiles from orbit, or does not fly to the Sognefjord on the key 7 and load the tiles there, or if anything throws, a WebGL call fails or a read fails. With `VGL_TEST_BROWSER` on, it is the `browser` test of the `emscripten` build, finding Chrome and Caddy on the path or as `VGL_CHROME` and `VGL_CADDY`, and it leaves a screenshot in the build directory. CI runs it on Linux.

```sh
cmake --preset emscripten -DVGL_TEST_BROWSER=ON
cmake --build --preset emscripten-release
ctest --preset emscripten-release -R browser
```

## Browsers

WebGL 2 and shared memory between workers: Chrome and Edge 92, Firefox 79 and Safari 15.2, or later. The depth convention is logarithmic by default; reversed-Z needs clip control, which WebGL 2 does not have.
