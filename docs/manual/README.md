# PitchNet Reference Manual

Source of the online manual, laid out like the Audacity Reference Manual.

- `pages.py` – the content of every page (HTML fragments) and the sidebar order.
- `build.py` – wraps the pages in the shared layout and writes the site to `site/`.
- `style.css`, `manual.js`, `search.html` – layout, sidebar and client-side search.
- `images/` – screenshots (`main-window.jpg`, …) and toolbar icons.

Build and preview locally:

```bash
python3 docs/manual/build.py          # -> docs/manual/site/
python3 -m http.server -d docs/manual/site 8000
python3 docs/manual/build.py --single # also writes one self-contained pitchnet-manual.html
```

`manual-pages.yml` is the `Manual (GitHub Pages)` workflow: move it to
`.github/workflows/manual-pages.yml`. It builds and publishes the site on every push to
`master` that touches `docs/manual/`. Enable it once in the repository's
**Settings › Pages › Build and deployment › Source: GitHub Actions**.

A screenshot referenced as `[[fig:name.png|caption]]` in `pages.py` is shown only
once `images/name.png` exists, so pages can name screenshots before they are taken.
The numbered pins on the home page are positioned in `WINDOW_REGIONS` (percent of
`images/main-window.jpg`).
