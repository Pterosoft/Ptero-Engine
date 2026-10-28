// SVG glyphs for the achievement icons, drawn in a 256 × 256 space over the game's
// wood-and-brass "slot" plate from farkle-atlas.png. Materials (gold, ivory dice, fire...)
// are shared gradients from DEFS, so every icon reads as one set.

let uidCounter = 0;
const uid = prefix => `${prefix}${++uidCounter}`;
const INK = '#26180a';

// ---------------------------------------------------------------- shared definitions
export const CATEGORY_GLOW = {
    rolls: '#ffd36b', risk: '#ff8f3d', farkle: '#e0402c', win: '#ffe48a', career: '#e8a860',
    secret: '#a477ff', light: '#fff2c8', quality: '#86d86c', upscale: '#62b6ff', display: '#4fd6c8', meta: '#ff7ad9',
};

export const DEFS = `
<linearGradient id="gold" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#fff2c4"/><stop offset=".35" stop-color="#ebc46a"/><stop offset=".7" stop-color="#b6862c"/><stop offset="1" stop-color="#6f4a15"/>
</linearGradient>
<!-- For strokes: fixed to the icon's space, since a straight line's bounding box has no height. -->
<linearGradient id="goldStroke" gradientUnits="userSpaceOnUse" x1="0" y1="40" x2="0" y2="220">
  <stop offset="0" stop-color="#fff2c4"/><stop offset=".3" stop-color="#ebc46a"/><stop offset=".75" stop-color="#b6862c"/><stop offset="1" stop-color="#7a5318"/>
</linearGradient>
<linearGradient id="goldEdge"x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#a77a28"/><stop offset="1" stop-color="#5a3a10"/></linearGradient>
<linearGradient id="ivory" x1="0" y1="0" x2="1" y2="1">
  <stop offset="0" stop-color="#fffbef"/><stop offset=".55" stop-color="#f0e3c2"/><stop offset="1" stop-color="#c9b385"/>
</linearGradient>
<radialGradient id="pip" cx=".4" cy=".35" r=".7"><stop offset="0" stop-color="#4a3824"/><stop offset="1" stop-color="#100a04"/></radialGradient>
<linearGradient id="fire" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#fff7c4"/><stop offset=".35" stop-color="#ffc53c"/><stop offset=".75" stop-color="#e8571a"/><stop offset="1" stop-color="#8c1c07"/>
</linearGradient>
<linearGradient id="steel" x1="0" y1="0" x2="1" y2="0">
  <stop offset="0" stop-color="#6e7680"/><stop offset=".45" stop-color="#eef2f6"/><stop offset=".55" stop-color="#b9c1ca"/><stop offset="1" stop-color="#555c64"/>
</linearGradient>
<linearGradient id="stone" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#d9d1bf"/><stop offset="1" stop-color="#7d7465"/></linearGradient>
<linearGradient id="wood" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#4a2a12"/><stop offset=".5" stop-color="#8a5328"/><stop offset="1" stop-color="#3e2310"/></linearGradient>
<linearGradient id="leather" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#9a6634"/><stop offset="1" stop-color="#43260f"/></linearGradient>
<linearGradient id="parchment" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f6e8c4"/><stop offset="1" stop-color="#cdb27a"/></linearGradient>
<linearGradient id="blood" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f05a44"/><stop offset="1" stop-color="#6e0f08"/></linearGradient>
<linearGradient id="night" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#4b5a8a"/><stop offset="1" stop-color="#141a30"/></linearGradient>
<linearGradient id="glass" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#e9f7ff" stop-opacity=".75"/><stop offset="1" stop-color="#6aa6cc" stop-opacity=".35"/></linearGradient>
<linearGradient id="window" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#bfe6ff"/><stop offset="1" stop-color="#2f5f88"/></linearGradient>
<linearGradient id="beam" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff6d0" stop-opacity=".95"/><stop offset="1" stop-color="#ffd76a" stop-opacity="0"/></linearGradient>
<linearGradient id="cloud" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fbf8f0"/><stop offset="1" stop-color="#b8b1a4"/></linearGradient>
<linearGradient id="potato" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#dcb177"/><stop offset="1" stop-color="#7d5428"/></linearGradient>
<filter id="emboss" x="-30%" y="-30%" width="160%" height="160%">
  <feDropShadow dx="0" dy="3" stdDeviation="2.4" flood-color="#000" flood-opacity=".85"/>
</filter>
<!-- A straight line has a zero-area bounding box, so a bbox-sized filter region would hide it. -->
<filter id="embossLine" filterUnits="userSpaceOnUse" x="-64" y="-64" width="384" height="384">
  <feDropShadow dx="0" dy="3" stdDeviation="2.4" flood-color="#000" flood-opacity=".85"/>
</filter>
<filter id="soft"x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="6"/></filter>
<filter id="haze" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="2.5"/></filter>
${Object.entries(CATEGORY_GLOW).map(([name, colour]) => `
<radialGradient id="glow-${name}" cx=".5" cy=".5" r=".5">
  <stop offset="0" stop-color="${colour}" stop-opacity=".55"/><stop offset=".55" stop-color="${colour}" stop-opacity=".18"/><stop offset="1" stop-color="${colour}" stop-opacity="0"/>
</radialGradient>`).join('')}
`;

// ---------------------------------------------------------------- the plate behind every glyph
export function icon(glyph, category, atlasUrl) {
    return `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
<svg width="256" height="256" viewBox="663 783 205 196" preserveAspectRatio="none"><image href="${atlasUrl}" width="1536" height="1024"/></svg>
<circle cx="128" cy="130" r="96" fill="url(#glow-${category})"/>
${glyph.replaceAll('ATLAS', atlasUrl)}
</svg>`;
}

// ---------------------------------------------------------------- primitives
const PIPS = {
    1: [[0, 0]], 2: [[-1, -1], [1, 1]], 3: [[-1, -1], [0, 0], [1, 1]],
    4: [[-1, -1], [1, -1], [-1, 1], [1, 1]], 5: [[-1, -1], [1, -1], [-1, 1], [1, 1], [0, 0]],
    6: [[-1, -1], [-1, 0], [-1, 1], [1, -1], [1, 0], [1, 1]],
};

