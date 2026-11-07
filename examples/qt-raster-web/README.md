# Browser raster presentation

This optional example runs the existing Windows calculator EXE through Sogen and
Unicorn inside a browser worker. A separate Qt/WASM program displays copied raster
frames and returns input. The Windows application and its matching platform plugin
remain Windows machine code. The emulator needs no host Qt SDK and uses no remote CPU
service.

Build the guest calculator, window diagnostic and matching plugin described in
`../qt-raster/README.md`. Keep the same EXE, plugin and runtime DLLs across native and
browser comparisons. The display component requires the Qt 6.8.3 single-thread WASM SDK
and its matching Emscripten toolchain. The emulator worker uses the main project's
Emscripten toolchain; these modules communicate through packets and are built separately.

## Build

From the repository root, configure an external browser-worker build with Node.js
filesystem support disabled:

```sh
emcmake cmake -S . -B /tmp/raster-worker -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSOGEN_EMSCRIPTEN_SUPPORT_NODEJS=OFF \
  -DSOGEN_ENABLE_RUST_CODE=OFF -DSOGEN_ENABLE_SDL3=OFF \
  -DSOGEN_ENABLE_STEAM=OFF -DSOGEN_ENABLE_PYTHON_BINDINGS=OFF
cmake --build /tmp/raster-worker --target qt-raster-web
```

Use the display SDK's matching toolchain for the second project:

```sh
cmake -S examples/qt-raster-web -B /tmp/raster-display -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/opt/qt/wasm_singlethread/lib/cmake/Qt6/qt.toolchain.cmake \
  -DQT_HOST_PATH=/opt/qt/gcc_64
cmake --build /tmp/raster-display
```

## Assemble and run locally

Select the files needed from an existing emulation root in a JSON array of relative
paths, including the registry hives, required system DLLs such as `apisetschema.dll`,
the guest EXEs, fonts, `qt.conf`, toolkit runtime DLLs and `platforms/qbridge.dll`.
Keep the selection and generated runtime outside the source tree. The assembly command
creates compressed files with a size/hash manifest:

```sh
python3 examples/qt-raster-web/assemble.py \
  --worker /tmp/raster-worker/artifacts --display /tmp/raster-display \
  --root /path/to/root --files /path/to/runtime-files.json \
  --application calculator=c:/qt-raster/calculator.exe \
  --application windows=c:/qt-raster/window-behavior.exe \
  --output /tmp/raster-site/nested/calculator
python3 -m http.server 8093 --bind 127.0.0.1 --directory /tmp/raster-site
```

Open `http://127.0.0.1:8093/nested/calculator/`, select an application and press Start.
Stop requests normal guest close; Start creates a fresh worker after it ends. The full
example and runtime must share an origin. Use HTTPS or a loopback origin for the browser's
hashing API. Assets use relative URLs so a nested static path works. Single-thread builds
need no shared-memory hosting headers. Generated binaries and runtime files are not part
of the source contribution.

## Ownership and limits

The optional worker launcher observes the existing per-device raster sessions under the
emulator's synchronization lock. It supplies screen metrics before the initial handshake,
so the guest creates windows at the selected scale. Guest scale is fixed for one run;
restart to apply a new device scale. Viewport changes still update available screen size. The host follows window drags locally
and sends the final geometry to the guest on release, avoiding intermediate guest repaints.
Create geometry, frame scale and pointer coordinates retain the version-2 wire units.

Browser callbacks queue input instead of entering guest C++. A full guest queue retains
the next input for retry. The host input queue is limited to 2048 packets, each at most
4096 bytes; overflow reports an error and stops the run. Focus loss releases held input.

The observer allows 64 windows across 16 sessions and at most 64 MiB of current session
frame pixels. Each window has one frame in flight; acknowledgment permits transfer of
the latest frame. The observer keeps a weak reference to the last sent frame, so obsolete
frames are not retained there. Session identity and frame sequence qualify acknowledgments.
Destroy and restart discard pending frames and stale input. The Qt display copies pixels
before the transfer buffer is released. These are transport/frame limits; total process
memory also includes the guest runtime, emulator heap, toolkit and browser graphics.

Transient windows are created after their owners and retired before them. The toolkit
handles popup, tooltip and modal presentation, while input returns to the guest's own
handlers. A stop closes children before owners. If normal close exceeds fifteen seconds,
the page terminates only its own worker and records a forced stop. A normal successful
close requires guest status zero. The display remains available for the next worker.

The runtime selection is limited to 512 files, 256 MiB per file and 512 MiB total. The
worker verifies uncompressed size and SHA-256 before installing each file in its own
filesystem. No guest filesystem persists between runs.

Session observer regressions are in `qt-raster-web-test`. Run the transport checks with
`node examples/qt-raster-web/worker.test.cjs`. Browser acceptance additionally needs real
calculator input, independent visible-result checks, resize, rapid input, secondary-window
behavior and stop/restart in Chromium and Firefox at DPR 1 and 2. A first frame alone is
only a startup checkpoint. GPU rendering, IME, clipboard and arbitrary applications remain
outside this example.
