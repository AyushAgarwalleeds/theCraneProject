import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import {createContext, runInContext} from 'node:vm';
import {acknowledge, FirmwareUpdateRequired, identify, readBoardInfo, withBoard} from '../docs/serial.mjs';
import {buildReport} from '../docs/report.mjs';
import {PDFDocument} from '../docs/vendor/pdf-lib.mjs';

function usbDevice(onLine) {
  let output;
  const out = new TextEncoder();
  const port = {
    signals: [],
    async open() {
      this.readable = new ReadableStream({start(controller) { output = controller; }});
      this.writable = new WritableStream({write(bytes) {
        const response = onLine(new TextDecoder().decode(bytes).trim());
        for (const part of response) output.enqueue(out.encode(part));
      }});
    },
    async setSignals(signals) { this.signals.push(signals); },
    async close() { this.closed = true; },
  };
  return port;
}

test('USB diagnostics assemble fragmented receiver replies and release the port', async () => {
  const port = usbDevice(line => line === 'identify'
    ? ['boot text\r\nCL IDENT REC', 'EIVER 10:06:1C:00:00:01 10:06:1C:00:00:01\r\n']
    : ['CL WIFI CraneLink-Bench-000002 1234567890ABCDEF 192.168.4.1\nCL RADIO 6 1\n',
       'CL STATE 1 0\nCL POWER 9 2 0\nCL SLOT 1 02:06:1C:00:00:02 0\nCL INFO DONE\n']);
  const info = await withBoard(port, async command => readBoardInfo(command, await identify(command)));
  assert.equal(info.password, '1234567890ABCDEF');
  assert.equal(info.resetReason, 9);
  assert.equal(info.stopped, true);
  assert.deepEqual(info.slots, [{slot:1, mac:'02:06:1C:00:00:02', linked:false}]);
  assert.equal(port.closed, true);
  assert.deepEqual(port.signals, [{dataTerminalReady:false, requestToSend:false}]);
  assert.equal(port.readable.locked, false);
  assert.equal(port.writable.locked, false);
  const pdf = await PDFDocument.load(await buildReport([info]));
  assert.equal(pdf.getTitle(), 'CraneLink setup report');
  assert.equal(pdf.getPageCount(), 1);
});

test('USB diagnostics distinguish a saved peer from a live link and report grounded GPIOs', async () => {
  const port = usbDevice(line => line === 'identify'
    ? ['CL IDENT SENDER 02:06:1C:00:00:02 10:06:1C:00:00:02\n']
    : ['CL FEATURES MASTER BATTERY WIRELESS\n',
       'CL SETUP AP CraneLink-TX-10061C000002 1234567890ABCDEF12345678 192.168.4.1 0\n',
       'CL TARGET 10:06:1C:00:00:01 6 1 0\nCL INPUTS 81\nCL INFO DONE\n']);
  const info = await withBoard(port, async command => readBoardInfo(command, await identify(command)));
  assert.equal(info.ready, true);
  assert.equal(info.linked, false);
  assert.equal(info.inputs, 0x81);
  assert.equal(info.wirelessEditable, true);
  assert.deepEqual(info.setupAp, {ssid:'CraneLink-TX-10061C000002', password:'1234567890ABCDEF12345678',
    ip:'192.168.4.1', active:false});
});