export function die(cx, cy, s, face, { rot = 0, gold = false, held = false, crack = false, shadow = true } = {}) {
    const h = s / 2, r = s * 0.2, p = s * 0.27, pr = s * 0.095;
    const glow = held ? `<rect x="${-h - 5}" y="${-h - 5}" width="${s + 10}" height="${s + 10}" rx="${r + 4}" fill="none" stroke="#ffe9a0" stroke-width="${s * 0.09}" opacity=".85" filter="url(#haze)"/>` : '';
    const pips = PIPS[face].map(([x, y]) => `<circle cx="${x * p}" cy="${y * p}" r="${pr}" fill="url(#pip)"/>`).join('');
    const cracks = crack ? `<path d="M${-h * .55},${-h} L${-h * .15},${-h * .35} L${-h * .4},${h * .05} L${h * .1},${h * .5} L${h * .05},${h}" fill="none" stroke="#2a1a0a" stroke-width="${s * .045}" stroke-linejoin="round"/>` : '';
    return `<g transform="translate(${cx},${cy}) rotate(${rot})" ${shadow ? 'filter="url(#emboss)"' : ''}>${glow}
<rect x="${-h}" y="${-h}" width="${s}" height="${s}" rx="${r}" fill="url(#${gold ? 'gold' : 'ivory'})" stroke="#4a3416" stroke-width="${s * 0.035}"/>
<rect x="${-h + s * .07}" y="${-h + s * .07}" width="${s * .86}" height="${s * .86}" rx="${r * .8}" fill="none" stroke="#fff" stroke-opacity=".45" stroke-width="${s * .025}"/>
${pips}${cracks}</g>`;
}

export const text = (str, x, y, size, { font = 'Grenze Gotisch', weight = 700, fill = 'url(#gold)', stroke = INK, rot = 0 } = {}) =>
    `<text x="${x}" y="${y}" transform="rotate(${rot} ${x} ${y})" font-family="${font}" font-weight="${weight}" font-size="${size}" text-anchor="middle" dominant-baseline="central" fill="${fill}" stroke="${stroke}" stroke-width="${size * 0.085}" stroke-linejoin="round" paint-order="stroke" filter="url(#emboss)">${str}</text>`;

const solid = (d, fill = 'url(#gold)', sw = 3.5, extra = '') =>
    `<path d="${d}" fill="${fill}" stroke="${INK}" stroke-width="${sw}" stroke-linejoin="round" ${extra}/>`;
const gold = (d, sw = 3.5) => `<g filter="url(#emboss)">${solid(d, 'url(#gold)', sw)}</g>`;
// A thick gold line with a dark rim: the dark pass is drawn first, wider.
const goldLine = (d, w = 12, colour = 'url(#goldStroke)') =>
    `<g filter="url(#embossLine)" fill="none" stroke-linecap="round" stroke-linejoin="round"><path d="${d}" stroke="${INK}" stroke-width="${w + 6}"/><path d="${d}" stroke="${colour}" stroke-width="${w}"/></g>`;
const poly = pts => 'M' + pts.map(p => p.join(',')).join(' L') + ' Z';

function star(cx, cy, outer, inner = outer * 0.45, points = 5, rot = -90) {
    const pts = [];
    for (let i = 0; i < points * 2; i++) {
        const a = (rot + i * 180 / points) * Math.PI / 180, r = i % 2 ? inner : outer;
        pts.push([+(cx + r * Math.cos(a)).toFixed(1), +(cy + r * Math.sin(a)).toFixed(1)]);
    }
    return poly(pts);
}
const sparkle = (cx, cy, r) => `<path d="${star(cx, cy, r, r * 0.22, 4)}" fill="#fffbe6" opacity=".95"/>`;

function flame(cx, by, w, h, inner = true) {
    const d = 'M0,-100 C12,-75 40,-62 43,-35 C46,-14 36,0 20,2 C8,6 -8,6 -20,2 C-36,0 -46,-14 -43,-35 C-41,-52 -27,-58 -22,-78 C-12,-64 -7,-60 -5,-50 C2,-62 5,-80 0,-100Z';
    const t = `translate(${cx},${by}) scale(${w / 90},${h / 100})`;
    return `<g filter="url(#emboss)"><path d="${d}" transform="${t}" fill="url(#fire)" stroke="#4a0f04" stroke-width="3.5" vector-effect="non-scaling-stroke"/>
${inner ? `<path d="${d}" transform="${t} translate(0,-4) scale(.52)" fill="#fff4b8" opacity=".9"/>` : ''}</g>`;
}

const SHIELD = 'M-46,-52 H46 V0 C46,36 22,54 0,64 C-22,54 -46,36 -46,0Z';
function shield(cx, cy, k = 1, field = '#5c1a12') {
    return `<g transform="translate(${cx},${cy}) scale(${k})" filter="url(#emboss)">
<path d="${SHIELD}" fill="url(#gold)" stroke="${INK}" stroke-width="3.5" stroke-linejoin="round"/>
<path d="${SHIELD}" transform="scale(.82) translate(0,-2)" fill="${field}" stroke="${INK}" stroke-width="2.5"/></g>`;
}

function coins(cx, by, n, rx = 34, ry = 10, step = 9) {
    let out = '';
    for (let i = 0; i < n; i++) {
        const y = by - i * step;
        out += `<ellipse cx="${cx}" cy="${y + 4}" rx="${rx}" ry="${ry}" fill="url(#goldEdge)" stroke="${INK}" stroke-width="2.5"/>
<ellipse cx="${cx}" cy="${y}" rx="${rx}" ry="${ry}" fill="url(#gold)" stroke="${INK}" stroke-width="2.5"/>`;
    }
    return `<g filter="url(#emboss)">${out}<ellipse cx="${cx}" cy="${by - (n - 1) * step}" rx="${rx * .6}" ry="${ry * .55}" fill="none" stroke="#7a531a" stroke-width="2"/></g>`;
}

// Two branches rising from the bottom of a circle; angles are screen degrees (90 = down).
function laurel(cx, cy, r, leaves = 8, from = 100, to = 232) {
    let out = '';
    const end = to * Math.PI / 180;
    for (const side of [-1, 1]) {
        const ex = (cx - side * r * Math.cos(end)).toFixed(1), ey = (cy + r * Math.sin(end)).toFixed(1);
        out += `<path d="M${cx},${cy + r} A${r},${r} 0 0,${side < 0 ? 1 : 0} ${side < 0 ? (cx + r * Math.cos(end)).toFixed(1) : ex},${ey}" fill="none" stroke="${INK}" stroke-width="6"/>`
            + `<path d="M${cx},${cy + r} A${r},${r} 0 0,${side < 0 ? 1 : 0} ${side < 0 ? (cx + r * Math.cos(end)).toFixed(1) : ex},${ey}" fill="none" stroke="url(#goldStroke)" stroke-width="3"/>`;
        for (let i = 0; i < leaves; i++) {
            const deg = from + i * (to - from) / (leaves - 1), a = deg * Math.PI / 180;
            const x = side < 0 ? cx + r * Math.cos(a) : cx - r * Math.cos(a), y = cy + r * Math.sin(a);
            const tilt = side < 0 ? deg + 90 + 28 : 180 - (deg + 90 + 28);
            for (const offset of [-1, 1]) {
                const lx = x + offset * r * 0.09 * Math.cos(a) * -side * -1, ly = y + offset * r * 0.09 * Math.sin(a);
                out += `<ellipse cx="${lx.toFixed(1)}" cy="${ly.toFixed(1)}" rx="${r * .17}" ry="${r * .075}" transform="rotate(${(tilt + offset * 38).toFixed(1)} ${lx.toFixed(1)} ${ly.toFixed(1)})" fill="url(#gold)" stroke="${INK}" stroke-width="2"/>`;
            }
        }
    }
    return `<g filter="url(#emboss)">${out}</g>`;
}

