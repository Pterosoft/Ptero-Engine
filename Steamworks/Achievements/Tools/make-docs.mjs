// Writes ../Achievements.md (what to type into Steamworks) and ../Achievements.csv.
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { achievements, stats } from './achievements.mjs';

const out = join(dirname(fileURLToPath(import.meta.url)), '..');
const cell = s => String(s).replaceAll('|', '\\|');

const md = [`# Farkle — Steamworks achievements

Generated from \`Tools/achievements.mjs\`; run \`Tools/render.ps1\` after editing it.

## 1. Stats (Stats & Achievements → Stats tab — create these first)

| API Name | Display Name | Set By | Type | Increment Only | Min | Max | Default | Notes |
|---|---|---|---|---|---|---|---|---|
${stats.map(s => `| \`${s.api}\` | ${s.name} | Client | ${s.type} | ${s.incrementOnly ? 'Yes' : 'No'} | ${s.min} | ${s.max} | 0 | ${cell(s.note)} |`).join('\n')}

## 2. Achievements (${achievements.length})

Every achievement: **Set By = Client**. Icons: \`Icons/achieved/<API Name>.png\` and \`Icons/unachieved/<API Name>.png\` (256 × 256).
Progress Stat "None" means leave Min/Max at 0.

| # | API Name | Display Name | Description | Hidden | Progress Stat | Min | Max |
|---|---|---|---|---|---|---|---|
${achievements.map((a, i) => `| ${i + 1} | \`${a.api}\` | ${cell(a.name)} | ${cell(a.desc)} | ${a.hidden ? '☑' : ''} | ${a.stat ? `\`${a.stat[0]}\`` : 'None'} | 0 | ${a.stat ? a.stat[1] : 0} |`).join('\n')}
`];
writeFileSync(join(out, 'Achievements.md'), md.join(''));

const csv = v => `"${String(v).replaceAll('"', '""')}"`;
writeFileSync(join(out, 'Achievements.csv'), ['API Name,Display Name,Description,Set By,Hidden,Progress Stat,Min,Max,Achieved Icon,Unachieved Icon',
    ...achievements.map(a => [a.api, a.name, a.desc, 'Client', a.hidden ? 'Yes' : 'No', a.stat ? a.stat[0] : 'None', 0, a.stat ? a.stat[1] : 0,
        `Icons/achieved/${a.api}.png`, `Icons/unachieved/${a.api}.png`].map(csv).join(','))].join('\r\n') + '\r\n');
console.log(`docs: ${achievements.length} achievements, ${stats.length} stats`);
