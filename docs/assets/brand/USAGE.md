# vgl brand assets

Suggested repo location: `docs/assets/brand/` (paths below assume that).
All SVGs are font-free (text is outlined) and use only `rect`, `circle`, `ellipse` and `path`.

The two masters are `svg/vgl-icon.svg` and `svg/vgl-banner.svg`. To regenerate everything after
editing a master: `python3 tools/build_brand_assets.py .` from this folder (needs `resvg-py`,
`fonttools`, `Pillow` and Inter in `fonts/Inter.ttf` for the social-card tagline).

The compact logos (`*-compact-*`) are derived from the full ones, SVG and PNG, by
`python3 tools/make_compact_logo.py .` from this folder (PNG export needs `resvg-py`, as for
`build_brand_assets.py`). Run it again after regenerating the full logos.

## README header (switches with GitHub light/dark theme)

```html
<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/brand/svg/vgl-logo-dark.svg">
    <img src="docs/assets/brand/svg/vgl-logo-light.svg" alt="vgl: Virtual Globe Library" width="360">
  </picture>
</p>
```

## Website `<head>`

```html
<link rel="icon" href="/favicon.ico" sizes="32x32">
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<link rel="apple-touch-icon" href="/apple-touch-icon.png">
<link rel="manifest" href="/site.webmanifest">
<meta name="theme-color" content="#0B132B">
<meta property="og:image" content="https://YOUR-SITE/vgl-og-image.png">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta name="twitter:card" content="summary_large_image">
```

The `favicon.ico` 16 px frame is a pixel-fitted redraw (`favicon-16.svg`); the 32 and 48 px
frames come from the 64 px master. Browsers that prefer `favicon.svg` render the master at
16 px, which is softer. Drop the SVG link if you want the crisp 16 px frame everywhere.

## GitHub social preview

Repo Settings > General > Social preview: upload `png/vgl-social-preview.png` (1280x640).

## Files

| File | Use |
|---|---|
| `svg/vgl-logo-dark.svg`, `-light.svg` | Horizontal lockup on transparent, for dark or light pages |
| `svg/vgl-logo-compact-dark.svg`, `-light.svg` | Horizontal lockup without the subtitle, `vgl` scaled to fill its height, for small sizes such as a nav bar |
| `svg/vgl-logo-stacked-compact-dark.svg`, `-light.svg` | Stacked lockup without the subtitle, `vgl` scaled to fill the height below the globe |
| `svg/vgl-logo-stacked-*.svg` | Icon above wordmark, for narrow spaces (docs sidebar, title pages) |
| `svg/vgl-wordmark-*.svg` | `vgl` letters only |
| `svg/vgl-mark.svg` | Globe mark, quadtree keyline knocked out to transparent |
| `svg/vgl-icon.svg` | 64x64 app icon on the Orbital Slate tile (master, Illustrator export; all other assets derive from its geometry) |
| `svg/vgl-banner.svg` | Master lockup on Orbital Slate (Illustrator export; all logos, wordmarks and share images derive from it) |
| `svg/vgl-social-preview.svg`, `vgl-og-image.svg` | Sources for the share images |
| `png/` | Raster exports (@1x and @2x lockups, compact ones included, icon 64 to 1024) |
| `favicon/` | ico, svg, apple-touch, 192/512, maskable 512, webmanifest |
| `css/vgl-tokens.css` | Palette, type stacks, light/dark semantic tokens |

Light-mode lockups use Orbital Slate for `vgl`, `#475569` for the subtitle and Bathymetry Blue
for the `g` dot (cyan is 2.1:1 on white).
