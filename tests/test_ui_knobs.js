/*
UI KNOB TESTS  (node tests/test_ui_knobs.js)

Checks the two bits of ui/amp.js that are easy to get quietly wrong:

  - typed entry: out-of-range snaps to the nearer limit, percent knobs are
    edited in percent, junk reverts instead of sending a garbage value
  - drag sending: with SEND_WHILE_TURNING = false the amp hears one value,
    on release, and it is the one the knob landed on

There is no browser here. We stub the handful of DOM calls makeKnob uses, then
eval the REAL makeKnob source sliced out of ui/amp.js - so this tests the code
that actually ships, not a copy of it. If makeKnob is renamed or its helpers
move, the slice below fails loudly rather than silently testing nothing.
*/
'use strict';
const fs = require('fs');
const path = require('path');

const AMP_JS = path.join(__dirname, '..', 'ui', 'amp.js');
const src = fs.readFileSync(AMP_JS, 'utf8');

/* ---------- slice out the real code under test ---------- */
function slice(from, toMarker) {
  const a = src.indexOf(from);
  if (a < 0) throw new Error(`ui/amp.js no longer contains: ${from}`);
  const m = src.indexOf(toMarker, a);
  if (m < 0) throw new Error(`ui/amp.js no longer contains: ${toMarker}`);
  return src.slice(a, src.indexOf('}', src.indexOf('\n', m)) + 1);
}
const knobSrc = slice('const fmt = (p,v) => p.pct', 'return { el:wrap, knob:k, inp:val, paint };');

// the page's own setting, so this test follows whatever it is set to
const modeMatch = src.match(/const SEND_WHILE_TURNING = (true|false);/);
if (!modeMatch) throw new Error('ui/amp.js no longer defines SEND_WHILE_TURNING');
const SEND_WHILE_TURNING = modeMatch[1] === 'true';

/* ---------- minimal DOM ---------- */
const handlers = new WeakMap();
function makeEl(tag) {
  const o = {
    tagName: tag, className: '', textContent: '', value: '', disabled: false, children: [],
    style: { setProperty() {} },
    classList: { toggle() {}, add() {}, remove() {}, contains() { return false; } },
    setAttribute(k, v) { o['_' + k] = v; },
    getAttribute(k) { return o['_' + k]; },
    append(...c) { o.children.push(...c); },
    appendChild(c) { o.children.push(c); },
    addEventListener(ev, fn) { handlers.get(o)[ev] = fn; },
    select() {}, blur() { global.document.activeElement = null; }, focus() {},
    setPointerCapture() {}, querySelector() { return makeEl('div'); }
  };
  handlers.set(o, {});
  return o;
}
global.document = { createElement: makeEl, activeElement: null };
global.performance = { now: () => Date.now() };
function fire(node, ev, e = {}) {
  const h = handlers.get(node)[ev];
  if (h) h({ preventDefault() {}, stopPropagation() {}, ...e });
}
let busMsg = '';
global.bus = m => { busMsg = m; };                  // applyTyped reports clamping through this
global.SEND_WHILE_TURNING = SEND_WHILE_TURNING;     // read by the drag handler

// new Function rather than eval: this file is strict mode, where eval keeps its
// declarations to itself, so the sliced code has to hand them back explicitly.
const { makeKnob, fmt } = new Function(
  knobSrc + '\nreturn { fmt, toFrac, fromFrac, makeKnob };'
)();

/* ---------- harness ---------- */
let fails = 0;
function check(ok, name, extra = '') {
  console.log((ok ? 'PASS' : 'FAIL') + '  ' + name + (ok ? '' : '   ' + extra));
  if (!ok) fails++;
}
function mount(p) {
  let v = p.def;
  const sent = [];
  const kn = makeKnob(p, () => v, nv => { v = nv; }, nv => sent.push(nv), 'porcelain');
  return {
    kn, sent,
    get: () => v,
    type(txt) { global.document.activeElement = null; kn.inp.value = txt; fire(kn.inp, 'blur'); },
    drag(px) {
      fire(kn.knob, 'pointerdown', { clientY: 200, pointerId: 1 });
      for (let y = 199; y >= 200 - px; y--) fire(kn.knob, 'pointermove', { clientY: y });
      fire(kn.knob, 'pointerup', {});
    }
  };
}

