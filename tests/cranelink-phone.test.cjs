const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const {test} = require('node:test');
const {runInNewContext} = require('node:vm');

const script = readFileSync(join(__dirname, '../CraneLink_PhoneBench/PhonePage.h'), 'utf8')
  .split('<script>')[1].split('</script>')[0];
const flush = () => new Promise(setImmediate);
const reply = (body, ok = true) => ({ok, json:async () => body});
const sessionId = '0123456789abcdef';

function element(mask) {
  return {
    dataset: {mask}, events: {}, children: [], setAttribute() {}, setPointerCapture() {},
    appendChild(child) { this.children.push(child); },
    addEventListener(name, fn) { this.events[name] = fn; },
    getBoundingClientRect() { return {left:0, right:100, top:0, bottom:100}; }
  };
}

async function page() {
  const elements = Object.fromEntries(['run','stop','reset','message','motion','outputs'].map(id => [id, element()]));
  const buttons = [element('1'), element('2')], calls = [], intervals = {};
  let override;
  const document = {
    hidden: false, events: {}, getElementById: id => elements[id] ??= element(),
    querySelectorAll: () => buttons,
    createElement: () => element(),
    addEventListener(name, fn) { this.events[name] = fn; }
  };
  runInNewContext(script, {
    document, window: element(), setTimeout, clearTimeout, AbortController, URLSearchParams,
    setInterval: (fn, ms) => { intervals[ms] = fn; },
    fetch: async (url, options) => {
      const path = url.replace('/api/', '');
      const fields = options.body ? Object.fromEntries(options.body) : {};
      calls.push({path, ...fields});
      const result = override?.(path, fields);
      if (result) return result;
      if (path === 'arm') return reply({token:sessionId});
      if (path === 'jog') {
        const mask = Number(fields.mask);
        return reply({armed:true, stopped:false, mask, relayMask:mask ? mask | 128 : 0});
      }
      return reply({armed:false, stopped:path === 'stop', mask:0, relayMask:0});
    }
  });
  await flush();
  return {elements, buttons, calls, document, tick: intervals[110], refresh: intervals[1200],
    mock: fn => { override = fn; }};
}

const touch = {button:0, pointerId:7};
const jogs = page => page.calls.filter(call => call.path === 'jog').map(call => Number(call.mask));

test('two pointers can hold directions and releasing one preserves the other', async () => {
  const p = await page();
  await p.elements.run.events.click();
  let finishHold;
  const hold = new Promise(resolve => { finishHold = resolve; });
  p.mock((path, fields) => path === 'jog' && fields.mask === '1' ? hold : null);
  p.buttons[0].events.pointerdown(touch);
  p.buttons[1].events.pointerdown({...touch, pointerId:8});
  await p.tick();
  assert.deepEqual(jogs(p), [0, 1]);
  finishHold(reply({armed:true, stopped:false, mask:1, relayMask:129}));
  await flush();
  assert.deepEqual(jogs(p), [0, 1, 3]);
  p.buttons[0].events.lostpointercapture(touch);
  await flush();
  assert.deepEqual(jogs(p), [0, 1, 3, 2]);
  assert.equal(p.elements.motion.textContent, 'Down');
});

test('leaving while arming releases the late token and never sends a jog', async () => {
  const p = await page();
  let finishArm;
  const arm = new Promise(resolve => { finishArm = resolve; });
  p.mock(path => path === 'arm' ? arm : null);
  const click = p.elements.run.events.click();
  p.document.hidden = true;
  p.document.events.visibilitychange();
  finishArm(reply({token:sessionId}));
  await click;
  assert.deepEqual(jogs(p), []);
  assert.deepEqual(p.calls.at(-1), {path:'end', token:sessionId});
});

test('a rejected heartbeat ends Web Run and requires a new explicit enable', async () => {
  const p = await page();
  await p.elements.run.events.click();
  p.mock(path => path === 'jog' ? reply({error:'Expired'}, false) : null);
  await p.tick();
  p.buttons[0].events.pointerdown(touch);
  await p.tick();
  assert.deepEqual(jogs(p), [0, 0]);
  assert.deepEqual(p.calls.at(-1), {path:'end', token:sessionId, leaving:'1'});
  assert.match(p.elements.message.textContent, /enable Web Run again/);
});

test('a stop cannot be overwritten by a late jog response', async () => {
  const p = await page();
  await p.elements.run.events.click();
  let finishHold;
  const hold = new Promise(resolve => { finishHold = resolve; });
  p.mock((path, fields) => path === 'jog' && fields.mask === '1' ? hold : null);
  p.buttons[0].events.pointerdown(touch);
  await p.elements.stop.events.click();
  finishHold(reply({armed:true, stopped:false, mask:1, relayMask:129}));
  await flush();
  assert.equal(p.elements.motion.textContent, 'Stop latched');
  assert.equal(p.elements.outputs.textContent, 'All off');
  assert.equal(p.elements.run.disabled, true);
});

test('a second tab observes Web Run without joining the existing session', async () => {
  const p = await page();
  p.mock(path => path === 'status' ? reply({armed:true, stopped:false, mask:1, relayMask:129}) : null);
  await p.refresh();
  await p.tick();
  assert.equal(p.elements.run.disabled, true);
  assert.equal(p.elements.outputs.textContent, 'R1 + R8');
  assert.match(p.elements.message.textContent, /Another tab/);
  assert.deepEqual(p.calls.map(call => call.path), ['status', 'status']);
});

test('idle status still shows a latched Power relay and its remaining time', async () => {
  const p = await page();
  p.mock(path => path === 'status' ? reply({
    armed:false, stopped:false, mask:0, relayMask:64, auxOn:true, auxSeconds:125
  }) : null);
  await p.refresh();
  assert.equal(p.elements.outputs.textContent, 'R7');
  assert.equal(p.elements.auxState.textContent, 'On · 2:05 idle timeout');
});

test('selecting an assigned relay swaps it and saves the pair in one request', async () => {
  const p = await page();
  const settings = {idleMinutes:60, mapping:'1234567', receiverMac:'02:00:00:00:00:01',
    senderMac:'', radioReady:true, channel:6, linked:false};
  p.mock((path, fields) => path === 'settings' ? reply({
    ...settings, mapping:fields.mapping || settings.mapping
  }) : null);
  p.elements.settings.open = true;
  await p.elements.settings.events.toggle();
  const selects = p.elements.assignments.children.map(label => label.children[0]);
  const submit = () => p.elements.settingsForm.events.submit({preventDefault() {}});
  selects[0].value = '2';
  selects[0].events.change();
  assert.equal(selects[1].value, '1');
  p.elements.idleMinutes.value = '90';
  await submit();
  assert.deepEqual(p.calls.at(-1), {path:'settings', idleMinutes:'90', mapping:'2134567', interlock:'0'});
});