const plaque = numeral => `<g filter="url(#emboss)"><rect x="96" y="190" width="64" height="28" rx="6" fill="#21150b" stroke="url(#goldStroke)" stroke-width="3"/></g>${text(numeral, 128, 204, 24, { font: 'IM FELL English SC', weight: 400 })}`;

const crown = (cx, cy, k = 1) => `<g transform="translate(${cx},${cy}) scale(${k})" filter="url(#emboss)">
${solid('M-50,18 L-56,-26 L-28,-2 L0,-42 L28,-2 L56,-26 L50,18 Z')}
${solid('M-52,16 H52 V34 H-52 Z')}
<circle cx="0" cy="25" r="6" fill="#d6332a" stroke="${INK}" stroke-width="2"/><circle cx="-30" cy="25" r="5" fill="#2f7fd6" stroke="${INK}" stroke-width="2"/><circle cx="30" cy="25" r="5" fill="#2f9a4a" stroke="${INK}" stroke-width="2"/>
<circle cx="0" cy="-44" r="6" fill="url(#gold)" stroke="${INK}" stroke-width="2"/><circle cx="-57" cy="-28" r="5" fill="url(#gold)" stroke="${INK}" stroke-width="2"/><circle cx="57" cy="-28" r="5" fill="url(#gold)" stroke="${INK}" stroke-width="2"/></g>`;

function eye(cx, cy, k = 1, iris = '') {
    return `<g transform="translate(${cx},${cy}) scale(${k})" filter="url(#emboss)">
<path d="M-64,0 Q0,-50 64,0 Q0,50 -64,0Z" fill="url(#ivory)" stroke="${INK}" stroke-width="4"/>
<circle r="27" fill="url(#gold)" stroke="${INK}" stroke-width="3"/>${iris || '<circle r="12" fill="url(#pip)"/>'}
<circle cx="-9" cy="-10" r="5" fill="#fff" opacity=".85"/></g>`;
}

function sword(cx, cy, rot, k = 1) {
    return `<g transform="translate(${cx},${cy}) rotate(${rot}) scale(${k})" filter="url(#emboss)">
${solid('M-7,40 L-7,-62 L0,-80 L7,-62 L7,40 Z', 'url(#steel)', 3)}<path d="M0,-66 V36" stroke="#59616a" stroke-width="2"/>
${solid('M-28,38 H28 V48 H-28 Z')}${solid('M-5,48 H5 V72 H-5 Z', '#5a3518', 3)}<circle cy="78" r="8" fill="url(#gold)" stroke="${INK}" stroke-width="3"/></g>`;
}

function hourglass(cx, cy, k, sandTop, sandBottom) {
    const glassPath = 'M-30,-48 H30 C30,-16 7,-8 7,0 C7,8 30,16 30,48 H-30 C-30,16 -7,8 -7,0 C-7,-8 -30,-16 -30,-48Z';
    const clip = uid('hg');
    return `<g transform="translate(${cx},${cy}) scale(${k})" filter="url(#emboss)">
<clipPath id="${clip}"><path d="${glassPath}"/></clipPath>
<path d="${glassPath}" fill="url(#glass)" stroke="${INK}" stroke-width="3"/>
<g clip-path="url(#${clip})" fill="#e7c06a"><rect x="-32" y="${-sandTop}" width="64" height="${sandTop}"/><path d="M-32,48 L-32,${48 - sandBottom * .5} Q0,${48 - sandBottom * 1.2} 32,${48 - sandBottom * .5} L32,48Z"/>
<rect x="-1.5" y="0" width="3" height="${48 - sandBottom * .7}"/></g>
${solid('M-42,-60 H42 V-48 H-42 Z')}${solid('M-42,48 H42 V60 H-42 Z')}
${solid('M-38,-48 H-32 V48 H-38 Z', 'url(#gold)', 2.5)}${solid('M32,-48 H38 V48 H32 Z', 'url(#gold)', 2.5)}</g>`;
}

function speaker(cx, cy) {
    return gold(`M${cx - 48},${cy - 16} H${cx - 26} L${cx + 6},${cy - 44} V${cy + 44} L${cx - 26},${cy + 16} H${cx - 48} Z`);
}

function monitor(cx, cy, w, h, screen = '') {
    return `<g filter="url(#emboss)">${solid(`M${cx - 14},${cy + h / 2} H${cx + 14} L${cx + 22},${cy + h / 2 + 22} H${cx - 22} Z`)}
${solid(`M${cx - 40},${cy + h / 2 + 20} H${cx + 40} V${cy + h / 2 + 30} H${cx - 40} Z`)}
<rect x="${cx - w / 2}" y="${cy - h / 2}" width="${w}" height="${h}" rx="6" fill="url(#gold)" stroke="${INK}" stroke-width="3.5"/>
<rect x="${cx - w / 2 + 9}" y="${cy - h / 2 + 9}" width="${w - 18}" height="${h - 18}" rx="3" fill="url(#night)" stroke="${INK}" stroke-width="2"/></g>${screen}`;
}

function crescent(cx, cy, r, cut = 0.8, dx = 0.42, dy = -0.22) {
    const m = uid('moon');
    return `<mask id="${m}"><rect x="0" y="0" width="256" height="256" fill="#fff"/><circle cx="${cx + r * dx}" cy="${cy + r * dy}" r="${r * cut}" fill="#000"/></mask>
<g filter="url(#emboss)"><circle cx="${cx}" cy="${cy}" r="${r}" fill="url(#gold)" stroke="${INK}" stroke-width="3.5" mask="url(#${m})"/></g>`;
}

function sunRays(cx, cy, r, from = 0, to = 360, count = 12, len = 18) {
    let out = '';
    for (let i = 0; i < count; i++) {
        const a = (from + (to - from) * (i + 0.5) / count) * Math.PI / 180, w = 0.12;
        const p = (rr, aa) => [+(cx + rr * Math.cos(aa)).toFixed(1), +(cy + rr * Math.sin(aa)).toFixed(1)];
        out += solid(poly([p(r + 4, a - w), p(r + 4 + len, a), p(r + 4, a + w)]), 'url(#gold)', 2.5);
    }
    return `<g filter="url(#emboss)">${out}</g>`;
}

function gear(cx, cy, outer, inner, teeth, hole) {
    const pts = [];
    for (let i = 0; i < teeth * 4; i++) {
        const a = (i / (teeth * 4)) * Math.PI * 2, r = (i % 4 < 2) ? outer : inner;
        pts.push([+(cx + r * Math.cos(a)).toFixed(1), +(cy + r * Math.sin(a)).toFixed(1)]);
    }
    const m = uid('gear');
    return `<mask id="${m}"><rect width="256" height="256" fill="#fff"/><circle cx="${cx}" cy="${cy}" r="${hole}" fill="#000"/></mask>
<g filter="url(#emboss)"><path d="${poly(pts)}" fill="url(#gold)" stroke="${INK}" stroke-width="3.5" stroke-linejoin="round" mask="url(#${m})"/>
<circle cx="${cx}" cy="${cy}" r="${hole}" fill="none" stroke="${INK}" stroke-width="3.5"/></g>`;
}

