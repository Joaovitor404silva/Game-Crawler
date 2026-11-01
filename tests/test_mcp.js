const net = require('net'), fs = require('fs'), os = require('os'), path = require('path'), { spawn, execFile } = require('child_process');
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('FAIL ' + m); } };
const root = path.join(__dirname, '..');

const received = {};
function fake(delayFor) {
  return new Promise(res => {
    const srv = net.createServer(s => {
      let buf = '';
      s.on('data', d => {
        buf += d.toString();
        let nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, nl); buf = buf.slice(nl + 1);
          const port = srv.address().port;
          (received[port] = received[port] || []).push(line);
          const delay = delayFor && delayFor(line) || 0;
          setTimeout(() => s.write('R:' + port + ':' + line + '\n.\n'), delay);
        }
      });
    });
    srv.listen(0, '127.0.0.1', () => res(srv));
  });
}

async function main() {
  const delayed = line => /wait/.test(line) ? 1200 : 0;
  const s1 = await fake(delayed), s2 = await fake(delayed), s3 = await fake(delayed);
  const p1 = s1.address().port, p2 = s2.address().port, p3 = s3.address().port;
  const logdir = fs.mkdtempSync(path.join(os.tmpdir(), 's2dev_mcp_'));
  const env = Object.assign({}, process.env, { ANOMALY_PORT: String(p1), ANOMALY_ALL_PORTS: [p1, p2, p3].join(','), ANOMALY_LOGDIR: logdir, ANOMALY_TIMEOUT: '600' });
  const srv = spawn(process.execPath, [path.join(root, 'mcp', 'server.js')], { env, stdio: ['pipe', 'pipe', 'inherit'] });
  let id = 0; const pending = {}; let buf = '';
  srv.stdout.on('data', d => {
    buf += d.toString(); let nl;
    while ((nl = buf.indexOf('\n')) >= 0) { const l = buf.slice(0, nl); buf = buf.slice(nl + 1); if (!l.trim()) continue; const m = JSON.parse(l); if (pending[m.id]) { pending[m.id](m); delete pending[m.id]; } }
  });
  const rpc = (method, params) => new Promise(res => { const i = ++id; pending[i] = res; srv.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: i, method, params }) + '\n'); });
  const call = async (name, args) => { const r = await rpc('tools/call', { name, arguments: args || {} }); return { text: r.result ? r.result.content[0].text : JSON.stringify(r.error), err: r.result && r.result.isError }; };

  const init = await rpc('initialize', { protocolVersion: '2024-11-05' });
  ok(init.result.serverInfo.name === 'anomaly-scanner', 'initialize');
  const tl = (await rpc('tools/list', {})).result.tools;
  const names = tl.map(t => t.name);
  for (const n of ['watchw', 'agg', 'scene', 'snap', 'sample', 'mark', 'hits', 'log', 'hook', 'bp', 'status']) ok(names.includes(n), 'tool listed ' + n);
  ok(tl.every(t => t.inputSchema.properties && t.inputSchema.properties.game && t.inputSchema.properties.json), 'every tool has game + json');
  ok(/E client 3/.test(tl[0].inputSchema.properties.game.description) && /all/.test(tl[0].inputSchema.properties.game.description), 'game description mentions E and all');
  ok(/E: 27064/.test(fs.readFileSync(path.join(root, 'mcp', 'server.js'), 'utf8')), 'GAMES has E=27064');

  const calls = [];
  for (let i = 0; i < 30; i++) {
    const port = [p1, p2, p3][i % 3];
    calls.push(call('status', { game: String(port) }).then(r => ({ port, r })));
  }
  const rs = await Promise.all(calls);
  ok(rs.every(x => x.r.text.includes('R:' + x.port + ':ping') && x.r.text.includes('R:' + x.port + ':ver') && !x.r.text.includes('R:' + (x.port === p1 ? p2 : p1))), 'concurrent status calls keep their own port for both sub-commands');

  const all = (await call('hooks', { game: 'all' })).text;
  ok([p1, p2, p3].every(p => all.includes('== port ' + p + ' ==\nR:' + p + ':hooks')), 'game=all fan-out sections');

  const dead = await call('hooks', { game: '1' });
  ok(dead.err && /not reachable on 127.0.0.1:1 /.test(dead.text), 'unreachable port error');
  const allDead = await call('hooks', { game: 'all' });
  ok(!allDead.err, 'game=all does not fail as a whole');

  await call('rd', { addr: '0x1000', n: 16, json: true, game: String(p2) });
  ok(received[p2].includes('json rd 0x1000 16'), 'json arg prefixes json: ' + received[p2].slice(-1));
  await call('reload_core', { json: true, game: String(p2) });
  ok(received[p2].slice(-1)[0] === 'reload', 'reload stays plain under json');
  await call('raw', { command: 'json status', json: true, game: String(p2) });
  ok(received[p2].slice(-1)[0] === 'json status', 'no double json prefix');

  const last = p => received[p].slice(-1)[0];
  await call('watchw', { target: 'sm+0x83', size: 1, secs: 20, stack: 8, game: String(p3) });
  ok(last(p3) === 'watchw sm+0x83 1 secs=20 stack=8', 'watchw start: ' + last(p3));
  await call('watchw', { target: 'pawn', path: 'Health.Cur', game: String(p3) });
  ok(last(p3) === 'watchw pawn Health.Cur', 'watchw prop: ' + last(p3));
  await call('watchw', { action: 'result', game: String(p3) });
  ok(last(p3) === 'watchw result', 'watchw result');
  await call('watchw', { action: 'stop', id: '3', game: String(p3) });
  ok(last(p3) === 'watchw stop 3', 'watchw stop');
  const w = await call('watchw', { target: '0x2000', size: 8, secs: 1, wait: true, game: String(p3) });
  ok(!w.err && last(p3) === 'watchw 0x2000 8 secs=1 wait', 'watchw wait extends the timeout beyond ANOMALY_TIMEOUT: ' + w.text);
  const sw = await call('sample', { cols: 'x=[pawn+0x1c8]:f32', hz: 50, secs: 1, wait: true, name: 'a', game: String(p3) });
  ok(!sw.err && last(p3) === 'sample x=[pawn+0x1c8]:f32 50 1 name=a wait', 'sample wait: ' + last(p3));
  const noWait = await call('con', { command: 'wait', game: String(p3) });
  ok(noWait.err && /timeout after 600 ms/.test(noWait.text), 'normal calls keep the short timeout');
  await call('sample', { action: 'status', game: String(p3) });
  ok(last(p3) === 'sample status', 'sample status');
  await call('agg', { top: 5, clear: true, game: String(p3) });
  ok(last(p3) === 'agg top=5 clear', 'agg: ' + last(p3));
  await call('scene', { require: 'world,pawn', prop: 'pawn|Health.Cur', game: String(p3) });
  ok(last(p3) === 'scene require=world,pawn prop=pawn|Health.Cur', 'scene: ' + last(p3));
  await call('snap', { target: 'pawn', len: 'auto', name: 'fx1', type: 'Actor', game: String(p3) });
  ok(last(p3) === 'snap pawn auto fx1 type=Actor', 'snap: ' + last(p3));
  await call('hits', { summary: 7, game: String(p3) });
  ok(last(p3) === 'agg top=7', 'hits summary');
  await call('hook', { target: 'Agent::IsInThreat', opts: 'agg stack=6 rcx=0x5', game: String(p3) });
  ok(last(p3) === 'hook Agent::IsInThreat agg stack=6 rcx=0x5', 'hook opts: ' + last(p3));
  await call('hook', { target: '0x1234', mode: 'ret', value: '1', exec: true, game: String(p3) });
  ok(last(p3) === 'hook 0x1234 ret 1 exec', 'hook ret exec: ' + last(p3));
  await call('bp', { kind: 'w', addr: 'pawn+0x10', len: 4, slot: 2, opts: 'agg', game: String(p3) });
  ok(last(p3) === 'bpw pawn+0x10 4 slot=2 agg', 'bp opts: ' + last(p3));
  await call('bp', { kind: 'x', addr: '0x1000', game: String(p3) });
  ok(last(p3) === 'bpx 0x1000', 'bpx: ' + last(p3));
  await call('mark', { text: 'hello world', game: String(p3) });
  ok(last(p3) === 'mark "hello world"', 'mark: ' + last(p3));

  for (const n of ['npcs', 'uid', 'comps', 'pose', 'quest', 'tarray', 'tset', 'tmap', 'layoutcheck', 'memdiff', 'batch', 'stats']) ok(names.includes(n), 'tool listed ' + n);
  ok(init.result.serverInfo.version === '2.2.0', 'server version');
  await call('hook', { target: '0x1234', hw: true, opts: 'agg', game: String(p3) });
  ok(last(p3) === 'hook 0x1234 hw agg', 'hook hw: ' + last(p3));
  await call('find', { query: 'isa:BP_AI_Human_C', max: 10, near: 'pawn', r: 300, loc: true, game: String(p3) });
  ok(last(p3) === 'find isa:BP_AI_Human_C 10 near=pawn r=300 loc', 'find near: ' + last(p3));
  await call('find', { query: 'isa:Obj', max: 1, new: 1, loc: true, game: String(p3) });
  ok(last(p3) === 'find isa:Obj 1 loc new=1', 'find new: ' + last(p3));
  await call('find', { query: 'x', hidden: 0, since: 5, nodef: true, game: String(p3) });
  ok(last(p3) === 'find x 30 since=5 hidden=0 nodef', 'find since/hidden: ' + last(p3));
  await call('npcs', { near: 'pawn', r: 800, max: 5, game: String(p3) });
  ok(last(p3) === 'npcs near=pawn r=800 max=5', 'npcs: ' + last(p3));
  await call('npcs', { new: 1, class: 'Human', all: true, game: String(p3) });
  ok(last(p3) === 'npcs class=Human new=1 all', 'npcs new: ' + last(p3));
  await call('uid', { obj: '0x10', game: String(p3) });
  ok(last(p3) === 'uid 0x10', 'uid: ' + last(p3));
  await call('comps', { obj: 'pawn', filter: 'Mesh', game: String(p3) });
  ok(last(p3) === 'comps pawn filter=Mesh', 'comps: ' + last(p3));
  await call('pose', { obj: 'pawn', bones: 'hands', ref: '0x1aec3d07a10', comps: true, space: 'parent', game: String(p3) });
  ok(last(p3) === 'pose pawn bones=hands ref=0x1aec3d07a10 comps space=parent', 'pose args: ' + last(p3));
  await call('quest', { verb: 'tap', sid: 'show', since: 5, match: 'Bar', game: String(p3) });
  ok(last(p3) === 'quest tap show since=5 sid=Bar', 'quest args: ' + last(p3));
  await call('tset', { obj: 'pawn', path: 'OwnedComponents', max: 3, game: String(p3) });
  ok(last(p3) === 'tset pawn OwnedComponents max=3', 'tset args: ' + last(p3));
  await call('layoutcheck', { class: 'SceneComponent', obj: 'pawn', game: String(p3) });
  ok(last(p3) === 'layoutcheck SceneComponent pawn', 'layoutcheck args: ' + last(p3));
  await call('memdiff', { target: '[pawn+0x650]', len: '0x400', secs: 10, hz: 30, wait: true, game: String(p3) });
  ok(last(p3) === 'memdiff [pawn+0x650] 0x400 10 hz=30 wait' || last(p3) === 'memdiff "[pawn+0x650]" 0x400 10 hz=30 wait', 'memdiff: ' + last(p3));
  await call('memdiff', { action: 'result', id: 'm1', game: String(p3) });
  ok(last(p3) === 'memdiff result m1', 'memdiff result');
  await call('stats', { clear: true, game: String(p3) });
  ok(last(p3) === 'stats clear', 'stats clear');
  const b = (await call('batch', { commands: ['mark probe7', 'npcs near=pawn r=600 max=3', 'comps pawn filter=Weapon'], game: String(p3) })).text;
  ok(b.split('\n').filter(l => l.startsWith('== ')).length === 3 && b.includes('== mark probe7\nR:' + p3 + ':mark probe7') && b.includes('R:' + p3 + ':comps pawn filter=Weapon'), 'batch sections: ' + b);
  const bj = (await call('batch', { commands: ['ping', 'scene'], json: true, game: String(p3) })).text;
  ok(received[p3].includes('json ping') && received[p3].slice(-1)[0] === 'json scene' && bj.startsWith('[') && bj.endsWith(']'), 'batch json: ' + bj);
  const tooMany = await call('batch', { commands: Array(51).fill('ping'), game: String(p3) });
  ok(tooMany.err && /max 50/.test(tooMany.text), 'batch cap 50');
  const bErr = (await call('batch', { commands: ['mark e1', 'con wait', 'mark e3'], game: String(p3) })).text;
  ok(!/^error:/.test(bErr) && bErr.includes('== mark e1\nR:' + p3 + ':mark e1') && /== con wait\nerror: .*timeout after 600 ms/.test(bErr) && bErr.includes('== mark e3\nR:' + p3 + ':mark e3'), 'batch keeps going after a failing command: ' + bErr);
  const bStop = (await call('batch', { commands: ['con wait', 'mark e4'], stop_on_error: true, game: String(p3) })).text;
  ok(/== con wait\nerror: /.test(bStop) && !bStop.includes('mark e4'), 'batch stop_on_error stops after a thrown error: ' + bStop);
  const bJErr = (await call('batch', { commands: ['con wait', 'ping'], json: true, game: String(p3) })).text;
  ok(bJErr.startsWith('[{"ok":false,"cmd":"con wait","error":"') && /timeout after 600 ms/.test(bJErr) && bJErr.includes('R:' + p3 + ':json ping') && bJErr.endsWith(']'), 'batch json error envelope: ' + bJErr);
  const bLong = (await call('batch', { commands: ['memdiff 0x1000 16 2 wait', 'sample x=0x1:f32 10 2 wait', 'watchw 0x2000 8 secs=1 wait', 'mark e5'], game: String(p3) })).text;
  ok(!/error:/.test(bLong) && (bLong.match(/^== /gm) || []).length === 4, 'batch gives memdiff/sample/watchw wait the long timeout: ' + bLong);
  const bAll = (await call('batch', { commands: ['ping', 'ver'], game: 'all' })).text;
  ok([p1, p2, p3].every(p => bAll.includes('== port ' + p + ' ==\n== ping\nR:' + p + ':ping')), 'batch fan-out with game=all');

  fs.writeFileSync(path.join(logdir, 's2dev_' + p2 + '.log'), 'line a\nline b\n');
  fs.writeFileSync(path.join(logdir, 's2dev.log'), 'shared one\n');
  const off = fs.statSync(path.join(logdir, 's2dev_' + p2 + '.log')).size;
  fs.appendFileSync(path.join(logdir, 's2dev_' + p2 + '.log'), 'line c\nline d hit #4\n');
  const l1 = await call('log', { n: 10, game: String(p2) });
  ok(l1.text === 'line a\nline b\nline c\nline d hit #4', 'per-port log: ' + l1.text);
  const l2 = await call('log', { n: 10, since: off, game: String(p2) });
  ok(l2.text === 'line c\nline d hit #4', 'log since offset: ' + l2.text);
  const l3 = await call('hits', { since: off, game: String(p2) });
  ok(l3.text === 'line d hit #4', 'hits since offset: ' + l3.text);
  const l4 = await call('log', { n: 10, game: String(p1) });
  ok(l4.text === 'shared one', 'fallback to shared log when the port has none: ' + l4.text);

  const cli = (args) => new Promise(res => execFile(process.execPath, [path.join(root, 'anomaly.js'), ...args], { env: Object.assign({}, process.env, { ANOMALY_PORT: '1' }), encoding: 'utf8' }, (e, so, se) => res({ stdout: so, status: e ? e.code : 0 })));
  let r = await cli(['--game', String(p2), 'ping']);
  ok(r.stdout.trim() === 'R:' + p2 + ':ping', 'cli --game: ' + r.stdout);
  r = await cli(['--json', '--port', String(p2), 'rd', '0x10', '8']);
  ok(r.stdout.trim() === 'R:' + p2 + ':json rd 0x10 8', 'cli --json: ' + r.stdout);
  r = await cli(['--json', '--game', String(p2), 'reload']);
  ok(r.stdout.trim() === 'R:' + p2 + ':reload', 'cli --json reload plain: ' + r.stdout);
  r = await cli(['--bogus']);
  ok(r.status === 2, 'cli unknown option');

  srv.stdin.end();
  s1.close(); s2.close(); s3.close();
  console.log(`mcp: ${pass} pass, ${fail} fail`);
  process.exit(fail ? 1 : 0);
}
main().catch(e => { console.log('ERROR ' + e.stack); process.exit(1); });
