// Writes build/sheet.html: every icon twice (achieved, then the greyed unachieved
// version), in a grid of 256 px tiles, plus build/layout.json for the cropper.
// Rendered by render.ps1 with headless Edge.
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { achievements } from './achievements.mjs';
import { DEFS, icon } from './icon-lib.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '../../..');
const build = join(here, 'build');
mkdirSync(build, { recursive: true });

const fileUrl = p => pathToFileURL(join(repo, p)).href;
const atlas = fileUrl('Data/UI/Farkle/Textures/farkle-atlas.png');
const fonts = {
    'Grenze Gotisch': 'Data/UI/Farkle/Fonts/Grenze_Gotisch/static/GrenzeGotisch-Bold.ttf',
    'Pirata One': 'Data/UI/Farkle/Fonts/Pirata_One/PirataOne-Regular.ttf',
    'IM FELL English SC': 'Data/UI/Farkle/Fonts/IM_Fell_English_SC/IMFellEnglishSC-Regular.ttf',
    'EB Garamond': 'Data/UI/Farkle/Fonts/EB_Garamond/static/EBGaramond-Bold.ttf',
};

export const COLUMNS = 10, TILE = 256;
const rows = Math.ceil(achievements.length / COLUMNS);
const tiles = [], layout = [];
for (const [variant, offset] of [['achieved', 0], ['unachieved', rows]]) {
    achievements.forEach((a, i) => {
        const x = (i % COLUMNS) * TILE, y = (Math.floor(i / COLUMNS) + offset) * TILE;
        tiles.push(`<div class="tile ${variant}" style="left:${x}px;top:${y}px">${icon(a.icon(), a.cat, atlas)}</div>`);
        layout.push({ file: `${variant}/${a.api}.png`, x, y });
    });
}

const width = COLUMNS * TILE, height = rows * 2 * TILE;
writeFileSync(join(build, 'sheet.html'), `<!doctype html><html><head><meta charset="utf-8"><style>
${Object.entries(fonts).map(([family, path]) => `@font-face { font-family: "${family}"; src: url("${fileUrl(path)}"); }`).join('\n')}
html, body { margin: 0; width: ${width}px; height: ${height}px; background: #000; overflow: hidden; }
.tile { position: absolute; width: ${TILE}px; height: ${TILE}px; }
.tile svg { display: block; }
/* Steam shows the unachieved icon for locked achievements: same art, greyed and dimmed. */
.unachieved { filter: grayscale(1) brightness(.52) contrast(1.08); }
</style></head><body>
<svg width="0" height="0" style="position:absolute"><defs>${DEFS}</defs></svg>
${tiles.join('\n')}
</body></html>`);
writeFileSync(join(build, 'layout.json'), JSON.stringify({ width, height, tile: TILE, tiles: layout }, null, 1));
console.log(`sheet ${width}x${height}, ${achievements.length} achievements`);
