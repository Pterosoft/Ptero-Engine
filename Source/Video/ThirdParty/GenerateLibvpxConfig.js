// Generates the libvpx configuration the Video project compiles against.
//
// libvpx normally builds through `configure && make`, which needs GNU make and an x86
// assembler (nasm/yasm). Neither is part of the engine's toolchain, so this script does
// the two jobs make would have done and lets MSBuild compile the sources directly:
//
//   1. Evaluates the libvpx .mk source lists for a VP8/VP9 *decoder-only* x86_64 build and
//      writes them to libvpx_sources.txt (pasted into Video.vcxproj).
//   2. Runs build/make/rtcd.pl to produce the run-time CPU dispatch headers.
//
// The assembler is avoided by treating HAVE_X86_ASM as "no": the rtcd definition files
// gate every hand-written .asm specialisation on that flag, so what remains are the C
// fallbacks plus the SSE2/SSSE3/SSE4.1/AVX2 *intrinsics* versions, which MSVC compiles.
//
// Usage (from any directory; needs perl and bash, both of which ship with Git for Windows):
//   node GenerateLibvpxConfig.js <configure-output-dir>
// where <configure-output-dir> is a directory in which libvpx's configure was run as
//   configure --target=x86_64-win64-vs17 --as=yasm --disable-vp8-encoder --disable-vp9-encoder
//     --disable-examples --disable-tools --disable-docs --disable-unit-tests --enable-multithread
//     --enable-runtime-cpu-detect --disable-avx512 --disable-postproc --disable-vp9-postproc
//     --disable-webm-io --disable-libyuv
// (`--as=yasm` only stops configure looking for an assembler; none is ever run.)

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const vpxRoot = path.resolve(__dirname, '..', '..', 'SDKs', 'libvpx-1.17.0');
const outDir = path.resolve(__dirname, 'libvpx_config');
const configureDir = process.argv[2];
if (!configureDir) {
  console.error('usage: node GenerateLibvpxConfig.js <configure-output-dir>');
  process.exit(1);
}

// ---- configuration -------------------------------------------------------------------

const libsMk = fs.readdirSync(configureDir).find((f) => /^libs-.*\.mk$/.test(f));
const vars = new Map();
const configLines = [];
for (const line of fs.readFileSync(path.join(configureDir, libsMk), 'utf8').split(/\r?\n/)) {
  const m = /^((?:CONFIG|HAVE|VPX_ARCH)_[A-Z0-9_]+)=(.*)$/.exec(line);
  if (!m) continue;
  let value = m[2];
  if (m[1] === 'HAVE_X86_ASM') value = 'no';
  vars.set(m[1], value);
  configLines.push(`${m[1]}=${value}`);
}
fs.mkdirSync(outDir, { recursive: true });
const rtcdConfig = path.join(configureDir, 'rtcd_config.mk');
fs.writeFileSync(rtcdConfig, configLines.join('\n') + '\n');

// ---- a small make evaluator ------------------------------------------------------------

function splitArgs(text) {
  // Splits "a,b" at top-level commas.
  const parts = [];
  let depth = 0;
  let current = '';
  for (const ch of text) {
    if (ch === '(') depth++;
    if (ch === ')') depth--;
    if (ch === ',' && depth === 0) {
      parts.push(current);
      current = '';
    } else {
      current += ch;
    }
  }
  parts.push(current);
  return parts;
}

function words(text) {
  return text.split(/\s+/).filter(Boolean);
}

function expand(text) {
  let result = '';
  for (let i = 0; i < text.length; i++) {
    if (text[i] === '$' && text[i + 1] === '(') {
      let depth = 1;
      let j = i + 2;
      while (j < text.length && depth > 0) {
        if (text[j] === '(') depth++;
        if (text[j] === ')') depth--;
        j++;
      }
      const inner = text.slice(i + 2, j - 1);
      result += evaluate(inner);
      i = j - 1;
    } else {
      result += text[i];
    }
  }
  return result;
}

function evaluate(inner) {
  const fn = /^(filter-out|filter|addprefix|call)\s+([\s\S]*)$/.exec(inner);
  if (!fn) return vars.get(expand(inner)) || '';
  const args = splitArgs(fn[2]).map((a) => expand(a));
  switch (fn[1]) {
    case 'filter': {
      const pats = new Set(words(args[0]));
      return words(args[1]).filter((w) => pats.has(w)).join(' ');
    }
    case 'filter-out': {
      const pats = new Set(words(args[0]));
      return words(args[1]).filter((w) => !pats.has(w)).join(' ');
    }
    case 'addprefix':
      return words(args[1]).map((w) => args[0].trim() + w).join(' ');
    case 'call':
      if (args[0].trim() === 'enabled') return vars.get(args[1].trim() + '-yes') || '';
      return '';
  }
  return '';
}

