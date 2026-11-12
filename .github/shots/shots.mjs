// Fork-only: screenshots of the built builder's kit and synth previews, and the screen SVGs
// they render, for rasterising with the device's face elsewhere.
import puppeteer from 'puppeteer-core';
import {mkdirSync, writeFileSync} from 'node:fs';
import {join} from 'node:path';

const [base, out] = process.argv.slice(2);
mkdirSync(out, {recursive:true});
const browser = await puppeteer.launch({executablePath:process.env.CHROME ?? '/usr/bin/google-chrome',
  args:['--no-sandbox', '--autoplay-policy=no-user-gesture-required']});
const page = await browser.newPage();
// The page's CSP keeps fetch to 'self'; reading the dialog's blob images back needs it lifted.
await page.setBypassCSP(true);
await page.setViewport({width:1360, height:1000, deviceScaleFactor:2});
const log = [], report = {};
page.on('console', m => log.push(`${m.type()}: ${m.text()}`));
page.on('pageerror', e => log.push('pageerror: ' + e.message));
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const svg = name => page.evaluate(() => document.querySelector('#art-preview svg').outerHTML).then(text => writeFileSync(join(out, name + '.svg'), text));
const state = () => page.evaluate(() => ({
  caption:document.getElementById('art-caption').textContent,
  controls:[...document.querySelectorAll('#controls .preview-control')].map(l => ({label:l.children[0].textContent,
    value:l.children[1].value, max:l.children[1].max, disabled:l.children[1].disabled})),
  keys:document.querySelectorAll('#preview-keys button').length,
  selected:document.querySelector('#preview-keys [data-selected=true]')?.getAttribute('aria-label') ?? null,
  model:document.querySelector('.audition-model select')?.selectedOptions[0]?.textContent ?? null,
  models:document.querySelectorAll('.audition-model option').length,
  header:[...document.querySelectorAll('#art-preview [data-opxy-header] text')].map(t => t.textContent),
}));
async function machine(id) {
  await page.goto(`${base}/#preview/machines/${id}`, {waitUntil:'networkidle0'});
  await page.waitForSelector('#art-preview svg', {timeout:20000});
  await page.evaluate(() => { document.getElementById('preview-controls-panel').open = true; });
  await page.waitForSelector('#preview-keys');
  await pause(400);
}
async function inspector(name) {
  await (await page.$('.inspector')).screenshot({path:join(out, name + '.png')});
  await page.screenshot({path:join(out, name + '-page.png')});
  await svg(name);
  report[name] = await state();
}
try {
  await machine('mdkit');
  await inspector('mdkit-1-default');
  // Key D4 (62): selects it and plays it, as on the device.
  const key = await page.$('#preview-keys [data-note="62"]');
  const box = await key.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height - 6);
  await page.mouse.down(); await pause(150); await page.mouse.up(); await pause(300);
  await inspector('mdkit-2-key-d4');
  // Another machine for that key.
  await page.select('.audition-model select', '14');
  await pause(300);
  await inspector('mdkit-3-machine-15');
  // A slider: the first control to 100.
  await page.evaluate(() => { const input = document.querySelector('#controls .preview-control input');
    input.value = '100'; input.dispatchEvent(new Event('input', {bubbles:true})); });
  await pause(300);
  await inspector('mdkit-4-control-100');
  await machine('mmsaw');
  await inspector('mmsaw-synth');
  // The selected-machines dialog: rendered default frames.
  await page.goto(`${base}/#preview/extensions`, {waitUntil:'networkidle0'});
  await page.waitForSelector('#preview-selected:not([disabled])', {timeout:20000});
  await page.click('#preview-selected');
  await page.waitForFunction(() => document.querySelectorAll('#selected-previews-list img').length >= 2, {timeout:20000});
  await pause(500);
  await (await page.$('#selected-previews')).screenshot({path:join(out, 'dialog.png')});
  report.dialog = await page.evaluate(async () => Promise.all([...document.querySelectorAll('#selected-previews-list figure')].map(async f => ({
    caption:f.textContent.slice(0, 160), image:Boolean(f.querySelector('img')),
    svg:f.querySelector('img') ? await (await fetch(f.querySelector('img').src)).text() : null}))));
  report.dialog.forEach((figure, i) => { if (figure.svg) writeFileSync(join(out, `dialog-${i}.svg`), figure.svg); delete figure.svg; });
} finally {
  writeFileSync(join(out, 'report.json'), JSON.stringify(report, null, 2));
  writeFileSync(join(out, 'console.log'), log.join('\n'));
  await browser.close();
}
