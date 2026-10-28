// Renders the Steam event images from event.html: cover (800x450) and header (1920x622).
const path = require('path');
const { chromium } = require(path.resolve(__dirname, '../../../Farkle/Website/qa/tooling/node_modules/playwright'));
const layouts = { cover: [800, 450], header: [1920, 622] };
(async () => {
  const b = await chromium.launch({ channel: 'msedge', headless: true });
  for (const [name, [width, height]] of Object.entries(layouts)) {
    const p = await b.newPage({ viewport: { width, height } });
    await p.goto('file:///' + path.resolve(__dirname, 'event.html').replace(/\\/g, '/') + '?layout=' + name);
    await p.evaluate(async () => { await document.fonts.ready; await Promise.all([...document.images].map(i => i.decode())); });
    await p.screenshot({ path: path.resolve(__dirname, `steam_event_${name}.png`) });
    await p.close();
  }
  await b.close();
})();
