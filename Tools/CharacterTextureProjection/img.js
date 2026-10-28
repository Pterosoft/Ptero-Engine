// RGB image IO through ffmpeg raw frames.
const { execFileSync } = require('child_process');
function probe(file) {
  const out = execFileSync('ffprobe', ['-v', 'error', '-show_entries', 'stream=width,height', '-of', 'csv=p=0', file]).toString().trim().split(',');
  return { w: +out[0], h: +out[1] };
}
function readImage(file) {
  const { w, h } = probe(file);
  const data = execFileSync('ffmpeg', ['-v', 'error', '-i', file, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], { maxBuffer: 1 << 30 });
  return { w, h, data };
}
function writeImage(file, img) {
  execFileSync('ffmpeg', ['-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', `${img.w}x${img.h}`, '-i', '-', file], { input: img.data, maxBuffer: 1 << 30 });
}
// Bilinear sample at continuous pixel coords; returns null outside.
function sample(img, x, y, out) {
  x -= 0.5; y -= 0.5;
  if (x < 0 || y < 0 || x > img.w - 1 || y > img.h - 1) return null;
  const x0 = Math.floor(x), y0 = Math.floor(y), x1 = Math.min(x0 + 1, img.w - 1), y1 = Math.min(y0 + 1, img.h - 1);
  const fx = x - x0, fy = y - y0, d = img.data;
  for (let c = 0; c < 3; c++) {
    const a = d[(y0 * img.w + x0) * 3 + c], b = d[(y0 * img.w + x1) * 3 + c], e = d[(y1 * img.w + x0) * 3 + c], g = d[(y1 * img.w + x1) * 3 + c];
    out[c] = (a * (1 - fx) + b * fx) * (1 - fy) + (e * (1 - fx) + g * fx) * fy;
  }
  return out;
}
module.exports = { readImage, writeImage, sample };