function evalMakefile(file) {
  const raw = fs.readFileSync(file, 'utf8').replace(/\\\r?\n/g, ' ');
  const stack = [];
  const active = () => stack.every(Boolean);
  for (let line of raw.split(/\r?\n/)) {
    line = line.replace(/#.*$/, '').trim();
    if (!line) continue;

    let m;
    if ((m = /^(ifeq|ifneq)\s*\((.*)\)$/.exec(line))) {
      const [a, b] = splitArgs(m[2]).map((x) => expand(x).trim());
      const equal = a === b;
      stack.push(m[1] === 'ifeq' ? equal : !equal);
      continue;
    }
    if (/^else$/.test(line)) {
      stack[stack.length - 1] = !stack[stack.length - 1];
      continue;
    }
    if (/^endif$/.test(line)) {
      stack.pop();
      continue;
    }
    if (!active()) continue;
    if (/^include\s/.test(line)) continue;

    if ((m = /^([^\s:+?=]+)\s*(\+=|:=|\?=|=)\s*(.*)$/.exec(line))) {
      const name = expand(m[1]).trim();
      const value = expand(m[3]).trim();
      if (m[2] === '+=') {
        vars.set(name, ((vars.get(name) || '') + ' ' + value).trim());
      } else if (m[2] === '?=') {
        if (!vars.has(name)) vars.set(name, value);
      } else {
        vars.set(name, value);
      }
    }
  }
}

const groups = [
  ['vpx/vpx_codec.mk', 'API_SRCS', 'vpx/'],
  ['vpx_mem/vpx_mem.mk', 'MEM_SRCS', 'vpx_mem/'],
  ['vpx_scale/vpx_scale.mk', 'SCALE_SRCS', 'vpx_scale/'],
  ['vpx_ports/vpx_ports.mk', 'PORTS_SRCS', 'vpx_ports/'],
  ['vpx_dsp/vpx_dsp.mk', 'DSP_SRCS', 'vpx_dsp/'],
  ['vpx_util/vpx_util.mk', 'UTIL_SRCS', 'vpx_util/'],
  ['vp8/vp8_common.mk', null, null],
  ['vp8/vp8dx.mk', 'VP8_DX_SRCS', 'vp8/'],
  ['vp9/vp9_common.mk', null, null],
  ['vp9/vp9dx.mk', 'VP9_DX_SRCS', 'vp9/'],
];

// The generic encoder API is listed unconditionally, but a decoder-only build never calls
// it, and on Win64 it needs the x87 control-word helpers from float_control_word.asm.
const excluded = new Set(['vpx/src/vpx_encoder.c']);

const sources = [];
for (const [mk, listName, prefix] of groups) {
  evalMakefile(path.join(vpxRoot, mk));
  if (!listName) continue;
  for (const src of words(vars.get(listName + '-yes') || '')) {
    if (/\.(c|cc)$/.test(src) && !excluded.has(prefix + src)) sources.push(prefix + src);
  }
}
const unique = [...new Set(sources)].sort();
for (const src of unique) {
  if (!fs.existsSync(path.join(vpxRoot, src))) throw new Error('missing source ' + src);
}
fs.writeFileSync(path.join(__dirname, 'libvpx_sources.txt'), unique.join('\n') + '\n');

// ---- generated headers ------------------------------------------------------------------

const rtcd = [
  ['vp8_rtcd', 'vp8/common/rtcd_defs.pl'],
  ['vp9_rtcd', 'vp9/common/vp9_rtcd_defs.pl'],
  ['vpx_dsp_rtcd', 'vpx_dsp/vpx_dsp_rtcd_defs.pl'],
  ['vpx_scale_rtcd', 'vpx_scale/vpx_scale_rtcd.pl'],
];
for (const [sym, defs] of rtcd) {
  const header = execFileSync(
    'perl',
    ['build/make/rtcd.pl', '--arch=x86_64', `--sym=${sym}`, `--config=${rtcdConfig}`, '--disable-avx512', defs],
    { cwd: vpxRoot, encoding: 'utf8' });
  fs.writeFileSync(path.join(outDir, sym + '.h'), header);
}

// vpx_config.h/.c come from configure; the only change is dropping the assembler.
const configH = fs.readFileSync(path.join(configureDir, 'vpx_config.h'), 'utf8')
  .replace(/#define HAVE_X86_ASM 1/, '#define HAVE_X86_ASM 0');
fs.writeFileSync(path.join(outDir, 'vpx_config.h'), configH);
fs.copyFileSync(path.join(configureDir, 'vpx_config.c'), path.join(outDir, 'vpx_config.c'));

execFileSync('bash', ['build/make/version.sh', '.', path.join(outDir, 'vpx_version.h').replace(/\\/g, '/')],
  { cwd: vpxRoot });

console.log(`${unique.length} sources, headers written to ${outDir}`);
