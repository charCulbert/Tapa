import './compost/components/compost-slider.js';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const send = text => window.parent.postMessage(encoder.encode(text).buffer, '*');
const controls = Object.fromEntries([...document.querySelectorAll('compost-slider')].map(control => [control.getAttribute('parameter-id'), control]));
const frame = document.querySelector('main');
const decayMode = document.querySelector('.decay-mode');
function showDecayMode(linked) {
  decayMode.setAttribute('aria-pressed', String(linked));
  decayMode.textContent = linked ? 'linked' : 'body';
}
decayMode.addEventListener('click', () => {
  const linked = decayMode.getAttribute('aria-pressed') !== 'true';
  showDecayMode(linked);
  send('begin:7'); send(`value:7:${linked ? 1 : 0}`); send('end:7');
});
let lastHit = 0, flashStarted = -Infinity, flashDuration = 280;
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');

addEventListener('parameter-begin', ({detail}) => {
  controls[detail.parameterID]?.setAttribute('data-editing','');
  send(`begin:${detail.parameterID}`);
});
addEventListener('parameter-edit', ({detail}) => send(`value:${detail.parameterID}:${detail.value}`));
addEventListener('parameter-end', ({detail}) => {
  if (detail.cancelled) send(`value:${detail.parameterID}:${detail.value}`);
  send(`end:${detail.parameterID}`);
  controls[detail.parameterID]?.removeAttribute('data-editing');
});
addEventListener('message', ({data}) => {
  if (!(data instanceof ArrayBuffer) && !ArrayBuffer.isView(data)) return;
  const text = decoder.decode(data);
  if (text.startsWith('visual:')) {
    const values = text.slice(7).split(',').map(Number);
    if (values.length !== 80 || !values.every(Number.isFinite)) return;
    if (values[0] !== 0 && lastHit !== values[0]) {
      flashStarted = performance.now();
      flashDuration = Math.max(1, values[4]);
      frame.style.setProperty('--border-alpha', reducedMotion.matches ? '0' : '.30');
    }
    lastHit = values[0];
    visualUpdated = performance.now();
    operatorFrames = values.slice(8);
  }
  if (!text.startsWith('values:')) return;
  for (const pair of text.slice(7).split(';')) {
    const [id,value] = pair.split('=');
    if (id === '7' && Number.isFinite(Number(value))) showDecayMode(Number(value) >= .5);
    if (id && controls[id] && Number.isFinite(Number(value))) controls[id].value = Number(value);
  }
});
setInterval(() => {
  if (!document.hidden) send('visual');
  const remaining = Math.max(0, 1 - (performance.now() - flashStarted) / flashDuration);
  frame.style.setProperty('--border-alpha', reducedMotion.matches ? '0' : String(.30 * remaining * remaining));
},33);
send('ready');

// Hand pointer capture to the next Compost slider when a drag crosses columns.
const sliderPanel = document.querySelector('.sliders');
let sweep = null;
function finishSweep() {
  const active = sweep;
  sweep = null;
  if (!active) return;
  const {control, pointerId} = active;
  if (control.pointerStart) {
    // A missing release is an ended drag, not a click or double-click.
    control.pointerStart.moved = true;
    control.endPointer({pointerId});
  }
  if (control.input.hasPointerCapture(pointerId)) control.input.releasePointerCapture(pointerId);
}
for (const type of ['pointermove', 'pointerover']) {
  window.addEventListener(type, event => {
    if (event.pointerId !== sweep?.pointerId || event.pointerType === 'touch') return;
    if ((event.buttons & 1) === 0) {
      finishSweep();
      event.stopPropagation();
    }
  }, true);
}
window.addEventListener('pointerup', event => {
  if (event.pointerId === sweep?.pointerId && !event.composedPath().includes(sweep.control)) finishSweep();
}, true);
window.addEventListener('lostpointercapture', event => {
  if (event.pointerId === sweep?.pointerId && event.composedPath().includes(sweep.control)) finishSweep();
}, true);
sliderPanel.addEventListener('pointerdown', event => {
  if (event.button !== 0 || sweep) return;
  const control = event.composedPath().find(node => node instanceof HTMLElement && node.matches('compost-slider'));
  if (control) sweep = { pointerId: event.pointerId, control };
}, true);
sliderPanel.addEventListener('pointermove', event => {
  if (!sweep || event.pointerId !== sweep.pointerId || !sweep.control.pointerStart) return;
  const next = Object.values(controls).find(control => {
    const rect = control.getBoundingClientRect();
    return event.clientX >= rect.left && event.clientX < rect.right;
  });
  if (!next || next === sweep.control) return;
  event.stopPropagation();
  const previous = sweep.control;
  sweep.control = next;
  previous.endPointer(event);
  if (previous.input.hasPointerCapture(event.pointerId)) previous.input.releasePointerCapture(event.pointerId);
  next.lastClickTime = 0;
  // Pointer moves report button=-1; the new slider needs a primary-button start.
  next.beginPointer(new PointerEvent('pointerdown', {
    button: 0, buttons: event.buttons, pointerId: event.pointerId,
    pointerType: event.pointerType, clientX: event.clientX, clientY: event.clientY,
    shiftKey: event.shiftKey
  }));
}, true);
for (const type of ['pointerup', 'pointercancel']) {
  sliderPanel.addEventListener(type, event => {
    if (event.pointerId === sweep?.pointerId) sweep = null;
  }, true);
}
window.addEventListener('blur', finishSweep);
document.addEventListener('visibilitychange', () => { if (document.hidden) finishSweep(); });

