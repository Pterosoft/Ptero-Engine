// Projects the face, ears and backs of the hands from a reference skin sheet onto Anka's existing
// Body UVs and blends them into the current diffuse.
//
//   node project.js [reference.png] [base_diffuse.png] [out.png]
//
// Each region is an orthographic projection of the mesh (front for the face, side for each ear,
// top for each hand) warped onto the reference with a thin-plate spline through hand-picked
// landmarks. Texels only take the reference where the surface faces the projection, is not
// occluded, and lands on reference skin (not the black background); everything else keeps the
// base diffuse, colour-matched to the reference.
const path = require('path');
const { loadMesh } = require('./mesh.js');
const { readImage, writeImage, sample } = require('./img.js');

const ANKA = path.resolve(__dirname, '../../Data/Characters/Anka');
const [refFile = path.join(ANKA, 'Anka_Reference_Diffuse.png'),
  baseFile = path.join(ANKA, 'anka_wip/anka_wip_Body_diffuse.png'),
  outFile = path.join(ANKA, 'Anka_Body_diffuse_projected.png')] = process.argv.slice(2);
const MESH = path.join(ANKA, 'Anka_Base.fbx');

// ---- Landmarks: mesh 2D (cm, in the projection plane) -> reference pixel ----
// Mesh is Z-up, centimetres, facing -Y; the character's left is +X.
// Face, front view: (x, z).
const FACE = [
  [[-3.24, 165.16], [951.5, 308.5]], [[3.2, 165.16], [1168.5, 308.5]],     // iris centres
  [[-5.64, 165.04], [902, 305]], [[5.76, 165.04], [1218, 305]],           // outer eye corners
  [[-1.76, 164.64], [1001, 318]], [[1.8, 164.64], [1116, 318]],           // inner eye corners
  [[-3.2, 166.7], [954.6, 255.8]], [[3.2, 166.7], [1171.6, 252.7]],       // brows
  [[0, 161.5], [1060, 414]], [[0, 160.04], [1060, 448]],                  // nose tip, columella
  [[-1.8, 160.6], [1007, 439]], [[1.84, 160.6], [1113, 439]],             // nose wings
  [[-2.56, 158.1], [976, 523.6]], [[2.56, 158.1], [1146.8, 523.6]],       // mouth corners
  [[0, 158.9], [1060, 496]], [[0, 157.1], [1060, 569]],                   // lips
  [[0, 153.84], [1060, 662]], [[0, 149], [1060, 790]],                    // chin, neck
  [[-6.6, 165], [765, 305]], [[6.6, 165], [1355, 305]],                   // face silhouette at the eyes
  [[-5.9, 158.1], [803, 524]], [[5.9, 158.1], [1317, 524]],               // jaw silhouette at the mouth
  [[0, 171.45], [1060, 106]], [[-6, 169.5], [850, 190]], [[6, 169.5], [1270, 190]], // hairline
];
// Right ear, viewed from -X: (y, z). The left ear uses the same mesh points (the head is
// symmetric) and the reference mirrored about EAR_MIRROR_X.
const EAR = [
  [[-0.58, 166.57], [670.2, 302.8]],   // helix top
  [[1.265, 164.45], [625.3, 355]],     // helix back
  [[-0.58, 161.57], [713.7, 430.4]],   // lobe bottom
  [[-0.94, 163.82], [681.8, 363.7]],   // concha
  [[-1.435, 163.1], [722.4, 384]],     // tragus
  [[-1.21, 165.53], [696.3, 305.7]],   // front attachment
];
const EAR_MIRROR_X = 2122;
// Left hand, viewed from +Z: (x, y). The right hand uses (-x, y) and the reference mirrored.
const HAND = [
  [[90.4, 0.92], [583.8, 1264.8]], [[91.96, 4.47], [503.2, 1274.1]],     // index, middle tips
  [[91.12, 7.4], [442.4, 1289.6]], [[88.72, 10.64], [329.6, 1323.7]],    // ring, little tips
  [[77.44, 0.08], [583.8, 1562.4]], [[82.48, 3.32], [524.9, 1488]],      // webs: thumb-index, index-middle
  [[82.72, 6.08], [469.1, 1494.2]], [[81.76, 8.6], [407.1, 1522.1]],     // webs: middle-ring, ring-little
  [[69, 2.3], [515.6, 1785.6]], [[69, 8.8], [391.6, 1754.6]],            // wrist sides
];
const HAND_MIRROR_X = 2097;
// The reference thumb is cut off, so the mesh thumb keeps the base texture.
const THUMB = { a: [75.5, -0.8], b: [81.2, -4.3], radius: 1.4, fade: 1.0 };