const fleur = (x, y, w, h) => `<g filter="url(#emboss)"><svg x="${x}" y="${y}" width="${w}" height="${h}" viewBox="1198 780 180 208"><image href="ATLAS" width="1536" height="1024"/></svg></g>`;

// ================================================================ rolls
export const straight = () => [1, 2, 3, 4, 5, 6].map((f, i) =>
    die(78 + (i % 3) * 50, 104 + Math.floor(i / 3) * 52, 44, f, { rot: [-8, 4, -4, 6, -6, 8][i] })).join('');

export const threePairs = () => [[2, 82], [4, 128], [6, 174]].map(([f, x], i) =>
    die(x - 4, 104, 40, f, { rot: -6 }) + die(x + 4, 152, 40, f, { rot: 6 })).join('');

export const threeOnes = () => die(128, 92, 58, 1, { rot: -6 }) + die(92, 156, 58, 1, { rot: 8 }) + die(164, 156, 58, 1, { rot: -10 });

export const fourKind = () => [[98, 100, -6], [158, 100, 5], [98, 160, 4], [158, 160, -7]].map(([x, y, r]) => die(x, y, 54, 4, { rot: r })).join('');

export const fiveKind = () => [[86, 88], [170, 88], [86, 170], [170, 170]].map(([x, y]) => die(x, y, 46, 5, { rot: x < 128 ? -8 : 8 })).join('')
    + die(128, 129, 54, 5, { held: true });

export function sixKind() {
    let out = '';
    for (let i = 0; i < 6; i++) {
        const a = (-90 + i * 60) * Math.PI / 180;
        out += die(128 + 60 * Math.cos(a), 130 + 60 * Math.sin(a), 40, 6, { rot: i * 12 - 30 });
    }
    return out + text('×6', 128, 132, 40);
}

export const sixOnes = () => crown(128, 76, 0.8) + die(128, 152, 92, 1, { gold: true, held: true });

// A horseshoe opening upward (circle centre 128,103) with its nail holes, the die in front.
export const luckyFives = () => goldLine('M78,74 A58,58 0 1,0 178,74', 17)
    + [-10, 20, 160, 190].map(d => { const a = d * Math.PI / 180; return `<circle cx="${(128 + 58 * Math.cos(a)).toFixed(1)}" cy="${(103 + 58 * Math.sin(a)).toFixed(1)}" r="3.2" fill="${INK}"/>`; }).join('')
    + die(128, 150, 70, 5, { rot: -8 });

export const hotDice = () => flame(128, 190, 150, 150) + die(86, 170, 42, 6, { rot: -12 }) + die(128, 178, 42, 6, { rot: 4 }) + die(170, 170, 42, 6, { rot: 14 });

export const redHot = () => flame(84, 186, 92, 112, false) + flame(172, 186, 92, 112, false) + flame(128, 196, 130, 150)
    + die(128, 150, 58, 6, { rot: -6 }) + text('×2', 182, 196, 36);

// ================================================================ risk and banking
export const oneDieLeft = () => goldLine('M40,190 H132', 10) + `<path d="M40,200 H134 L120,220 H40 Z" fill="#1a0f06" opacity=".55"/>`
    + die(134, 140, 66, 1, { rot: 45 }) + text('?', 192, 92, 46);

export const bank2000 = () => coins(92, 192, 5) + coins(164, 192, 7) + text('2000', 128, 74, 50);

export const bank3000 = () => coins(70, 204, 2, 22, 7, 7) + coins(190, 204, 3, 22, 7, 7)
    + `<g filter="url(#emboss)">${solid('M128,98 C94,98 70,134 72,166 C74,196 100,208 128,208 C156,208 182,196 184,166 C186,134 162,98 128,98Z', 'url(#leather)', 3.5)}
${solid('M108,100 L98,74 L116,86 L128,66 L140,86 L158,74 L148,100Z', 'url(#leather)', 3)}</g>`
    + goldLine('M104,100 Q128,110 152,100', 7) + text('3000', 128, 160, 38);

export const playedSafe = () => shield(128, 122, 1.25) + die(128, 118, 56, 3, { rot: -6 });

export const fiftyDone = () => die(88, 132, 66, 5, { rot: -10 }) + text('50', 168, 134, 76);

export const soClose = () => text('1000', 128, 82, 42) + `<path d="M80,93 L176,89" stroke="#a3221a" stroke-width="5" stroke-linecap="round" filter="url(#emboss)"/>`
    + die(128, 160, 72, 4, { rot: 14, crack: true });

export function heartbreaker() {
    const zig = 'L134,168 L120,148 L136,128 L118,110 Z';
    return `<g filter="url(#emboss)" stroke="${INK}" stroke-width="3.5" stroke-linejoin="round" fill="url(#blood)">
<path transform="translate(-8,4) rotate(-7 128 140)" d="M128,90 C104,62 68,66 60,92 C52,118 70,150 128,190 ${zig}"/>
<path transform="translate(8,4) rotate(7 128 140)" d="M128,90 C152,62 188,66 196,92 C204,118 186,150 128,190 ${zig}"/></g>
<path d="M76,96 C80,84 92,80 100,84" stroke="#ffd2c4" stroke-width="5" fill="none" stroke-linecap="round" opacity=".7"/>`;
}

// ================================================================ farkles
export const firstFarkle = () => die(128, 98, 68, 2, { rot: -16, crack: true }) + text('Farkle!', 128, 172, 58, { font: 'Pirata One', weight: 400 });

export const cursed = () => `<g filter="url(#emboss)">${solid('M128,58 C90,58 70,86 72,116 C73,136 84,146 92,152 L92,172 L164,172 L164,152 C172,146 183,136 184,116 C186,86 166,58 128,58Z', 'url(#ivory)', 4)}
<ellipse cx="106" cy="116" rx="15" ry="17" fill="url(#pip)"/><ellipse cx="150" cy="116" rx="15" ry="17" fill="url(#pip)"/>
<path d="M128,134 L119,150 H137 Z" fill="url(#pip)"/>
${[108, 120, 132, 144].map(x => `<path d="M${x + 4},156 V172" stroke="${INK}" stroke-width="3"/>`).join('')}</g>`
    + die(92, 202, 26, 1, { rot: -8 }) + die(128, 204, 26, 1) + die(164, 202, 26, 1, { rot: 8 });

export const jitters = () => die(116, 148, 78, 1, { rot: -8 }) + gold('M182,52 L150,108 H170 L156,154 L198,90 H176 L194,52 Z', 3)
    + `<g fill="#9fd6ff" stroke="${INK}" stroke-width="2.5" filter="url(#emboss)"><path d="M70,88 C64,100 64,108 70,108 C76,108 76,100 70,88Z"/><path d="M58,116 C53,126 53,132 58,132 C63,132 63,126 58,116Z"/></g>`;

