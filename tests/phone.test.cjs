const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {test} = require('node:test');
const {runInNewContext} = require('node:vm');

const script = readFileSync(require('node:path').join(__dirname, '../CraneBench/PhonePage.h'), 'utf8')
  .split('<script>')[1].split('</script>')[0];
const flush = () => new Promise(setImmediate);

function page() {
  const arm = {}, stop = {}, state = {}, buttons = [], events = {}, calls = [];
  let tick, block;
  const document = {
    hidden: false,
    querySelector: id => ({'#arm': arm, '#stop': stop, '#state': state, '#grid': {appendChild: b => buttons.push(b)}})[id],
    createElement: () => ({setPointerCapture() {}}),
    addEventListener: (name, handler) => { events[name] = handler; }
  };
  runInNewContext(script, {
    document, window: {addEventListener: (name, handler) => { events[name] = handler; }},
    AbortController, URLSearchParams, setTimeout, clearTimeout,
    setInterval: handler => { tick = handler; },
    fetch: async (path, options) => {
      calls.push({path, ...Object.fromEntries(options.body)});
      return block ? block : {ok:true, text: async () => path === '/claim' ? '0123456789abcdef' : 'OK'};
    }
  });
  return {arm, stop, state, buttons, events, calls, document, tick: () => tick(), blockOn: p => { block = p; }};
}

const finger = {button: 0, pointerId: 5};
const masks = p => p.calls.filter(c => c.path === '/command').map(c => Number(c.mask));

test('arming sends neutral; release during an in-flight hold sends neutral next', async () => {
  const p = page();
  assert.equal(p.buttons.length, 7);
  await p.arm.onclick();
  let finish;
  p.blockOn(new Promise(resolve => { finish = resolve; }));
  p.buttons[6].onpointerdown(finger);
  p.buttons[6].onpointerup(finger);
  p.buttons[0].onpointerdown(finger);
  assert.deepEqual(masks(p), [0, 64]);
  p.blockOn(null);
  finish({ok:true, text: async () => 'OK'});
  await flush();
  assert.deepEqual(masks(p), [0, 64, 0]);
});

test('simulated stop survives release during a pending hold and disables the phone session', async () => {
  const p = page();
  await p.arm.onclick();
  let finish;
  p.blockOn(new Promise(resolve => { finish = resolve; }));
  p.buttons[0].onpointerdown(finger);
  p.stop.onclick();
  p.buttons[0].onpointerup(finger);
  p.blockOn(null);
  finish({ok:true, text: async () => 'OK'});
  await flush();
  await p.tick();
  assert.deepEqual(masks(p), [0, 1, 128]);
  assert.match(p.state.textContent, /stop latched/);
});

test('hiding the page cancels a claim and prevents further commands', async () => {
  const p = page();
  let finish;
  p.blockOn(new Promise(resolve => { finish = resolve; }));
  const pending = p.arm.onclick();
  p.document.hidden = true;
  p.events.visibilitychange();
  finish({ok:true, text: async () => '0123456789abcdef'});
  await pending;
  p.document.hidden = false;
  p.buttons[0].onpointerdown(finger);
  await p.tick();
  assert.deepEqual(masks(p), []);
  assert.match(p.state.textContent, /Disabled/);
});

test('a rejected heartbeat disables commands until another explicit enable', async () => {
  const p = page();
  await p.arm.onclick();
  p.blockOn(Promise.resolve({ok:false}));
  await p.tick();
  assert.match(p.state.textContent, /Session lost/);
  p.buttons[0].onpointerdown(finger);
  await p.tick();
  assert.deepEqual(masks(p), [0, 0]);
});
