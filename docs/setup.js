import {ESPLoader, Transport} from './vendor/esptool.js';
import {acknowledge, FirmwareUpdateRequired, identify, probeRole, readBoardInfo, withBoard} from './serial.mjs?v=6';
import {renderReport, downloadReport} from './report.mjs?v=6';
import {requestPort, supported, useAndroidUsb} from './usb.mjs?v=6';

const parts = [
  ['bootloader.bin', 0x1000],
  ['partitions.bin', 0x8000],
  ['boot_app0.bin', 0xe000],
  ['app.bin', 0x10000],
];
const radioProtocol = 4;
const firmwareRelease = 6;
const get = id => document.getElementById(id);
const connect = get('connect');
const configure = get('configure');
const progress = get('progress');
const status = get('status');
const restart = get('restart');
const saveNetwork = get('saveNetwork');
let plan, step, receiver, installed, busy;
let master;
let checkedBoard, readingInputs = false, stopInspection = false;
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
  saveNetwork.disabled = busy || !supported;
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
    const response = await fetch('./firmware/' + role + '/' + name + '?v=' + firmwareRelease, {cache:'no-store'});
    if (!response.ok) throw Error('Firmware file unavailable: ' + name);
    const data = new Uint8Array(await response.arrayBuffer());
    if (!data.length || (name === 'app.bin' && data[0] !== 0xe9))
      throw Error('Invalid firmware file: ' + name);
    return {data, address};
  }));
}

async function flash(port, role, options = {}) {
  const {progressBar = progress, reportStatus = message, exclude = used} = options;
  const fileArray = await readFirmware(role);
  const transport = new Transport(port, false);
  const loader = new ESPLoader({transport, baudrate:useAndroidUsb ? 115200 : 460800, romBaudrate:115200});
  const wakeLock = useAndroidUsb ? await navigator.wakeLock?.request('screen').catch(() => null) : null;
  try {
    reportStatus('Identifying ESP32…');
    const chip = await loader.main();
    if (loader.chip.CHIP_NAME !== 'ESP32') throw Error('Unsupported chip: ' + chip + '. Use ESP32-WROOM.');
    const size = await loader.detectFlashSize();
    if (!['4MB', '8MB', '16MB', '32MB', '64MB', '128MB'].includes(size))
      throw Error('Use an ESP32-WROOM with at least 4 MB flash. Detected: ' + (size || 'unknown'));
    const factory = (await loader.chip.readMac(loader)).toUpperCase();
    if (exclude?.has(factory)) throw Error('This board is already assigned. Unplug it and select the next board.');
    progressBar.hidden = false;
    progressBar.value = 0;
    await loader.writeFlash({
      fileArray, flashMode:'keep', flashFreq:'keep', flashSize:'keep', eraseAll:false, compress:true,
      reportProgress(index, done, total) {
        progressBar.value = ((index + done / total) / fileArray.length) * 100;
        reportStatus('Uploading ' + role + ' firmware… ' + Math.floor(progressBar.value) + '%');
      },
    });
    return factory;
  } finally {
    try {
      if (port.readable || port.writable) await transport.disconnect();
    } finally {
      await wakeLock?.release().catch(() => {});
    }
  }
}