export const rockBottom = () => text('0', 128, 132, 150) + `<path d="M104,62 L126,100 L112,124 L138,152 L124,198" fill="none" stroke="#1a0f06" stroke-width="6" stroke-linejoin="round"/>`;

// ================================================================ winning
export const firstVictory = () => goldLine('M86,80 C56,80 56,126 94,134', 9) + goldLine('M170,80 C200,80 200,126 162,134', 9)
    + gold('M82,64 H174 V96 C174,140 154,160 128,162 C102,160 82,140 82,96Z') + gold('M120,160 H136 V180 H120 Z')
    + gold('M96,200 H160 L150,178 H106Z') + `<path d="${star(128, 106, 20)}" fill="#7a531a" opacity=".7"/>`;

export function flawless() {
    const f = [
        [[[66, 110], [96, 78], [110, 110]], '#e8fbff'], [[[96, 78], [160, 78], [146, 110], [110, 110]], '#c4ecff'],
        [[[160, 78], [190, 110], [146, 110]], '#92d2f6'], [[[66, 110], [110, 110], [128, 198]], '#64ade0'],
        [[[110, 110], [146, 110], [128, 198]], '#a6e0ff'], [[[146, 110], [190, 110], [128, 198]], '#3a83c0'],
    ];
    return `<g filter="url(#emboss)">${f.map(([pts, c]) => `<path d="${poly(pts)}" fill="${c}" stroke="#0e2a3e" stroke-width="2.5" stroke-linejoin="round"/>`).join('')}</g>`
        + sparkle(184, 70, 16) + sparkle(70, 168, 11) + sparkle(102, 92, 8);
}

export const shutout = () => shield(128, 120, 1.3) + text('0', 128, 116, 84);

export const comeback = () => goldLine('M56,96 C68,182 124,204 170,100', 16)
    + gold('M186,64 L194,112 L150,92 Z', 3) + text('1500', 92, 190, 26);

export function nailBiter() {
    return goldLine('M128,82 V186', 9) + gold('M96,204 H160 L150,184 H106Z') + goldLine('M58,88 H198', 9)
        + `<circle cx="128" cy="80" r="10" fill="url(#gold)" stroke="${INK}" stroke-width="3" filter="url(#emboss)"/>`
        + `<g stroke="${INK}" stroke-width="2.5"><path d="M62,92 L46,142 M62,92 L84,142 M194,92 L172,142 M194,92 L210,142"/></g>`
        + die(65, 128, 24, 3, { rot: -6 }) + die(191, 128, 24, 3, { rot: 6 })
        + gold('M38,142 H92 C88,162 42,162 38,142Z', 3) + gold('M164,142 H218 C214,162 168,162 164,142Z', 3);
}

export function heldLine() {
    const top = 'M78,206 V86 H90 V70 H106 V86 H120 V70 H136 V86 H150 V70 H166 V86 H178 V206 Z';
    return `<g filter="url(#emboss)">${solid(top, 'url(#stone)', 3.5)}
${[112, 140, 168].map(y => `<path d="M80,${y} H176" stroke="#6b6253" stroke-width="2"/>`).join('')}
${solid('M108,206 V168 A20,20 0 0,1 148,168 V206 Z', '#20140a', 3)}${solid('M120,112 h16 v26 h-16 Z', '#20140a', 2.5)}</g>`
        + fleur(108, 54, 40, 46);
}

export const lastDitch = () => hourglass(128, 128, 1.15, 6, 34) + sparkle(178, 84, 12);

export function speedRun() {
    const wing = 'M96,128 C70,98 44,94 30,104 C48,108 54,114 56,120 C42,118 34,122 30,130 C46,130 54,134 58,140 C48,142 44,148 44,156 C62,150 82,148 96,144Z';
    return gold(wing, 3) + `<g transform="translate(256,0) scale(-1,1)">${gold(wing, 3)}</g>` + die(128, 136, 66, 5, { rot: -6 });
}

export const marathon = () => laurel(128, 128, 78) + text('10K', 128, 128, 58);

export const quickDraw = () => sword(128, 124, -40) + sword(128, 124, 40) + text('1000', 128, 202, 26);

export const houseRules = () => `<g filter="url(#emboss)">${solid('M72,82 H180 V182 H72 Z', 'url(#parchment)', 3)}
${solid('M62,70 H190 Q198,78 190,86 H62 Q54,78 62,70 Z', 'url(#parchment)', 3)}${solid('M62,178 H190 Q198,186 190,194 H62 Q54,186 62,178 Z', 'url(#parchment)', 3)}
${[104, 120, 136, 152].map((y, i) => `<path d="M86,${y} H${i === 3 ? 140 : 166}" stroke="#6b4a24" stroke-width="3" stroke-linecap="round"/>`).join('')}</g>`
    + `<g filter="url(#emboss)">${solid('M206,44 C176,58 146,98 128,152 L134,155 C156,110 182,80 210,54Z', 'url(#ivory)', 3)}<path d="M206,46 L122,166" stroke="${INK}" stroke-width="2.5"/></g>`;

// ================================================================ long-term
export function tankard(tier) {
    return goldLine('M164,98 C198,98 198,160 164,160', 11)
        + `<g filter="url(#emboss)">${solid('M82,88 H166 V176 Q166,184 158,184 H90 Q82,184 82,176 Z', 'url(#wood)', 3.5)}
${solid('M80,98 H168 V108 H80 Z', 'url(#gold)', 2.5)}${solid('M80,162 H168 V172 H80 Z', 'url(#gold)', 2.5)}
${[[92, 86, 13], [110, 78, 15], [130, 76, 16], [150, 79, 14], [164, 87, 11]].map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r}" fill="#fff8e6" stroke="${INK}" stroke-width="3"/>`).join('')}
${[[92, 86, 13], [110, 78, 15], [130, 76, 16], [150, 79, 14], [164, 87, 11]].map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r - 1.6}" fill="#fff8e6"/>`).join('')}</g>`
        + plaque(['I', 'II', 'III'][tier - 1]);
}

export const streak = (num, tier) => flame(128, 202, 140 + tier * 8, 150 + tier * 10)
    + text(num, 128, 150, num.length > 1 ? 58 : 66, { fill: '#3a1406', stroke: '#ffe9b0' });

export const banker = () => `<g filter="url(#emboss)">${solid('M66,122 C66,84 190,84 190,122Z', 'url(#wood)', 3.5)}${solid('M66,122 H190 V190 H66 Z', 'url(#wood)', 3.5)}
${solid('M80,88 H92 V190 H80 Z', 'url(#gold)', 2.5)}${solid('M164,88 H176 V190 H164 Z', 'url(#gold)', 2.5)}${solid('M64,118 H192 V128 H64 Z', 'url(#gold)', 2.5)}
${solid('M116,114 H140 V144 H116 Z', 'url(#gold)', 3)}<circle cx="128" cy="126" r="4" fill="${INK}"/><path d="M128,128 V138" stroke="${INK}" stroke-width="4"/></g>`
    + coins(92, 208, 2, 18, 6, 6) + coins(150, 210, 3, 18, 6, 6) + coins(180, 208, 1, 14, 5, 6);

