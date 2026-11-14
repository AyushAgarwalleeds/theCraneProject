import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {extname, resolve, sep} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = fileURLToPath(new URL('../docs', import.meta.url));
const types = {'.html':'text/html', '.js':'text/javascript', '.mjs':'text/javascript',
  '.css':'text/css', '.bin':'application/octet-stream'};
const port = Number(process.argv[2] || 8787);
createServer(async (req, res) => {
  try {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const file = resolve(root, '.' + (path === '/' ? '/index.html' : path));
    if (!file.startsWith(root + sep)) throw Error('Path outside site');
    const data = await readFile(file);
    res.writeHead(200, {'Content-Type':types[extname(file)] || 'text/plain', 'Cache-Control':'no-store'});
    res.end(data);
  } catch {
    res.writeHead(404).end();
  }
}).listen(port, '127.0.0.1', () => console.log('CraneLink USB Setup: http://localhost:' + port + '/'));
