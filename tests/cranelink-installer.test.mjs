import assert from 'node:assert/strict';
import test from 'node:test';
import {FirmwareUpdateRequired, identify, readBoardInfo, withBoard} from '../docs/serial.mjs';
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
    : ['CL TARGET 10:06:1C:00:00:01 6 1 0\nCL INPUTS 81\nCL INFO DONE\n']);
  const info = await withBoard(port, async command => readBoardInfo(command, await identify(command)));
  assert.equal(info.ready, true);
  assert.equal(info.linked, false);
  assert.equal(info.inputs, 0x81);
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
