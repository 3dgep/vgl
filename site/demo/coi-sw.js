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
  event.respondWith(fetch(request).then((response) => {
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
