import {ESPLoader, Transport} from './vendor/esptool.js';
import {acknowledge, FirmwareUpdateRequired, identify, readBoardInfo, withBoard} from './serial.mjs?v=3';
import {renderReport, downloadReport} from './report.mjs';

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
let checkedBoard, readingInputs = false;
const configuredBoards = [];
const observedBoards = new Map();
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
    : 'Connect the ' + names[step].toLowerCase() + ' with a USB data cable. Select its port when asked.';
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
  configuredBoards.length = 0;
  observedBoards.clear();
  checkedBoard = undefined;
  refreshReport();
  get('plan').hidden = true;
  get('setup').hidden = false;
  message('');
  updateStep();
}

async function readFirmware(role) {
  return Promise.all(parts.map(async ([name, address]) => {
    const response = await fetch('./firmware/' + role + '/' + name, {cache:'no-store'});
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
    return factory;
  } finally {
    if (port.readable || port.writable) await transport.disconnect();
  }
}

function serialSetup(port, role, factory, restart) {
  return withBoard(port, async command => {
    const board = await identify(command, role, factory);
    if (used.has(board.factory)) throw Error('This board was already assigned. Connect the next board.');
    // Check the firmware before writing any new pairing settings.
    await readBoardInfo(command, board);
    if (role === 'receiver') {
      await acknowledge(command, 'peers-begin ' + plan.length, 'CL PEERS READY');
      for (const sender of plan)
        await acknowledge(command, 'peers-add ' + sender.mac + ' ' + sender.key, 'CL PEERS ADDED');
      await acknowledge(command, 'peers-save', 'CL PEERS SAVED');
      const info = await readBoardInfo(command, board);
      if (!info.ssid || !info.ready || !plan.every((sender, i) =>
        info.slots.some(slot => slot.slot === i + 1 && slot.mac === sender.mac)))
        throw Error('Receiver settings could not be confirmed. Retry pairing on this board.');
      receiver = board.mac;
      return info;
    }
    const sender = plan[step - 1];
    await acknowledge(command, 'pair-as ' + receiver + ' ' + sender.key + ' ' + sender.mac, 'CL PAIRED');
    await new Promise(resolve => setTimeout(resolve, 500));
    const verified = await identify(command, role, board.factory);
    const info = await readBoardInfo(command, verified);
    if (!info.ready || info.mac !== sender.mac || info.target !== receiver)
      throw Error('Sender identity or radio setup could not be confirmed. Retry pairing on this board.');
    return {...info, slot:step};
  }, {restart});
}

async function run(upload) {
  if (busy || readingInputs) return;
  busy = true;
  updateStep();
  const role = step === 0 ? 'receiver' : 'sender';
  try {
    const port = await navigator.serial.requestPort();
    const factory = upload ? await flash(port, role) : installed;
    installed = factory || true;
    message('Saving pairing on the ' + role + '…');
    const board = await serialSetup(port, role, typeof factory === 'string' ? factory : undefined, upload);
    used.add(board.factory);
    observedBoards.delete(board.factory);
    configuredBoards.push(board);
    refreshReport();
    ++step;
    installed = false;
    message('Pairing saved on the ' + role + '. Power both boards for the radio check.');
  } catch (error) {
    if (error instanceof FirmwareUpdateRequired) installed = false;
    message(error.name === 'NotFoundError' ? 'No port selected. Try again when connected.' : error.message, true);
  } finally {
    busy = false;
    updateStep();
  }
}

function reportBoards() {
  const boards = new Map(configuredBoards.map(board => [board.factory, board]));
  for (const [factory, info] of observedBoards)
    boards.set(factory, {...boards.get(factory), ...info});
  return [...boards.values()];
}

function refreshReport() {
  renderReport(get('report'), reportBoards(), checkedBoard);
}

get('inspect').addEventListener('click', async () => {
  if (busy) return;
  if (readingInputs) {
    readingInputs = false;
    return;
  }
  readingInputs = true;
  const button = get('inspect');
  const note = get('inspectStatus');
  note.textContent = 'Select the USB port of the board to check.';
  try {
    const port = await navigator.serial.requestPort();
    await withBoard(port, async command => {
      const board = await identify(command);
      button.textContent = 'Stop USB check';
      while (readingInputs) {
        checkedBoard = await readBoardInfo(command, board);
        observedBoards.set(board.factory, checkedBoard);
        note.textContent = board.role === 'sender'
          ? 'Hold a button or briefly close its GPIO to GND and watch the input display.'
          : 'Receiver status refreshes while this check is open.';
        refreshReport();
        await new Promise(resolve => setTimeout(resolve, 250));
      }
    });
  } catch (error) {
    note.textContent = error.name === 'NotFoundError' ? 'No port selected.' : error.message;
  } finally {
    readingInputs = false;
    button.textContent = 'Check connected board';
  }
});
get('downloadReport').addEventListener('click', async () => {
  await downloadReport(reportBoards(), checkedBoard).catch(() => {
    get('inspectStatus').textContent = 'Could not create the PDF. Reload the page and try again.';
  });
});

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
  get('inspect').disabled = true;
  get('plan').querySelector('button').disabled = true;
  const alert = get('compatibility');
  alert.hidden = false;
  alert.classList.add('error');
  alert.textContent = 'Open this page over HTTPS or localhost in desktop Chrome or Edge to connect over USB.';
}
