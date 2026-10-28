const path = require('path');
const { chromium } = require(path.resolve(__dirname, '../../../Farkle/Website/qa/tooling/node_modules/playwright'));
(async () => {
  const b = await chromium.launch({ channel: 'msedge', headless: true });
  const p = await b.newPage({ viewport: { width: 1920, height: 1080 } });
  await p.goto('file:///' + path.resolve(__dirname, 'launch.html').replace(/\\/g, '/'));
  await p.evaluate(() => document.fonts.ready);
  await p.waitForTimeout(300);
  await p.screenshot({ path: path.resolve(__dirname, 'farkle_launch_announcement.png') });
  await b.close();
})();