// ---- Thin-plate spline ----
function fitTps(pairs) {
  const n = pairs.length, N = n + 3, A = [], bx = [], by = [];
  const U = r2 => (r2 > 0 ? r2 * Math.log(r2) : 0);
  for (let i = 0; i < N; i++) A.push(new Float64Array(N));
  for (let i = 0; i < n; i++) {
    const [p] = pairs[i];
    for (let j = 0; j < n; j++) { const q = pairs[j][0]; A[i][j] = U((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2); }
    A[i][i] += 1e-3; // slight smoothing against landmark noise
    A[i][n] = A[n][i] = 1; A[i][n + 1] = A[n + 1][i] = p[0]; A[i][n + 2] = A[n + 2][i] = p[1];
    bx[i] = pairs[i][1][0]; by[i] = pairs[i][1][1];
  }
  for (let i = n; i < N; i++) bx[i] = by[i] = 0;
  const solve = rhs => { // Gaussian elimination with partial pivoting
    const M = A.map((r, i) => [...r, rhs[i]]);
    for (let c = 0; c < N; c++) {
      let piv = c; for (let r = c + 1; r < N; r++) if (Math.abs(M[r][c]) > Math.abs(M[piv][c])) piv = r;
      [M[c], M[piv]] = [M[piv], M[c]];
      for (let r = 0; r < N; r++) if (r !== c) { const f = M[r][c] / M[c][c]; for (let k = c; k <= N; k++) M[r][k] -= f * M[c][k]; }
    }
    return M.map((r, i) => r[N] / r[i]);
  };
  const wx = solve(bx), wy = solve(by);
  return (x, y) => {
    let ox = wx[n] + wx[n + 1] * x + wx[n + 2] * y, oy = wy[n] + wy[n + 1] * x + wy[n + 2] * y;
    for (let i = 0; i < n; i++) {
      const q = pairs[i][0], u = U((x - q[0]) ** 2 + (y - q[1]) ** 2);
      ox += wx[i] * u; oy += wy[i] * u;
    }
    return [ox, oy];
  };
}

const smooth = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];

// ---- Mesh: Body triangles, connected components, smooth normals ----
const { tris: allTris } = loadMesh(MESH);
const tris = allTris.filter(t => t.mat === 'Body');
const parent = new Map(), find = x => { while (parent.get(x) !== x) { parent.set(x, parent.get(parent.get(x))); x = parent.get(x); } return x; };
for (const t of tris) for (const v of t.vi) if (!parent.has(v)) parent.set(v, v);
for (const t of tris) { parent.set(find(t.vi[1]), find(t.vi[0])); parent.set(find(t.vi[2]), find(t.vi[0])); }
const compSize = new Map();
for (const t of tris) { t.comp = find(t.vi[0]); compSize.set(t.comp, (compSize.get(t.comp) || 0) + 1); }
const normals = new Map();
for (const t of tris) {
  const n = cross(sub(t.p[1], t.p[0]), sub(t.p[2], t.p[0]));
  for (const v of t.vi) { const a = normals.get(v) || [0, 0, 0]; a[0] += n[0]; a[1] += n[1]; a[2] += n[2]; normals.set(v, a); }
}
for (const n of normals.values()) { const l = Math.hypot(...n) || 1; n[0] /= l; n[1] /= l; n[2] /= l; }
// Receivers: the head shell and the two arms (largest components). Eyeballs, teeth, tongue and
// lashes have their own texture tiles and are left alone; lashes are too thin to occlude.
const comps = [...compSize.entries()].sort((a, b) => b[1] - a[1]);
const bboxOf = c => { const lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9]; for (const t of tris) if (t.comp === c) for (const p of t.p) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); } return { lo, hi }; };
const big = comps.slice(0, 4).map(([c]) => ({ c, ...bboxOf(c) }));
const headComp = big.find(b => b.hi[2] > 170 && b.lo[0] > -20).c;
const armComps = new Set(big.filter(b => b.hi[0] > 60 || b.lo[0] < -60).map(b => b.c));
const occluders = tris.filter(t => compSize.get(t.comp) >= 300);