test('master setup keeps existing sender slots and saves distinct keys in receiver order', async () => {
  const senderMac = '02:00:00:00:00:01';
  const saved = new Map();
  let pending = [], targets = [];
  const sender = usbDevice(line => {
    if (line === 'identify') return [`CL IDENT SENDER ${senderMac} ${senderMac}\n`];
    if (line === 'targets-begin 2') { pending = []; return ['CL TARGETS READY\n']; }
    if (line.startsWith('targets-add ')) {
      pending.push(line.split(' ').slice(1));
      return ['CL TARGET ADDED\n'];
    }
    if (line === 'targets-save') { targets = pending; return ['CL TARGETS SAVED\n']; }
    return ['CL PROTOCOL 4\nCL FEATURES MASTER BATTERY\nCL BATTERY 1 3480 1\n',
      ...targets.map(([mac, , name], i) => `CL PICK ${i + 1} ${mac} ${name} ${Number(!i)}\n`),
      `CL TARGET ${targets[0]?.[0] ?? '-'} 6 ${Number(!!targets.length)} 0\nCL INFO DONE\n`];
  });
  const receiver = (mac, name) => usbDevice(line => {
    if (line === 'identify') return [`CL IDENT RECEIVER ${mac} ${mac}\n`];
    if (line.startsWith('peer ')) {
      const [, slot, sender, key] = line.split(' ');
      assert.equal(slot, '2');
      assert.equal(sender, senderMac);
      saved.set(mac, key);
      return ['CL PEER 2\n'];
    }
    return [`CL PROTOCOL 4\nCL RADIO 6 1\nCL WIFI ${name} examplepass 192.168.4.1\n`,
      'CL SLOT 1 02:00:00:00:00:FF 1\n',
      saved.has(mac) ? `CL SLOT 2 ${senderMac} 0\n` : '', 'CL INFO DONE\n'];
  });
  const first = receiver('10:06:1C:00:00:01', 'MH14AB4292');
  const second = receiver('10:06:1C:00:00:02', 'MH14AB4293');
  const ports = [sender, first, first, second, sender];
  const elements = new Map();
  function element(id) {
    if (!elements.has(id)) elements.set(id, {value:'2', events:{}, classList:{toggle() {}},
      addEventListener(name, fn) { this.events[name] = fn; }, replaceChildren() {}, setAttribute() {}});
    return elements.get(id);
  }
  const reports = [];
  const ctx = createContext({acknowledge, FirmwareUpdateRequired, identify, readBoardInfo, withBoard,
    document:{getElementById:element, createElement:() => ({setAttribute() {}})},
    crypto, supported:true, requestPort:async () => ports.shift(),
    renderReport:(_, boards) => reports.push(boards)});
  const script = readFileSync(new URL('../docs/setup.js', import.meta.url), 'utf8').replace(/^import .*;\r?\n/gm, '');
  runInContext(script, ctx);
  element('masterPlan').events.submit({preventDefault() {}});
  for (let i = 0; i < 3; ++i) await runInContext('runMaster()', ctx);
  assert.match(element('masterStatus').textContent, /already in the list/);
  assert.equal(saved.size, 1);
  for (let i = 0; i < 2; ++i) await runInContext('runMaster()', ctx);
  assert.match(element('masterStatus').textContent, /saved and verified/);
  assert.notEqual(...saved.values());
  assert.deepEqual(targets.map(([mac, key, name]) => [key, name]), [
    [saved.get('10:06:1C:00:00:01'), 'MH14AB4292'],
    [saved.get('10:06:1C:00:00:02'), 'MH14AB4293'],
  ]);
  const installed = reports.at(-1).find(board => board.role === 'sender');
  assert.equal(installed.targets[0].selected, true);
  assert.deepEqual(installed.battery, {enabled:true, millivolts:3480, low:true});
});

test('USB diagnostics identify older firmware on both board roles', async () => {
  for (const [role, reply] of [
    ['RECEIVER', 'CL ERROR peer not saved'],
    ['SENDER', 'Use: pair RECEIVER_MAC 32_HEX_KEY'],
  ]) {
    const port = usbDevice(line => line === 'identify'
      ? [`CL IDENT ${role} 10:06:1C:00:00:01 10:06:1C:00:00:01\n`]
      : [reply + '\n']);
    await assert.rejects(withBoard(port, async command => readBoardInfo(command, await identify(command))),
      FirmwareUpdateRequired);
    assert.equal(port.closed, true);
  }
});

test('USB identification reports a transport failure without retrying it', async () => {
  let calls = 0;
  const failure = Error('USB disconnected before the board replied.');
  const command = async () => {
    ++calls;
    throw failure;
  };
  await assert.rejects(identify(command), {message:failure.message});
  assert.equal(calls, 1);
});

test('the post-upload serial session pulses reset with BOOT released before identifying', async () => {
  const port = usbDevice(() => {
    assert.deepEqual(port.signals, [
      {dataTerminalReady:false, requestToSend:true},
      {dataTerminalReady:false, requestToSend:false},
    ]);
    return ['CL IDENT RECEIVER 10:06:1C:00:00:01 10:06:1C:00:00:01\n'];
  });
  const board = await withBoard(port, command => identify(command), {restart:true});
  assert.equal(board.role, 'receiver');
});
