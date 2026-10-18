// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Checks the OpenGL sample's page in a browser (samples/gl/web/README.md): serves it with Caddy
// and samples/gl/web/Caddyfile, opens it in headless Chrome on its software renderer, and drives
// it over the DevTools protocol in real time, as a user would. The page must come up cross-origin
// isolated on WebGL 2, draw tiles from orbit, and fly to the Sognefjord on a key press, loading the
// tiles there; nothing may throw, no WebGL call may fail and no read may fail on the way.
//
//     node tests/browser/page_check.mjs --chrome <chrome> --caddy <caddy> --web <directory>
//          [--packs <directory>] [--port <port>] [--screenshot <png>]
//
// --web is the directory of the built page; --packs, the sample set's packs (samples/data/packs).
// Exits with 1 on a failure, after a screenshot of the page.

import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const option = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
};
const chromePath = option('--chrome', process.env.CHROME);
const caddyPath = option('--caddy', process.env.CADDY);
const web = option('--web');
const packs = option('--packs', join(repo, 'samples', 'data', 'packs'));
const port = Number(option('--port', '18091'));
const screenshot = option('--screenshot', '');
if (!chromePath || !caddyPath || !web) {
  console.error('usage: page_check.mjs --chrome <chrome> --caddy <caddy> --web <directory> ' +
                '[--packs <directory>] [--port <port>] [--screenshot <png>]');
  process.exit(2);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);
const failures = [];
const fail = (what) => {
  failures.push(what);
  console.log(stamp(), 'FAILED:', what);
};

// What the page may log that is not a fault: Chrome's cache refusing a range of a file it is still
// storing, which the reader tries again (vgl/defaults.h).
const harmless = [/ERR_CACHE_OPERATION_NOT_SUPPORTED/];
// What is one.
const faults = [/WebGL: /, /GL_INVALID/, /\bfailed\b/i, /Aborted/, /RuntimeError/, /exception/i];

const children = [];
const profile = mkdtempSync(join(tmpdir(), 'vgl-page-check-'));
const cleanup = () => {
  for (const c of children) {
    c.kill();
  }
  try {
    rmSync(profile, { recursive: true, force: true });
  } catch {}
};

try {
  // The page's server.
  const caddy = spawn(caddyPath, ['run', '--config', join(repo, 'samples', 'gl', 'web', 'Caddyfile')], {
    cwd: repo,
    env: { ...process.env, VGL_SITE: `:${port}`, VGL_WEB: web, VGL_PACKS: packs },
    stdio: 'ignore',
  });
  children.push(caddy);
  const page = `http://localhost:${port}/vgl-gl.html`;
  let served = false;
  for (let i = 0; i < 50 && !served; ++i) {
    await sleep(200);
    try {
      served = (await fetch(page)).ok;
    } catch {}
  }
  if (!served) {
    throw new Error(`Caddy does not serve ${page}`);
  }

  // The browser, on SwiftShader, so that it needs no GPU.
  const debugPort = port + 1;
  const chrome = spawn(chromePath, [
    '--headless=new', `--remote-debugging-port=${debugPort}`, `--user-data-dir=${profile}`,
    '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--window-size=1280,720',
    '--no-first-run', '--no-default-browser-check', '--no-sandbox', 'about:blank'], { stdio: 'ignore' });
  children.push(chrome);
  let target;
  for (let i = 0; i < 100 && !target; ++i) {
    await sleep(200);
    try {
      const list = await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json();
      target = list.find((t) => t.type === 'page');
    } catch {}
  }
  if (!target) {
    throw new Error('Chrome does not answer on its debugging port');
  }

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r, e) => {
    ws.addEventListener('open', r);
    ws.addEventListener('error', e);
  });
  let id = 0;
  const pending = new Map();
  const send = (method, params = {}) => new Promise((done) => {
    pending.set(++id, done);
    ws.send(JSON.stringify({ id, method, params }));
  });
  const log = [];
  const note = (text) => {
    log.push(text);
    console.log(stamp(), text);
    if (faults.some((f) => f.test(text)) && !harmless.some((h) => h.test(text))) {
      fail(text);
    }
  };
  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) {
      pending.get(m.id)(m.result ?? m.error);
      pending.delete(m.id);
    } else if (m.method === 'Runtime.consoleAPICalled') {
      note(m.params.args.map((a) => a.value ?? a.description).join(' '));
    } else if (m.method === 'Runtime.exceptionThrown') {
      const d = m.params.exceptionDetails;
      fail('exception: ' + (d.exception?.description ?? d.text));
    } else if (m.method === 'Log.entryAdded') {
      note(`${m.params.entry.level}: ${m.params.entry.text} ${m.params.entry.url ?? ''}`);
    }
  });
  const evaluate = async (expression) =>
    (await send('Runtime.evaluate', { expression, returnByValue: true }))?.result?.value;
  // The title the sample keeps: "vgl - <depth> depth - <fps> fps - <tiles> tiles - ... - <km> km".
  const title = async () => {
    const t = (await evaluate('document.title')) ?? '';
    const tiles = /(\d+) tiles/.exec(t);
    const km = /(\d+) km$/.exec(t);
    return { text: t, tiles: tiles ? Number(tiles[1]) : 0, km: km ? Number(km[1]) : Infinity };
  };
  const until = async (what, seconds, test) => {
    const end = Date.now() + seconds * 1000;
    while (Date.now() < end) {
      if (await test()) {
        console.log(stamp(), 'ok:', what);
        return true;
      }
      await sleep(500);
    }
    fail(`${what}, within ${seconds} s`);
    return false;
  };

  await send('Runtime.enable');
  await send('Log.enable');
  await send('Page.enable');
  await send('Page.navigate', { url: page });

  await until('the page is cross-origin isolated', 20, async () => (await evaluate('window.crossOriginIsolated')) === true);
  await until('the backend draws with OpenGL ES 3.0 (WebGL 2)', 60,
              async () => log.some((l) => /vgl_gl_backend_create: drawing with OpenGL ES 3\.0/.test(l)));
  await until('tiles are drawn from orbit', 60, async () => (await title()).tiles >= 10);

  // A real key press: 7 flies to the Sognefjord, 4 km up.
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: 900, y: 400, button: 'left', clickCount: 1 });
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 900, y: 400, button: 'left', clickCount: 1 });
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: '7', code: 'Digit7', text: '7', windowsVirtualKeyCode: 55 });
  await sleep(100);
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: '7', code: 'Digit7', windowsVirtualKeyCode: 55 });
  await until('the camera reaches the Sognefjord', 90, async () => (await title()).km <= 10);
  await until('the tiles there load', 120, async () => (await title()).tiles >= 50);
  console.log(stamp(), 'title:', (await title()).text);

  if (screenshot) {
    const shot = await send('Page.captureScreenshot', { format: 'png' });
    writeFileSync(screenshot, Buffer.from(shot.data, 'base64'));
    console.log(stamp(), 'screenshot:', screenshot);
  }
  ws.close();
} catch (e) {
  fail(String(e?.message ?? e));
}
cleanup();
if (failures.length !== 0) {
  console.log(`\n${failures.length} check(s) failed`);
  process.exit(1);
}
console.log('\nthe page passed its checks');
process.exit(0);