// ---- Projections ----
// Depth buffer over the projection plane: max depth (toward the viewer) per cell.
function depthMap(proj, tset, cell) {
  let lo = [1e9, 1e9], hi = [-1e9, -1e9];
  const q = tset.map(t => t.p.map(proj));
  for (const tq of q) for (const v of tq) { lo = [Math.min(lo[0], v[0]), Math.min(lo[1], v[1])]; hi = [Math.max(hi[0], v[0]), Math.max(hi[1], v[1])]; }
  const W = Math.ceil((hi[0] - lo[0]) / cell) + 1, H = Math.ceil((hi[1] - lo[1]) / cell) + 1, z = new Float32Array(W * H).fill(-1e9);
  for (const tq of q) {
    const s = tq.map(v => [(v[0] - lo[0]) / cell, (v[1] - lo[1]) / cell, v[2]]);
    const den = (s[1][1] - s[2][1]) * (s[0][0] - s[2][0]) + (s[2][0] - s[1][0]) * (s[0][1] - s[2][1]);
    if (Math.abs(den) < 1e-12) continue;
    const x0 = Math.max(0, Math.floor(Math.min(s[0][0], s[1][0], s[2][0]))), x1 = Math.min(W - 1, Math.ceil(Math.max(s[0][0], s[1][0], s[2][0])));
    const y0 = Math.max(0, Math.floor(Math.min(s[0][1], s[1][1], s[2][1]))), y1 = Math.min(H - 1, Math.ceil(Math.max(s[0][1], s[1][1], s[2][1])));
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const w0 = ((s[1][1] - s[2][1]) * (x - s[2][0]) + (s[2][0] - s[1][0]) * (y - s[2][1])) / den;
      const w1 = ((s[2][1] - s[0][1]) * (x - s[2][0]) + (s[0][0] - s[2][0]) * (y - s[2][1])) / den, w2 = 1 - w0 - w1;
      if (w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4) continue;
      const d = w0 * s[0][2] + w1 * s[1][2] + w2 * s[2][2], i = y * W + x;
      if (d > z[i]) z[i] = d;
    }
  }
  return v => {
    const x = Math.round((v[0] - lo[0]) / cell), y = Math.round((v[1] - lo[1]) / cell);
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;
    let best = -1e9; // max over a 3x3 neighbourhood so texels on steep slopes are not self-occluded
    for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
      const xx = x + dx, yy = y + dy;
      if (xx >= 0 && yy >= 0 && xx < W && yy < H) best = Math.max(best, z[yy * W + xx]);
    }
    return 1 - smooth(0.12, 0.35, best - v[2]);
  };
}

