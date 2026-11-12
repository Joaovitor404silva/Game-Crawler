'use strict';
// agent_* MCP tools: one call = one whole intent, run by agentd (src/agent) against the game's AgentLink.
// agentd is started on demand (bin/agentd.exe, autodiscovers the game) and exits with the game.
const net = require('net'), fs = require('fs'), path = require('path'), { spawn } = require('child_process');

module.exports = ({ S, str, int, HOME }) => {
  const PORT = +(process.env.AGENT_PORT || 27070);
  const EXE = process.env.AGENTD || path.join(HOME, 'bin', 'agentd.exe');
  let sock = null, chain = Promise.resolve(), starting = null;

  const connect = () => new Promise((res, rej) => {
    const s = net.createConnection({ host: '127.0.0.1', port: PORT }, () => res(s));
    s.once('error', rej);
  });
  function start() {
    if (starting) return starting;
    starting = new Promise((res, rej) => {
      if (!fs.existsSync(EXE)) return rej(new Error('agentd not built: run src\\build.bat agentd'));
      const p = spawn(EXE, ['--port', String(PORT)], { detached: true, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
      let err = '';
      p.stderr.on('data', d => { err += d; });
      p.stdout.once('data', () => { p.stdout.destroy(); p.stderr.destroy(); p.unref(); res(); });
      p.on('exit', c => rej(new Error((err.trim() || 'agentd exited ' + c) + ' (is the game running with an AgentLink host?)')));
    }).finally(() => { starting = null; });
    return starting;
  }
  async function link() {
    if (sock && !sock.destroyed) return sock;
    try { sock = await connect(); } catch { await start(); sock = await connect(); }
    sock.on('error', () => { sock = null; }); sock.on('close', () => { sock = null; });
    return sock;
  }
  function ask(line, timeoutMs) {
    const p = chain.then(async () => {
      const s = await link();
      return new Promise((res, rej) => {
        let buf = '';
        const done = (f, v) => { clearTimeout(t); s.removeListener('data', on); s.removeListener('close', cl); f(v); };
        const t = setTimeout(() => { s.destroy(); done(rej, new Error('agentd timeout after ' + timeoutMs + ' ms')); }, timeoutMs);
        const on = d => { buf += d.toString('utf8'); if (buf.endsWith('\n.\n')) done(res, buf.slice(0, -3)); };
        const cl = () => done(rej, new Error('agentd connection closed (game exited?)'));
        s.on('data', on); s.once('close', cl);
        s.write(line.replace(/[\r\n]/g, ' ') + '\n');
      });
    });
    chain = p.catch(() => {});
    return p;
  }
  const kv = (o) => Object.entries(o).filter(([, v]) => v !== undefined && v !== null && v !== '').map(([k, v]) => `${k}=${String(v).replace(/\s+/g, '')}`).join(' ');
  const num = (d) => ({ type: 'number', description: d });
  const EVENTS = 'comma list of: damaged, hp_low, enemy_near, area_changed, menu, died';

  async function look(a) {
    const r = await ask('look ' + kv({ width: a.width || 960, overlay: a.overlay === false ? 0 : undefined, save: a.save }), 15000);
    const m = /^image (.+?) (\d+)x(\d+)\n?/.exec(r);
    if (!m) return r;
    const data = fs.readFileSync(m[1]).toString('base64');
    const saved = a.save ? `saved ${m[1]}\n` : '';
    return { content: [{ type: 'image', data, mimeType: 'image/jpeg' }, { type: 'text', text: saved + r.slice(m[0].length) }] };
  }

  return [
    ['agent_state', 'Game state as short text: player position (host coords), speed, facing yaw, hp, flags (locked on, menu), nearby entities with distance and side, and events since the last call. Instant; use it instead of screenshots to know where you are.',
      S({}), () => ask('state', 5000)],
    ['agent_goto', 'Walk the player to a point and return when it ARRIVES, gets STUCK (after unstuck moves: back off, sidestep, dodge), reaches the spot on the ground but on another level (WRONG_LEVEL: target more than level_tol m above/below), TIMES OUT or an event fires. The controller steers at frame rate; you get one result. Target: `to` = "x,y,z", a mark name, "#<entity id>" (from agent_state), or several separated by ";" (waypoints); or fwd/right metres relative to the camera. A single `to` near ground walked before follows the learned walk graph around walls and up stairs ("via walk graph" in the result); path=0 walks straight.',
      S({ to: str('"x,y,z" | mark | "#<entity id>" | "a;b;c" waypoints'), fwd: num('metres forward of the camera (with right)'), right: num('metres to the right of the camera'), tol: num('arrival radius m (default 0.8)'), timeout: num('seconds (default 30)'), stop_on: str(EVENTS), path: num('0 = ignore the walk graph, walk straight') }),
      a => ask(['goto', a.to ? String(a.to).replace(/\s+/g, '') : '', kv({ fwd: a.fwd, right: a.right, tol: a.tol, timeout: a.timeout, stop_on: a.stop_on, path: a.path })].filter(Boolean).join(' '), ((a.timeout || 30) + 10) * 1000)],
    ['agent_face', 'Turn the camera (and so the run direction) toward a point, mark, entity id or absolute yaw; returns when aligned.',
      S({ to: str('"x,y,z" or mark'), yaw: num('absolute yaw degrees (as agent_state reports)'), entity: int('entity id') }),
      a => ask(['face', a.to ? String(a.to).replace(/\s+/g, '') : '', kv({ yaw: a.yaw, entity: a.entity })].filter(Boolean).join(' '), 10000)],
    ['agent_do', 'Run a short input timeline, then report how far the player moved. Steps separated by ";": tap <button> | hold <b[+b]> <ms> | wait <ms> | stick lx=<-1..1> ly=<-1..1 (-1 forward)> rx= ry= <ms>. Buttons: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad up down left right. Example: "tap r1; wait 400; tap r1" or "stick ly=-1 800; tap circle".',
      S({ steps: str('timeline') }, ['steps']), a => ask('do ' + a.steps, 60000)],
    ['agent_wait', 'Idle until one of the events fires or the timeout passes; returns the event and the state.',
      S({ until: str(EVENTS), timeout: num('seconds (default 10)') }), a => ask('wait ' + kv({ until: a.until, timeout: a.timeout }), ((a.timeout || 10) + 5) * 1000)],
    ['agent_look', 'One frame of the game window (JPEG) with an overlay: header with position/facing, labels at the player pivot, saved marks and entities projected through the game camera. Use when text state is not enough (what is ahead, menus, visual checks).',
      S({ width: int('image width px (default 960)'), overlay: { type: 'boolean', description: 'draw labels (default true)' }, save: str('keep this frame as evidence: agent/logs/<game>-<save>.jpg (else overwritten)') }), look],
    ['agent_mark', 'Named places, saved per game: set (current position), list (with distance), remove. Marks work as goto/face targets.',
      S({ action: { type: 'string', enum: ['set', 'list', 'remove'] }, name: str('mark name (no spaces)') }),
      a => { const act = a.action || (a.name ? 'set' : 'list'); return ask(act === 'list' ? 'marks' : (act === 'remove' ? 'unmark ' : 'mark ') + (a.name || '').replace(/\s+/g, '_'), 5000); }],
    ['agent_calibrate', 'Measure the movement profile (run speed, start lag, overrun, strafe sign, turn rate) and save it. Needs ~6 m of open ground ahead and to the right. Run once per new game.',
      S({}), () => ask('calibrate', 30000)],
    ['agent_tp', 'Teleport the player to "x,y,z" or a mark (hosts with teleport, e.g. Unreal; not Bloodborne).',
      S({ to: str('"x,y,z" or mark') }, ['to']), a => ask('tp ' + String(a.to).replace(/\s+/g, ''), 5000)],
    ['agent_stop', 'Release all inputs now.', S({}), () => ask('stop', 5000)],
  ];
};
