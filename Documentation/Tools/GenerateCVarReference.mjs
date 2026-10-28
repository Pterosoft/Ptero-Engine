// Regenerates Documentation/Manual/CVar-Reference.md from the cvars the engine actually
// registers (Source/Renderer_DX12/EngineCVars.cpp) and the defaults in the settings
// headers, so the page cannot drift from the code. Run it from the repository root after
// adding, renaming or re-describing a cvar:
//
//   node Documentation/Tools/GenerateCVarReference.mjs
//
// Any cvar whose default it cannot work out is listed on stderr; give it an entry in
// kManualDefaults below.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const sourceDir = path.join(repoRoot, 'Source', 'Renderer_DX12');
const outputPath = path.join(repoRoot, 'Documentation', 'Manual', 'CVar-Reference.md');

// Cvars bound to a renderer getter rather than a settings struct field, so the header
// scan cannot see their default.
// Keep in step with the member initialisers in DX12SceneRenderer.h / EntityMeshRenderer.h.
const kManualDefaults = {
    'r.viewdistance': '8000',
    'r.grid': 'true',
    'r.wireframe': 'false',
    'shadows.enabled': 'true',
    'stats.overlay': 'false',
    'game.skipintro': 'false',
    'gi.mode': 'rtgi',
    'lights.clustered': 'true',
    'lights.gridcell': '4.0',
};

// Extra explanation appended to the engine's own description where that is terse or
// assumes background. The engine description stays first and authoritative.
const kExtraNotes = {
    'r.viewdistance': 'Nothing further away than this is drawn.',
    'rtgi.maxbounces': 'More bounces fill dark interiors better and cost more.',
    'rtgi.nrd.atrousiterations': 'More passes give smoother, blurrier GI.',
    'rtgi.nrd.sharpen': 'Restores detail the denoiser softened.',
    'probes.enabled': 'Takes over from RTGI when on (voxel GI still takes precedence).',
    'probes.originx': 'Only used when probes.followcamera is off.',
    'probes.originy': 'Only used when probes.followcamera is off.',
    'probes.originz': 'Only used when probes.followcamera is off.',
    'voxelgi.specular.enabled': 'Reflections traced through the voxels; SSR takes over where it finds a hit.',
    'rc.cascadecount': 'Radiance cascades are not drawn by the current renderer, so the rc.* settings have no visible effect.',
    'pt.reset': 'Set to true to throw away the accumulated image and start again.',
    'pt.maxbounces': '0 = direct light only.',
    'rtao.raysperpixel': 'More rays, less noise, more cost.',
    'gtao.quality': '0 = Low, 1 = Medium, 2 = High, 3 = Ultra.',
    'gtao.radiusmultiplier': 'Larger values darken bigger areas.',
    'gtao.debugview': '0 = composited into the lighting, 1 = AO only.',
    'rtao.debugview': '0 = final AO, 1 = raw noisy AO, 2 = denoised AO.',
    'rtao.locallightocclusion': '0 = sky light and GI only, which leaves AO invisible in rooms lit by lamps or candles.',
    'shadow.rt.locallights': 'Lights beyond 8 on a pixel keep their shadow maps.',
    'shadow.rt.raysperpixel': 'More rays, less noise, more cost.',
    'shadow.rt.raybias': 'It grows on its own where light grazes a surface.',
    'shadow.rt.temporalblend': 'Lower is smoother but lags behind moving objects.',
    'ssr.debugview': 'Ray march: 0 = composite, 1 = reflection only, 2 = confidence. FidelityFX: 0 = composite, 1 = denoised reflection, 2 = raw reflection.',
    'ssr.technique': '`raymarch` = one sharp ray per pixel; `fidelityfx` = AMD SSSR, blurred glossy reflections with a denoiser.',
    'sss.mode': '`screenspace` = cheap blur, cannot see past silhouettes; `raytraced` = follows the real mesh, needs DXR (falls back to screen space).',
    'taa.quality': 'Unreal\'s TAA quality: higher is sharper and more stable in motion, and costs a little more.',
    'dlss.mode': 'Lower-performance modes render at a higher resolution; DLAA renders at native resolution (anti-aliasing only).',
    'bloom.knee': '0 = hard cut at the threshold, 1 = very gradual.',
    'bloom.radius': 'Mip-chain method: how far the glow spreads.',
    'lensflare.lights': 'Placed lights (not just the sun) can produce flares.',
    'lensflare.wavelengths': '1 = monochrome ghosts, 3 = RGB colour fringes, up to 6.',
    'agx.ev100max': 'With auto exposure, the darkest the meter may settle at. Set min and max equal to pin the exposure.',
    'agx.ev100min': 'With auto exposure, the brightest the meter may settle at.',
    'tod.dayev100': 'Used when tod.controlexposure is on. Higher = darker.',
    'tod.moonphase': '0.25 = first quarter, 0.75 = last quarter. Also sets when the moon rises.',
    'tod.starfield': '`procedural` = sharp twinkling stars; `texture` = the real sky from a star map; `both` = map for the Milky Way, procedural for the sharp points.',
    'fog.density': 'A ray of length d keeps e^(-density x d) of its light. 0.02 is light haze, 0.1 thick fog.',
    'fog.color.r': 'Colour components are linear; 1, 1, 1 is neutral white.',
    'fog.emissive.r': 'Light the fog gives off by itself (glowing mist); 0 = none.',
    'clouds.cloudtype': '0 = flat stratus, 0.5 = cumulus, 1 = towering cumulonimbus.',
    'clouds.debugview': '0 = composite, 1 = luminance, 2 = transmittance, 3 = coverage, 4 = cloud distance.',
    'clouds.phaseg0': 'How strongly clouds glow when looking towards the sun.',
    'clouds.phaseg1': 'How much light scatters back towards the sun side.',
    'clouds.multiscatteroctaves': 'Approximates light bouncing many times inside thick clouds; more octaves brighten their interiors.',
    'clouds.maxsteps': 'Quality vs cost of the cloud ray march.',
    'shadow.point.filterradius': 'Softness of point-light cubemap shadows (only used when the virtual shadow map is off for lights).',
    'shadow.point.bias': 'Raise it if point-light shadows show stripes (acne); lower it if they detach from objects.',
    'shadow.vsm.levels': 'How far sun shadows reach: each level doubles the area covered.',
};

