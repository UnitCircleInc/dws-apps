# DWS Apps

Browser-based tools for testing and supporting DWS hardware, hosted at
<https://dws-apps.unitcircle.ca/>. Each tool runs entirely in the browser and
is also published as a single HTML file for offline use.

| Tool | Path |
|------|------|
| Sensor Board Tester — live hydrophone spectrum, pressure, temperature and firmware versions over Web Serial (Chrome/Edge). Add `?demo` to the app URL for simulated data. | [`/sensor-board-tester/`](https://dws-apps.unitcircle.ca/sensor-board-tester/) |

## Layout

```
sensor_wasm.c, utils/          C core for the sensor board tester → WebAssembly
web/index.html                 hub page listing the tools
web/assets/                    shared styles (site.css) and Unit Circle icons
web/<tool>/index.html          tool landing page
web/<tool>/app/                the tool itself
web/<tool>/img/                screenshots
tools/build_site.py            assembles _site/ and each tool's single-file download
tools/screenshots.py           regenerates screenshots from demo mode
```

To add a tool, create `web/<tool>/` with an `index.html` landing page and an
`app/index.html`, and add a card for it to `web/index.html`. `build_site.py`
picks up any `web/*/app/index.html` and builds `downloads/dws-<tool>.html`
by inlining its local `<script src>` files.

## Building

Needs Emscripten (`brew install emscripten`), python3 and git.

```
make setup        # fetch pinned kissfft into vendor/
make run          # build, then serve web/ on http://localhost:8080
make site         # build the deployable site into _site/
make screenshots  # refresh screenshots (needs Google Chrome)
```

Pushing to `main` builds and deploys to GitHub Pages
(`.github/workflows/pages.yml`). Tag a commit (`git tag v1.0.0`) to give the
site and downloads a release version instead of a commit hash.

## License

© Unit Circle Inc. Licensed under the [Apache License 2.0](LICENSE).
Includes [KISS FFT](https://github.com/mborgerding/kissfft) (BSD-3-Clause),
fetched at build time.
