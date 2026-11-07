(() => {
  const header = 64, maxPayload = 64 * 1024 * 1024;
  function view(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < header || bytes.length > header + maxPayload)
      throw Error('Invalid raster packet length');
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (v.getUint32(0, true) !== 0x51425231 || v.getUint32(4, true) !== 2 || v.getUint32(20, true) !== bytes.length - header)
      throw Error('Invalid raster packet header');
    return v;
  }
  function packet(op, id, args, session, payload = new Uint8Array()) {
    if (args.length > 8 || payload.length > 4032) throw Error('Input packet budget exceeded');
    const bytes = new Uint8Array(header + payload.length), v = new DataView(bytes.buffer);
    [0x51425231, 2, op, id, 0, payload.length].forEach((x, i) => v.setUint32(i * 4, x, true));
    args.forEach((x, i) => v.setInt32(24 + i * 4, x, true));
    v.setBigUint64(56, BigInt(session), true); bytes.set(payload, header); return bytes;
  }
  function key(v) { return v.getBigUint64(56, true).toString() + ':' + v.getUint32(12, true); }
  globalThis.RasterWire = Object.freeze({view, packet, key, header, maxPayload});
})();
