const inputs = [
  ['Up', 4], ['Down', 32], ['Left', 33], ['Right', 25],
  ['Front', 26], ['Back', 27], ['Power', 13], ['Emergency Stop', 14],
];
const resets = {1:'Power-on', 2:'External reset', 3:'Software reset', 4:'Panic', 5:'Interrupt watchdog',
  6:'Task watchdog', 7:'Watchdog', 8:'Deep sleep', 9:'Brownout', 10:'SDIO'};

function reportSections(installed, checked) {
  const boards = installed.slice();
  if (checked) {
    const existing = boards.findIndex(board => board.factory === checked.factory);
    if (existing >= 0) boards[existing] = {...boards[existing], ...checked};
    else boards.push(checked);
  }
  return boards.map(board => {
    const rows = [
      ['Station MAC', board.mac],
      ['Factory MAC', board.factory],
      ['Radio channel', board.channel || 'Unavailable'],
      ['Radio protocol', board.protocol ?? 'Older firmware'],
    ];
    if (board.resetReason !== undefined)
      rows.push(['Last reset', resets[board.resetReason] || 'Code ' + board.resetReason],
        ['Uptime when read', board.uptime + ' seconds']);
    if (board.role === 'receiver') {
      rows.push(['Wi-Fi network', board.ssid || 'Access point unavailable']);
      rows.push(['ESP-NOW', board.ready ? 'Ready' : 'Unavailable']);
      if (board.password) rows.push(['Wi-Fi password', board.password]);
      if (board.ip) rows.push(['Phone controls', 'http://' + board.ip + '/']);
      if (board.clients !== undefined) rows.push(['Connected phones', board.clients]);
      if (board.stopped) rows.push(['Emergency Stop', 'Latched']);
      if (board.clients) rows.push(['Control owner', 'Phone Wi-Fi - disconnect to use the handheld']);
      for (const slot of board.slots)
        rows.push(['Sender slot ' + slot.slot, slot.mac + (slot.linked ? ' (heard)' : ' (waiting)')]);
    } else {
      if (board.setupAp) {
        rows.push(['Setup Wi-Fi name', board.setupAp.ssid], ['Setup Wi-Fi password', board.setupAp.password],
          ['Setup address', 'http://' + board.setupAp.ip + '/'],
          ['Setup mode', board.setupAp.active ? 'Wi-Fi setup active' : 'Hold Stop alone for 8 seconds, then release']);
      }
      rows.push(['Paired receiver MAC', board.target === '-' ? 'Unpaired' : board.target]);
      rows.push(['Radio link when read', !board.ready ? 'Unavailable or unpaired' :
        board.linked ? 'Receiver heard' : 'Waiting for receiver']);
      if (board.radioStarts !== undefined)
        rows.push(['Radio recovery', board.setupMode ? 'Setup Wi-Fi active' :
          board.radioResting ? 'Battery saving; tap Power to search now' : 'Automatic search enabled'],
          ['Radio starts since boot', board.radioStarts], ['Free heap', board.freeHeap + ' bytes']);
      for (const target of board.targets ?? []) {
        const name = target.name === '-' ? target.mac : target.name;
        rows.push(['Receiver ' + target.slot, name + (target.selected ? ' (selected)' : '')]);
      }
      if (board.battery) rows.push(['Battery voltage', !board.battery.enabled ? 'Monitoring off' :
        board.battery.millivolts ? (board.battery.millivolts / 1000).toFixed(2) + ' V' +
          (board.battery.low ? ' (low)' : '') : 'Waiting for valid pack voltage']);
      if (board.factory === checked?.factory && Number.isInteger(board.inputs))
        rows.push(['Closed inputs', inputs.filter((_, i) => board.inputs & (1 << i))
          .map(([label, pin]) => label + ' (GPIO' + pin + ')').join(', ') || 'All released']);
    }
    return {title:board.role === 'receiver' ? 'Receiver' : 'Sender' + (board.slot ? ' ' + board.slot : ''), rows};
  });
}

export function renderReport(container, installed, checked) {
  const sections = reportSections(installed, checked);
  container.hidden = !sections.length;
  const details = container.querySelector('.board-details');
  details.replaceChildren(...sections.map(({title, rows}) => {
    const section = document.createElement('section');
    const heading = document.createElement('h3');
    heading.textContent = title;
    const list = document.createElement('dl');
    for (const [label, value] of rows) {
      const term = document.createElement('dt');
      const description = document.createElement('dd');
      term.textContent = label;
      description.textContent = value;
      list.append(term, description);
    }
    section.append(heading, list);
    return section;
  }));
}

export async function buildReport(installed, checked) {
  const {PDFDocument, StandardFonts, rgb} = await import('./vendor/pdf-lib.mjs');
  const document = await PDFDocument.create();
  document.setTitle('CraneLink setup report');
  const plain = await document.embedFont(StandardFonts.Helvetica);
  const bold = await document.embedFont(StandardFonts.HelveticaBold);
  const accent = rgb(.07, .32, .26);
  const ink = rgb(.09, .15, .20);
  let page, y;
  function startPage() {
    page = document.addPage([595, 842]);
    page.drawRectangle({x:0, y:801, width:595, height:41, color:accent});
    page.drawText('CRANELINK  /  SETUP REPORT', {x:42, y:816, font:bold, size:12, color:rgb(1,1,1)});
    page.drawText(String(document.getPageCount()), {x:546, y:32, font:plain, size:10, color:ink});
    y = 772;
  }
  function line(value, x = 42, font = plain, size = 11) {
    page.drawText(String(value), {x, y, font, size, color:ink});
  }
  function row(label, value) {
    if (y < 65) startPage();
    line(label, 42, bold, 10);
    const words = String(value ?? 'Unavailable').split(' ');
    const max = 340;
    let text = '';
    for (const word of words) {
      const candidate = text ? text + ' ' + word : word;
      if (text && plain.widthOfTextAtSize(candidate, 11) > max) {
        line(text, 206);
        y -= 15;
        text = word;
      } else text = candidate;
    }
    line(text, 206);
    y -= 17;
  }
  function heading(title) {
    if (y < 150) startPage();
    line(title, 42, bold, 15);
    y -= 23;
  }
  startPage();
  line('Keep this report private. It can contain your receiver Wi-Fi password.');
  y -= 34;
  for (const section of reportSections(installed, checked)) {
    heading(section.title);
    for (const [label, value] of section.rows) row(label, value);
    y -= 20;
  }
  heading('Sender wiring');
  line('Connect each normally open button between its GPIO and sender GND.');
  y -= 21;
  row('Up / Down', 'GPIO4 / GPIO32');
  row('Left / Right', 'GPIO33 / GPIO25');
  row('Front / Back', 'GPIO26 / GPIO27');
  row('Power / Emergency Stop', 'GPIO13 / GPIO14');
  return document.save();
}

export async function downloadReport(installed, checked) {
  const bytes = await buildReport(installed, checked);
  const url = URL.createObjectURL(new Blob([bytes], {type:'application/pdf'}));
  const link = document.createElement('a');
  link.href = url;
  link.download = 'CraneLink-setup.pdf';
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