export function collector() {
    let out = '';
    for (let i = 0; i < 6; i++) {
        const a = (-90 + i * 60) * Math.PI / 180;
        out += die(128 + 64 * Math.cos(a), 130 + 64 * Math.sin(a), 36, i + 1, { rot: i * 10 - 25 });
    }
    return out + fleur(102, 100, 52, 60);
}

export const whisperer = () => eye(128, 130, 1.45, [[-1, -1], [1, -1], [-1, 1], [1, 1], [0, 0]].map(([x, y]) => `<circle cx="${x * 9}" cy="${y * 9}" r="4.2" fill="url(#pip)"/>`).join(''));

// ================================================================ hidden and funny
export const couldntResist = () => `<g filter="url(#emboss)" fill="url(#blood)" stroke="${INK}" stroke-width="3" stroke-linejoin="round">
<path d="M98,112 C84,92 86,72 98,60 C98,78 106,92 116,106Z"/><path d="M158,112 C172,92 170,72 158,60 C158,78 150,92 140,106Z"/></g>`
    + goldLine('M160,176 C196,184 204,152 190,140', 6, '#c62a1c') + `<path d="M182,128 L200,134 L188,148Z" fill="#c62a1c" stroke="${INK}" stroke-width="2.5"/>`
    + die(128, 144, 76, 6, { rot: -4 });

export const indecisive = () => goldLine('M78,96 C96,58 160,58 178,96', 9) + gold('M188,84 L178,112 L160,92 Z', 2.5)
    + goldLine('M178,166 C160,204 96,204 78,166', 9) + gold('M68,178 L78,150 L96,170 Z', 2.5)
    + die(96, 132, 50, 3, { rot: -10 }) + die(162, 132, 50, 4, { rot: 10, held: true });

export const nightOwl = () => crescent(112, 128, 64) + `<g filter="url(#emboss)"><path d="${star(178, 82, 16)}" fill="url(#gold)" stroke="${INK}" stroke-width="2.5"/>
<path d="${star(192, 140, 11)}" fill="url(#gold)" stroke="${INK}" stroke-width="2"/><path d="${star(160, 186, 9)}" fill="url(#gold)" stroke="${INK}" stroke-width="2"/></g>`;

// ================================================================ lighting and path tracing
export const followLight = () => `<path d="M112,74 L144,74 L206,212 L50,212 Z" fill="url(#beam)" opacity=".75"/>`
    + `<circle cx="128" cy="66" r="18" fill="url(#gold)" stroke="${INK}" stroke-width="3" filter="url(#emboss)"/>` + sunRays(128, 66, 18, 0, 360, 10, 12)
    + die(128, 162, 64, 1, { held: true });

export function groundTruth() {
    const colours = ['#ff4d3d', '#ff9d2e', '#ffe43a', '#58d24a', '#3d9bff', '#8a5cff'];
    const fan = colours.map((c, i) => `<path d="M150,128 L222,${112 + i * 12} L222,${124 + i * 12} Z" fill="${c}" opacity=".9"/>`).join('');
    return `<path d="M34,152 L110,124" stroke="#fffbe8" stroke-width="7" stroke-linecap="round" filter="url(#haze)"/><path d="M34,152 L110,124" stroke="#fff" stroke-width="4" stroke-linecap="round"/>`
        + fan + `<g filter="url(#emboss)"><path d="M128,66 L182,172 L74,172 Z" fill="url(#glass)" stroke="url(#goldStroke)" stroke-width="6" stroke-linejoin="round"/>
<path d="M128,66 L182,172 L74,172 Z" fill="none" stroke="${INK}" stroke-width="2" stroke-linejoin="round"/><path d="M112,104 L100,150" stroke="#fff" stroke-width="4" opacity=".6" stroke-linecap="round"/></g>`;
}

export const photographer = () => `<g filter="url(#emboss)">${solid('M96,82 H134 V98 H96 Z', 'url(#gold)', 3)}${solid('M62,96 H194 Q200,96 200,102 V178 Q200,184 194,184 H62 Q56,184 56,178 V102 Q56,96 62,96 Z', '#2a1c12', 3.5)}
${solid('M56,110 H200 V118 H56 Z', 'url(#gold)', 2)}${solid('M56,166 H200 V174 H56 Z', 'url(#gold)', 2)}
<circle cx="128" cy="142" r="38" fill="url(#gold)" stroke="${INK}" stroke-width="3.5"/><circle cx="128" cy="142" r="28" fill="url(#night)" stroke="${INK}" stroke-width="3"/>
<circle cx="128" cy="142" r="14" fill="#0c1020"/><circle cx="118" cy="132" r="6" fill="#fff" opacity=".7"/>
<circle cx="178" cy="106" r="6" fill="#ffe9a0" stroke="${INK}" stroke-width="2"/></g>` + sparkle(196, 70, 14);

export function lightDark() {
    const m = uid('half');
    return `<clipPath id="${m}"><rect x="0" y="0" width="128" height="256"/></clipPath>` + sunRays(128, 128, 54, 95, 265, 7, 18)
        + `<g filter="url(#emboss)"><path d="M128,74 A54,54 0 0,0 128,182 Z" fill="url(#gold)" stroke="${INK}" stroke-width="3.5"/>
<path d="M128,74 A54,54 0 0,1 128,182 Z" fill="url(#night)" stroke="${INK}" stroke-width="3.5"/></g>`
        + `<path d="${star(150, 108, 7)}" fill="#fff6d0"/><path d="${star(160, 146, 5)}" fill="#fff6d0"/><path d="${star(142, 164, 4)}" fill="#fff6d0"/>`;
}

export const bounceHouse = () => goldLine('M50,58 V204', 10) + goldLine('M206,58 V204', 10)
    + `<polyline points="58,66 198,100 58,134 198,168 128,196" fill="none" stroke="#ffd76a" stroke-width="10" stroke-linejoin="round" filter="url(#haze)"/>`
    + `<polyline points="58,66 198,100 58,134 198,168 128,196" fill="none" stroke="#fffbe6" stroke-width="4" stroke-linejoin="round"/>`
    + sparkle(198, 100, 12) + sparkle(58, 134, 12) + sparkle(198, 168, 12);

// ================================================================ quality extremes
export const potato = () => `<g filter="url(#emboss)">${solid('M70,120 C66,84 118,70 156,82 C196,94 202,140 184,166 C164,196 104,198 82,176 C70,164 72,140 70,120Z', 'url(#potato)', 3.5)}
${[[104, 108], [150, 104], [168, 146], [116, 160], [138, 132]].map(([x, y]) => `<ellipse cx="${x}" cy="${y}" rx="5" ry="3.5" fill="#5a3a18"/>`).join('')}
<path d="M150,82 C152,68 160,60 170,58 C168,68 162,76 154,84Z" fill="#6fb84a" stroke="${INK}" stroke-width="2.5"/></g>` + text('LOW', 128, 214, 22, { font: 'IM FELL English SC', weight: 400 });

