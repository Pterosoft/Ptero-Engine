// Renders reel.html frame by frame (deterministic, via window.render(t)) into frames/, then encodes
// multiplayer_reel.mp4 (1080x1920, 30 fps, H.264 + AAC) for Instagram Reels, Facebook Reels and YouTube Shorts.
// Usage: node render-reel.cjs [onlyTimesCommaSeparated]   (times only write preview stills, no video)
const path = require('path');
const fs = require('fs');
const { execFileSync } = require('child_process');
const { chromium } = require(path.resolve(__dirname, '../../../Website/qa/tooling/node_modules/playwright'));
const FPS = 30;
const sfx = path.resolve(__dirname, '../../../Data/fmod_project/Ptero-Engine/Assets');

// [file, start in reel (s), trim start (s), length (s), volume]; times follow the scenes in reel.html
const cues = [
  ['Emilio_Villalba.-_Viola_de_teclas_medieval_(Nickelharpa)._Saltarello.webm.1080p.vp9.wav', 0, 2, 21, 0.55],
  ['387128__nooneisreal__the-fireplace-3.wav', 0, 10, 21, 0.35],
  ['832754__kollim__rolling-4-dice-wooden-floor.wav', 0, 7.3, 2.3, 1.0],
  ['ui_click_1.wav', 1.4, 0, 0.22, 0.9],
  ['ui_click_2.wav', 3.6, 0, 0.22, 0.7],
  ['ui_click_1.wav', 5.95, 0, 0.22, 0.7],
  ['ui_click_2.wav', 8.55, 0, 0.22, 0.7],
  ['ui_click_1.wav', 11.15, 0, 0.22, 0.7],
  ['ui_click_3.wav', 12.6, 0, 0.22, 0.9],
  ['ui_click_2.wav', 14.05, 0, 0.22, 0.7],
  ['ui_hover_1.wav', 14.6, 0, 0.3, 0.9],
  ['ui_hover_2.wav', 15.2, 0, 0.3, 0.9],
  ['ui_hover_3.wav', 15.8, 0, 0.3, 0.9],
  ['ui_click_1.wav', 17.6, 0, 0.22, 0.7],
];

(async () => {
  const out = path.resolve(__dirname, 'frames');
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });
  const b = await chromium.launch({ channel: 'msedge', headless: true });
  const p = await b.newPage({ viewport: { width: 1080, height: 1920 } });
  await p.goto('file:///' + path.resolve(__dirname, 'reel.html').replace(/\\/g, '/'));
  await p.evaluate(async () => { await document.fonts.ready; await Promise.all([...document.images].map(i => i.decode())); });
  const only = process.argv[2];
  const times = only ? only.split(',').map(Number) : null;
  const duration = await p.evaluate(() => window.DURATION);
  const total = times ? times.length : Math.round(duration * FPS);
  for (let i = 0; i < total; i++) {
    const t = times ? times[i] : i / FPS;
    await p.evaluate(t => window.render(t), t);
    const name = times ? `preview_${t}.png` : `f${String(i).padStart(4, '0')}.png`;
    await p.screenshot({ path: path.join(out, name) });
  }
  await b.close();
  if (times) return;

  const inputs = cues.flatMap(([file]) => ['-i', path.join(sfx, file)]);
  const chains = cues.map(([, at, from, len, vol], i) =>
    `[${i + 1}:a]atrim=${from}:${from + len},asetpts=PTS-STARTPTS,aformat=sample_rates=48000:channel_layouts=stereo,volume=${vol},adelay=${Math.round(at * 1000)}:all=1[a${i}]`);
  const mix = `${cues.map((_, i) => `[a${i}]`).join('')}amix=inputs=${cues.length}:normalize=0,afade=t=out:st=${duration - 1.2}:d=1.2,loudnorm=I=-14:TP=-1.5:LRA=11,aresample=48000[aout]`;
  execFileSync('ffmpeg', ['-y', '-hide_banner', '-loglevel', 'error', '-framerate', String(FPS), '-i', path.join(out, 'f%04d.png'), ...inputs,
    '-filter_complex', [...chains, mix].join(';'), '-map', '0:v', '-map', '[aout]',
    '-c:v', 'libx264', '-preset', 'slow', '-crf', '18', '-pix_fmt', 'yuv420p', '-profile:v', 'high', '-movflags', '+faststart',
    '-c:a', 'aac', '-b:a', '192k', '-t', String(duration), path.resolve(__dirname, 'multiplayer_reel.mp4')], { stdio: 'inherit' });
  fs.rmSync(out, { recursive: true, force: true });
})();
