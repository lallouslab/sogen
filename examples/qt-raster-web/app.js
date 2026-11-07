const statusElement = document.querySelector('#status'), startButton = document.querySelector('#start'), stopButton = document.querySelector('#stop');
let host, worker = null, generation = 0, stopTimer, sessionScale = 1;
const frames = new Map(), windows = new Set();
globalThis.rasterEvidence = {logs: [], errors: [], frames: 0, runs: [], heartbeat: 0, maxHeartbeatGap: 0};
let lastBeat = performance.now();
setInterval(() => { const now = performance.now(); rasterEvidence.maxHeartbeatGap = Math.max(rasterEvidence.maxHeartbeatGap, now - lastBeat); lastBeat = now; ++rasterEvidence.heartbeat; }, 100);
function log(text) {
  rasterEvidence.logs.push(text); if (rasterEvidence.logs.length > 2000) rasterEvidence.logs.shift();
  document.querySelector('#log').textContent = rasterEvidence.logs.join('\n');
}
function fail(error) { rasterEvidence.errors.push(String(error)); statusElement.textContent = 'Could not run the guest'; log(String(error)); }
function receive(bytes) {
  const p = host._malloc(bytes.length);
  if (!p) throw Error('Display allocation failed');
  try { host.HEAPU8.set(bytes, p); if (!host._raster_receive(p, bytes.length)) throw Error('Display rejected a raster packet'); }
  finally { host._free(p); }
}
function clearDisplay() { frames.clear(); windows.clear(); host?._raster_clear(); }
function finish(run, result) {
  if (run !== generation || !worker) return;
  clearTimeout(stopTimer); worker.terminate(); worker = null;
  clearDisplay(); rasterEvidence.runs.push(result);
  startButton.disabled = false; stopButton.disabled = true;
  statusElement.textContent = result.forced ? 'Stopped after deadline' : (result.error || result.status ? 'Guest failed' : 'Guest stopped');
  if (result.error) fail(result.error);
}
function drain() {
  try {
    for (const [key, item] of frames) {
      if (item.run !== generation || !worker || !windows.has(key)) continue;
      receive(item.bytes); ++rasterEvidence.frames; rasterEvidence.firstFrame ??= performance.now();
      worker.postMessage({type: 'ack', key, sequence: RasterWire.view(item.bytes).getUint32(16, true)});
      statusElement.textContent = 'Running';
    }
    frames.clear();
  } catch (error) { fail(error); if (worker) finish(generation, {status: 1, error: String(error)}); }
  requestAnimationFrame(drain);
}
globalThis.rasterHostReady = module => { host = module; };
globalThis.rasterHostSend = bytes => { if (worker) worker.postMessage({type: 'input', bytes}, [bytes.buffer]); };
globalThis.rasterPointerButtons = 0;
window.addEventListener('pointerdown', event => { rasterPointerButtons = event.buttons; }, true);
window.addEventListener('pointerup', event => {
  rasterPointerButtons = event.buttons;
  if (!rasterPointerButtons) requestAnimationFrame(() => host?._raster_release_geometry());
}, true);
function releaseInput() {
  rasterPointerButtons = 0; host?._raster_release_input(); host?._raster_release_geometry();
}
window.addEventListener('pointercancel', releaseInput, true);
window.addEventListener('blur', releaseInput);
document.addEventListener('focusin', event => { if (!document.querySelector('#screen').contains(event.target)) releaseInput(); });
document.addEventListener('visibilitychange', () => { if (document.hidden) releaseInput(); });
function screenMetrics() {
  const screen = document.querySelector('#screen'), dpr = sessionScale;
  return [Math.max(1, Math.round(screen.clientWidth * dpr)), Math.max(1, Math.round(screen.clientHeight * dpr)), Math.round(dpr * 1000)];
}
function start() {
  if (!host || worker) return;
  clearDisplay(); sessionScale = Math.min(3, devicePixelRatio); const run = ++generation;
  const current = worker = new Worker(new URL('worker.js', location.href));
  startButton.disabled = true; stopButton.disabled = false; statusElement.textContent = 'Loading runtime';
  rasterEvidence.logs = []; rasterEvidence.errors = []; rasterEvidence.frames = 0; delete rasterEvidence.firstFrame;
  current.onerror = event => finish(run, {status: 1, error: event.message});
  current.onmessage = ({data}) => {
    if (run !== generation || current !== worker) return;
    try {
      if (data.type === 'packet') {
        rasterEvidence.workerHeapBytes = data.heapBytes;
        const v = RasterWire.view(data.bytes), key = RasterWire.key(v), op = v.getUint32(8, true);
        if (op === 2) windows.add(key);
        if (op === 5) { frames.delete(key); windows.delete(key); }
        if (op === 6) { if (!windows.has(key)) return; frames.set(key, {bytes: data.bytes, run}); }
        else receive(data.bytes);
      } else if ((data.type === 'log' || data.type === 'stdout')) log(data.text);
      else if (data.type === 'phase') statusElement.textContent = data.text;
      else if (data.type === 'done') finish(run, data);
    } catch (error) { finish(run, {status: 1, error: String(error)}); }
  };
  current.postMessage({type: 'start', application: document.querySelector('#application').value, screen: screenMetrics()});
}
startButton.onclick = start;
stopButton.onclick = () => {
  if (!worker || stopButton.disabled) return;
  host._raster_release_input(); stopButton.disabled = true; statusElement.textContent = 'Stopping';
  worker.postMessage({type: 'stop'}); const run = generation;
  stopTimer = setTimeout(() => finish(run, {status: null, forced: true}), 15000);
};
new ResizeObserver(() => { if (worker) worker.postMessage({type: 'screen', screen: screenMetrics()}); }).observe(document.querySelector('#screen'));
(async () => {
  try {
    host = await qtLoad({qt: {entryFunction: globalThis.raster_display_entry, containerElements: [document.querySelector('#screen')]}, print: log, printErr: log});
    const response = await fetch('runtime.json');
    if (!response.ok) throw Error('Could not load the runtime manifest');
    const manifest = await response.json(), labels = {calculator: 'Calculator', windows: 'Window behavior'};
    if (!manifest.applications || !Object.keys(manifest.applications).length) throw Error('No guest applications are configured');
    document.querySelector('#application').replaceChildren(...Object.keys(manifest.applications).map(name => new Option(labels[name] || name, name)));
    startButton.disabled = false; statusElement.textContent = 'Ready'; requestAnimationFrame(drain);
  } catch (error) { fail(error); }
})();