const art = document.querySelector('.kick-art');
const context = art.getContext('2d', {willReadFrequently: true});
const snapshots = Array.from({length: 24}, () => ({envelope:0, slap:0, click:0, scatter:0}));
const littleEndian = new Uint8Array(new Uint32Array([1]).buffer)[0] === 1;
const brightness = pixel => littleEndian
  ? (pixel & 255) * 54 + ((pixel >>> 8) & 255) * 183 + ((pixel >>> 16) & 255) * 19
  : (pixel >>> 24) * 54 + ((pixel >>> 16) & 255) * 183 + ((pixel >>> 8) & 255) * 19;
const ascending = (a,b) => brightness(a) - brightness(b);
const descending = (a,b) => brightness(b) - brightness(a);
const cleanTexture = document.createElement('canvas');
let original, image, textureRatio;
const wander = [{position: 0, velocity: 0}, {position: 0, velocity: 0}];
let operatorFrames = Array(72).fill(0), visualUpdated = -Infinity, lastDraw = -Infinity;
function animateTexture(now) {
  requestAnimationFrame(animateTexture);
  if (document.hidden || now - lastDraw < 32) return;
  const dt = Math.min(.05, Math.max(0, (now - lastDraw) / 1000));
  lastDraw = now;
  const drift = reducedMotion.matches ? 0
    : Math.pow(Math.max(0, 1 - (now - flashStarted) / flashDuration), 2);
  const angle = lastHit * 2.399963;
  for (const axis of wander) {
    if (reducedMotion.matches) { axis.position = 0; axis.velocity = 0; continue; }
    axis.velocity = axis.velocity * Math.exp(-dt * .5)
      + (Math.random() * 2 - 1) * Math.sqrt(dt) * .60;
    axis.position += axis.velocity * dt;
    if (Math.abs(axis.position) > 3.5) {
      axis.position = Math.sign(axis.position) * 3.5;
      axis.velocity *= -.7;
    }
  }
  const [wanderX, wanderY] = wander.map(axis => axis.position);
  // Overscan covers the maximum wander plus the small note nudge.
  art.style.transform = `translate(${wanderX + Math.cos(angle) * drift * .35}%, ${wanderY + Math.sin(angle) * drift * .35}%) scale(1.09)`;
  // Sorting at CSS resolution keeps the texture grain and bounds the UI work.
  const width = Math.max(1, Math.round(art.clientWidth));
  const height = Math.max(1, Math.round(art.clientHeight));
  const ratio = controls[6].value;
  if (!original || art.width !== width || art.height !== height || textureRatio !== ratio) {
    textureRatio = ratio;
    art.width = width; art.height = height;
    context.fillStyle = '#000';
    context.fillRect(0, 0, width, height);
    const glyphs = [
      ['11111','00100','00100','00100','00100','00100','00100'],
      ['01110','10001','10001','11111','10001','10001','10001'],
      ['11110','10001','10001','11110','10000','10000','10000'],
      ['01110','10001','10001','11111','10001','10001','10001']
    ];
    const fontSize = Math.max(4, Math.min(7, width / 110 * Math.pow(2.6025 / ratio, .18)));
    context.font = `400 ${fontSize}px "SFMono-Regular", Menlo, monospace`;
    context.textBaseline = 'top';
    context.fillStyle = '#b8b8b8';
    const characterWidth = context.measureText('t').width;
    // Large letter silhouettes made entirely from fine, repeating text.
    for (let y = 0, row = 0; y < height; y += fontSize, ++row) {
      const gy = Math.floor(y / height * 9) - 1;
      if (gy < 0 || gy >= 7) continue;
      for (let x = 0, column = 0; x < width; x += characterWidth, ++column) {
        const gx = Math.floor(x / width * 25) - 1;
        if (gx < 0 || gx >= 23 || gx % 6 === 5) continue;
        if (glyphs[Math.floor(gx / 6)][gy][gx % 6] === '1')
          context.fillText('TAPA'[(row + column) % 4], x, y);
      }
    }
    cleanTexture.width = width; cleanTexture.height = height;
    cleanTexture.getContext('2d').drawImage(art, 0, 0);
    original = context.getImageData(0, 0, width, height);
    image = context.createImageData(width, height);
  }
  image.data.set(original.data);
  const pixels = new Uint32Array(image.data.buffer);
  const stale = now - visualUpdated > 150;
  const decay = Math.max(0, Math.min(1, (controls[1].value - 20) / 3980));
  const fm = controls[2].value / 100;
  const transient = controls[3].value / 100;
  const saturation = controls[4].value / 12;
  const feedback = controls[5].value / 100;
  const noise = controls[8].value / 100;
  snapshots.forEach((snapshot, group) => {
    const base = group * 3;
    snapshot.scatter += ((Math.random() * 2 - 1) - snapshot.scatter) * .22;
    snapshot.envelope += ((stale ? 0 : operatorFrames[base]) - snapshot.envelope) * .4;
    snapshot.slap += ((stale ? 0 : operatorFrames[base + 1]) - snapshot.slap) * .35;
    snapshot.click += ((stale ? 0 : operatorFrames[base + 2]) - snapshot.click) * .35;
  });
  const length = Math.round(8 + decay * 150);
  const period = Math.round(12 - saturation * 9);
  for (let y = 0; y < height; y += period) {
    const band = Math.floor(y / period);
    const sample = snapshots[band % snapshots.length];
    const signal = reducedMotion.matches ? 0 : sample.envelope * sample.slap;
    const edge = reducedMotion.matches ? 0 : sample.envelope * sample.click;
    const scatter = reducedMotion.matches ? 0 : sample.scatter * (Math.abs(signal) + Math.abs(edge)) * (20 + feedback * 50);
    const offset = Math.round(noise * Math.sin(band * 13.71) * 50 + scatter + fm * 50 + feedback * Math.sin(band * ratio * .65) * 45 + signal * 36 + edge * 24);
    const bandLength = Math.max(4, Math.round(length * (1 + fm * .8 + Math.min(4, Math.abs(signal) * 1.2 + Math.abs(edge) + Math.abs(scatter) * .035))));
    const tear = Math.round(Math.max(-90, Math.min(90, fm * Math.sin(band * ratio * .3) * 12 + scatter + signal * (10 + fm * 30) + edge * 35)));
    const reverse = (band + Math.round(transient * 11)) % 2;
    const thickness = 1 + Math.round(transient * 3 + Math.min(3, Math.abs(edge) * 3));
    for (let dy = 0; dy < Math.min(thickness, period) && y + dy < height; ++dy) {
      const row = (y + dy) * width;
      // Rotate row fragments without discarding pixels or changing their colours.
      const shift = ((tear % width) + width) % width;
      if (shift) {
        const strip = pixels.subarray(row, row + width);
        strip.reverse();
        strip.subarray(0, shift).reverse();
        strip.subarray(shift).reverse();
      }
      for (let x = ((offset % bandLength) + bandLength) % bandLength; x < width; x += bandLength)
        pixels.subarray(row + x, row + Math.min(x + bandLength, width)).sort(reverse ? descending : ascending);
    }
  }
  context.putImageData(image, 0, 0);
  // Retain a faint intact shape beneath the displaced text.
  context.globalAlpha = .25;
  context.drawImage(cleanTexture, 0, 0);
  context.globalAlpha = 1;
}
requestAnimationFrame(animateTexture);
