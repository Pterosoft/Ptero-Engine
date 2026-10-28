// Minimal binary FBX reader: dumps geometry/model/material structure.
const fs = require('fs'), zlib = require('zlib');
function parseFbx(file) {
  const b = fs.readFileSync(file);
  const ver = b.readUInt32LE(23);
  const big = ver >= 7500;
  let p = 27;
  function readProp() {
    const t = String.fromCharCode(b[p++]);
    switch (t) {
      case 'Y': { const v = b.readInt16LE(p); p += 2; return v; }
      case 'C': return !!b[p++];
      case 'I': { const v = b.readInt32LE(p); p += 4; return v; }
      case 'F': { const v = b.readFloatLE(p); p += 4; return v; }
      case 'D': { const v = b.readDoubleLE(p); p += 8; return v; }
      case 'L': { const v = Number(b.readBigInt64LE(p)); p += 8; return v; }
      case 'S': case 'R': { const n = b.readUInt32LE(p); p += 4; const s = b.subarray(p, p + n); p += n; return t === 'S' ? s.toString('latin1') : s; }
      case 'f': case 'd': case 'l': case 'i': case 'b': {
        const n = b.readUInt32LE(p), enc = b.readUInt32LE(p + 4), clen = b.readUInt32LE(p + 8); p += 12;
        let raw = b.subarray(p, p + clen); p += clen;
        if (enc === 1) raw = zlib.inflateSync(raw);
        const out = new Array(n);
        const sz = { f: 4, d: 8, l: 8, i: 4, b: 1 }[t];
        for (let k = 0; k < n; k++) {
          const o = k * sz;
          out[k] = t === 'f' ? raw.readFloatLE(o) : t === 'd' ? raw.readDoubleLE(o) : t === 'l' ? Number(raw.readBigInt64LE(o)) : t === 'i' ? raw.readInt32LE(o) : raw[o];
        }
        return out;
      }
      default: throw new Error('bad prop ' + t + ' at ' + p);
    }
  }
  function readNode() {
    const end = big ? Number(b.readBigUInt64LE(p)) : b.readUInt32LE(p);
    const np = big ? Number(b.readBigUInt64LE(p + 8)) : b.readUInt32LE(p + 4);
    p += big ? 24 : 12;
    const nl = b[p++];
    if (end === 0) return null;
    const name = b.subarray(p, p + nl).toString('latin1'); p += nl;
    const props = []; for (let i = 0; i < np; i++) props.push(readProp());
    const children = [];
    while (p < end) { const c = readNode(); if (!c) break; children.push(c); }
    p = end;
    return { name, props, children };
  }
  const nodes = [];
  while (p < b.length - 200) { const n = readNode(); if (!n) break; nodes.push(n); }
  return { ver, nodes };
}
module.exports = { parseFbx, child: (n, name) => n.children.find(c => c.name === name), kids: (n, name) => n.children.filter(c => c.name === name) };
if (require.main === module) {
  const { ver, nodes } = parseFbx(process.argv[2]);
  console.log('version', ver);
  const objs = nodes.find(n => n.name === 'Objects');
  for (const o of objs.children) {
    const line = [o.name, o.props[0], JSON.stringify(o.props[1]), o.props[2]];
    if (o.name === 'Geometry') {
      for (const c of o.children) {
        if (Array.isArray(c.props[0])) line.push(c.name + ':' + c.props[0].length);
        if (c.name.startsWith('Layer')) {
          const m = c.children.find(x => x.name === 'MappingInformationType'), r = c.children.find(x => x.name === 'ReferenceInformationType');
          line.push(c.name + '[' + (m && m.props[0]) + '/' + (r && r.props[0]) + ' ' + c.children.filter(x => Array.isArray(x.props[0])).map(x => x.name + ':' + x.props[0].length).join(',') + ']');
        }
      }
    }
    if (o.name === 'Model' || o.name === 'Material' || o.name === 'Geometry' || o.name === 'Texture' || o.name === 'Video') console.log(line.join(' | '));
  }
  const conns = nodes.find(n => n.name === 'Connections');
  console.log('connections', conns.children.length);
}
