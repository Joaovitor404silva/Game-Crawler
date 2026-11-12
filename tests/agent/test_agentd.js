// End-to-end: simhost (toy world behind a real AgentLink) + agentd over TCP.
// Checks goto/face/do/mark/calibrate, the ttl safety (agentd killed mid-goto -> pawn stops) and exit-with-game.
const { spawn } = require('child_process');
const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');

const here = __dirname;
const AGENTD = process.env.AGENTD || path.join(here, '..', '..', 'bin', 'agentd.exe');
const PORT = 27170;
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'agentd-test-'));
let pass = 0, fail = 0;
const check = (ok, what, extra = '') => { ok ? pass++ : fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${extra ? '  ' + extra : ''}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function startSim(args = []) {
  return new Promise((res, rej) => {
    const p = spawn(path.join(here, 'simhost.exe'), args);
    p.stdout.once('data', (d) => res({ p, pid: parseInt(d.toString(), 10) }));
    p.on('error', rej);
  });
}
function startAgentd(pid, d = dir) {
  return new Promise((res, rej) => {
    const p = spawn(AGENTD, ['--pid', String(pid), '--port', String(PORT), '--dir', d]);
    p.stdout.once('data', () => res(p));
    p.stderr.on('data', (d) => process.stderr.write(d));
    p.on('error', rej);
    p.on('exit', (c) => { if (c) rej(new Error('agentd exit ' + c)); });
  });
}
function client() {
  return new Promise((res, rej) => {
    const s = net.connect(PORT, '127.0.0.1', () => res(s));
    s.on('error', rej);
    let buf = '', waiters = [];
    s.on('data', (d) => {
      buf += d.toString();
      let i;
      while ((i = buf.indexOf('\n.\n')) >= 0) { const r = buf.slice(0, i); buf = buf.slice(i + 3); waiters.shift()(r); }
    });
    s.ask = (line) => new Promise((r) => { waiters.push(r); s.write(line + '\n'); });
  });
}
const pos = (t) => { const m = /pos ([-\d.]+) ([-\d.]+) ([-\d.]+)/.exec(t); return m ? [+m[1], +m[2], +m[3]] : null; };
const dist = (a, b) => Math.hypot(a[0] - b[0], a[2] - b[2]);

(async () => {
  let sim = await startSim(), ad = await startAgentd(sim.pid), c = await client();
  try {
    let ping = await c.ask('ping');
    for (let i = 0; i < 50 && !/pos=yes/.test(ping); ++i) { await sleep(20); ping = await c.ask('ping'); } // sim's first frame
    check(/pos=yes/.test(ping) && /game=sim/.test(ping), 'ping', ping);

    let t0 = Date.now(), r = await c.ask('goto 0,0,10');
    let p = pos(r.split('\n')[1]);
    check(/^arrived/.test(r) && dist(p, [0, 0, 10]) < 1.0, 'goto 0,0,10', `${Date.now() - t0} ms: ${r.split('\n')[0]}`);

    r = await c.ask('mark spot');
    check(/marked spot/.test(r), 'mark');
    r = await c.ask('goto fwd=4 right=3 tol=0.6');
    check(/^arrived/.test(r), 'goto relative', r.split('\n')[0]);
    r = await c.ask('goto spot tol=0.6');
    p = pos(r.split('\n')[1]);
    check(/^arrived/.test(r) && dist(p, [0, 0, 10]) < 1.0, 'goto mark', r.split('\n')[0]);
    check(fs.existsSync(path.join(dir, 'maps', 'sim.txt')), 'marks saved');

    r = await c.ask('face yaw=90');
    const fy = /facing (-?\d+) deg/.exec(r);
    check(/^aligned/.test(r) && fy && Math.abs(+fy[1] - 90) <= 8, 'face yaw=90', r.replace('\n', ' | '));

    r = await c.ask('do stick ly=-1 700; wait 100; tap circle');
    const mv = /moved (\S+) m/.exec(r);
    check(mv && +mv[1] > 0.8, 'do timeline', r.split('\n')[0]);

    r = await c.ask('goto 0,0,10;0,0,30;0,0,40 timeout=2');
    check(/^timeout/.test(r), 'goto timeout', r.split('\n')[0]);

    r = await c.ask('calibrate');
    const run = /run (\S+) m\/s/.exec(r), rs = /right_sign ([+-]\d)/.exec(r);
    check(run && Math.abs(+run[1] - 3.3) < 0.4 && rs && rs[1] === '+1', 'calibrate', r);
    check(fs.readFileSync(path.join(dir, 'profiles', 'sim.ini'), 'utf8').includes('run_speed'), 'profile saved');

    // An event from an earlier command must not stop a new goto (stop_on watches only what happens during it).
    await c.ask('do tap touchpad; wait 200');
    r = await c.ask('goto fwd=3 stop_on=damaged');
    check(/^arrived/.test(r), 'goto stop_on ignores events from earlier commands', r.split('\n')[0]);
    await c.ask('state');

    r = await c.ask('bogus');
    check(/^error/.test(r), 'unknown command');

    // A second client (a script) is served while the first (the MCP server) keeps its connection open.
    const c2 = await client();
    r = await Promise.race([c2.ask('ping'), sleep(2000).then(() => 'timeout')]);
    check(/^pong/.test(r), 'second client while the first stays open', r.split('\n')[0]);
    r = await c.ask('ping');
    check(/^pong/.test(r), 'first client still served');
    c2.destroy();

    // ttl: kill agentd mid-goto, the host must stop on its own.
    c.write('goto 0,0,200 timeout=60\n');
    await sleep(1500);
    ad.kill();
    await sleep(1200);
    ad = await startAgentd(sim.pid); c = await client();
    const a = pos(await c.ask('state')); await sleep(400); const b = pos(await c.ask('state'));
    check(a && b && dist(a, b) < 0.05, 'ttl: pawn stops when agentd dies', `drift ${a && b ? dist(a, b).toFixed(3) : '?'} m`);

    // unstuck: short wall in the way
    sim.p.kill(); await sleep(300);
    sim = await startSim(['--walls']); ad.kill(); await sleep(200);
    ad = await startAgentd(sim.pid); c = await client();
    r = await c.ask('goto 0,0,9 timeout=20');
    check(/^arrived/.test(r) && /[12] unstuck/.test(r), 'unstuck around a short wall', r.split('\n')[0]);

    // exit with the game
    ad.removeAllListeners('exit');
    const exited = new Promise((res) => ad.on('exit', () => res(true)));
    sim.p.kill();
    check(await Promise.race([exited, sleep(2000).then(() => false)]), 'agentd exits with the game');
    const logs = fs.readdirSync(path.join(dir, 'logs'));
    check(logs.length > 0 && fs.statSync(path.join(dir, 'logs', logs[0])).size > 100, 'flight log written');
    const results = logs.flatMap((f) => fs.readFileSync(path.join(dir, 'logs', f), 'utf8').split('\n'))
      .filter((l) => l.includes('"result"')).map((l) => JSON.parse(l));
    check(results.some((x) => x.status === 'arrived') && results.some((x) => x.status === 'timeout') &&
          results.every((x) => x.result === 'goto' && x.straight >= 0), 'goto result rows in the flight log', `${results.length} rows`);

    // Walk graph (own data dir: a fresh world): straight is stuck at a long wall, the route around it is walked
    // once, then plain gotos both ways plan through the learned graph.
    const dir2 = fs.mkdtempSync(path.join(os.tmpdir(), 'agentd-graph-'));
    try {
      sim = await startSim(['--longwall']); ad = await startAgentd(sim.pid, dir2); c.destroy(); c = await client();
      for (let i = 0; i < 50 && !/pos=yes/.test(await c.ask('ping')); ++i) await sleep(20);
      r = await c.ask('goto 0,0,10 path=0 timeout=20');
      check(/^stuck/.test(r), 'graph: straight goto stuck at a long wall', r.split('\n')[0]);
      await c.ask('goto 0,0,0');
      r = await c.ask('goto 9.5,0,2;9.5,0,7;0,0,10 timeout=40');
      check(/^arrived/.test(r), 'graph: route walked once by waypoints', r.split('\n')[0]);
      r = await c.ask('goto 0,0,0 timeout=40');
      check(/^arrived/.test(r) && /via walk graph/.test(r), 'graph: plain goto back plans around the wall', r.split('\n')[0]);
      r = await c.ask('goto #7 tol=1.5 timeout=40');
      check(/^arrived/.test(r) && /via walk graph/.test(r), 'graph: goto an entity id plans around the wall', r.split('\n')[0]);
      r = await c.ask('graph');
      check(/walk graph: \d+ nodes/.test(r) && fs.existsSync(path.join(dir2, 'maps', 'sim.graph')), 'graph: saved', r);
    } finally {
      try { sim.p.kill(); } catch {}
      await sleep(300);
      fs.rmSync(dir2, { recursive: true, force: true });
    }

    // A door closes on a learned route: the planned goto gets stuck and forgets that leg, the next one goes around.
    const dir3 = fs.mkdtempSync(path.join(os.tmpdir(), 'agentd-door-'));
    try {
      sim = await startSim(); ad = await startAgentd(sim.pid, dir3); c.destroy(); c = await client();
      for (let i = 0; i < 50 && !/pos=yes/.test(await c.ask('ping')); ++i) await sleep(20);
      await c.ask('goto 0,0,10'); await c.ask('goto 0,0,0');
      await c.ask('goto 9.5,0,2;9.5,0,7;0,0,10 timeout=40'); await c.ask('goto 0,0,0 timeout=40');
      await c.ask('do tap up; wait 100');
      r = await c.ask('goto 0,0,10 timeout=40');
      check(/^stuck/.test(r) && /forgot [1-9]\d* walk-graph edge/.test(r), 'graph: closed door -> stuck, edges forgotten', r.split('\n')[0]);
      r = await c.ask('goto 0,0,0 timeout=40'); // back to the start (around, the door is shut)
      r = await c.ask('goto 0,0,10 timeout=40');
      check(/^arrived/.test(r) && /via walk graph/.test(r), 'graph: next goto goes around the door', r.split('\n')[0]);
    } finally {
      try { sim.p.kill(); } catch {}
      await sleep(300);
      fs.rmSync(dir3, { recursive: true, force: true });
    }
  } catch (e) {
    fail++; console.log('FAIL exception', e);
  } finally {
    try { sim.p.kill(); } catch {}
    try { ad.kill(); } catch {}
    c.destroy();
    fs.rmSync(dir, { recursive: true, force: true });
  }
  console.log(`agentd tests: ${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})();
