#pragma once

const char phonePage[] PROGMEM = R"BENCH(
<!doctype html>
<html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Crane Bench</title>
<style>
body{font:18px system-ui;max-width:34rem;margin:2rem auto;padding:0 1rem;background:#111c2b;color:#f6f9fd}
button{min-height:3.5rem;border:0;border-radius:.5rem;font:inherit;touch-action:none;user-select:none;-webkit-user-select:none}
#grid{display:grid;grid-template-columns:1fr 1fr;gap:.6rem;margin:1rem 0}
#arm{padding:0 1rem;background:#b8e6d2}#state{min-height:3em}button:active{background:#ffd582}
</style>
<h1>Crane Bench</h1>
<p>Simulation only. The receiver must be in Wi-Fi mode.</p>
<button id="arm">Enable simulation</button>
<button id="stop">Simulated stop</button>
<p id="state" role="status">Disabled</p>
<div id="grid"></div>
<script>
'use strict';
const arm = document.querySelector('#arm'), state = document.querySelector('#state');
const stop = document.querySelector('#stop');
let token = '', seq = 0, mask = 0, pointer = null, busy = false, generation = 0, neutral = false;
function reset(message) {
  ++generation;
  token = ''; seq = 0; mask = 0; pointer = null; neutral = false;
  state.textContent = message;
}
async function post(path, fields) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 220);
  try {
    const response = await fetch(path, {
      method: 'POST', headers: {'X-Crane-Bench': '1'},
      body: new URLSearchParams(fields), signal: controller.signal, cache: 'no-store'
    });
    if (!response.ok) throw Error('Rejected');
    return await response.text();
  } finally { clearTimeout(timeout); }
}
arm.onclick = async () => {
  if (busy || token || document.hidden) return;
  busy = true;
  const attempt = generation;
  try {
    const result = await post('/claim', {source:'p'});
    if (attempt !== generation) return;
    if (!/^[0-9a-f]{16}$/.test(result)) throw Error('Bad token');
    await post('/command', {token:result, seq:'1', mask:'0'});
    if (attempt !== generation) return;
    token = result;
    seq = 1;
    neutral = true;
    state.textContent = 'Ready: press and hold one channel.';
  } catch {
    if (attempt === generation) reset('Unavailable. Check receiver Wi-Fi and the simulated stop latch, then try again.');
  } finally { busy = false; }
};
async function pump() {
  if (!token || busy || document.hidden) return;
  busy = true;
  const attempt = generation, sent = mask;
  try {
    await post('/command', {token, seq:String(++seq), mask:String(sent)});
    if (attempt === generation && sent === 128) {
      reset('Simulated stop latched. Send R in the receiver serial monitor to reset.');
      return;
    }
    if (attempt === generation && !sent && !mask) neutral = true;
    if (attempt === generation && mask === sent)
      state.textContent = sent ? 'Simulating ' + labels[Math.log2(sent)] : 'Ready: all channels idle.';
  } catch {
    if (attempt === generation) reset('Session lost. Release and enable again.');
  } finally {
    busy = false;
    if (attempt === generation && mask !== sent) void pump();
  }
}
function release(event) {
  if (event.pointerId !== pointer) return;
  pointer = null; mask = 0;
  void pump();
}
stop.onclick = () => {
  if (!token) return;
  pointer = null;
  mask = 128;
  neutral = false;
  void pump();
};
const labels = ['Up', 'Down', 'Left', 'Right', 'Front', 'Back', 'AUX'];
for (let i = 0; i < labels.length; ++i) {
  const b = document.createElement('button');
  b.textContent = labels[i];
  b.oncontextmenu = e => e.preventDefault();
  b.onpointerdown = e => {
    if (!token || !neutral || pointer !== null || e.button !== 0) return;
    b.setPointerCapture(e.pointerId);
    pointer = e.pointerId; mask = 1 << i; neutral = false;
    void pump();
  };
  b.onpointerup = b.onpointercancel = b.onlostpointercapture = release;
  document.querySelector('#grid').appendChild(b);
}
function leave() {
  reset('Disabled. Enable again to use this page.');
}
document.addEventListener('visibilitychange', () => { if (document.hidden) leave(); });
window.addEventListener('blur', leave);
window.addEventListener('pagehide', leave);
setInterval(pump, 90);
</script>
</html>
)BENCH";
