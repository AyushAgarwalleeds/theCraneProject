import {ESPLoader, Transport} from './vendor/esptool.js';

const parts = [
  ['bootloader.bin', 0x1000],
  ['partitions.bin', 0x8000],
  ['boot_app0.bin', 0xe000],
  ['app.bin', 0x10000],
];
const get = id => document.getElementById(id);
const connect = get('connect');
const configure = get('configure');
const progress = get('progress');
const status = get('status');
const restart = get('restart');
const supported = window.isSecureContext && !!navigator.serial;
let plan, step, receiver, installed, busy;
const used = new Set();

function message(text, error = false) {
  status.textContent = text;
  status.classList.toggle('error', error);
}

function randomHex(length) {
  return Array.from(crypto.getRandomValues(new Uint8Array(length)),
    byte => byte.toString(16).padStart(2, '0')).join('').toUpperCase();
}

function newPlan(count) {
  const addresses = new Set();
  return Array.from({length:count}, () => {
    let mac;
    do { mac = '02:' + randomHex(5).match(/../g).join(':'); } while (addresses.has(mac));
    addresses.add(mac);
    return {mac, key:randomHex(16)};
  });
}

function updateStep() {
  const names = ['Receiver', ...plan.map((_, index) => 'Sender ' + (index + 1))];
  const complete = step === names.length;
  get('devices').replaceChildren(...names.map((name, index) => {
    const item = document.createElement('li');
    item.textContent = index < step ? name + ' ✓' : name;
    if (index === step) item.setAttribute('aria-current', 'step');
    return item;
  }));
  get('device').textContent = complete ? 'Setup complete' : names[step];
  get('instruction').textContent = complete
    ? 'Disconnect USB, power the receiver and senders, and test each handheld on the bench.'
    : 'Connect only the ' + names[step].toLowerCase() + ' with a USB data cable. Select its port when asked.';
  connect.hidden = complete;
  get('pairing-note').hidden = step !== 0;
  configure.hidden = complete || !installed;
  progress.hidden = true;
  connect.disabled = configure.disabled = restart.disabled = busy;
}

function start(count) {
  plan = newPlan(count);
  step = 0;
  receiver = null;
  installed = busy = false;
  used.clear();
  get('plan').hidden = true;
  get('setup').hidden = false;
  message('');
  updateStep();
}

async function readFirmware(role) {
  return Promise.all(parts.map(async ([name, address]) => {
    const response = await fetch('./firmware/' + role + '/' + name);
    if (!response.ok) throw Error('Firmware file unavailable: ' + name);
    const data = new Uint8Array(await response.arrayBuffer());
    if (!data.length || (name === 'app.bin' && data[0] !== 0xe9))
      throw Error('Invalid firmware file: ' + name);
    return {data, address};
  }));
}

async function flash(port, role) {
  const fileArray = await readFirmware(role);
  const transport = new Transport(port, false);
  const loader = new ESPLoader({transport, baudrate:460800, romBaudrate:115200});
  try {
    message('Identifying ESP32…');
    const chip = await loader.main();
    if (loader.chip.CHIP_NAME !== 'ESP32') throw Error('Unsupported chip: ' + chip + '. Use ESP32-WROOM.');
    const size = await loader.detectFlashSize();
    if (!['4MB', '8MB', '16MB', '32MB', '64MB', '128MB'].includes(size))
      throw Error('Use an ESP32-WROOM with at least 4 MB flash. Detected: ' + (size || 'unknown'));
    const factory = (await loader.chip.readMac(loader)).toUpperCase();
    if (used.has(factory)) throw Error('This board is already assigned. Unplug it and select the next board.');
    progress.hidden = false;
    progress.value = 0;
    await loader.writeFlash({
      fileArray, flashMode:'keep', flashFreq:'keep', flashSize:'keep', eraseAll:false, compress:true,
      reportProgress(index, done, total) {
        progress.value = ((index + done / total) / fileArray.length) * 100;
        message('Uploading ' + role + ' firmware… ' + Math.floor(progress.value) + '%');
      },
    });
    await loader.after('hard_reset');
    return factory;
  } finally {
    if (port.readable || port.writable) await transport.disconnect();
  }
}

