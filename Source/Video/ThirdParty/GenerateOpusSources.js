// Lists the libopus sources the Video project compiles, from libopus's own *_sources.mk.
//
// libopus 1.5 ships no Visual Studio project, so the Video project compiles its portable C
// directly: the float build of CELT, SILK and the Opus layer, with no SIMD and no DNN
// (neither is needed to decode a soundtrack). Writes opus_sources.txt next to this file.
//
// Usage: node GenerateOpusSources.js

const fs = require('fs');
const path = require('path');

const opusRoot = path.resolve(__dirname, '..', '..', 'SDKs', 'opus-1.5.2');
const wanted = ['CELT_SOURCES', 'SILK_SOURCES', 'SILK_SOURCES_FLOAT', 'OPUS_SOURCES', 'OPUS_SOURCES_FLOAT'];

const lists = {};
for (const mk of ['celt_sources.mk', 'silk_sources.mk', 'opus_sources.mk']) {
  const text = fs.readFileSync(path.join(opusRoot, mk), 'utf8').replace(/\\\r?\n/g, ' ');
  for (const line of text.split(/\r?\n/)) {
    const m = /^([A-Z0-9_]+)\s*=\s*(.*)$/.exec(line.trim());
    if (m) lists[m[1]] = m[2].split(/\s+/).filter(Boolean);
  }
}

const sources = [];
for (const name of wanted) {
  if (!lists[name]) throw new Error('missing list ' + name);
  for (const src of lists[name]) {
    if (!src.endsWith('.c')) continue;
    if (!fs.existsSync(path.join(opusRoot, src))) throw new Error('missing source ' + src);
    sources.push(src);
  }
}

fs.writeFileSync(path.join(__dirname, 'opus_sources.txt'), [...new Set(sources)].join('\n') + '\n');
console.log(`${sources.length} opus sources`);