/* real parameter definitions, matching ui/amp.js */
const gain  = { name: 'adj_gain',   label: 'Gain', min: 0, max: 20, def: 1, step: .1, dec: 1, curve: 2.5 };
const mix   = { name: 'wah_mix',    label: 'Mix',  min: 0, max: 1,  def: 1, step: .01, pct: true };
const eqlow = { name: 'adj_eq_low', label: 'Bass', min: -15, max: 15, def: 0, step: .5, dec: 1, unit: ' dB', signed: true };
const gate  = { name: 'adj_gate',   label: 'Threshold', min: -90, max: 0, def: -50, step: 1, dec: 0, unit: ' dB' };

console.log('--- typed entry: in range ---');
{
  const m = mount(gain); m.type('7.5');
  check(m.get() === 7.5, 'a plain number is accepted', 'got ' + m.get());
  check(m.sent.length === 1 && m.sent[0] === 7.5, 'and is sent exactly once', JSON.stringify(m.sent));
}

console.log('\n--- typed entry: past a limit snaps to it ---');
{ const m = mount(gain);  m.type('999');  check(m.get() === 20,  'above max snaps to max', 'got ' + m.get()); }
{ const m = mount(gain);  m.type('-40');  check(m.get() === 0,   'below min snaps to min', 'got ' + m.get()); }
{ const m = mount(eqlow); m.type('100');  check(m.get() === 15,  'eq above max snaps to +15 dB', 'got ' + m.get()); }
{ const m = mount(eqlow); m.type('-100'); check(m.get() === -15, 'eq below min snaps to -15 dB', 'got ' + m.get()); }
{ const m = mount(gate);  m.type('-500'); check(m.get() === -90, 'gate below min snaps to -90 dBFS', 'got ' + m.get()); }
{
  const m = mount(gain); busMsg = ''; m.type('999');
  check(/limited/i.test(busMsg), 'and says so rather than silently clamping', 'bus said: ' + busMsg);
}

console.log('\n--- percent knobs are edited in percent ---');
{ const m = mount(mix); m.type('50');  check(Math.abs(m.get() - 0.5) < 1e-9, 'typing 50 gives 0.5, not 50', 'got ' + m.get()); }
{ const m = mount(mix); m.type('50%'); check(Math.abs(m.get() - 0.5) < 1e-9, 'a trailing % is tolerated', 'got ' + m.get()); }
{ const m = mount(mix); m.type('250'); check(m.get() === 1, 'above 100% snaps to 1.0', 'got ' + m.get()); }

console.log('\n--- units and junk ---');
{ const m = mount(eqlow); m.type('+6.0 dB'); check(m.get() === 6, 'the displayed unit can be left in place', 'got ' + m.get()); }
{ const m = mount(gain); m.type('banana'); check(m.get() === 1 && m.sent.length === 0, 'gibberish reverts and sends nothing', 'got ' + m.get()); }
{ const m = mount(gain); m.type('');       check(m.get() === 1 && m.sent.length === 0, 'empty reverts and sends nothing', 'got ' + m.get()); }
{ const m = mount(gain); m.type('1');      check(m.sent.length === 0, 'retyping the same value sends nothing', JSON.stringify(m.sent)); }

console.log('\n--- quantising ---');
{ const m = mount(gain); m.type('7.54'); check(m.get() === 7.5, '7.54 quantises to the 0.1 step', 'got ' + m.get()); }

console.log(`\n--- dragging (SEND_WHILE_TURNING = ${SEND_WHILE_TURNING}) ---`);
if (!SEND_WHILE_TURNING) {
  const m = mount(gain);
  fire(m.kn.knob, 'pointerdown', { clientY: 200, pointerId: 1 });
  for (let y = 199; y >= 160; y--) fire(m.kn.knob, 'pointermove', { clientY: y });
  check(m.sent.length === 0, 'nothing is sent while the knob is moving', JSON.stringify(m.sent));
  fire(m.kn.knob, 'pointerup', {});
  check(m.sent.length === 1, 'exactly one send on release', JSON.stringify(m.sent));
  check(m.sent[0] === m.get(), 'and it is the value the knob landed on', m.sent[0] + ' vs ' + m.get());
} else {
  const m = mount(gain); m.drag(40);
  check(m.sent.length > 1, 'live mode streams values while turning', JSON.stringify(m.sent));
  check(m.sent[m.sent.length - 1] === m.get(), 'and the last one is where it landed');
}
{ const m = mount(gain); m.drag(0); check(m.sent.length === 0, 'a click with no movement sends nothing', JSON.stringify(m.sent)); }

console.log(fails ? `\n${fails} UI test(s) FAILED` : '\nall UI knob tests passed');
process.exit(fails ? 1 : 0);
