#pragma once

const char phonePage[] PROGMEM = R"CLBENCH(
<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CraneLink Phone Bench</title>
<style>
:root{color-scheme:dark;--bg:#0d171b;--card:#17262b;--line:#35494e;--text:#eef6f3;--blue:#205f92;--accent:#a5e8ce}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 system-ui,sans-serif}
main{max-width:34rem;margin:auto;padding:1rem}
h1{font-size:clamp(1.65rem,6vw,2.4rem);margin:.2rem 0;letter-spacing:-.045em}
.handoff{padding:.7rem .85rem;border:1px solid var(--line);border-radius:.65rem;color:var(--accent);margin:1rem 0}
p{margin:.5rem 0}
button{min-height:3.4rem;border:1px solid var(--line);border-radius:.65rem;color:var(--text);background:var(--card);font:inherit}
button:focus-visible{outline:3px solid #a6ceff}
button:disabled{opacity:.48}
.wide{width:100%;margin:.35rem 0;font-weight:650}
#run[aria-pressed=true]{background:#145638}
#stop{background:#7a2e27}
#message{min-height:3rem;color:#c2d0de}
.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:.6rem;margin:.5rem 0}
.jog{touch-action:none;user-select:none;-webkit-user-select:none;font-weight:650}
.jog[aria-pressed=true]{background:var(--blue)}
.up{grid-area:1/2}.left{grid-area:2/1}.right{grid-area:2/3}.down{grid-area:3/2}
.fwd{grid-area:4/1}.aux{grid-area:4/2}.back{grid-area:4/3}
.status{padding:.6rem .8rem;background:var(--card);border-radius:.65rem}
details{margin-top:1rem;padding:.7rem;background:var(--card);border-radius:.65rem}
summary{cursor:pointer;font-weight:650}
label{display:block;margin:.7rem 0}
input,select{display:block;width:100%;padding:.5rem;margin-top:.2rem;background:var(--bg);color:var(--text);border:1px solid var(--line);border-radius:.4rem;font:inherit}
.assignment{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 .75rem}
progress{width:100%}
</style>
<main>
  <h1>CraneLink Phone Bench</h1>
  <p class="handoff" id="controlSource">Checking control source…</p>
  <button class="wide" id="run" type="button" aria-pressed="false" disabled>Web Run off</button>
  <p id="message" role="status">Connecting to the bench receiver…</p>
  <div class="grid" aria-label="Jog controls">
    <button class="jog up" data-mask="1" type="button" disabled>↑ Up</button>
    <button class="jog left" data-mask="4" type="button" disabled>← Left</button>
    <button class="jog right" data-mask="8" type="button" disabled>Right →</button>
    <button class="jog down" data-mask="2" type="button" disabled>↓ Down</button>
    <button class="jog fwd" data-mask="16" type="button" disabled>Front</button>
    <button class="jog aux" data-mask="64" type="button" disabled>Power On/Off · hold 3 s</button>
    <button class="jog back" data-mask="32" type="button" disabled>Back</button>
  </div>
  <button class="wide" id="stop" type="button" disabled>Emergency Stop</button>
  <button class="wide" id="reset" type="button" disabled>Reset Emergency Stop</button>
  <p class="status">Receiver state <strong id="motion">Unknown</strong></p>
  <p class="status">Commanded relays <strong id="outputs">Unknown</strong></p>
  <p class="status">Power <strong id="auxState">Unknown</strong><progress id="auxProgress" max="3000" value="0" hidden aria-label="Power button hold progress"></progress></p>
  <details id="settings">
    <summary>Settings and sender pairing</summary>
    <form id="settingsForm">
      <label>Power idle timeout in minutes <input id="idleMinutes" type="number" min="1" max="720" required></label>
      <label>Direction control <select id="interlock">
        <option value="0">All directions together</option>
        <option value="1">NO/NC interlock · one direction per axis</option>
      </select></label>
      <div id="assignments" class="assignment"></div>
      <p>R8 is reserved for Enable during directional jogs.</p>
      <button id="saveSettings" class="wide" type="submit" disabled>Save relay settings</button>
      <p id="settingsMessage" role="status"></p>
    </form>
    <form id="peerForm">
      <p>Receiver station MAC <strong id="receiverMac"></strong></p>
      <label>Sender slot <select id="peerSlot"><option>1</option><option>2</option><option>3</option><option>4</option></select></label>
      <p id="radioStatus"></p>
      <label>Sender station MAC <input id="senderMac" placeholder="AA:BB:CC:DD:EE:FF" maxlength="17" spellcheck="false" autocomplete="off"></label>
      <label>Shared key <input id="sharedKey" placeholder="32 hex digits" maxlength="32" spellcheck="false" autocomplete="off"></label>
      <button id="generateKey" type="button" class="wide">Generate pairing key</button>
      <p>Copy the key before saving a sender. Clear the MAC and save to forget that slot.</p>
      <button id="savePeer" class="wide" type="submit" disabled>Save sender slot</button>
      <p id="peerMessage" role="status"></p>
    </form>
  </details>
</main>
<script>
'use strict';
const run = document.getElementById('run');
const stop = document.getElementById('stop');
const reset = document.getElementById('reset');
const message = document.getElementById('message');
const motion = document.getElementById('motion');
const outputs = document.getElementById('outputs');
const auxState = document.getElementById('auxState');
const auxProgress = document.getElementById('auxProgress');
const controlSource = document.getElementById('controlSource');
const settings = document.getElementById('settings');
const settingsForm = document.getElementById('settingsForm');
const idleInput = document.getElementById('idleMinutes');
const senderInput = document.getElementById('senderMac');
const keyInput = document.getElementById('sharedKey');
const saveButton = document.getElementById('saveSettings');
const settingsMessage = document.getElementById('settingsMessage');
const radioStatus = document.getElementById('radioStatus');
const interlockInput = document.getElementById('interlock');
const peerSlot = document.getElementById('peerSlot');
const savePeer = document.getElementById('savePeer');
const peerMessage = document.getElementById('peerMessage');
const buttons = [...document.querySelectorAll('.jog')];
const names = {0:'Idle',1:'Up',2:'Down',4:'Left',8:'Right',16:'Front',32:'Back',64:'Power'};

let token = '', sequence = 0, requested = 0, acknowledged = 0;
let relayMask = null;
let auxOn = false, auxSeconds = 0, auxHold = 0;
const pointers = new Map();
let generation = 0, pendingJog = null, transition = false;
let connected = false, stopped = false, anotherTab = false, checking = false;
let handheld = false, settingsBusy = false;
let phoneConnected = false;
let latestSettings;
let note = 'Connecting to the bench receiver…';

function render() {
  run.disabled = !connected || transition || stopped || anotherTab;
  run.textContent = token ? 'Web Run on · tap to turn off' : 'Web Run off · tap to enable';
  run.setAttribute('aria-pressed', String(!!token));
  stop.disabled = !connected || stopped;
  reset.disabled = !connected || !stopped || transition;
  message.textContent = note;
  controlSource.textContent = !connected ? 'Receiver offline' : stopped ? 'Control locked by Emergency Stop' :
    phoneConnected ? 'Phone Wi‑Fi connected · disconnect Wi‑Fi to use the handheld' : 'Handheld control';
  const actions = Object.keys(names).filter(bit => acknowledged & Number(bit)).map(bit => names[bit]);
  motion.textContent = !connected ? 'Unknown' : stopped ? 'Stop latched' : actions.join(' + ') || 'Idle';
  if (!connected || relayMask === null) outputs.textContent = 'Unknown';
  else if (!relayMask) outputs.textContent = 'All off';
  else {
    const active = [];
    for (let i = 0; i < 8; ++i) if (relayMask & (1 << i)) active.push('R' + (i + 1));
    outputs.textContent = active.join(' + ');
  }
  for (const button of buttons) {
    button.disabled = !token;
    button.setAttribute('aria-pressed', String(!!(Number(button.dataset.mask) & acknowledged)));
  }
  const minutes = Math.floor(auxSeconds / 60);
  const seconds = String(auxSeconds % 60).padStart(2, '0');
  auxState.textContent = relayMask === null ? 'Unknown' : auxOn ? 'On · ' + minutes + ':' + seconds + ' idle timeout' : 'Off';
  auxProgress.hidden = !auxHold;
  auxProgress.value = auxHold;
}

function updateOutputs(state) {
  phoneConnected = !!state.phoneConnected;
  relayMask = state.relayMask;
  auxOn = !!state.auxOn;
  auxSeconds = state.auxSeconds || 0;
  auxHold = state.auxHoldMs || 0;
}

async function request(path, fields, keepalive = false) {
  const controller = new AbortController();
  const timeout = keepalive ? null : setTimeout(() => controller.abort(), 400);
  const options = fields ? {
    method: 'POST', headers: {'X-CraneLink-Bench':'1'},
    body: new URLSearchParams(fields), keepalive
  } : {};
  try {
    const response = await fetch('/api/' + path, {...options, signal:controller.signal, cache:'no-store'});
    const result = await response.json();
    if (!response.ok) throw Error(result.error || 'Receiver rejected the request.');
    return result;
  } finally {
    if (timeout !== null) clearTimeout(timeout);
  }
}

function endSession(noteText, leaving = false) {
  const oldToken = token;
  ++generation;
  token = '';
  sequence = requested = acknowledged = 0;
  relayMask = null;
  auxHold = 0;
  pointers.clear();
  pendingJog = null;
  transition = false;
  note = noteText;
  render();
  if (oldToken) {
    const epoch = generation;
    const fields = leaving ? {token:oldToken, leaving:'1'} : {token:oldToken};
    void request('end', fields, true).then(state => {
      if (epoch === generation && !transition) {
        updateOutputs(state);
        render();
      }
    }).catch(() => {});
  }
}

async function enable() {
  if (transition || document.hidden) return;
  if (token) {
    endSession('Web Run turned off.');
    return;
  }
  const epoch = ++generation;
  transition = true;
  note = 'Enabling Web Run…';
  render();
  try {
    const reply = await request('arm', {});
    if (!/^[0-9a-f]{16}$/.test(reply.token)) throw Error('Invalid session reply.');
    if (epoch !== generation) {
      void request('end', {token:reply.token}, true).catch(() => {});
      return;
    }
    token = reply.token;
    await pump();
    if (epoch === generation && token) note = 'Hold a direction or Power On/Off.';
  } catch (error) {
    if (epoch === generation) note = error.message;
  } finally {
    if (epoch === generation) {
      transition = false;
      render();
    }
  }
}

async function pump() {
  if (!token || pendingJog || document.hidden) return;
  const packet = {epoch:generation, mask:requested};
  pendingJog = packet;
  try {
    const state = await request('jog', {token, seq:String(++sequence), mask:String(packet.mask)});
    if (packet.epoch === generation) {
      if (state.stopped || !state.armed || (state.requested ?? state.mask) !== packet.mask)
        throw Error('Receiver session changed.');
      acknowledged = state.mask;
      updateOutputs(state);
      render();
    }
  } catch {
    if (packet.epoch === generation) endSession('Jog interrupted. Check Wi-Fi, then enable Web Run again.', true);
  } finally {
    if (pendingJog === packet) {
      pendingJog = null;
      if (requested !== packet.mask) void pump();
    }
  }
}

function press(event, button) {
  if (!token || transition || event.button !== 0 || pointers.has(event.pointerId)) return;
  try { button.setPointerCapture(event.pointerId); }
  catch { return; }
  pointers.set(event.pointerId, Number(button.dataset.mask));
  updateRequested();
}

function updateRequested() {
  requested = 0;
  for (const bit of pointers.values()) requested |= bit;
  void pump();
}

function release(event) {
  if (pointers.delete(event.pointerId)) updateRequested();
}

for (const button of buttons) {
  button.addEventListener('pointerdown', event => press(event, button));
  for (const eventName of ['pointerup','pointercancel','lostpointercapture'])
    button.addEventListener(eventName, release);
  button.addEventListener('pointermove', event => {
    if (!pointers.has(event.pointerId)) return;
    const bounds = button.getBoundingClientRect();
    if (event.clientX < bounds.left || event.clientX > bounds.right ||
        event.clientY < bounds.top || event.clientY > bounds.bottom) release(event);
  });
  button.addEventListener('contextmenu', event => event.preventDefault());
}

async function changeStop(action) {
  endSession(action === 'stop' ? 'Stopping relays…' : 'Clearing stop…');
  const epoch = generation;
  transition = true;
  render();
  try {
    const state = await request(action, {});
    if (epoch !== generation) return;
    stopped = state.stopped;
    anotherTab = state.armed;
    updateOutputs(state);
    note = stopped ? 'Stop latched.' : 'Stop cleared. Enable Web Run to jog.';
  } catch {
    if (epoch === generation) note = 'Receiver did not confirm. Check Wi-Fi and try again.';
  } finally {
    if (epoch === generation) {
      transition = false;
      render();
    }
  }
}

async function refresh() {
  if (document.hidden || token || transition || checking) return;
  checking = true;
  const epoch = generation;
  try {
    const state = await request('status');
    if (epoch !== generation) return;
    if (!connected) note = 'Connected. Enable Web Run to jog.';
    connected = true;
    stopped = state.stopped;
    anotherTab = state.armed;
    handheld = !!state.handheld;
    acknowledged = state.mask;
    updateOutputs(state);
    if (anotherTab) note = handheld ? 'The sender has control.' : 'Another tab has Web Run.';
    else if (note === 'Another tab has Web Run.' || note === 'The sender has control.')
      note = 'Ready. Enable Web Run to jog.';
  } catch {
    if (epoch !== generation) return;
    connected = false;
    note = 'Receiver unavailable. Stay connected to the receiver’s Wi-Fi.';
  } finally {
    checking = false;
    if (epoch === generation) render();
  }
}

const assignmentLabels = ['Up', 'Down', 'Left', 'Right', 'Front', 'Back', 'Power On/Off'];
const assignmentInputs = [];
function showPeer() {
  const sender = latestSettings?.senders?.[Number(peerSlot.value || 1) - 1];
  senderInput.value = sender?.mac || '';
  keyInput.value = '';
  radioStatus.textContent = latestSettings.radioReady
    ? 'Channel ' + latestSettings.channel + ' · ' + (sender?.linked ? 'Sender linked' : sender?.mac ? 'Waiting for sender' : 'Empty slot')
    : 'ESP-NOW unavailable';
  savePeer.disabled = !latestSettings.radioReady;
}

function showSettings(data) {
  latestSettings = data;
  if (!assignmentInputs.length) {
    for (const name of assignmentLabels) {
      const label = document.createElement('label');
      label.textContent = name;
      const select = document.createElement('select');
      for (let relay = 1; relay <= 7; ++relay) {
        const option = document.createElement('option');
        option.value = String(relay);
        option.textContent = 'R' + relay;
        select.appendChild(option);
      }
      assignmentInputs.push(select);
      select.addEventListener('change', () => {
        const previous = select.dataset.relay;
        const other = assignmentInputs.find(input => input !== select && input.value === select.value);
        if (other) {
          other.value = previous;
          other.dataset.relay = previous;
        }
        select.dataset.relay = select.value;
      });
      label.appendChild(select);
      document.getElementById('assignments').appendChild(label);
    }
  }
  idleInput.value = data.idleMinutes;
  interlockInput.value = data.interlock ? '1' : '0';
  document.getElementById('receiverMac').textContent = data.receiverMac;
  showPeer();
  assignmentInputs.forEach((select, index) => { select.value = select.dataset.relay = data.mapping[index]; });
  saveButton.disabled = false;
}

settings.addEventListener('toggle', async () => {
  if (!settings.open || settingsBusy) return;
  if (token || transition) endSession('Web Run turned off for settings.');
  settingsBusy = true;
  saveButton.disabled = true;
  try {
    showSettings(await request('settings'));
    settingsMessage.textContent = '';
  } catch {
    settingsMessage.textContent = 'Could not load settings.';
  } finally {
    settingsBusy = false;
  }
});
document.getElementById('generateKey').addEventListener('click', () => {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  keyInput.value = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
  peerMessage.textContent = 'Copy this key for the sender before saving.';
});
peerSlot.addEventListener('change', () => { if (latestSettings) showPeer(); });
document.getElementById('peerForm').addEventListener('submit', async event => {
  event.preventDefault();
  if (settingsBusy) return;
  settingsBusy = true;
  savePeer.disabled = true;
  try {
    showSettings(await request('peer', {
      slot:peerSlot.value, senderMac:senderInput.value.trim(), key:keyInput.value.trim()
    }));
    peerMessage.textContent = 'Sender slot saved.';
  } catch (error) {
    peerMessage.textContent = error.message;
  } finally {
    settingsBusy = false;
    savePeer.disabled = !latestSettings?.radioReady;
  }
});
settingsForm.addEventListener('submit', async event => {
  event.preventDefault();
  if (settingsBusy || !assignmentInputs.length) return;
  const mapping = assignmentInputs.map(input => input.value).join('');
  if (new Set(mapping).size !== 7) {
    settingsMessage.textContent = 'Assign each of R1 through R7 exactly once.';
    return;
  }
  settingsBusy = true;
  saveButton.disabled = true;
  try {
    const data = await request('settings', {
      idleMinutes:idleInput.value, mapping, interlock:interlockInput.value
    });
    showSettings(data);
    settingsMessage.textContent = 'Settings saved.';
  } catch (error) {
    settingsMessage.textContent = error.message;
  } finally {
    settingsBusy = false;
    saveButton.disabled = false;
  }
});

run.addEventListener('click', enable);
stop.addEventListener('click', () => changeStop('stop'));
reset.addEventListener('click', () => changeStop('reset'));
function leave() {
  if (token || transition) endSession('Web Run ended. Enable it again to jog.', true);
}
document.addEventListener('visibilitychange', () => document.hidden ? leave() : void refresh());
window.addEventListener('blur', leave);
window.addEventListener('pagehide', leave);
window.addEventListener('offline', leave);
setInterval(pump, 110);
setInterval(refresh, 1200);
void refresh();
</script>
</html>
)CLBENCH";