const tpsFace = fitTps(FACE), tpsEar = fitTps(EAR), tpsHand = fitTps(HAND);
const segDist = (p, a, b) => {
  const ab = [b[0] - a[0], b[1] - a[1]], t = Math.max(0, Math.min(1, ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1]) / (ab[0] ** 2 + ab[1] ** 2)));
  return Math.hypot(p[0] - a[0] - t * ab[0], p[1] - a[1] - t * ab[1]);
};
const head = tris.filter(t => t.comp === headComp), arms = tris.filter(t => armComps.has(t.comp));
// Each projection: receivers, plane projection [u, v, depth], weight from normal/position, and ref lookup.
const projections = [
  {
    name: 'face', receivers: head, proj: p => [p[0], p[2], -p[1]],
    weight: (p, n) => smooth(0.25, 0.6, -n[1]) * smooth(146, 149.5, p[2]) * (1 - smooth(170, 172, p[2])),
    toRef: q => tpsFace(q[0], q[1]),
  },
  ...[-1, 1].map(side => ({
    name: side < 0 ? 'right ear' : 'left ear', receivers: head, proj: p => [p[1], p[2], side * p[0]],
    weight: (p, n) => {
      const e = Math.hypot((p[1] + 0.1) / 2.6, (p[2] - 164.1) / 3.4); // ellipse around the ear
      return smooth(0.15, 0.45, side * n[0]) * (1 - smooth(1, 1.35, e)) * (side * p[0] > 4 ? 1 : 0);
    },
    toRef: q => { const r = tpsEar(q[0], q[1]); return side < 0 ? r : [EAR_MIRROR_X - r[0], r[1]]; },
  })),
  ...[1, -1].map(side => ({
    name: side > 0 ? 'left hand' : 'right hand', receivers: arms.filter(t => side * t.p[0][0] > 0), proj: p => [side * p[0], p[1], p[2]],
    weight: (p, n) => {
      const q = [side * p[0], p[1]], thumb = smooth(THUMB.radius, THUMB.radius + THUMB.fade, segDist(q, THUMB.a, THUMB.b));
      return smooth(0.15, 0.45, n[2]) * smooth(66, 69, q[0]) * thumb;
    },
    toRef: q => { const r = tpsHand(q[0], q[1]); return side > 0 ? r : [HAND_MIRROR_X - r[0], r[1]]; },
  })),
];
for (const pr of projections) {
  const occ = pr.name.endsWith('hand') ? arms : occluders;
  pr.visible = depthMap(pr.proj, occ, 0.03);
}

// ---- Bake in UV space ----
const ref = readImage(refFile), base = readImage(baseFile);
const W = base.w, H = base.h;
const projected = new Float32Array(W * H * 3), weight = new Float32Array(W * H), covered = new Uint8Array(W * H);
const col = [0, 0, 0];
// 0 on or near the black background: the darkest of a few taps, so island edges (where bilinear
// filtering pulls in black) fade out instead of drawing dark outlines.
const tap = [0, 0, 0];
const refSkin = r => {
  let l = 255;
  for (const [dx, dy] of [[0, 0], [6, 0], [-6, 0], [0, 6], [0, -6]]) {
    if (!sample(ref, r[0] + dx, r[1] + dy, tap)) return 0;
    l = Math.min(l, 0.299 * tap[0] + 0.587 * tap[1] + 0.114 * tap[2]);
  }
  return smooth(40, 70, l);
};
for (const pr of projections) {
  for (const t of pr.receivers) {
    const s = t.uv.map(u => [u[0] * W, (1 - u[1]) * H]);
    const den = (s[1][1] - s[2][1]) * (s[0][0] - s[2][0]) + (s[2][0] - s[1][0]) * (s[0][1] - s[2][1]);
    if (Math.abs(den) < 1e-12) continue;
    const ns = t.vi.map(v => normals.get(v));
    const x0 = Math.max(0, Math.floor(Math.min(s[0][0], s[1][0], s[2][0]))), x1 = Math.min(W - 1, Math.ceil(Math.max(s[0][0], s[1][0], s[2][0])));
    const y0 = Math.max(0, Math.floor(Math.min(s[0][1], s[1][1], s[2][1]))), y1 = Math.min(H - 1, Math.ceil(Math.max(s[0][1], s[1][1], s[2][1])));
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const px = x + 0.5, py = y + 0.5;
      const w0 = ((s[1][1] - s[2][1]) * (px - s[2][0]) + (s[2][0] - s[1][0]) * (py - s[2][1])) / den;
      const w1 = ((s[2][1] - s[0][1]) * (px - s[2][0]) + (s[0][0] - s[2][0]) * (py - s[2][1])) / den, w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      const i = y * W + x;
      covered[i] = 1;
      const p = [0, 1, 2].map(k => w0 * t.p[0][k] + w1 * t.p[1][k] + w2 * t.p[2][k]);
      let n = [0, 1, 2].map(k => w0 * ns[0][k] + w1 * ns[1][k] + w2 * ns[2][k]);
      const nl = Math.hypot(...n) || 1; n = n.map(v => v / nl);
      let w = pr.weight(p, n);
      if (w <= 0) continue;
      const q = pr.proj(p);
      w *= pr.visible(q);
      if (w <= 0) continue;
      const r = pr.toRef(q);
      if (!sample(ref, r[0], r[1], col)) continue;
      w *= refSkin(r);
      if (w <= weight[i]) continue; // overlapping projections: strongest wins
      weight[i] = w;
      projected[i * 3] = col[0]; projected[i * 3 + 1] = col[1]; projected[i * 3 + 2] = col[2];
    }
  }
}

