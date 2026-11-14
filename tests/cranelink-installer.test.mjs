import assert from 'node:assert/strict';
import test from 'node:test';
import {identify, readBoardInfo, withBoard} from '../docs/serial.mjs';
import {buildReport} from '../docs/report.mjs';
import {PDFDocument} from '../docs/vendor/pdf-lib.mjs';

function usbDevice(onLine) {
  let output;
  const out = new TextEncoder();
  const port = {
    async open() {
      this.readable = new ReadableStream({start(controller) { output = controller; }});
      this.writable = new WritableStream({write(bytes) {
        const response = onLine(new TextDecoder().decode(bytes).trim());
        for (const part of response) output.enqueue(out.encode(part));
      }});
    },
    async setSignals() {},
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
