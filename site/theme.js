// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The site's light and dark themes (site/README.md). Without a choice, the page follows the
// system; the switch in the bar sets data-theme on the root, which the brand's tokens read, and
// remembers it in this browser. Loaded in the head, before the page is drawn, so that a chosen
// theme shows from the start.
'use strict';
(() => {
  const key = 'vgl-theme';
  const root = document.documentElement;
  const system = window.matchMedia('(prefers-color-scheme: dark)');
  let stored = null;
  try {
    stored = localStorage.getItem(key);
  } catch (e) {
  }
  if (stored === 'light' || stored === 'dark') {
    root.dataset.theme = stored;
  }

  const current = () => root.dataset.theme || (system.matches ? 'dark' : 'light');
  const label = (button) => {
    const next = current() === 'dark' ? 'light' : 'dark';
    button.setAttribute('aria-label', `Switch to the ${next} theme`);
    button.title = `Switch to the ${next} theme`;
  };

  document.addEventListener('DOMContentLoaded', () => {
    const button = document.getElementById('theme-switch');
    if (!button) {
      return;
    }
    label(button);
    button.addEventListener('click', () => {
      const next = current() === 'dark' ? 'light' : 'dark';
      root.dataset.theme = next;
      try {
        localStorage.setItem(key, next);
      } catch (e) {
      }
      label(button);
    });
    system.addEventListener('change', () => label(button));
  });
})();