async function serialSetup(port, role, factory) {
  await port.open({baudRate:115200});
  const reader = port.readable.getReader();
  const writer = port.writable.getWriter();
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  let lines = '';
  let ready = false;
  let senderSaved = false;
  let verifiedSlots = 0;
  let settle;
  const wait = new Promise((resolve, reject) => { settle = {resolve,reject}; });
  const timer = setTimeout(() => settle.reject(Error('The board did not confirm setup. Tap EN, then retry pairing.')), 12000);
  const probe = setInterval(() => {
    if (!ready || senderSaved) void writer.write(encoder.encode('identify\n')).catch(err => settle.reject(err));
  }, 450);
  const readLoop = (async () => {
    try {
      for (;;) {
        const {value, done} = await reader.read();
        if (done) throw Error('USB disconnected before setup was confirmed.');
        lines += decoder.decode(value, {stream:true});
        let end;
        while ((end = lines.indexOf('\n')) >= 0) {
          const line = lines.slice(0, end).trim();
          lines = lines.slice(end + 1);
          if (line.startsWith('CL IDENT ') && (!ready || role === 'sender')) {
            const [, , actualRole, mac, board] = line.split(' ');
            if (actualRole !== role.toUpperCase() || (factory && board !== factory) || used.has(board))
              throw Error('The connected board does not match this setup step.');
            if (!ready) {
              ready = true;
              factory = board;
              if (role === 'receiver') {
                receiver = mac;
                await writer.write(encoder.encode('clear-peers\n'));
              } else {
                const sender = plan[step - 1];
                await writer.write(encoder.encode('pair-as ' + receiver + ' ' + sender.key + ' ' + sender.mac + '\n'));
              }
            }
          } else if (line.startsWith('CL ERROR')) {
            throw Error('The board rejected pairing. Check that all controls are released.');
          } else if (role === 'receiver' && (line === 'CL CLEARED' ||
                     line === 'CL PEER ' + (verifiedSlots + 1))) {
            if (line !== 'CL CLEARED') ++verifiedSlots;
            if (verifiedSlots === plan.length) settle.resolve(factory);
            else {
              const sender = plan[verifiedSlots];
              await writer.write(encoder.encode('peer ' + (verifiedSlots + 1) + ' ' + sender.mac + ' ' + sender.key + '\n'));
            }
          }
          if (role === 'sender' && ready && /^CL IDENT SENDER /.test(line) &&
              line.split(' ')[3] === plan[step - 1].mac && senderSaved) settle.resolve(factory);
          if (line === 'CL PAIRED') senderSaved = true;
        }
        if (lines.length > 4096) lines = '';
      }
    } catch (error) { settle.reject(error); }
  })();
  try { return await wait; }
  finally {
    clearInterval(probe);
    clearTimeout(timer);
    await reader.cancel().catch(() => {});
    await readLoop;
    reader.releaseLock();
    writer.releaseLock();
    await port.close();
  }
}

async function run(upload) {
  if (busy) return;
  busy = true;
  updateStep();
  let port;
  const role = step === 0 ? 'receiver' : 'sender';
  try {
    port = await navigator.serial.requestPort();
    const factory = upload ? await flash(port, role) : installed;
    installed = factory || true;
    message('Saving pairing on the ' + role + '…');
    const board = await serialSetup(port, role, typeof factory === 'string' ? factory : undefined);
    used.add(board);
    ++step;
    installed = false;
    message('Pairing saved on the ' + role + '.');
  } catch (error) {
    message(error.name === 'NotFoundError' ? 'No port selected. Try again when connected.' : error.message, true);
  } finally {
    busy = false;
    updateStep();
  }
}

get('plan').addEventListener('submit', event => {
  event.preventDefault();
  start(Number(get('count').value));
});
connect.addEventListener('click', () => void run(true));
configure.addEventListener('click', () => void run(false));
restart.addEventListener('click', () => {
  if (busy) return;
  get('setup').hidden = true;
  get('plan').hidden = false;
});
if (!supported) {
  get('plan').querySelector('button').disabled = true;
  const alert = get('compatibility');
  alert.hidden = false;
  alert.classList.add('error');
  alert.textContent = 'Open this page over HTTPS or localhost in desktop Chrome or Edge to connect over USB.';
}
