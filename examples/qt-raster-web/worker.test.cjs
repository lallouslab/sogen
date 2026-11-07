const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
function runtime() {
  const messages = [];
  const context = vm.createContext({Uint8Array, DataView, BigInt, URL, TextDecoder, console,
    self: {location: new URL('http://localhost/nested/calculator/worker.js')},
    postMessage: (message, transfers) => messages.push(structuredClone(message, {transfer: transfers})),
    importScripts: file => vm.runInContext(fs.readFileSync(path.join(__dirname, file), 'utf8'), context)});
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'worker.js'), 'utf8'), context);
  return {context, messages};
}
function emit(r, op, id, args = [], identity = 1n, seq = 0) {
  const bytes = r.context.RasterWire.packet(op, id, args, identity, op === 6 ? new Uint8Array(16) : undefined);
  new DataView(bytes.buffer).setUint32(16, seq, true); r.context.sogenRasterSend(bytes);
}
function begin(r) { emit(r, 1, 0); emit(r, 2, 5, [0,0,2,2,1,0,0,1000]); }
(async () => {
  {
    const r = runtime(); begin(r); emit(r, 6, 5, [2,2,8,1000,0,0,2,2], 1n, 10);
    assert.equal(r.context.sogenRasterReady(1,0,5), false);
    await r.context.onmessage({data:{type:'ack',key:'1:5',sequence:9}});
    assert.equal(r.context.sogenRasterReady(1,0,5), false);
    await r.context.onmessage({data:{type:'ack',key:'1:5',sequence:10}});
    assert.equal(r.context.sogenRasterReady(1,0,5), true);
    emit(r, 6, 5, [2,2,8,1000,0,0,2,2], 1n, 11);
    await r.context.onmessage({data:{type:'ack',key:'1:5',sequence:10}});
    assert.equal(r.context.sogenRasterReady(1,0,5), false);
  }
  {
    const r=runtime();begin(r);emit(r,2,1,[0,0,2,2,3,1,5,1000]);
    r.context.sogenRasterInput=[];
    await r.context.onmessage({data:{type:'stop'}});
    assert.deepEqual(Array.from(r.context.sogenRasterInput,b=>r.context.RasterWire.view(b).getUint32(12,true)),[1,5]);
    emit(r,5,1);assert.deepEqual(Array.from(r.context.sogenRasterInput,b=>r.context.RasterWire.view(b).getUint32(12,true)),[5]);
    emit(r,5,5);emit(r,10,0);assert.equal(r.context.sogenRasterInput.length,0);
    emit(r,1,0,[],2n);emit(r,2,5,[0,0,2,2,1,0,0,1000],2n);emit(r,6,5,[2,2,8,1000,0,0,2,2],2n,11);
    await r.context.onmessage({data:{type:'ack',key:'1:5',sequence:11}});
    assert.equal(r.context.sogenRasterReady(2,0,5),false);
  }
  {
    const r=runtime();begin(r);r.context.sogenRasterInput=[];
    const release=r.context.RasterWire.packet(21,5,[7,65],1n);
    for(let i=0;i<2048;++i)await r.context.onmessage({data:{type:'input',bytes:release.slice()}});
    assert.equal(r.context.sogenRasterInput.length,2048);
    await r.context.onmessage({data:{type:'input',bytes:release.slice()}});
    const done=r.messages.find(m=>m.type==='done');assert.match(done.error,/queue exhausted/);assert.equal(r.context.sogenRasterInput.length,0);
  }
  {
    const r=runtime();const bytes=r.context.RasterWire.packet(21,1,[6,65],1n);
    bytes[4]=1;assert.throws(()=>r.context.RasterWire.view(bytes),/header/);
    assert.throws(()=>r.context.RasterWire.packet(21,1,[],1n,new Uint8Array(4033)),/budget/);
  }
  console.log('Worker checks passed: frame credits, stale acknowledgments, destruction, close order, bounded input and wire limits');
})().catch(error=>{console.error(error);process.exitCode=1;});
