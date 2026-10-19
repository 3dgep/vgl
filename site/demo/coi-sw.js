// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Makes the demo cross-origin isolated on a host that cannot send headers (site/README.md):
// it adds Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy to every response the
// demo's page receives, which the page's threads need. coi.js registers it.
'use strict';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
  const request = event.request;
  // Requests the worker cannot repeat are left to the browser.
  if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') {
    return;
  }
  // GitHub Pages compresses pack files, and then answers range requests with ranges of the
  // compressed stream, which no reader can decode. The site's copies of the packs are named
  // with a .zip suffix instead, a type it leaves alone (site/build.sh), and the demo's reads
  // of data/packs go to those names, with their Range header.
  const url = new URL(request.url);
  const pack = url.origin === self.location.origin && url.pathname.includes('/data/packs/') &&
               !url.pathname.endsWith('.zip');
  const target = pack ? new Request(url.href + '.zip', request) : request;
  // Chrome's HTTP cache refuses a range of a file it is still storing, as when several of the
  // demo's reads of one shard overlap: such a read is tried again past the cache.
  const read = fetch(target).catch(() => fetch(target, { cache: 'no-store' }));
  event.respondWith(read.then((response) => {
    // Opaque responses cannot be changed, and cross-origin ones keep their own headers.
    if (response.status === 0 || response.type === 'opaque') {
      return response;
    }
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    if (response.type === 'basic') {
      headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    }
    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers: headers,
    });
  }));
});