function serialSetup(port, role, factory, restart) {
  return withBoard(port, async command => {
    const board = await identify(command, role, factory);
    if (used.has(board.factory)) throw Error('This board was already assigned. Connect the next board.');
    // Check the firmware before writing any new pairing settings.
    const before = await readBoardInfo(command, board);
    if (before.protocol !== radioProtocol) throw new FirmwareUpdateRequired();
    if (role === 'sender' && !before.masterEditable) throw new FirmwareUpdateRequired();
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
    const port = await requestPort();
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
    stopInspection = true;
    return;
  }
  readingInputs = true;
  stopInspection = false;
  const button = get('inspect');
  const note = get('inspectStatus');
  note.textContent = 'Select the USB port of the board to check.';
  try {
    const port = await requestPort();
    await withBoard(port, async command => {
      const board = await identify(command);
      button.textContent = 'Stop USB check';
      while (!stopInspection) {
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

get('network').addEventListener('submit', async event => {
  event.preventDefault();
  const status = get('networkStatus');
  if (busy || readingInputs) {
    status.textContent = 'Finish the upload or stop the USB check first.';
    return;
  }
  const name = get('networkName').value.toUpperCase();
  const password = get('networkKey').value;
  busy = saveNetwork.disabled = true;
  status.textContent = 'Select the receiver’s USB port.';
  try {
    const port = await requestPort();
    const factory = await withBoard(port, async command => {
      const identity = await identify(command, 'receiver');
      const info = await readBoardInfo(command, identity);
      if (!info.networkEditable) throw new FirmwareUpdateRequired();
      await acknowledge(command, 'wifi-set ' + name + ' ' + password, 'CL WIFI SAVED');
      return identity.factory;
    });
    status.textContent = 'Saved. Waiting for the receiver to restart…';
    get('networkKey').value = '';
    await new Promise(resolve => setTimeout(resolve, 1200));
    const board = await withBoard(port, async command => readBoardInfo(command,
      await identify(command, 'receiver', factory)));
    if (board.ssid !== name || board.password !== password)
      throw Error('Settings were saved. Use Check connected board to verify the active Wi-Fi.');
    observedBoards.set(board.factory, board);
    checkedBoard = board;
    refreshReport();
    status.textContent = 'Verified. Join ' + name + ' on your phone.';
  } catch (error) {
    status.textContent = error.name === 'NotFoundError' ? 'No port selected.' : error.message;
  } finally {
    busy = saveNetwork.disabled = false;
  }
});

function masterMessage(text, error = false) {
  const status = get('masterStatus');
  status.textContent = text;
  status.classList.toggle('error', error);
}

function showMasterStep() {
  const next = master.targets.length;
  const connect = get('masterConnect');
  get('masterTitle').textContent = master.done ? 'Master sender ready' : 'Master sender setup';
  get('masterInstruction').textContent = master.done
    ? 'Power the boards on the bench. Confirm the LED identifies the intended receiver before trying a direction.'
    : !master.sender ? 'Connect the handheld you want to use as the master. If it has older firmware, upload the sender firmware here.'
    : next < master.count ? 'Unplug the sender and connect receiver ' + (next + 1) + '. This uses one sender slot on that receiver.'
    : 'Reconnect the same master sender to save the receiver list. Saving replaces that sender’s previous receiver list.';
  connect.textContent = !master.sender ? 'Read sender over USB' : next < master.count ? 'Add connected receiver' : 'Save to master sender';
  connect.hidden = master.done;
  get('masterUpload').hidden = !!master.sender;
  get('masterProgress').hidden = true;
  get('masterReceivers').replaceChildren(...Array.from({length:master.count}, (_, i) => {
    const item = document.createElement('li');
    item.textContent = (i + 1) + ' · ' + (master.targets[i]?.name ?? 'Receiver');
    if (i === next && master.sender) item.setAttribute('aria-current', 'step');
    return item;
  }));
}

get('masterPlan').addEventListener('submit', event => {
  event.preventDefault();
  if (busy || readingInputs) return;
  master ??= {count:Number(get('receiverCount').value), targets:[], pending:new Map()};
  get('masterPlan').hidden = true;
  get('master').hidden = false;
  masterMessage('');
  showMasterStep();
});

get('masterCancel').addEventListener('click', () => {
  if (busy) return;
  if (!master.targets.length || master.done) master = undefined;
  get('receiverCount').disabled = !!master;
  get('masterPlan').querySelector('button').textContent = master ? 'Resume master setup' : 'Set up master sender over USB';
  get('master').hidden = true;
  get('masterPlan').hidden = false;
});

async function uploadHandheld(port, progressBar, reportStatus) {
  if (await withBoard(port, probeRole) === 'receiver')
    throw Error('This board identifies as a receiver. Connect your handheld to upload sender firmware.');
  return flash(port, 'sender', {progressBar, reportStatus, exclude:null});
}

async function configureMaster(port, upload) {
  if (!master.sender) {
    const factory = upload ? await uploadHandheld(port, get('masterProgress'), masterMessage) : undefined;
    const sender = await withBoard(port, async command => readBoardInfo(command,
      await identify(command, 'sender', factory)), {restart:upload});
    if (!sender.masterEditable) throw Error('Select Upload sender firmware to enable master setup.');
    if (sender.protocol !== radioProtocol) throw Error('The master sender firmware uses a different radio protocol. Upload the sender firmware from this page.');
    master.sender = sender;
    return sender;
  }
  if (master.targets.length < master.count) {
    return withBoard(port, async command => {
      const identity = await identify(command, 'receiver');
      if (master.targets.some(target => target.mac === identity.mac))
        throw Error('This receiver is already in the list. Connect the next receiver.');
      const before = await readBoardInfo(command, identity);
      if (before.protocol !== radioProtocol || !before.ready)
        throw Error('This receiver must be running compatible CraneLink firmware with ESP-NOW ready.');
      const occupied = before.slots.find(slot => slot.mac === master.sender.mac);
      const slot = occupied?.slot ?? [1, 2, 3, 4].find(number => !before.slots.some(slot => slot.slot === number));
      if (!slot) throw Error('All sender slots are taken. Free a slot on this receiver or choose another receiver.');
      let key = master.pending.get(identity.factory);
      if (!key) {
        key = randomHex(16);
        master.pending.set(identity.factory, key);
      }
      await acknowledge(command, 'peer ' + slot + ' ' + master.sender.mac + ' ' + key, 'CL PEER ' + slot);
      const after = await readBoardInfo(command, identity);
      if (!after.slots.some(item => item.slot === slot && item.mac === master.sender.mac))
        throw Error('Could not verify the saved sender slot. Retry with the same receiver.');
      const name = /^[A-Za-z0-9-]{1,32}$/.test(after.ssid ?? '') ? after.ssid : identity.mac.replaceAll(':', '');
      master.targets.push({mac:identity.mac, name, key});
      return after;
    });
  }
  return withBoard(port, async command => {
    const identity = await identify(command, 'sender', master.sender.factory);
    if (identity.mac !== master.sender.mac) throw Error('The sender address changed. Begin master setup again with this sender.');
    await acknowledge(command, 'targets-begin ' + master.targets.length, 'CL TARGETS READY');
    for (const {mac, name, key} of master.targets)
      await acknowledge(command, `targets-add ${mac} ${key} ${name}`, 'CL TARGET ADDED');
    await acknowledge(command, 'targets-save', 'CL TARGETS SAVED');
    const after = await readBoardInfo(command, identity);
    if (!after.ready || !after.targets?.[0]?.selected || after.targets.length !== master.targets.length ||
        !master.targets.every((target, i) => after.targets[i].mac === target.mac && after.targets[i].name === target.name))
      throw Error('Could not verify the receiver list. Retry with the same sender.');
    master.done = true;
    master.pending.clear();
    for (const target of master.targets) delete target.key;
    return after;
  });
}

async function runMaster(upload = false) {
  if (busy || readingInputs || master.done) return;
  busy = true;
  masterMessage('Select the board you just connected.');
  try {
    const port = await requestPort();
    const board = await configureMaster(port, upload);
    observedBoards.set(board.factory, board);
    checkedBoard = board;
    refreshReport();
    masterMessage(master.done ? 'Receiver order saved and verified on the master sender.' : 'Board verified. Continue with the next step.');
  } catch (error) {
    masterMessage(error.name === 'NotFoundError' ? 'No port selected.' : error.message, true);
  } finally {
    busy = false;
    showMasterStep();
  }
}

get('masterConnect').addEventListener('click', () => void runMaster());
get('masterUpload').addEventListener('click', () => void runMaster(true));

get('wirelessUpload').addEventListener('click', async event => {
  const status = get('wirelessStatus');
  if (busy || readingInputs) {
    status.textContent = 'Finish the current USB operation first.';
    return;
  }
  const button = event.currentTarget;
  const progressBar = get('wirelessProgress');
  busy = button.disabled = true;
  status.textContent = 'Select the handheld’s USB port.';
  try {
    const port = await requestPort();
    const factory = await uploadHandheld(port, progressBar, text => { status.textContent = text; });
    const sender = await withBoard(port, async command => readBoardInfo(command,
      await identify(command, 'sender', factory)), {restart:true});
    if (!sender.wirelessEditable || !sender.setupAp)
      throw Error('Could not read the setup Wi-Fi credentials. Reconnect the handheld and check USB diagnostics.');
    observedBoards.set(sender.factory, sender);
    checkedBoard = sender;
    refreshReport();
    status.textContent = 'Handheld updated. Its setup Wi-Fi name, password and station MAC are in Setup details.';
  } catch (error) {
    status.textContent = error.name === 'NotFoundError' ? 'No port selected.' : error.message;
  } finally {
    busy = button.disabled = false;
    progressBar.hidden = true;
  }
});

get('batteryConfig').addEventListener('submit', async event => {
  event.preventDefault();
  const status = get('batteryStatus');
  if (busy || readingInputs) {
    status.textContent = 'Finish the upload or stop the USB check first.';
    return;
  }
  busy = true;
  const enable = get('batteryMode').value === '1';
  status.textContent = 'Select the sender’s USB port.';
  try {
    const port = await requestPort();
    const board = await withBoard(port, async command => {
      const identity = await identify(command, 'sender');
      const before = await readBoardInfo(command, identity);
      if (!before.batteryEditable) throw Error('Upload the updated sender firmware first.');
      await acknowledge(command, 'battery ' + Number(enable), 'CL BATTERY SAVED');
      await new Promise(resolve => setTimeout(resolve, 300));
      return readBoardInfo(command, identity);
    });
    if (board.battery?.enabled !== enable) throw Error('Could not verify the battery setting.');
    observedBoards.set(board.factory, board);
    checkedBoard = board;
    refreshReport();
    status.textContent = enable ? 'Battery monitoring enabled. Compare the voltage with a multimeter.' : 'Battery monitoring disabled.';
  } catch (error) {
    status.textContent = error.name === 'NotFoundError' ? 'No port selected.' : error.message;
  } finally {
    busy = false;
  }
});

get('downloadReport').addEventListener('click', async () => {
  await downloadReport(reportBoards(), checkedBoard).catch(() => {
    get('inspectStatus').textContent = 'Could not create the PDF. Reload the page and try again.';
  });
});

get('plan').addEventListener('submit', event => {
  event.preventDefault();
  if (busy) return;
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
  saveNetwork.disabled = true;
  get('plan').querySelector('button').disabled = true;
  get('masterPlan').querySelector('button').disabled = true;
  get('wirelessUpload').disabled = true;
  get('batteryConfig').querySelector('button').disabled = true;
  const alert = get('compatibility');
  alert.hidden = false;
  alert.classList.add('error');
  alert.textContent = 'USB setup needs HTTPS and desktop Chrome, Edge, or Android Chrome with USB host support.';
}
