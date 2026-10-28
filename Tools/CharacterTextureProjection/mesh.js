// Loads Anka_Base.fbx into triangles: per-corner position (model space) and UV, plus material name per triangle.
const { parseFbx, child } = require('./fbx.js');
function loadMesh(file) {
  const { nodes } = parseFbx(file);
  const objs = nodes.find(n => n.name === 'Objects');
  const geo = objs.children.find(n => n.name === 'Geometry');
  const model = objs.children.find(n => n.name === 'Model');
  const V = child(geo, 'Vertices').props[0];
  const PVI = child(geo, 'PolygonVertexIndex').props[0];
  const luv = child(geo, 'LayerElementUV');
  const UV = child(luv, 'UV').props[0], UVI = child(luv, 'UVIndex').props[0];
  const MAT = child(child(geo, 'LayerElementMaterial'), 'Materials').props[0];
  // Material order = order of OO connections Material -> Model.
  const mats = new Map(objs.children.filter(n => n.name === 'Material').map(n => [n.props[0], n.props[1].split('\0')[0]]));
  const conns = nodes.find(n => n.name === 'Connections').children;
  const matOrder = conns.filter(c => c.props[0] === 'OO' && c.props[2] === model.props[0] && mats.has(c.props[1])).map(c => mats.get(c.props[1]));
  const tris = []; // {p:[[x,y,z]x3], uv:[[u,v]x3], mat}
  let poly = [], pi = 0;
  for (let k = 0; k < PVI.length; k++) {
    let vi = PVI[k]; const end = vi < 0; if (end) vi = ~vi;
    poly.push({ p: [V[vi * 3], V[vi * 3 + 1], V[vi * 3 + 2]], uv: [UV[UVI[k] * 2], UV[UVI[k] * 2 + 1]], vi });
    if (end) {
      const mat = matOrder[MAT.length === 1 ? MAT[0] : MAT[pi]];
      for (let j = 1; j + 1 < poly.length; j++) {
        const c = [poly[0], poly[j], poly[j + 1]];
        tris.push({ p: c.map(x => x.p), uv: c.map(x => x.uv), vi: c.map(x => x.vi), mat });
      }
      poly = []; pi++;
    }
  }
  return { tris, matOrder };
}
module.exports = { loadMesh };
if (require.main === module) {
  const { tris, matOrder } = loadMesh(process.argv[2]);
  console.log('materials', matOrder, 'tris', tris.length);
  const cnt = {}; const bb = {};
  for (const t of tris) {
    cnt[t.mat] = (cnt[t.mat] || 0) + 1;
    const b = bb[t.mat] || (bb[t.mat] = { lo: [1e9, 1e9, 1e9], hi: [-1e9, -1e9, -1e9], uvlo: [1e9, 1e9], uvhi: [-1e9, -1e9] });
    for (const p of t.p) for (let a = 0; a < 3; a++) { b.lo[a] = Math.min(b.lo[a], p[a]); b.hi[a] = Math.max(b.hi[a], p[a]); }
    for (const u of t.uv) for (let a = 0; a < 2; a++) { b.uvlo[a] = Math.min(b.uvlo[a], u[a]); b.uvhi[a] = Math.max(b.uvhi[a], u[a]); }
  }
  console.log(cnt); console.log(JSON.stringify(bb, null, 0));
}
