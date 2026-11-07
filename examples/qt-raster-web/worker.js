importScripts('wire.js');
const wire = RasterWire;
const inflight = new Map(), windows = new Map(), sessions = new Set();
let started = false, ended = false, screen = [1280, 900, 1000];
globalThis.sogenRasterInput = [];
const send = (type, extra = {}) => postMessage({type, ...extra});
let stdoutTail = '';
globalThis.sogenRasterStdout = text => {
  stdoutTail += text;
  if (stdoutTail.length > 65536) throw Error('Guest output line exceeds 64 KiB');
  let end;
  while ((end = stdoutTail.indexOf('\n')) !== -1) {
    send('stdout', {text: stdoutTail.slice(0, end).replace(/\r$/, '')});
    stdoutTail = stdoutTail.slice(end + 1);
  }
};
function enqueue(bytes) {
  const v = wire.view(bytes), op = v.getUint32(8, true);
  if (bytes.length > 4096 || op < 20 || op > 26) throw Error('Invalid browser input');
  if (!sessions.has(v.getBigUint64(56, true).toString())) return;
  if (op !== 26 && !windows.has(wire.key(v))) return;
  const queue = globalThis.sogenRasterInput;
  if (queue.length >= 2048) throw Error('Browser input queue exhausted');
  queue.push(bytes);
}
function screenInput(identity) { enqueue(wire.packet(26, 0, screen, identity)); }
globalThis.sogenRasterReady = (low, high, id) => !ended && !inflight.has(((BigInt(high) << 32n) | BigInt(low)).toString() + ':' + id);
globalThis.sogenRasterSend = bytes => {
  if (ended) return;
  const v = wire.view(bytes), op = v.getUint32(8, true), id = v.getUint32(12, true);
  const identity = v.getBigUint64(56, true).toString(), key = wire.key(v);
  if (op === 1) { sessions.add(identity); screenInput(identity); }
  if (op === 2) windows.set(key, {identity, id, parent: v.getUint32(48, true)});
  if (op === 5) {
    windows.delete(key); inflight.delete(key);
    globalThis.sogenRasterInput = sogenRasterInput.filter(b => wire.key(wire.view(b)) !== key);
  }
  if (op === 10) {
    sessions.delete(identity);
    for (const [key, value] of windows) if (value.identity === identity) { windows.delete(key); inflight.delete(key); }
    globalThis.sogenRasterInput = sogenRasterInput.filter(b => wire.view(b).getBigUint64(56, true).toString() !== identity);
  }
  if (op === 6) {
    if (inflight.has(key)) throw Error('A raster frame is already in flight');
    inflight.set(key, v.getUint32(16, true));
  }
  postMessage({type: 'packet', bytes, heapBytes: globalThis.sogenRasterHeapBytes}, [bytes.buffer]);
};
function finish(status, error) {
  if (ended) return; ended = true;
  globalThis.sogenRasterInput = [];
  send('done', {status, error, inflight: inflight.size});
  inflight.clear(); windows.clear(); sessions.clear();
}
globalThis.sogenRasterDone = finish;
async function fetchBytes(url) {
  const target = new URL(url, self.location.href);
  if (target.origin !== self.location.origin) throw Error('Runtime assets must share the page origin');
  const response = await fetch(target);
  if (!response.ok) throw Error(`HTTP ${response.status}: ${target.pathname}`);
  return new Uint8Array(await response.arrayBuffer());
}
async function loadRuntime(module, application) {
  const manifest = JSON.parse(new TextDecoder().decode(await fetchBytes('runtime.json')));
  if (!Array.isArray(manifest.files) || manifest.files.length > 512 || !Object.hasOwn(manifest.applications, application))
    throw Error('Invalid runtime manifest');
  let total = 0;
  for (const entry of manifest.files) {
    if (typeof entry.path !== 'string' || entry.path.startsWith('/') || entry.path.split('/').some(x => !x || x === '.' || x === '..') || entry.path.includes('\\') ||
        !Number.isSafeInteger(entry.bytes) || entry.bytes < 0 || entry.bytes > 256 * 1024 * 1024 || !/^[0-9a-f]{64}$/.test(entry.sha256))
      throw Error('Invalid runtime file entry');
    total += entry.bytes;
  }
  if (total > 512 * 1024 * 1024) throw Error('Runtime exceeds 512 MiB');
  let next = 0, loaded = 0;
  await Promise.all(Array.from({length: 2}, async () => {
    while (next < manifest.files.length) {
      const entry = manifest.files[next++];
      let bytes = await fetchBytes(entry.url);
      if (entry.gzip) bytes = new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
      if (bytes.length !== entry.bytes) throw Error('Runtime size mismatch: ' + entry.path);
      const digest = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), x => x.toString(16).padStart(2, '0')).join('');
      if (digest !== entry.sha256) throw Error('Runtime hash mismatch: ' + entry.path);
      const target = '/root/' + entry.path;
      module.FS.mkdirTree(target.slice(0, target.lastIndexOf('/')));
      module.FS.writeFile(target, bytes, {canOwn: true});
      send('phase', {text: `Loading runtime ${++loaded}/${manifest.files.length}`});
    }
  }));
  return manifest.applications[application];
}
onmessage = async ({data}) => {
  if (ended) return;
  try {
    if (data.type === 'input') { enqueue(data.bytes); return; }
    if (data.type === 'ack') {
      if (windows.has(data.key) && inflight.get(data.key) === data.sequence) inflight.delete(data.key);
      return;
    }
    if (data.type === 'screen') {
      screen = data.screen; globalThis.sogenRasterScreen = screen;
      for (const identity of sessions) screenInput(identity);
      return;
    }
    if (data.type === 'stop') {
      const depth = item => { let result = 0; for (let parent = item.parent; parent && result < 64; ++result) parent = windows.get(item.identity + ':' + parent)?.parent || 0; return result; };
      for (const item of [...windows.values()].sort((a,b) => depth(b) - depth(a))) enqueue(wire.packet(23, item.id, [], item.identity));
      return;
    }
    if (data.type !== 'start' || started) return;
    started = true; screen = data.screen; globalThis.sogenRasterScreen = screen;
    const module = globalThis.Module = {
      noInitialRun: true,
      locateFile: path => new URL(path, self.location.href).href,
      print: text => send('log', {text: text.replace(/<\/?span\b[^>]*>/g, '')}), printErr: text => send('log', {text: text.replace(/<\/?span\b[^>]*>/g, '')}),
      onAbort: reason => finish(1, String(reason)),
      onExit: status => finish(status, null),
      onRuntimeInitialized: () => {
        (async () => {
          try {
            const entry = await loadRuntime(module, data.application);
            send('phase', {text: 'Starting Windows guest'});
            module.callMain(['/root', entry]);
          } catch (error) { finish(1, String(error)); }
        })();
      }
    };
    importScripts('qt-raster-web.js');
  } catch (error) { finish(1, String(error)); }
};
