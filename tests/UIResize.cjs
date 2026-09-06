// Run after the native build: NODE_PATH=/path/to/node_modules node tests/UIResize.cjs
const assert = require('node:assert/strict');
const http = require('node:http');
const fs = require('node:fs/promises');
const path = require('node:path');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../build-native/tapa-resources');
(async () => {
  const server = http.createServer(async (request, response) => {
    try {
      const file = path.resolve(root, '.' + new URL(request.url, 'http://localhost').pathname);
      if (!file.startsWith(root + path.sep)) throw Error('Invalid path');
      response.setHeader('Content-Type', file.endsWith('.js') ? 'text/javascript' : 'text/html');
      response.end(await fs.readFile(file));
    } catch { response.writeHead(404).end(); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({headless:true});
    for (const deviceScaleFactor of [1, 2]) {
      const page = await browser.newPage({deviceScaleFactor});
      const errors = [];
      page.on('pageerror', error => errors.push(error.message));
      await page.goto(`http://127.0.0.1:${server.address().port}/ui/index.html`);
      const mode = page.getByRole('button', {name:'Link FM decay to body'});
      await mode.click();
      assert.equal(await mode.getAttribute('aria-pressed'), 'true');
      assert.equal(await mode.textContent(), 'linked');
      await mode.press('Space');
      assert.equal(await mode.getAttribute('aria-pressed'), 'false');
      assert.equal(await mode.textContent(), 'body');
      await page.evaluate(() => window.postMessage(new TextEncoder().encode('values:7=1;').buffer, '*'));
      await page.waitForFunction(() => document.querySelector('.decay-mode').getAttribute('aria-pressed') === 'true');
      assert.equal(await mode.textContent(), 'linked');
      for (const [width, height] of [[640,460], [420,220], [1280,800], [320,180], [1920,1080], [640,460]]) {
        await page.setViewportSize({width, height});
        await page.waitForFunction(() => {
          const c = document.querySelector('canvas');
          return c.width === innerWidth && c.height === innerHeight;
        });
        const layout = await page.evaluate(() => {
          const c = document.querySelector('canvas');
          const controls = [...document.querySelectorAll('compost-slider')];
          return {
            overflow: document.documentElement.scrollWidth > innerWidth || document.documentElement.scrollHeight > innerHeight,
            rects: controls.map(control => {
              const r = control.getBoundingClientRect();
              return {x:r.x, y:r.y, width:r.width, height:r.height};
            }),
            drawn: c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i) => i % 4 !== 3 && v > 0)
          };
        });
        assert.equal(layout.overflow, false, `overflow at ${width}x${height}`);
        assert.equal(layout.drawn, true);
        layout.rects.forEach((r,i) => {
          assert.ok(Math.abs(r.x - i * width / 7) < 1);
          assert.ok(Math.abs(r.width - width / 7) < 1);
          assert.equal(r.height, height);
        });
        await page.mouse.move(width / 14, height * .3);
        await page.mouse.down();
        for (let i = 1; i < 7; ++i) await page.mouse.move((i + .5) * width / 7, height * .3, {steps:3});
        await page.mouse.up();
        const values = await page.evaluate(() => [...document.querySelectorAll('compost-slider')].map(c =>
          (c.value - Number(c.getAttribute('min'))) / (Number(c.getAttribute('max')) - Number(c.getAttribute('min')))));
        assert.ok(values.every(value => Math.abs(value - .7) < .025), `sweep values: ${values}`);
      }
      assert.deepEqual(errors, []);
      // Model a native WebView missing mouse-up outside its window. The first
      // unpressed re-entry event must end the gesture before changing any value.
      for (const type of ['pointermove', 'pointerover', 'blur', 'lostpointercapture', 'outside']) {
        await page.evaluate(() => {
          window.dragEnds = 0;
          document.querySelectorAll('compost-slider').forEach(c => { c.lastClickTime = 0; });
          window.countDragEnd ??= () => ++window.dragEnds;
          window.removeEventListener('parameter-end', window.countDragEnd);
          window.addEventListener('parameter-end', window.countDragEnd);
        });
        await page.mouse.move(40,200);
        await page.mouse.down();
        await page.mouse.move(40,220);
        if (type === 'outside') await page.mouse.move(-40,-40);
        const before = await page.evaluate(() => [...document.querySelectorAll('compost-slider')].map(c => c.value));
        await page.evaluate(type => {
          if (type === 'outside') return;
          const control = document.querySelector('compost-slider');
          const pointerId = control.pointerStart.pointerId;
          if (type === 'blur') window.dispatchEvent(new Event('blur'));
          else if (type === 'lostpointercapture') control.input.releasePointerCapture(pointerId);
          else control.input.dispatchEvent(new PointerEvent(type, {
            bubbles:true, composed:true, pointerId, pointerType:'mouse', buttons:0, clientX:400, clientY:60
          }));
        }, type);
        await page.mouse.up();
        await page.mouse.move(400,60);
        const after = await page.evaluate(() => ({
          values:[...document.querySelectorAll('compost-slider')].map(c => c.value),
          dragging:[...document.querySelectorAll('compost-slider')].some(c => c.pointerStart || c.hasAttribute('data-editing')),
          ends:window.dragEnds
        }));
        assert.deepEqual(after.values, before, type);
        assert.equal(after.dragging, false, type);
        assert.equal(after.ends, 1, type);
      }
      assert.deepEqual(errors, []);
      await page.close();
    }
    console.log('Resize, texture coverage, cross-slider dragging, and outside-release cleanup passed at 1x/2x display scale.');
  } finally {
    if (browser) await browser.close();
    server.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
