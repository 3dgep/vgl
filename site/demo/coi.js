// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Loaded first by the demo's page: when the page is not cross-origin isolated, registers
// coi-sw.js and reloads the page once it is active, so that the page then gets the headers its
// threads need (site/README.md). The first load stops here, before the demo starts without
// them.
'use strict';
(() => {
  if (window.crossOriginIsolated || !window.isSecureContext || !('serviceWorker' in navigator)) {
    return;
  }
  // After one reload, the page runs as it is: isolated, or saying what is missing.
  const key = 'vgl-coi-reloaded';
  let reloaded = false;
  try {
    reloaded = sessionStorage.getItem(key) === '1';
    sessionStorage.removeItem(key);
  } catch (e) {
  }
  if (reloaded) {
    return;
  }
  const reload = () => {
    try {
      sessionStorage.setItem(key, '1');
    } catch (e) {
    }
    window.location.reload();
  };
  window.stop();
  navigator.serviceWorker.register(new URL('coi-sw.js', document.currentScript.src))
    .then(() => navigator.serviceWorker.ready)
    .then(reload, (error) => {
      console.warn('vgl demo: the service worker failed to register', error);
      reload();
    });
})();
