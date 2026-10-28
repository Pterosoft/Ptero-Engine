// Renders reel.html frame by frame (deterministic, via window.render(t)) into frames/.
// Usage: node render-reel.cjs [onlyTimesCommaSeparated]
const path = require('path');
const fs = require('fs');
const { chromium } = require(path.resolve(__dirname, '../../../Farkle/Website/qa/tooling/node_modules/playwright'));
const FPS = 30;
(async () => {
  const out = path.resolve(__dirname, 'frames');
  fs.mkdirSync(out, { recursive: true });
  const b = await chromium.launch({ channel: 'msedge', headless: true });
  const p = await b.newPage({ viewport: { width: 1080, height: 1920 } });
  await p.goto('file:///' + path.resolve(__dirname, 'reel.html').replace(/\\/g, '/'));
  await p.evaluate(async () => { await document.fonts.ready; await Promise.all([...document.images].map(i => i.decode())); });
  const only = process.argv[2];
  const times = only ? only.split(',').map(Number) : null;
  const total = times ? times.length : Math.round(await p.evaluate(() => window.DURATION) * FPS);
  for (let i = 0; i < total; i++) {
    const t = times ? times[i] : i / FPS;
    await p.evaluate(t => window.render(t), t);
    const name = times ? `preview_${t}.png` : `f${String(i).padStart(4, '0')}.png`;
    await p.screenshot({ path: path.join(out, name) });
  }
  await b.close();
})();
