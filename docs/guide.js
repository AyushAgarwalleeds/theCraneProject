const usbSteps = [
  {title:'Choose the handheld count', description:'Open USB setup, choose one to four handhelds, and press Start setup.', role:'Receiver'},
  {title:'Upload the receiver', description:'Connect the receiver by USB. Select Connect and upload, choose its port, and wait until pairing is saved.', role:'Receiver'},
  {title:'Upload each handheld', description:'Unplug the receiver. Connect each sender when prompted and complete every sender step in the same tab.', role:'Handheld'},
  {title:'Save the report and test', description:'Download the setup report. Power the boards, disconnect the phone from receiver Wi-Fi, reset the startup Stop, then test a fresh direction press on the bench.', role:'Ready'},
];
const walkthrough = document.querySelector('[data-walkthrough="usb"]');
let step = 0;
function showStep() {
  const current = usbSteps[step];
  walkthrough.querySelector('[data-position]').textContent = step + 1;
  walkthrough.querySelector('[data-step-title]').textContent = current.title;
  walkthrough.querySelector('[data-step-description]').textContent = current.description;
  walkthrough.querySelector('.usb-role').textContent = current.role;
  walkthrough.querySelector('[data-previous]').disabled = step === 0;
  walkthrough.querySelector('[data-next]').textContent = step === usbSteps.length - 1 ? 'Start again' : 'Next step';
}
walkthrough.querySelector('[data-next]').addEventListener('click', () => { step = (step + 1) % usbSteps.length; showStep(); });
walkthrough.querySelector('[data-previous]').addEventListener('click', () => { step = Math.max(0, step - 1); showStep(); });
walkthrough.querySelector('[data-step-description]').setAttribute('aria-live', 'polite');

const situations = {
  handheld:['Ready for the handheld', 'After the startup Stop reset, release all buttons, wait for the receiver link, then press a direction. The phone page can stay closed.'],
  phone:['Phone joined to receiver Wi-Fi', 'Handheld motion is paused. Open http://192.168.4.1/ and enable Web Run to jog from the phone. Disconnect from this Wi-Fi to give control back.'],
  loss:['Outputs drop on command expiry', 'Motion and Power turn off when the controlling command stream expires. When the link returns, release the buttons and start a new press.'],
};
document.querySelectorAll('[data-mode-choice]').forEach(button => button.addEventListener('click', () => {
  const mode = button.dataset.modeChoice;
  document.querySelector('.handoff').dataset.mode = mode;
  document.querySelectorAll('[data-mode-choice]').forEach(choice => choice.setAttribute('aria-pressed', String(choice === button)));
  document.getElementById('handoff-title').textContent = situations[mode][0];
  document.getElementById('handoff-description').textContent = situations[mode][1];
}));
const patterns = {
  searching:'Short flash about once a second. A periodic tone accompanies the search.',
  connected:'Solid light when the receiver acknowledges fresh handheld packets. One short beep accompanies connection. Release the buttons before a fresh command.',
  lost:'Rapid flashes after link loss. Two short beeps accompany the loss. Confirm receiver power and move back into range.',
  low:'Rapid flashes over the link pattern. Three beeps every 30 seconds while the monitored battery remains low.',
  setup:'Repeating double flash while handheld setup Wi-Fi is active. Radio control is paused in this mode.',
};
document.querySelectorAll('[data-led]').forEach(button => button.addEventListener('click', () => {
  document.querySelector('.preview-led').className = 'preview-led ' + button.dataset.led;
  document.getElementById('led-explanation').textContent = patterns[button.dataset.led];
  document.querySelectorAll('[data-led]').forEach(choice => choice.setAttribute('aria-pressed', String(choice === button)));
}));
document.getElementById('printGuide').addEventListener('click', () => {
  const collapsed = [...document.querySelectorAll('details:not([open])')];
  collapsed.forEach(item => { item.open = true; });
  window.addEventListener('afterprint', () => collapsed.forEach(item => { item.open = false; }), {once:true});
  window.print();
});
const sections = [...document.querySelectorAll('main>section[id]')];
const navigation = [...document.querySelectorAll('.contents a')];
if ('IntersectionObserver' in window) {
  const observer = new IntersectionObserver(entries => {
    for (const entry of entries) {
      if (!entry.isIntersecting) continue;
      navigation.forEach(link => {
        if (link.hash === '#' + entry.target.id) link.setAttribute('aria-current', 'location');
        else link.removeAttribute('aria-current');
      });
    }
  }, {rootMargin:'-10% 0px -65% 0px'});
  sections.forEach(section => observer.observe(section));
}
