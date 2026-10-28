// Renders the multiplayer update images from update.html: key art, feature board and
// lighting board (1920x1080), Steam event cover (800x450) and header (1920x622).
const path = require('path');
const { chromium } = require(path.resolve(__dirname, '../../../Website/qa/tooling/node_modules/playwright'));
const layouts = {
  key:    [1920, 1080, 'multiplayer_update.png'],
  board:  [1920, 1080, 'multiplayer_features.png'],
  light:  [1920, 1080, 'raytraced_lighting.png'],
  cover:  [800, 450, 'steam_event_cover.png'],
  header: [1920, 622, 'steam_event_header.png'],
};
(async () => {
  const b = await chromium.launch({ channel: 'msedge', headless: true });
  for (const [name, [width, height, file]] of Object.entries(layouts)) {
    const p = await b.newPage({ viewport: { width, height } });
    await p.goto('file:///' + path.resolve(__dirname, 'update.html').replace(/\\/g, '/') + '?layout=' + name);
    await p.evaluate(async () => { await document.fonts.ready; await Promise.all([...document.images].map(i => i.decode())); });
    await p.screenshot({ path: path.resolve(__dirname, file) });
    await p.close();
  }
  await b.close();
})();
