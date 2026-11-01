#!/usr/bin/env node
'use strict';
const net = require('net'), fs = require('fs'), path = require('path');
const { AsyncLocalStorage } = require('async_hooks');
const PORT = +(process.env.ANOMALY_PORT || 27060), TIMEOUT = +(process.env.ANOMALY_TIMEOUT || 30000), CAP = +(process.env.ANOMALY_MCP_CAP || 12000);
const HOME = process.env.ANOMALY_HOME || path.resolve(__dirname, '..');
const NAMES = process.env.ANOMALY_NAMES || path.join(HOME, 'names', 'names.tsv'), DECOMP = process.env.ANOMALY_DECOMP || path.join(HOME, 'decomp');
const LOGDIR = process.env.ANOMALY_LOGDIR || HOME, LOG = path.join(LOGDIR, 's2dev.log');

const GAMES = { B: 27060, A: 27061, D: 27062, C: 27063, E: 27064 };
const ALL_PORTS = (process.env.ANOMALY_ALL_PORTS || Object.values(GAMES).join(',')).split(',').map(Number).filter(Boolean);
const ctx = new AsyncLocalStorage();
const socks = {}, chains = {};
function connect(port) {
  return new Promise((res, rej) => {
    const s = net.createConnection({ host: '127.0.0.1', port }, () => res(s));
    s.once('error', rej);
  });
}
async function sendOnce(line, port, timeout) {
  let s = socks[port];
  if (!s || s.destroyed) { s = socks[port] = await connect(port); s.on('error', () => { socks[port] = null; }); s.on('close', () => { socks[port] = null; }); }
  return new Promise((res, rej) => {
    let buf = '';
    const done = (f, v) => { clearTimeout(t); s.removeListener('data', on); s.removeListener('close', cl); f(v); };
    const t = setTimeout(() => { s.destroy(); done(rej, new Error('timeout after ' + timeout + ' ms')); }, timeout);
    const on = d => { buf += d.toString('utf8'); if (buf === '.\n' || buf.endsWith('\n.\n')) done(res, buf.slice(0, -2).replace(/\n$/, '')); };
    const cl = () => done(rej, new Error('connection closed'));
    s.on('data', on); s.once('close', cl);
    s.write(line.replace(/[\r\n]/g, ' ') + '\n');
  });
}
const loaderOnly = line => /^\s*reload\s*$/i.test(line);
function game(line) {
  const c = ctx.getStore() || { port: PORT };
  const port = c.port, timeout = c.timeout || TIMEOUT;
  const send = c.json && !loaderOnly(line) && !/^\s*json\b/i.test(line) ? 'json ' + line : line;
  const prev = chains[port] || Promise.resolve();
  const p = prev.then(() => sendOnce(send, port, timeout).catch(e => { throw new Error('S2Dev not reachable on 127.0.0.1:' + port + ' (is the dev game running? start it with launch.ps1): ' + e.message); }));
  chains[port] = p.catch(() => {});
  return p;
}
function withTimeout(secs, fn) {
  if (!secs) return fn();
  const c = ctx.getStore() || { port: PORT };
  return ctx.run(Object.assign({}, c, { timeout: Math.max(TIMEOUT, secs * 1000) }), fn);
}
function resolvePort(g) {
  if (g === undefined || g === null || g === '') return PORT;
  return GAMES[String(g).toUpperCase()] || +g || PORT;
}
function waitSecs(line) {
  const t = String(line).trim().split(/\s+/), c = (t[0] || '').toLowerCase();
  if (!t.includes('wait')) return 0;
  if (c === 'memdiff' || c === 'sample') { const n = parseInt(t[3], 10); return n > 0 ? n + 15 : 0; }
  if (c === 'watchw') { const m = t.map(x => /^secs=(\d+)$/.exec(x)).find(Boolean); return (m ? +m[1] : 30) + 15; }
  return 0;
}
const q = s => { s = String(s); return /[\s"]/.test(s) || s === '' ? '"' + s.replace(/"/g, '') + '"' : s; };
const cmd = (...parts) => game(parts.filter(x => x !== undefined && x !== null && x !== '').map(q).join(' '));

let names = null, namesMtime = 0;
function loadNames() {
  const st = fs.statSync(NAMES); if (names && st.mtimeMs === namesMtime) return names;
  names = []; namesMtime = st.mtimeMs;
  for (const l of fs.readFileSync(NAMES, 'utf8').split(/\r?\n/).slice(1)) { const c = l.split('\t'); if (c.length >= 3) names.push({ rva: parseInt(c[0], 16), kind: c[1], name: c[2] }); }
  return names;
}
let idx = null, idxMtime = 0;
function loadIndex() {
  const f = path.join(DECOMP, 'index.tsv'), st = fs.statSync(f); if (idx && st.mtimeMs === idxMtime) return idx;
  idx = []; idxMtime = st.mtimeMs;
  for (const l of fs.readFileSync(f, 'utf8').split(/\r?\n/).slice(1)) { const c = l.split('\t'); if (c.length >= 5) idx.push({ rva: parseInt(c[0], 16), name: c[1], file: c[2], line: +c[3], size: +c[4], callers: +(c[5] || 0) }); }
  idx.sort((a, b) => a.rva - b.rva);
  return idx;
}
function decomp(what, maxLines) {
  const ix = loadIndex(); let rva = null, label = what;
  const s = String(what).trim();
  if (/^(0x)?[0-9a-f]+$/i.test(s) && (/^0x/i.test(s) || /[a-f]/i.test(s) || s.length >= 5)) { rva = parseInt(s.replace(/^0x/i, ''), 16); if (rva >= 0x140000000) rva -= 0x140000000; }
  else {
    const ls = s.toLowerCase();
    let e = ix.find(x => x.name.toLowerCase() === ls);
    if (!e) { const n = loadNames().find(x => x.name.toLowerCase() === ls); if (n) rva = n.rva; }
    else rva = e.rva;
    if (rva === null) {
      const cands = ix.filter(x => x.name.toLowerCase().includes(ls)).slice(0, 30).map(x => x.rva.toString(16) + '  ' + x.name);
      return cands.length ? 'no exact match; candidates (decomp index):\n' + cands.join('\n') : 'no function named ' + s + ' in the decomp index (' + ix.length + ' functions indexed) or names.tsv';
    }
  }
  let lo = 0, hi = ix.length - 1, hit = -1;
  while (lo <= hi) { const m = (lo + hi) >> 1; if (ix[m].rva <= rva) { hit = m; lo = m + 1; } else hi = m - 1; }
  const e = hit >= 0 ? ix[hit] : null;
  if (!e || rva >= e.rva + Math.max(e.size, 1)) return `rva 0x${rva.toString(16)} not in the decomp index yet (${ix.length} functions indexed; the decompile may still be running)`;
  const lines = fs.readFileSync(path.join(DECOMP, e.file), 'utf8').split(/\r?\n/);
  const out = [];
  for (let i = e.line - 1; i < lines.length && out.length < maxLines; i++) { if (i > e.line - 1 && lines[i].startsWith('// ==== ')) break; out.push(lines[i]); }
  return `${e.name} rva 0x${e.rva.toString(16)} (+0x${(rva - e.rva).toString(16)}) size ${e.size} callers ${e.callers} ${e.file}:${e.line}\n` + out.join('\n');
}
function namesSearch(query, limit) {
  const ql = String(query).toLowerCase(), r = []; let n = 0;
  for (const e of loadNames()) if (e.name.toLowerCase().includes(ql)) { if (r.length < limit) r.push(`rva 0x${e.rva.toString(16)}  ${e.kind.padEnd(10)} ${e.name}`); n++; }
  return r.join('\n') + `\n${n} matches (names.tsv, ${names.length} entries)`;
}
function logFile() {
  const c = ctx.getStore() || { port: PORT };
  const pf = path.join(LOGDIR, 's2dev_' + c.port + '.log');
  return fs.existsSync(pf) ? pf : LOG;
}
function logTail(n, filter, since) {
  const f = logFile(), st = fs.statSync(f);
  const from = since !== undefined && since !== null && since !== '' ? Math.max(0, Math.min(+since, st.size)) : Math.max(0, st.size - 512 * 1024);
  const len = Math.min(st.size - from, 8 * 1024 * 1024), fd = fs.openSync(f, 'r'), b = Buffer.alloc(len);
  fs.readSync(fd, b, 0, len, from); fs.closeSync(fd);
  let lines = b.toString('utf8').split(/\r?\n/).filter(Boolean);
  if (filter) { const re = new RegExp(filter, 'i'); lines = lines.filter(l => re.test(l)); }
  return lines.slice(-n).join('\n') || '(no matching log lines)';
}

const S = (props, req) => ({ type: 'object', properties: props, required: req || [] });
const str = d => ({ type: 'string', description: d }), int = d => ({ type: 'integer', description: d });
const OBJ = str('UObject: pawn, pc, object name, full path, or 0x address');
const OPTS = str('extra options appended verbatim: agg (count per unique stack instead of one log line per hit), stack[=N] (caller chain depth), max=N, rcx=<expr>/rdx=/r8=/r9= (only count hits where the register equals the value)');
const ADDR = str('address expression: 0x7ff..., base+0x50C40C, exe+rva, [pawn+0x338]+0x10, Class::Func (names.tsv), object name');
const tools = [
  ['status', 'Ping the dev game (pid, base, tick count, core generation) and the core build time.', S({}), async () => (await game('ping')) + '\n' + (await game('ver'))],
  ['find', 'Find UObjects by name substring, class:ExactClass or isa:BaseClass. Optional: near/r (distance filter, sorted nearest first), loc (print RootComponent.RelativeLocation), new (only the N most recently created), since (object index above N), hidden (bHidden filter), nodef (skip Default__ objects).', S({ query: str('substring | class:Name | isa:Name'), max: int('max rows (default 30)'), near: str('object (pawn, 0x..) or x,y,z; keeps objects within r cm of it'), r: int('radius cm for near (default 500)'), loc: { type: 'boolean', description: 'print location (and idx, hidden)' }, new: int('only the N highest object indexes that match'), since: int('only objects with index greater than this'), hidden: int('0 or 1: filter on bHidden'), nodef: { type: 'boolean', description: 'skip Default__ objects' } }, ['query']),
    a => cmd('find', a.query, a.max || 30, a.near ? 'near=' + a.near : '', a.r !== undefined ? 'r=' + a.r : '', a.loc ? 'loc' : '', a.new ? 'new=' + a.new : '', a.since !== undefined ? 'since=' + a.since : '', a.hidden !== undefined ? 'hidden=' + a.hidden : '', a.nodef ? 'nodef' : '')],
  ['obj', 'Dump a UObject: class chain, outer and every property with its live value.', S({ obj: OBJ }, ['obj']), a => cmd('obj', a.obj)],
  ['get', 'Read one property (dotted path, [i] for arrays/static arrays, descends structs and object pointers).', S({ obj: OBJ, path: str('e.g. CharacterMovement.MaxWalkSpeed') }, ['obj', 'path']), a => cmd('get', a.obj, a.path)],
  ['set', 'Write one property. Values: number, true/false, x,y,z for structs, object name/0x/null, FName text.', S({ obj: OBJ, path: str('property path'), value: str('new value') }, ['obj', 'path', 'value']), a => cmd('set', a.obj, a.path, a.value)],
  ['call', 'Call a UFunction via ProcessEvent on the game thread; returns return/out values.', S({ obj: OBJ, func: str('function name'), args: { type: 'array', items: { type: 'string' }, description: 'input params in order' } }, ['obj', 'func']), a => cmd('call', a.obj, a.func, ...(a.args || []))],
  ['funcs', 'List UFunctions of a class or object (signature + native impl symbol).', S({ cls: str('class name or object'), filter: str('name substring') }, ['cls']), a => cmd('funcs', a.cls, a.filter)],
  ['props', 'List properties (offset, size, type) of a class/struct or object.', S({ cls: str('class/struct name or object'), filter: str('name substring') }, ['cls']), a => cmd('props', a.cls, a.filter)],
  ['classes', 'List classes whose name contains a substring.', S({ filter: str('substring') }), a => cmd('classes', a.filter)],
  ['rd', 'Hex dump memory.', S({ addr: ADDR, n: int('bytes (default 64, max 4096)') }, ['addr']), a => cmd('rd', a.addr, a.n || 64)],
  ['wr', 'Write bytes (hex string) to memory.', S({ addr: ADDR, hex: str('e.g. 90 90 or c3') }, ['addr', 'hex']), a => game('wr ' + q(a.addr) + ' ' + a.hex)],
  ['ptr', 'Evaluate a pointer chain expression and show the target (qword/float/int, symbol, object).', S({ expr: str('e.g. [[pawn+0x650]+0x660]+0x38') }, ['expr']), a => game('ptr ' + a.expr)],
  ['sym', 'Address -> rva + symbol name (names.tsv, .pdata function start, natives).', S({ addr: ADDR }, ['addr']), a => cmd('sym', a.addr)],
  ['addr', 'Name -> live address (exact or substring match in names.tsv).', S({ name: str('e.g. Agent::IsChangingWeapon') }, ['name']), a => cmd('addr', a.name)],
  ['names', 'Search names.tsv offline (works without the game): rva, kind, name.', S({ query: str('substring'), limit: int('default 40') }, ['query']), a => namesSearch(a.query, a.limit || 40)],
  ['decomp', 'Decompiled C (Ghidra dump in ANOMALY_DECOMP) of a function by name or rva (rva may point inside the function). Offline.', S({ what: str('name like Agent::IsChangingWeapon, or rva like 0x441a7e'), max_lines: int('default 200') }, ['what']), a => decomp(a.what, a.max_lines || 200)],
  ['native', 'Look up natives (exec thunk + impl addresses) by name substring.', S({ name: str('substring') }, ['name']), a => cmd('native', a.name)],
  ['hook', 'Inline hook an address or native. log = log args+caller per hit; block = return 0; ret = return value.', S({ target: ADDR, mode: { type: 'string', enum: ['log', 'block', 'ret'] }, value: str('return value for ret'), exec: { type: 'boolean', description: 'hook the exec thunk instead of the impl' }, hw: { type: 'boolean', description: 'hardware exec breakpoint instead of patched bytes (log only, aggregates); use when the target is already detoured by another module' }, opts: OPTS }, ['target']), a => game(['hook', q(a.target), a.mode && a.mode !== 'log' ? a.mode : '', a.mode === 'ret' ? q(a.value || '0') : '', a.exec ? 'exec' : '', a.hw ? 'hw' : '', a.opts || ''].filter(Boolean).join(' '))],
  ['unhook', 'Remove a hook by id, or all.', S({ id: str('hook id or all') }), a => cmd('unhook', a.id || 'all')],
  ['hooks', 'List hooks with hit counts.', S({}), () => game('hooks')],
  ['bp', 'Hardware breakpoint (DR0-3, all threads): x = execute, w = write, rw = read/write. Hits go to the log (see hits).', S({ kind: { type: 'string', enum: ['x', 'w', 'rw'] }, addr: ADDR, len: int('1|2|4|8 for w/rw (default 4)'), slot: int('debug register 0-3 (shared game: use only your own slots); default = first free'), opts: OPTS }, ['kind', 'addr']), a => game([a.kind === 'x' ? 'bpx' : a.kind === 'w' ? 'bpw' : 'bprw', q(a.addr), a.kind === 'x' ? '' : (a.len || 4), a.slot !== undefined ? 'slot=' + a.slot : '', a.opts || ''].filter(x => x !== '').join(' '))],
  ['bpc', 'Clear hardware breakpoint slot(s).', S({ slot: str('0-3 or all') }), a => cmd('bpc', a.slot || 'all')],
  ['bps', 'List hardware breakpoints with hit counts.', S({}), () => game('bps')],
  ['hits', 'Recent hook / breakpoint / watch / plugin events from the S2Dev log (offline read). With summary=N: instead the top N unique call stacks per aggregating hook/bp/watch (live `agg top=N`).', S({ n: int('lines (default 40)'), filter: str('regex (default hook|bp|watch|plug hits)'), summary: int('top N stack groups (needs hooks/bps armed with opts "agg")'), since: int('log byte offset from the `mark` tool; only lines after it') }), a => a.summary ? game('agg top=' + a.summary) : logTail(a.n || 40, a.filter || '(hit #|^\\[[^\\]]+\\] bp\\d|watch \\d|\\[plug )', a.since)],
  ['watchw', 'WHO WRITES THIS FIELD: hardware write watch on up to 32 bytes (8 byte chunks over DR0-3, all threads) with stack-walked, aggregated writers (function names, old/new value, count). action=start (default) arms; result/list/stop read or release. Auto-released after secs. Needs free debug registers.', S({ action: { type: 'string', enum: ['start', 'result', 'list', 'stop'] }, target: str('start: address expression (0x.., obj+0x83, [pawn+0x338]+0x10) or an object name together with path'), size: int('bytes 1-64 (aligned chunks must fit in the free debug registers)'), path: str('start with an object as target: property path whose storage is watched'), secs: int('auto release after N seconds (default 30)'), stack: int('caller frames (default 12)'), max: int('top groups to print (default 20)'), wait: { type: 'boolean', description: 'block until the watch is released and return the result' }, id: str('watch id for result/stop (default last/all)'), opts: OPTS }, []),
    a => { const act = a.action || 'start'; if (act !== 'start') return game(['watchw', act, a.id || ''].filter(Boolean).join(' ')); if (!a.target) throw new Error('target is required'); const o = ['watchw', q(a.target), a.path ? q(a.path) : (a.size || 4)]; if (a.secs) o.push('secs=' + a.secs); if (a.stack) o.push('stack=' + a.stack); if (a.max) o.push('max=' + a.max); if (a.wait) o.push('wait'); if (a.opts) o.push(a.opts); return withTimeout(a.wait ? (a.secs || 30) + 15 : 0, () => game(o.join(' '))); }],
  ['agg', 'Aggregated hook/bp/watch hits: count per unique stack (top N groups), optionally clear the tables.', S({ top: int('top N groups per source (default 10)'), clear: { type: 'boolean', description: 'clear the tables after reading' }, src: str('limit to hookN | bpN | wN') }), a => game(['agg', a.top ? 'top=' + a.top : '', a.clear ? 'clear' : '', a.src || ''].filter(Boolean).join(' '))],
  ['scene', 'Preflight: is a world loaded, player controller/pawn present, level name. require= makes it fail loudly (ok=false) so a test script stops early. prop= reads extra properties.', S({ require: str('comma list: world,pc,pawn,level:<text>,class:<text>'), prop: str('comma list of obj|Prop.Path to read') }), a => game(['scene', a.require ? 'require=' + a.require : '', a.prop ? 'prop=' + a.prop : ''].filter(Boolean).join(' '))],
  ['snap', 'Fixture: dump a typed struct/object memory to LogDir\\snaps\\<name>.json (+ .bin) for offline tests.', S({ target: str('address expression, obj+off, or object'), len: str('byte count or auto'), name: str('fixture name'), type: str('Class or Struct name for typed fields') }, ['target', 'len', 'name']), a => game(['snap', q(a.target), a.len, a.name, a.type ? 'type=' + a.type : ''].filter(Boolean).join(' '))],
  ['sample', 'In-core sampler (no round trips): columns of memory/property values at hz for secs into a TSV (tick, ms, columns). action=start (default) | status | stop | show.', S({ action: { type: 'string', enum: ['start', 'status', 'stop', 'show'] }, cols: str('comma list name=<addr expr>:<f32|f64|i8..u64|hex|bool> or name=@obj|Prop.Path'), hz: int('1-200'), secs: int('1-3600'), name: str('file name tag'), file: str('output path (default LogDir\\samples\\...)'), wait: { type: 'boolean', description: 'block until done and show the last rows' }, id: str('sampler id for stop/show'), n: int('rows for show') }, []),
    a => { const act = a.action || 'start'; if (act !== 'start') return game(['sample', act, a.id || '', a.n ? 'n=' + a.n : ''].filter(Boolean).join(' ')); if (!a.cols || !a.hz || !a.secs) throw new Error('cols, hz and secs are required'); const o = ['sample', q(a.cols), a.hz, a.secs]; if (a.name) o.push('name=' + a.name); if (a.file) o.push('file=' + a.file); if (a.wait) o.push('wait'); return withTimeout(a.wait ? a.secs + 15 : 0, () => game(o.join(' '))); }],
  ['npcs', 'Per-NPC census (every living Obj character): uid, address, class, hp, distance, location, hidden, object index. Sorted by distance to near (default pawn). Use new=1 right after a spawn to get the spawned NPC.', S({ near: str('object or x,y,z (default pawn)'), r: int('radius cm filter'), class: str('class name substring'), uid: int('only this uid'), new: int('only the N most recently created'), max: int('max rows (default 50)'), all: { type: 'boolean', description: 'include the local pawn' } }),
    a => cmd('npcs', a.near ? 'near=' + a.near : '', a.r !== undefined ? 'r=' + a.r : '', a.class ? 'class=' + a.class : '', a.uid !== undefined ? 'uid=' + a.uid : '', a.new ? 'new=' + a.new : '', a.max ? 'max=' + a.max : '', a.all ? 'all' : '')],
  ['uid', 'UID of an actor (from its UIDActorComponent); usable with console functions such as XKillNpcByUID.', S({ obj: OBJ }, ['obj']), a => cmd('uid', a.obj)],
  ['comps', 'Components of an actor as an attachment tree: class, name, socket, parent, relative location, visibility, mesh and anim class for skeletal meshes.', S({ obj: OBJ, filter: str('substring matched against class or name'), flat: { type: 'boolean', description: 'no tree order/indent' } }, ['obj']), a => cmd('comps', a.obj, a.filter ? 'filter=' + a.filter : '', a.flat ? 'flat' : '')],
  ['pose', 'Bone, socket and attachment table of an actor: bone transforms (world/comp/parent space), optional reference body (ref) with d_loc/d_rot per bone, attached components with their socket gap. Flags TWIST (hand vs fore_arm roll > 95 deg, or d_rot > 35 deg vs ref) and HOVER (component > 3 cm from its socket).', S({ obj: OBJ, bones: str('hands (default) | arms | legs | all | comma list of bone names'), ref: str('reference actor (stock NPC) to compare against'), comps: { type: 'boolean', description: 'list components attached to a socket with their gap to it' }, space: str('world (default) | comp | parent') }, ['obj']), a => cmd('pose', a.obj, a.bones ? 'bones=' + a.bones : '', a.ref ? 'ref=' + a.ref : '', a.comps ? 'comps' : '', a.space ? 'space=' + a.space : '')],
  ['quest', 'Live quest graph: list (quests with active/finished counts), state <QuestSID|NodeSID> (active|finished|none, quest, node object), tap on|off|clear|show (poll-diff event ring: start/end/done per node, since=<mark>), end/start <NodeSID> (state before and after), layout (offsets and sanity check).', S({ verb: { type: 'string', enum: ['list', 'state', 'tap', 'end', 'start', 'layout'] }, sid: str('quest or node SID (state/end/start) or on|off|clear|show (tap)'), filter: str('list: substring filter'), since: int('tap show: mark from tap on/show'), match: str('tap show: SID substring'), max: int('tap show: max events') }, ['verb']), a => cmd('quest', a.verb, a.sid || '', a.filter ? 'filter=' + a.filter : '', a.since !== undefined ? 'since=' + a.since : '', a.match ? 'sid=' + a.match : '', a.max ? 'max=' + a.max : '')],
  ['tarray', 'Typed TArray dump: elements of an ArrayProperty with address and value.', S({ obj: OBJ, path: str('property path'), max: int('max elements (default 50)') }, ['obj', 'path']), a => cmd('tarray', a.obj, a.path, a.max ? 'max=' + a.max : '')],
  ['tset', 'Typed TSet dump (sparse array walk with allocation bits). raw=true: obj is a container address and path the element size.', S({ obj: OBJ, path: str('property path (raw: element size)'), max: int('max elements (default 50)'), raw: { type: 'boolean', description: 'untyped dump of a container found by offset' } }, ['obj', 'path']), a => a.raw ? cmd('tset', 'raw', a.obj, a.path, a.max || '') : cmd('tset', a.obj, a.path, a.max ? 'max=' + a.max : '')],
  ['tmap', 'Typed TMap dump: key => value per element; key= filters keys containing the text.', S({ obj: OBJ, path: str('property path'), max: int('max elements (default 50)'), key: str('key substring filter') }, ['obj', 'path']), a => cmd('tmap', a.obj, a.path, a.max ? 'max=' + a.max : '', a.key ? 'key=' + a.key : '')],
  ['layoutcheck', 'Compare the reflected property offsets/sizes of a class with the SDK table (sdk\\<Class>.tsv from s2rag.py sdkexport) and, with obj, check each property is readable.', S({ class: str('class or struct name'), obj: str('optional object to read every property from') }, ['class']), a => cmd('layoutcheck', a.class, a.obj || '')],
  ['memdiff', 'Poll a memory range for secs at hz and report which byte runs changed (offset, first/last value, f32/i32 view, property name for objects). action=start (default) | result | list | stop.', S({ action: { type: 'string', enum: ['start', 'result', 'list', 'stop'] }, target: ADDR, len: str('bytes 1-65536 or auto (object size)'), secs: int('1-600'), hz: int('1-200 (default 20)'), name: str('tag'), wait: { type: 'boolean', description: 'block until done and return the result' }, id: str('id for result/stop') }, []),
    a => { const act = a.action || 'start'; if (act !== 'start') return game(['memdiff', act, a.id || ''].filter(Boolean).join(' ')); if (!a.target || !a.len || !a.secs) throw new Error('target, len and secs are required'); const o = ['memdiff', q(a.target), a.len, a.secs]; if (a.hz) o.push('hz=' + a.hz); if (a.name) o.push('name=' + a.name); if (a.wait) o.push('wait'); return withTimeout(a.wait ? a.secs + 15 : 0, () => game(o.join(' '))); }],
  ['stats', 'Per-command call count, total and max milliseconds since core load (slow commands >=250 ms also go to the log). clear=true resets.', S({ clear: { type: 'boolean', description: 'reset the counters' } }), a => game(a.clear ? 'stats clear' : 'stats')],
  ['batch', 'Run several S2Dev command lines in order on one connection (max 50). Each section starts with "== <command>". With json:true the reply is a JSON array of envelopes.', S({ commands: { type: 'array', items: { type: 'string' }, description: 'S2Dev command lines, run in order' }, stop_on_error: { type: 'boolean', description: 'stop at the first error/cannot/unknown/refused line (or ok=false in json mode)' } }, ['commands']),
    async a => {
      const cs = a.commands || [];
      if (!Array.isArray(cs) || !cs.length) throw new Error('commands must be a non-empty array');
      if (cs.length > 50) throw new Error('max 50 commands');
      const c = ctx.getStore() || {}, out = [];
      for (const line of cs) {
        let r, failed = false;
        try { r = String(await withTimeout(waitSecs(String(line)), () => game(String(line)))); }
        catch (e) { failed = true; r = c.json ? JSON.stringify({ ok: false, cmd: String(line), error: e.message }) : 'error: ' + e.message; }
        out.push(c.json ? r : '== ' + line + '\n' + r);
        if (a.stop_on_error && (failed || (c.json ? /"ok":false/.test(r) : /^(error|cannot|unknown|refused|FAILED)/m.test(r)))) break;
      }
      return c.json ? '[' + out.join(',') + ']' : out.join('\n');
    }],
  ['mark', 'Write a marker line into the per-port log and return the log byte offset; pass it as since= to log/hits to read only what happened afterwards.', S({ text: str('marker text') }, ['text']), a => cmd('mark', a.text)],
  ['watch', 'Log a property each time it changes (per tick).', S({ obj: OBJ, path: str('property path') }, ['obj', 'path']), a => cmd('watchprop', a.obj, a.path)],
  ['unwatch', 'Remove a property watch (id) or all.', S({ id: str('watch id, empty = all') }), a => cmd('unwatch', a.id)],
  ['plugin', 'Plugins (plugins\\<name>\\*.cpp, auto-rebuilt on save): list | build (build+hot reload) | reload | unload | log (build log) | watch on/off.', S({ action: { type: 'string', enum: ['list', 'build', 'reload', 'unload', 'log', 'watch'] }, name: str('plugin name (or on/off for watch)') }, ['action']), a => cmd('plug', a.action, a.name)],
  ['con', 'Run a UE console command (ExecuteConsoleCommand).', S({ command: str('console command') }, ['command']), a => game('con ' + a.command)],
  ['log', 'Tail of the per-port S2Dev log s2dev_<port>.log (offline read; falls back to the shared s2dev.log).', S({ n: int('lines (default 40)'), filter: str('regex'), since: int('byte offset from the `mark` tool') }), a => logTail(a.n || 40, a.filter, a.since)],
  ['reload_core', 'Hot reload S2DevCore.dll after `src\\build.bat core`.', S({}), () => game('reload')],
  ['raw', 'Run any S2Dev CLI command line verbatim (see `help`).', S({ command: str('e.g. help, aob 48 8B ?? 20, uinfo') }, ['command']), a => game(a.command)],
];
for (const t of tools) if (t[2] && t[2].properties) { t[2].properties.game = { type: 'string', description: 'which dev game: B client (default), A host, D client 2, C host 2, E client 3, all (run on every port, one section each), or a port. Applied per call, no reconnect needed.' };
  t[2].properties.json = { type: 'boolean', description: 'ask the game for structured JSON ({ok,cmd,port,ms,data,lines}) instead of text' };
  t[2].properties.out_file = { type: 'string', description: 'write the FULL output (no size cap) to this file path; the reply is the path + a preview' }; }
const byName = Object.fromEntries(tools.map(t => [t[0], t]));

const send = m => process.stdout.write(JSON.stringify(m) + '\n');
async function onMessage(m) {
  if (m.method === 'initialize') return send({ jsonrpc: '2.0', id: m.id, result: { protocolVersion: (m.params && m.params.protocolVersion) || '2024-11-05', capabilities: { tools: {} }, serverInfo: { name: 'anomaly-scanner', version: '2.2.0' } } });
  if (m.id === undefined) return;
  if (m.method === 'ping') return send({ jsonrpc: '2.0', id: m.id, result: {} });
  if (m.method === 'tools/list') return send({ jsonrpc: '2.0', id: m.id, result: { tools: tools.map(([name, description, inputSchema]) => ({ name, description, inputSchema })) } });
  if (m.method === 'tools/call') {
    const t = byName[m.params && m.params.name];
    if (!t) return send({ jsonrpc: '2.0', id: m.id, error: { code: -32602, message: 'unknown tool ' + (m.params && m.params.name) } });
    let text, isError = false;
    const args = m.params.arguments || {}, g = args.game, json = args.json === true;
    try {
      if (String(g).toLowerCase() === 'all') {
        const parts = await Promise.all(ALL_PORTS.map(port => ctx.run({ port, json }, async () => {
          try { return '== port ' + port + ' ==\n' + String(await t[3](args)); } catch (e) { return '== port ' + port + ' ==\nerror: ' + e.message; }
        })));
        text = parts.join('\n');
      } else text = String(await ctx.run({ port: resolvePort(g), json }, () => t[3](args)));
    } catch (e) { text = 'error: ' + e.message; isError = true; }
    const of = (m.params.arguments || {}).out_file;
    if (of) { try { fs.writeFileSync(of, text); text = `full output (${text.length} chars) -> ${of}\n` + text.slice(0, 1500); } catch (e) { text = 'out_file write failed: ' + e.message + '\n' + text; } }
    if (text.length > CAP) text = text.slice(0, CAP) + `\n... [truncated ${text.length - CAP} chars; narrow the query]`;
    return send({ jsonrpc: '2.0', id: m.id, result: { content: [{ type: 'text', text }], isError } });
  }
  send({ jsonrpc: '2.0', id: m.id, error: { code: -32601, message: 'method not found: ' + m.method } });
}
let inBuf = '';
process.stdin.on('data', d => {
  inBuf += d.toString('utf8'); let nl;
  while ((nl = inBuf.indexOf('\n')) >= 0) {
    const line = inBuf.slice(0, nl).trim(); inBuf = inBuf.slice(nl + 1); if (!line) continue;
    let m; try { m = JSON.parse(line); } catch { send({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'parse error' } }); continue; }
    onMessage(m).catch(e => m.id !== undefined && send({ jsonrpc: '2.0', id: m.id, error: { code: -32603, message: e.message } }));
  }
});
process.stdin.on('end', () => { for (const s of Object.values(socks)) if (s) s.end(); process.exit(0); });
