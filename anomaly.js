#!/usr/bin/env node
const net = require('net'), readline = require('readline'), fs = require('fs');
const GAMES = { B: 27060, A: 27061, D: 27062, C: 27063, E: 27064 };
let PORT = +(process.env.ANOMALY_PORT || 27060), TIMEOUT = +(process.env.ANOMALY_TIMEOUT || 30000), JSONMODE = false;
const argv = process.argv.slice(2);
while (argv.length && /^--/.test(argv[0])) {
  const o = argv.shift();
  if (o === '--json') JSONMODE = true;
  else if (o === '--game' || o === '--port') { const v = String(argv.shift()); PORT = GAMES[v.toUpperCase()] || +v || PORT; }
  else if (o === '--timeout') TIMEOUT = +argv.shift() || TIMEOUT;
  else { console.error('unknown option ' + o); process.exit(2); }
}

function connect() {
  return new Promise((res, rej) => {
    const s = net.createConnection({ host: '127.0.0.1', port: PORT }, () => res(s));
    s.on('error', rej);
  });
}
function send(sock, line) {
  return new Promise((res, rej) => {
    let buf = '';
    const t = setTimeout(() => { sock.removeListener('data', on); rej(new Error('timeout')); }, TIMEOUT);
    const on = d => {
      buf += d.toString('utf8');
      if (buf === '.\n' || buf.endsWith('\n.\n')) { clearTimeout(t); sock.removeListener('data', on); res(buf.slice(0, buf.length - 2).replace(/\n$/, '')); }
    };
    sock.on('data', on);
    const l = line.replace(/\n/g, ' ');
    sock.write((JSONMODE && !/^\s*(reload|json)\b/i.test(l) ? 'json ' : '') + l + '\n');
  });
}
(async () => {
  const a = argv;
  let sock;
  try { sock = await connect(); } catch (e) { console.error('cannot connect to S2Dev on port ' + PORT + ': ' + e.message); process.exit(1); }
  if (a[0] === '-i') {
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout, prompt: 's2dev> ' });
    rl.prompt();
    for await (const line of rl) {
      if (line.trim() === 'quit' || line.trim() === 'exit') break;
      if (line.trim()) { try { console.log(await send(sock, line)); } catch (e) { console.error(e.message); } }
      rl.prompt();
    }
  } else if (a[0] === '-f') {
    for (const line of fs.readFileSync(a[1], 'utf8').split(/\r?\n/)) {
      if (!line.trim() || line.startsWith('#')) continue;
      console.log('> ' + line); console.log(await send(sock, line));
    }
  } else {
    const q = s => (/[\s]/.test(s) || s === '' ?'"' + s + '"' : s);
    const line = a.length === 1 ? a[0] : a.map(q).join(' ');
    try { console.log(await send(sock, line || 'help')); } catch (e) { console.error(e.message); process.exitCode = 1; }
  }
  sock.end();
})();