export const maxedOut = () => crown(128, 106, 1.3) + `<g filter="url(#emboss)"><rect x="70" y="170" width="116" height="30" rx="6" fill="#21150b" stroke="url(#goldStroke)" stroke-width="3"/></g>`
    + text('ULTRA', 128, 186, 24, { font: 'IM FELL English SC', weight: 400 });

export const purist = () => goldLine('M156,156 L200,200', 16) + `<g filter="url(#emboss)"><circle cx="118" cy="118" r="52" fill="url(#glass)"/></g>`
    + die(118, 118, 52, 1, { shadow: false }) + `<circle cx="118" cy="118" r="52" fill="none" stroke="${INK}" stroke-width="16"/><circle cx="118" cy="118" r="52" fill="none" stroke="url(#goldStroke)" stroke-width="10"/>`
    + `<path d="M86,96 A38,38 0 0,1 104,78" stroke="#fff" stroke-width="5" fill="none" opacity=".7" stroke-linecap="round"/>`;

export const fullSpectrum = () => [['L', 30], ['M', 58], ['H', 86], ['U', 114]].map(([label, h], i) => {
    const x = 62 + i * 36;
    return gold(`M${x},${184 - h} H${x + 26} V184 H${x} Z`, 3) + text(label, x + 13, 202, 20, { font: 'IM FELL English SC', weight: 400 });
}).join('');

export const lightsOut = () => `<g fill="none" stroke="#c9c2b4" stroke-linecap="round" opacity=".85"><path d="M128,88 C112,74 140,62 124,46" stroke-width="5"/><path d="M134,94 C150,80 136,68 152,56" stroke-width="3.5"/></g>`
    + `<g filter="url(#emboss)">${solid('M108,104 H148 V182 H108 Z', 'url(#ivory)', 3.5)}${solid('M108,104 C116,112 128,100 136,108 C140,112 146,106 148,104', 'none', 3)}
<path d="M128,104 V92" stroke="${INK}" stroke-width="4" stroke-linecap="round"/>${solid('M76,182 H180 C176,198 80,198 76,182Z')}</g>`;

export function clearSkies() {
    const puffs = [[96, 158, 26], [124, 144, 32], [154, 156, 26], [76, 172, 18], [174, 172, 18], [126, 174, 22]];
    return `<circle cx="164" cy="98" r="34" fill="url(#gold)" stroke="${INK}" stroke-width="3.5" filter="url(#emboss)"/>` + sunRays(164, 98, 34, 0, 360, 12, 16)
        + `<g filter="url(#emboss)">${puffs.map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r}" fill="none" stroke="${INK}" stroke-width="7"/>`).join('')}
${puffs.map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r}" fill="url(#cloud)"/>`).join('')}</g>`;
}

export const contactShadows = () => goldLine('M48,186 H208', 6) + `<ellipse cx="130" cy="184" rx="78" ry="10" fill="#000" opacity=".8" filter="url(#soft)"/>`
    + die(100, 148, 64, 2, { rot: -4 }) + die(160, 150, 62, 5, { rot: 8 }) + `<path d="M130,120 L130,182" stroke="#000" stroke-width="10" opacity=".45" filter="url(#haze)"/>`;

// ================================================================ upscalers and anti-aliasing
export const alphabetSoup = () => `<g fill="none" stroke="#e8e2d4" stroke-width="4" stroke-linecap="round" opacity=".7"><path d="M104,96 C96,84 112,76 104,62"/><path d="M152,96 C144,84 160,76 152,62"/></g>`
    + gold('M56,130 H200 C200,172 170,198 128,198 C86,198 56,172 56,130Z')
    + `<ellipse cx="128" cy="130" rx="72" ry="17" fill="#b5602a" stroke="${INK}" stroke-width="3.5"/>`
    + [['D', 94, 128, -15], ['F', 118, 134, 10], ['X', 142, 126, -8], ['A', 166, 132, 14]].map(([c, x, y, r]) => text(c, x, y, 20, { font: 'EB Garamond', fill: '#fff4d6', stroke: '#5a2a10', rot: r })).join('');

export const inHouse = () => `<g filter="url(#emboss)">${solid('M70,120 H186 V196 H70 Z', 'url(#parchment)', 3.5)}
${solid('M56,124 L128,62 L200,124 Z', '#6b2a1a', 3.5)}<path d="M70,150 H186 M98,120 V196 M158,120 V196" stroke="#3b2412" stroke-width="5"/>
${solid('M114,196 V164 A14,14 0 0,1 142,164 V196 Z', '#3b2412', 3)}</g>`
    + `<g filter="url(#emboss)"><path d="M162,82 V96 M206,82 V96" stroke="${INK}" stroke-width="3"/>${solid('M152,96 H216 V122 H152 Z', '#21150b', 3)}</g>` + text('AAR', 184, 109, 19, { font: 'IM FELL English SC', weight: 400 });

export const sharpEye = () => eye(128, 132, 1.45) + sparkle(152, 112, 20);

export const edgeLord = () => sword(128, 122, 30, 1.25) + text('AA', 184, 190, 34, { font: 'IM FELL English SC', weight: 400 }) + sparkle(160, 64, 14);

export function ultraPerformance() {
    // A 10 × 10 pixel die: corners cut for the rounding, 2 × 2 pips with a gap between them.
    const n = 10, cell = 11, ox = 75, oy = 75, pipCells = [[1, 1], [7, 1], [4, 4], [1, 7], [7, 7]];
    let out = '';
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
        if ((x === 0 || x === n - 1) && (y === 0 || y === n - 1)) continue;
        const pip = pipCells.some(([px, py]) => (x === px || x === px + 1) && (y === py || y === py + 1));
        out += `<rect x="${ox + x * cell}" y="${oy + y * cell}" width="${cell}" height="${cell}" fill="${pip ? '#1a120a' : ((x + y) % 2 ? '#efe2c0' : '#f8eed6')}"/>`;
    }
    return `<g filter="url(#emboss)">${out}</g>`
        + `<g stroke="url(#goldStroke)" stroke-width="5" stroke-linecap="round"><path d="M40,100 H62 M34,130 H62 M40,160 H62"/></g>`;
}

export function twiceFrames() {
    const frame = (x, y, o) => `<g opacity="${o}" filter="url(#emboss)"><rect x="${x}" y="${y}" width="96" height="100" rx="4" fill="#1d140c" stroke="url(#goldStroke)" stroke-width="3"/>
${[0, 1, 2, 3, 4].map(i => `<rect x="${x + 6}" y="${y + 8 + i * 18}" width="8" height="10" rx="2" fill="#e8d9b4"/><rect x="${x + 82}" y="${y + 8 + i * 18}" width="8" height="10" rx="2" fill="#e8d9b4"/>`).join('')}
<rect x="${x + 20}" y="${y + 12}" width="56" height="76" fill="#3a4c6e"/></g>`;
    return frame(54, 64, 0.55) + die(102, 114, 36, 3, { shadow: false }) + frame(106, 96, 1) + die(154, 146, 40, 3);
}

