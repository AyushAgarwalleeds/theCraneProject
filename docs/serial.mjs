class SerialReplyTimeout extends Error {}

export class FirmwareUpdateRequired extends Error {
  constructor() {
    super('This board is running older CraneLink firmware. Use Connect and upload for this board.');
  }
}

export async function withBoard(port, work, {restart = false} = {}) {
  try {
    await port.open({baudRate:115200, flowControl:'none'});
  } catch (error) {
    if (error.name === 'NetworkError' || error.name === 'InvalidStateError')
      throw Error('Could not open the USB port. Close any app or tab using it, then reconnect the board.');
    throw error;
  }
  let reader, writer, reading, request;
  async function command(text, accept, timeout = 3500) {
    const encoder = new TextEncoder();
    let timer;
    try {
      return await new Promise((resolve, reject) => {
        request = {accept, resolve, reject};
        timer = setTimeout(() => reject(new SerialReplyTimeout('No reply to ' + text.split(' ')[0] + '.')), timeout);
        writer.write(encoder.encode('\n' + text + '\n')).catch(reject);
      });
    } finally {
      clearTimeout(timer);
      request = null;
    }
  }
  try {
    if (restart) {
      // Pulse EN with GPIO0 released so the new firmware starts from flash.
      await port.setSignals({dataTerminalReady:false, requestToSend:true});
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    await port.setSignals({dataTerminalReady:false, requestToSend:false});
    reader = port.readable.getReader();
    writer = port.writable.getWriter();
    const decoder = new TextDecoder();
    reading = (async () => {
      let buffered = '';
      try {
        for (;;) {
          const {done, value} = await reader.read();
          if (done) break;
          buffered += decoder.decode(value, {stream:true});
          let end;
          while ((end = buffered.indexOf('\n')) >= 0) {
            const line = buffered.slice(0, end).trim();
            buffered = buffered.slice(end + 1);
            if (!request) continue;
            const result = request.accept(line);
            if (result instanceof Error) {
              request.reject(result);
              continue;
            }
            if (line.startsWith('CL ERROR ')) {
              request.reject(Error(line.slice(9)));
              continue;
            }
            if (result !== undefined) request.resolve(result);
          }
          if (buffered.length > 4096) buffered = '';
        }
        request?.reject(Error('USB disconnected before the board replied.'));
      } catch (error) { request?.reject(error); }
    })();
    return await work(command);
  } finally {
    if (reader) {
      await reader.cancel().catch(() => {});
      await reading;
      reader.releaseLock();
    }
    writer?.releaseLock();
    await port.close().catch(() => {});
  }
}

const mac = '([0-9a-f]{2}(?::[0-9a-f]{2}){5})';
const identityPattern = new RegExp('^CL IDENT (RECEIVER|SENDER) ' + mac + ' ' + mac + '$', 'i');

export async function identify(command, role, factory) {
  let sawOutput = false;
  let bootloader = false;
  let brownout = false;
  for (let attempt = 0; attempt < 6; ++attempt) {
    let identity;
    try {
      identity = await command('identify', line => {
        sawOutput ||= !!line;
        bootloader ||= /waiting for download|DOWNLOAD_BOOT/.test(line);
        brownout ||= /brownout/i.test(line);
        return identityPattern.exec(line) || undefined;
      }, 1200);
    } catch (error) {
      if (!(error instanceof SerialReplyTimeout)) throw error;
      if (attempt === 5) {
        if (brownout) throw Error('The ESP32 reported a brownout. Test it on USB with the relay module disconnected.');
        if (bootloader) throw Error('The ESP32 is in upload mode. Release BOOT and tap EN/RESET once.');
        if (sawOutput) throw Error('The USB port opened, but the board did not identify itself as CraneLink. Check the port or use Connect and upload.');
        throw Error('The USB port opened, but the ESP32 sent no reply. Tap EN/RESET with BOOT released, then retry.');
      }
      continue;
    }
    const result = {role:identity[1].toLowerCase(), mac:identity[2].toUpperCase(), factory:identity[3].toUpperCase()};
    if ((role && result.role !== role) || (factory && result.factory !== factory))
      throw Error('This is a different board from the one selected for this step.');
    return result;
  }
}

export async function readBoardInfo(command, identity) {
  const result = {...identity, slots:[]};
  await command('setup-info', line => {
    if (line === 'CL ERROR peer not saved' || line === 'Use: pair RECEIVER_MAC 32_HEX_KEY')
      return new FirmwareUpdateRequired();
    const [prefix, kind, ...fields] = line.split(' ');
    if (prefix !== 'CL') return;
    if (kind === 'WIFI') {
      if (fields.length === 3) [result.ssid, result.password, result.ip] = fields;
      else result.wifiDown = true;
    }
    if (kind === 'RADIO') {
      result.channel = Number(fields[0]);
      result.ready = fields[1] === '1';
    }
    if (kind === 'STATE') {
      result.stopped = fields[0] === '1';
      result.phoneOwns = fields[1] === '1';
    }
    if (kind === 'POWER') {
      result.resetReason = Number(fields[0]);
      result.uptime = Number(fields[1]);
      result.clients = Number(fields[2]);
    }
    if (kind === 'SLOT' && fields[1] !== '-')
      result.slots.push({slot:Number(fields[0]), mac:fields[1], linked:fields[2] === '1'});
    if (kind === 'TARGET') {
      result.target = fields[0];
      result.channel = Number(fields[1]);
      result.ready = fields[2] === '1';
      result.linked = fields[3] === '1';
    }
    if (kind === 'INPUTS') result.inputs = Number.parseInt(fields[0], 16);
    if (kind === 'INFO' && fields[0] === 'DONE') return true;
  });
  return result;
}

export function acknowledge(command, text, response) {
  return command(text, line => line === response ? true : undefined);
}