// Colour-match the base texture to the reference: per-channel gain from texels the reference fully covers.
const sumRef = [0, 0, 0], sumBase = [0, 0, 0];
for (let i = 0; i < W * H; i++) if (weight[i] > 0.9) for (let c = 0; c < 3; c++) { sumRef[c] += projected[i * 3 + c]; sumBase[c] += base.data[i * 3 + c]; }
const gain = sumRef.map((v, c) => v / sumBase[c]);
// Tiles of the eyeballs, teeth, tongue and lashes keep their original colours.
const keepTile = new Uint8Array(W * H);
for (const t of tris) {
  if (t.comp === headComp || armComps.has(t.comp)) continue;
  const s = t.uv.map(u => [u[0] * W, (1 - u[1]) * H]);
  const x0 = Math.max(0, Math.floor(Math.min(s[0][0], s[1][0], s[2][0])) - 2), x1 = Math.min(W - 1, Math.ceil(Math.max(s[0][0], s[1][0], s[2][0])) + 2);
  const y0 = Math.max(0, Math.floor(Math.min(s[0][1], s[1][1], s[2][1])) - 2), y1 = Math.min(H - 1, Math.ceil(Math.max(s[0][1], s[1][1], s[2][1])) + 2);
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) keepTile[y * W + x] = 1;
}

const out = { w: W, h: H, data: Buffer.alloc(W * H * 3) };
for (let i = 0; i < W * H; i++) for (let c = 0; c < 3; c++) {
  const b = keepTile[i] && !covered[i] ? base.data[i * 3 + c] : base.data[i * 3 + c] * gain[c];
  out.data[i * 3 + c] = Math.max(0, Math.min(255, Math.round(b * (1 - weight[i]) + projected[i * 3 + c] * weight[i])));
}
// Bleed the result 8 texels past UV island borders so mip filtering never reaches the old colours.
let frontier = covered;
for (let pass = 0; pass < 8; pass++) {
  const next = Uint8Array.from(frontier);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x; if (frontier[i]) continue;
    const acc = [0, 0, 0]; let n = 0;
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
      const j = yy * W + xx; if (!frontier[j]) continue;
      for (let c = 0; c < 3; c++) acc[c] += out.data[j * 3 + c]; n++;
    }
    if (n) { for (let c = 0; c < 3; c++) out.data[i * 3 + c] = Math.round(acc[c] / n); next[i] = 1; }
  }
  frontier = next;
}
writeImage(outFile, out);
let n = 0; for (let i = 0; i < W * H; i++) if (weight[i] > 0.5) n++;
console.log(`gain ${gain.map(g => g.toFixed(3)).join(' ')}; ${n} texels take the reference; wrote ${outFile}`);