// ================================================================ display, image and sound
export function windowShopper() {
    const win = (x, y, w, h) => { const r = w / 2; return `<g filter="url(#emboss)">${solid(`M${x},${y + h} V${y + r} A${r},${r} 0 0,1 ${x + w},${y + r} V${y + h} Z`, 'url(#window)', 3.5)}
<path d="M${x + r},${y + 4} V${y + h} M${x},${y + r + (h - r) * .45} H${x + w}" stroke="url(#goldStroke)" stroke-width="4"/>
<path d="M${x},${y + h} V${y + r} A${r},${r} 0 0,1 ${x + w},${y + r} V${y + h} Z" fill="none" stroke="url(#goldStroke)" stroke-width="4"/></g>`; };
    return win(48, 104, 44, 84) + win(164, 104, 44, 84) + win(98, 70, 60, 122);
}

export const bigScreen = () => monitor(128, 106, 156, 104, die(128, 96, 42, 6) + text('1440p', 128, 136, 18, { font: 'IM FELL English SC', weight: 400 }));

export const throughLens = () => `<g filter="url(#emboss)"><circle cx="128" cy="128" r="66" fill="url(#gold)" stroke="${INK}" stroke-width="3.5"/>
<circle cx="128" cy="128" r="52" fill="#0c1020" stroke="${INK}" stroke-width="3"/></g>
<g fill="none" stroke-width="6" style="mix-blend-mode:screen"><circle cx="119" cy="130" r="28" stroke="#ff3b30"/><circle cx="128" cy="119" r="28" stroke="#34d058"/><circle cx="137" cy="132" r="28" stroke="#2f7dff"/></g>
<path d="M98,100 A40,40 0 0,1 120,88" stroke="#fff" stroke-width="5" fill="none" opacity=".6" stroke-linecap="round"/>`;

export const numbersPerson = () => `<g filter="url(#emboss)">${solid('M54,66 H202 V190 H54 Z', '#21150b', 3.5)}<rect x="54" y="66" width="148" height="124" fill="none" stroke="url(#goldStroke)" stroke-width="4"/></g>
<path d="M70,82 V176 H190" fill="none" stroke="#8a6a3a" stroke-width="3"/>
<polyline points="74,150 96,136 116,146 136,112 156,122 176,94 188,100" fill="none" stroke="#ffd76a" stroke-width="5" stroke-linejoin="round" stroke-linecap="round"/>`
    + text('FPS', 128, 214, 22, { font: 'IM FELL English SC', weight: 400 });

export function benchmark() {
    let ticks = '';
    for (let i = 0; i <= 8; i++) {
        const a = Math.PI + i * Math.PI / 8;
        ticks += `<path d="M${(128 + 58 * Math.cos(a)).toFixed(1)},${(146 + 58 * Math.sin(a)).toFixed(1)} L${(128 + 70 * Math.cos(a)).toFixed(1)},${(146 + 70 * Math.sin(a)).toFixed(1)}" stroke="${INK}" stroke-width="4"/>`;
    }
    const a = -Math.PI / 5;
    return goldLine('M50,146 A78,78 0 0,1 206,146', 12) + ticks
        + goldLine(`M128,146 L${(128 + 62 * Math.cos(a)).toFixed(1)},${(146 + 62 * Math.sin(a)).toFixed(1)}`, 7, '#d6332a')
        + `<circle cx="128" cy="146" r="11" fill="url(#gold)" stroke="${INK}" stroke-width="3"/>` + text('60', 128, 190, 44);
}

export const silent = () => speaker(118, 128) + `<g stroke-linecap="round" filter="url(#emboss)"><path d="M150,104 L196,152 M196,104 L150,152" stroke="${INK}" stroke-width="16"/><path d="M150,104 L196,152 M196,104 L150,152" stroke="#d6332a" stroke-width="10"/></g>`;

export const justMusic = () => goldLine('M110,176 V76 L180,60 V160', 8) + gold('M106,76 L184,58 L184,78 L106,96 Z', 3)
    + `<g filter="url(#emboss)"><ellipse cx="96" cy="178" rx="20" ry="15" transform="rotate(-20 96 178)" fill="url(#gold)" stroke="${INK}" stroke-width="3.5"/>
<ellipse cx="166" cy="162" rx="20" ry="15" transform="rotate(-20 166 162)" fill="url(#gold)" stroke="${INK}" stroke-width="3.5"/></g>`;

// ================================================================ meta
export const tinkerer = () => gear(108, 140, 58, 46, 10, 20)
    + `<g transform="translate(150,112) rotate(45)" filter="url(#emboss)">${solid('M-8,-72.6 A24,24 0 1,0 8,-72.6 L8,-56 L-8,-56 Z', 'url(#steel)', 3)}${solid('M-8,-30 H8 V66 Q8,74 0,74 Q-8,74 -8,66 Z', 'url(#steel)', 3)}</g>`;

export const backToBasics = () => goldLine('M159,78.3 A62,62 0 1,1 97,78.3', 13) + gold('M122,62 L104,96 L86,62 Z', 3) + die(128, 134, 54, 1);

export const connoisseur = () => `<g filter="url(#emboss)">${solid('M128,64 C74,64 46,104 54,144 C60,176 96,196 128,192 C150,190 148,170 162,166 C182,160 206,164 208,134 C210,94 180,64 128,64Z', 'url(#wood)', 3.5)}
<circle cx="96" cy="160" r="12" fill="#1a0f06"/>
${[['#d6332a', 86, 106], ['#ffd23a', 120, 88], ['#3d9bff', 158, 94], ['#58d24a', 182, 124], ['#f4f0e6', 120, 132]].map(([c, x, y]) => `<circle cx="${x}" cy="${y}" r="12" fill="${c}" stroke="${INK}" stroke-width="2.5"/>`).join('')}</g>`
    + `<g transform="translate(164,150) rotate(35)" filter="url(#emboss)">${solid('M-5,-8 H5 V60 H-5 Z', 'url(#wood)', 2.5)}${solid('M-6,-18 H6 V-6 H-6 Z', 'url(#steel)', 2)}${solid('M-6,-18 C-6,-34 0,-44 0,-50 C0,-44 6,-34 6,-18 Z', '#d6332a', 2)}</g>`;

export const completionist = () => laurel(128, 130, 80) + die(128, 128, 70, 6, { gold: true, held: true });

// The lion shield from the title sign, cut to its outline so none of the atlas around it shows.
export function farkleMaster() {
    const clip = uid('lion');
    return `<clipPath id="${clip}"><path d="${SHIELD}" transform="translate(128,141) scale(1.1,1.04)"/></clipPath>
<g filter="url(#emboss)"><g clip-path="url(#${clip})"><svg x="74" y="84" width="108" height="128" viewBox="962 28 146 174"><image href="ATLAS" width="1536" height="1024"/></svg></g></g>`
        + crown(128, 68, 0.72);
}
