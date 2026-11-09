// Renders the preview's Fus Ro Dah offline (the same synthesis the browser plays) to JSON samples, 48 kHz mono,
// for tools/make_sounds.py --shout. Usage: node tools/render_shout.js out.json
const { chromium } = require("playwright");
const path = require("path");
const fs = require("fs");
(async () => {
  const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
  const page = await browser.newPage();
  // three.js from a local copy (THREE_JS=path/to/three.min.js, r128) when the CDN can't be reached
  if (process.env.THREE_JS) {
    await page.route(/cdnjs\.cloudflare\.com\/.*three\.min\.js/, (r) => r.fulfill({ path: process.env.THREE_JS, contentType: "text/javascript" }));
  }
  await page.route(/fonts\.googleapis\.com/, (r) => r.fulfill({ body: "", contentType: "text/css" }));
  await page.goto("file://" + path.resolve(__dirname, "../preview/yacht-local.html"));
  await page.waitForFunction(() => window.__yacht && window.__yacht.renderSound, null, { timeout: 60000 });
  const samples = await page.evaluate(() => window.__yacht.renderSound("shout"));
  fs.writeFileSync(process.argv[2], JSON.stringify(samples.map((v) => Math.round(v * 1e5) / 1e5)));
  console.log("shout:", samples.length, "samples, peak", samples.reduce((m, v) => Math.max(m, Math.abs(v)), 0).toFixed(3));
  await browser.close();
})();