// ---- sources -----------------------------------------------------------------------
const cvarSource = fs.readFileSync(path.join(sourceDir, 'EngineCVars.cpp'), 'utf8').replace(/\r/g, '');
const headerText = fs.readdirSync(sourceDir)
    .filter(name => name.endsWith('.h'))
    .map(name => fs.readFileSync(path.join(sourceDir, name), 'utf8').replace(/\r/g, ''))
    .join('\n');

// ---- enum classes: "Enum::Member" -> numeric value ----------------------------------
const enumValues = {};
for (const match of headerText.matchAll(/enum\s+(?:class\s+)?(\w+)(?:\s*:\s*\w+)?\s*\{([^}]*)\}/g)) {
    let next = 0;
    // Comments first: they contain commas of their own.
    for (let entry of match[2].replace(/\/\/.*$/gm, '').replace(/\/\*[\s\S]*?\*\//g, '').split(',')) {
        entry = entry.trim();
        if (!entry) continue;
        const [member, value] = entry.split('=').map(part => part.trim());
        if (!/^\w+$/.test(member)) continue;
        if (value !== undefined && /^-?\d+$/.test(value)) next = Number(value);
        enumValues[`${match[1]}::${member}`] = next;
        enumValues[member] ??= next;
        next += 1;
    }
}

// ---- struct field types and defaults -------------------------------------------------
const fieldDefaults = {};   // "Struct::Field" -> default text
const fieldTypes = {};      // "Struct::Field" -> type name
for (const match of headerText.matchAll(/struct\s+(\w+)\s*\{([\s\S]*?)\n\};/g)) {
    const [, structName, body] = match;
    for (const rawLine of body.split('\n')) {
        const line = rawLine.replace(/\/\/.*$/, '');
        const declaration = line.match(/^\s*(?:const\s+|inline\s+|static\s+)*((?:unsigned\s+|signed\s+|long\s+)*[\w:<>]+)\s+(\w+\s*(?:=|\{)[^;]*);/);
        if (!declaration) continue;
        const type = declaration[1].replace(/\s+/g, ' ');
        if (['return', 'using', 'case'].includes(type)) continue;
        for (const declarator of declaration[2].split(/,(?![^{(]*[})])/)) {
            const field = declarator.match(/^\s*(\w+)\s*(?:=\s*(.+?)|\{\s*(.*?)\s*\})\s*$/);
            if (!field) continue;
            fieldTypes[`${structName}::${field[1]}`] = type;
            const value = field[2] ?? field[3];
            if (value !== undefined && value !== '') fieldDefaults[`${structName}::${field[1]}`] = value.trim();
        }
    }
}
// Values set in constructors or factory functions rather than in the declaration.
fieldDefaults['AgxColorGradeControl::Total'] ??= '0';
const kMoonGlare = { Intensity: '0.08', HaloCore: '1.0', HaloRadius: '20.0', HaloFalloff: '2.0', StreakStrength: '0.0', ChromaticSpread: '0.1' };

// ---- local settings variables: "rtgi" -> "RtGISettings" -------------------------------
const variableTypes = {};
for (const match of cvarSource.matchAll(/^\s*(\w+)&\s+(\w+)\s*=\s*renderer\.[^;]*;/gm))
    variableTypes[match[2]] = match[1];

function resolveDefault(pointerExpression) {
    let expression = pointerExpression.trim()
        .replace(/^AsIntPointer\((.*)\)$/s, '$1')
        .replace(/^reinterpret_cast<[^>]+>\((.*)\)$/s, '$1')
        .trim();
    const fieldPath = expression.match(/^&(\w+)((?:\.\w+)+)$/);
    if (!fieldPath || !variableTypes[fieldPath[1]]) return undefined;
    const fields = fieldPath[2].slice(1).split('.');
    let structName = variableTypes[fieldPath[1]];
    for (let i = 0; i < fields.length; ++i) {
        const key = `${structName}::${fields[i]}`;
        if (i === fields.length - 1) {
            if (structName === 'SkyGlareSettings' && fields[i - 1] === 'MoonGlare' && kMoonGlare[fields[i]] !== undefined)
                return kMoonGlare[fields[i]];
            return fieldDefaults[key];
        }
        structName = fieldTypes[key];
        if (!structName) return undefined;
    }
    return undefined;
}

// ---- register calls -------------------------------------------------------------------
// Splits the argument list of the call starting at `open` (the index of its '(') into
// top-level arguments, honouring strings, nested brackets and lambda bodies.
function splitCallArguments(text, open) {
    const args = [];
    let depth = 0, current = '', inString = false;
    for (let i = open + 1; i < text.length; ++i) {
        const c = text[i];
        if (inString) {
            current += c;
            if (c === '\\') { current += text[++i]; continue; }
            if (c === '"') inString = false;
            continue;
        }
        if (c === '"') { inString = true; current += c; continue; }
        if ('([{'.includes(c)) depth++;
        if (')]}'.includes(c)) {
            if (depth === 0) { args.push(current.trim()); return args; }
            depth--;
        }
        if (c === ',' && depth === 0) { args.push(current.trim()); current = ''; continue; }
        current += c;
    }
    return args;
}

const stringLiterals = text => [...text.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map(m => m[1]);
const cleanNumber = text => text.trim().replace(/f$/, '').replace(/^\+/, '');
const isNumber = text => text !== undefined && /^-?[\d.]+f?$/.test(text.trim());

function prettyDefault(value, kind, labels, firstValue) {
    if (value === undefined) return undefined;
    let text = String(value).trim().replace(/^\{\s*(.*?)\s*\}$/, '$1');
    if (/^k[A-Z]\w*$/.test(text)) {
        // A named constant: "inline constexpr float kAgxDefaultEv100 = -4.0f;"
        const constant = headerText.match(new RegExp(`\\b${text}\\s*=\\s*([^;]+);`));
        if (constant) text = constant[1].trim();
    } else if ((/^[A-Z]\w*::\w+$/.test(text) || /^[A-Z]\w+$/.test(text)) && enumValues[text] !== undefined) {
        text = String(enumValues[text]);
    }
    text = text.replace(/(\d)f$/, '$1').replace(/u$/, '');
    if (kind === 'Enum' && /^-?\d+$/.test(text)) {
        const label = labels[Number(text) - firstValue];
        if (label !== undefined) return label;
    }
    return text;
}

const rows = [];
const unresolved = [];
for (const match of cvarSource.matchAll(/CVar::Register(Bool|Int|Float|Enum|String)\s*\(/g)) {
    const kind = match[1];
    const args = splitCallArguments(cvarSource, match.index + match[0].length - 1);
    const nameMatch = args[0].match(/^"([^"]+)"$/);
    if (!nameMatch) continue;   // the colour-grade helpers, expanded below
    const name = nameMatch[1];
    const engineDescription = stringLiterals(args[2] ?? '').join('');
    const description = kExtraNotes[name] ? `${engineDescription} ${kExtraNotes[name]}` : engineDescription;
    let min = '', max = '', labels = [], firstValue = 0;
    if (kind === 'Int' || kind === 'Float') {
        if (isNumber(args[3]) && isNumber(args[4])) { min = cleanNumber(args[3]); max = cleanNumber(args[4]); }
    } else if (kind === 'Enum') {
        labels = stringLiterals(args[3] ?? '');
        if (isNumber(args[4])) firstValue = Number(cleanNumber(args[4]));
    }
    let value = kManualDefaults[name] ?? prettyDefault(resolveDefault(args[1]), kind, labels, firstValue);
    if (value === undefined) unresolved.push(name);
    if (kind === 'Bool' && value !== undefined) value = value === '1' ? 'true' : value === '0' ? 'false' : value;
    rows.push({ name, kind, description, min, max, labels, firstValue, value });
}

// RegisterColorGradeRegion: 5 controls x 5 channels per tonal region.
const gradeDefaults = { 'agx.global.contrast.total': '0.10', 'agx.global.saturation.total': '0.12' };
for (const [prefix, region] of [['agx.global', 'whole image'], ['agx.shadows', 'shadows only'],
                                ['agx.midtones', 'midtones only'], ['agx.highlights', 'highlights only']]) {
    if (!cvarSource.includes(`"${prefix}"`)) continue;
    for (const control of ['contrast', 'gamma', 'gain', 'saturation', 'vibrance'])
        for (const channel of ['total', 'red', 'green', 'blue', 'yellow'])
            rows.push({
                name: `${prefix}.${control}.${channel}`, kind: 'Float',
                description: `${control[0].toUpperCase()}${control.slice(1)}, ${region}: ${channel === 'total' ? 'all channels' : channel}. 0 leaves it unchanged.`,
                min: '-2', max: '2', labels: [], value: gradeDefaults[`${prefix}.${control}.${channel}`] ?? '0',
            });
}

// ---- grouping ---------------------------------------------------------------------------
const kGroups = [
    ['r',         'Renderer',                           '01-How-Ptero-Works.md'],
    ['shadows',   'Shadows (master switch)',            '08-Shadows.md'],
    ['stats',     'Statistics overlay',                 '02-The-Editor.md#renderer-statistics'],
    ['game',      'Play sessions',                      '11-Gameplay.md'],
    ['vg',        'Virtualized geometry',               '04-Components.md#virtualized-geometry'],
    ['gi',        'Global illumination mode',           '07-Global-Illumination.md#the-gi-systems'],
    ['rtgi',      'Ray-traced GI (RTGI)',               '07-Global-Illumination.md#rtgi-ray-traced-global-illumination'],
    ['probes',    'Radiance probes',                    '07-Global-Illumination.md#radiance-probes'],
    ['voxelgi',   'Voxel GI',                           '07-Global-Illumination.md#voxel-gi'],
    ['rc',        'Radiance cascades (not currently drawn)', '07-Global-Illumination.md#the-gi-systems'],
    ['pt',        'Path tracer',                        '10-Image-Quality.md#path-tracer'],
    ['rtao',      'Ray-traced ambient occlusion',       '07-Global-Illumination.md#ambient-occlusion'],
    ['gtao',      'XeGTAO (screen-space AO)',           '07-Global-Illumination.md#ambient-occlusion'],
    ['dple',      'DPLE',                               '07-Global-Illumination.md#dple'],
    ['ssr',       'Screen-space reflections',           '07-Global-Illumination.md#screen-space-reflections'],
    ['lights',    'Shading model and lights',           '05-Materials.md#how-surfaces-are-shaded'],
    ['sss',       'Subsurface scattering',              '07-Global-Illumination.md#subsurface-scattering'],
    ['taa',       'Temporal anti-aliasing (TAA / TAAU)', '10-Image-Quality.md#anti-aliasing'],
    ['smaa',      'SMAA',                               '10-Image-Quality.md#anti-aliasing'],
    ['edgeaa',    'Analytic Edge AA',                   '10-Image-Quality.md#anti-aliasing'],
    ['aar',       'AAR upscaling',                      '10-Image-Quality.md#upscaling'],
    ['msaa',      'MSAA',                               '10-Image-Quality.md#anti-aliasing'],
    ['dlss',      'DLSS',                               '10-Image-Quality.md#upscaling'],
    ['fsr',       'AMD FSR',                            '10-Image-Quality.md#upscaling'],
    ['xess',      'Intel XeSS',                         '10-Image-Quality.md#upscaling'],
    ['sharpen',   'Sharpening',                         '10-Image-Quality.md#sharpening'],
    ['bloom',     'Bloom',                              '10-Image-Quality.md#bloom'],
    ['lensflare', 'Lens flares',                        '10-Image-Quality.md#lens-flares'],
    ['chroma',    'Chromatic aberration',               '10-Image-Quality.md#chromatic-aberration'],
    ['agx',       'AgX tonemapping, exposure and colour grade', '10-Image-Quality.md#tonemapping-and-exposure'],
    ['tod',       'Time of day, sky, sun and moon',     '06-Lighting.md#time-of-day'],
    ['wind',      'Wind',                               '06-Lighting.md#wind'],
    ['fog',       'Volumetric fog',                     '09-Outdoor-Worlds.md#volumetric-fog'],
    ['clouds',    'Volumetric clouds',                  '09-Outdoor-Worlds.md#volumetric-clouds'],
    ['shadow',    'Shadows (virtual, point-light and ray-traced)', '08-Shadows.md'],
];
const groupOf = name => name.split('.')[0];
const known = new Set(kGroups.map(g => g[0]));
for (const row of rows)
    if (!known.has(groupOf(row.name))) { kGroups.push([groupOf(row.name), groupOf(row.name), null]); known.add(groupOf(row.name)); }

const escapeCell = text => String(text).replace(/\|/g, '\\|').replace(/\n/g, ' ');
const anchor = title => title.toLowerCase().replace(/[^a-z0-9 -]/g, '').replace(/ /g, '-');

function valuesCell(row) {
    switch (row.kind) {
        case 'Bool':  return '`true` / `false`';
        case 'Enum':  return row.labels.map((label, i) => `\`${label}\` (${i + row.firstValue})`).join(', ');
        case 'Int':   return row.min !== '' ? `whole number, \`${row.min}\` – \`${row.max}\`` : 'whole number';
        case 'Float': return row.min !== '' ? `\`${row.min}\` – \`${row.max}\`` : 'number';
        default:      return 'text';
    }
}

// ---- output -------------------------------------------------------------------------------
let out = '';
out += '# CVar reference\n\n';
out += '<!-- Generated by Documentation/Tools/GenerateCVarReference.mjs. Do not edit by hand: change the\n';
out += '     descriptions in Source/Renderer_DX12/EngineCVars.cpp and run the script again. -->\n\n';
out += `Every console variable (cvar) the engine registers: **${rows.length}** settings in **${kGroups.filter(g => rows.some(r => groupOf(r.name) === g[0])).length}** groups. `;
out += 'Each one is the same value as a control in a settings window, so changing it in the Console and moving the slider do exactly the same thing.\n\n';
out += '## Using cvars\n\n';
out += 'Type into the **Console** (**Windows → Console**):\n\n';
out += '```\nrtgi.maxbounces          show the current value, its range and its default\nrtgi.maxbounces 3        set it\nlist fog                 every cvar whose name or description mentions "fog"\nhelp taa.enabled         describe one cvar\nreset rtgi.maxbounces    back to its default\nreset all                every cvar back to its default\ndumpcvars                every cvar and its current value\n```\n\n';
out += '| Type | What you can type |\n| --- | --- |\n';
out += '| **bool** | `true` / `false`, `1` / `0`, `on` / `off`, `yes` / `no` |\n';
out += '| **int** | A whole number inside the range |\n';
out += '| **float** | A number inside the range, such as `0.25` |\n';
out += '| **enum** | One of the listed names, or its number (shown in brackets) |\n\n';
out += 'A value outside the range is **rejected, not clamped**, so a typo never silently does something else. ';
out += 'The **Default** column is the value the engine starts with, which is what `reset` restores. A level that saves its own settings ';
out += 'overrides it when it loads, so the value you see in a level may differ. Settings that belong to a level are saved with it; ';
out += 'see [1. How Ptero works](01-How-Ptero-Works.md#levels-entities-and-components) for which ones.\n\n';
out += '**Units**: distances in metres, angles in degrees, times in seconds, light in lux or lumens, exposure in EV100, unless the description says otherwise.\n\n';

out += '## Groups\n\n';
for (const [prefix, title] of kGroups) {
    const count = rows.filter(r => groupOf(r.name) === prefix).length;
    if (count) out += `- [${title}](#${anchor(title)}) (\`${prefix}.*\`, ${count})\n`;
}
out += '\n';

for (const [prefix, title, manualPage] of kGroups) {
    const groupRows = rows.filter(r => groupOf(r.name) === prefix).sort((a, b) => a.name.localeCompare(b.name));
    if (!groupRows.length) continue;
    out += `## ${title}\n\n`;
    if (manualPage) {
        // The chapter's own title, from its first heading.
        const chapter = fs.readFileSync(path.join(path.dirname(outputPath), manualPage.split('#')[0]), 'utf8').match(/^# (.+)$/m)[1];
        out += `How it works: [${chapter}](${manualPage}).\n\n`;
    }
    out += '| CVar | What it does | Values | Default |\n| --- | --- | --- | --- |\n';
    for (const row of groupRows)
        out += `| \`${row.name}\` | ${escapeCell(row.description)} | ${escapeCell(valuesCell(row))} | ${row.value !== undefined ? '`' + escapeCell(row.value) + '`' : ''} |\n`;
    out += '\n';
}

fs.writeFileSync(outputPath, out);
console.log(`Wrote ${path.relative(repoRoot, outputPath)}: ${rows.length} cvars.`);
if (unresolved.length) console.error(`No default found for: ${unresolved.join(', ')}`);
