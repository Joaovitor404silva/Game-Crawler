const fs = require('fs'), path = require('path'), cp = require('child_process');
const ROOT = path.resolve(__dirname, '..');
const args = process.argv.slice(2), opt = {};
for (let i = 0; i < args.length; i++) { const a = args[i]; if (a === '--offline') opt.offline = true; else if (/^--(exe|ue4ss|dir|out|port)$/.test(a)) opt[a.slice(2)] = args[++i]; else { console.error('usage: node tools/build_names.js [--offline] [--exe <game exe>] [--ue4ss <UE4SS.log>] [--dir <raw dir>] [--out <names.tsv>] [--port N]'); process.exit(2); } }
const HOME = process.env.ANOMALY_HOME || ROOT;
const D = path.resolve(opt.dir || path.join(HOME, 'names'));
const OUT = path.resolve(opt.out || process.env.ANOMALY_NAMES || path.join(D, 'names.tsv'));
const EXE = opt.exe || 'C:\\Program Files (x86)\\Steam\\steamapps\\common\\S.T.A.L.K.E.R. 2 Heart of Chornobyl\\Stalker2\\Binaries\\Win64\\Stalker2-Win64-Shipping.exe';
if (!fs.existsSync(EXE)) { console.error('game exe not found: ' + EXE + ' (use --exe)'); process.exit(2); }
fs.mkdirSync(D, { recursive: true });
const rd = f => fs.existsSync(f) ? fs.readFileSync(f, 'utf8').split(/\r?\n/) : [];
const tsv = f => rd(f).filter(l => l).map(l => l.split('\t'));
const hex = s => parseInt(s, 16);
const raw = n => path.join(D, n);

const pe = (() => {
  const fd = fs.openSync(EXE, 'r'); const hdr = Buffer.alloc(0x1000); fs.readSync(fd, hdr, 0, 0x1000, 0);
  const p = hdr.readUInt32LE(0x3c), coff = p + 4, ns = hdr.readUInt16LE(coff + 2), soh = hdr.readUInt16LE(coff + 16), o = coff + 20;
  const secs = []; for (let i = 0; i < ns; i++) { const s = o + soh + i * 40; secs.push({ name: hdr.toString('latin1', s, s + 8).replace(/\0/g, ''), va: hdr.readUInt32LE(s + 12), vs: hdr.readUInt32LE(s + 8), raw: hdr.readUInt32LE(s + 20), ch: hdr.readUInt32LE(s + 36) }); }
  return { fd, hdr, opt: o, secs, size: hdr.readUInt32LE(o + 56) };
})();
const text = pe.secs.find(s => s.name === '.text') || pe.secs.find(s => s.ch & 0x20);
const TEXT_BEG = text.va, TEXT_END = text.va + text.vs;
const inText = r => r >= TEXT_BEG && r < TEXT_END;

function s2(cmd) {
  const a = [path.join(ROOT, 'anomaly.js'), '--timeout', '600000']; if (opt.port) a.push('--port', opt.port);
  const r = cp.spawnSync(process.execPath, a.concat(cmd), { encoding: 'utf8' });
  const o = (r.stdout || '') + (r.stderr || '');
  if (r.status !== 0 || /^error|cannot/im.test(o)) { console.error(cmd.join(' ') + ': ' + o.trim()); process.exit(1); }
  console.log(cmd[0] + ': ' + o.trim());
}
if (!opt.offline) {
  s2(['ping']);
  s2(['dumpfuncs', raw('ufuncs_raw.tsv')]);
  s2(['dumpvtables', raw('vtables_raw.tsv')]);
  s2(['dumpfieldvt', raw('fieldvt_raw.tsv')]);
  const uf = tsv(raw('ufuncs_raw.tsv')).slice(1), cnt = new Map();
  for (const r of uf) { const v = hex(r[4]); if (v) cnt.set(v, (cnt.get(v) || 0) + 1); }
  const execs = [...new Set(uf.filter(r => { const v = hex(r[4]); return v && inText(v) && cnt.get(v) === 1 && (hex(r[3]) & 0x400); }).map(r => r[4]))];
  fs.writeFileSync(raw('execs.txt'), execs.join('\n') + '\n');
  const ctors = new Set();
  for (const r of tsv(raw('vtables_raw.tsv')).slice(1)) if (r[2] === 'Class' && r[7]) for (const x of r[7].split(',')) { const [o, f] = x.split(':'); if (hex(o) === 0xb0) ctors.add(f); }
  fs.writeFileSync(raw('ctors.txt'), [...ctors].join('\n') + '\n');
  s2(['callees', raw('execs.txt'), raw('exec_callees.tsv')]);
  s2(['callees', raw('ctors.txt'), raw('ctor_callees.tsv')]);
}
for (const f of ['ufuncs_raw.tsv', 'vtables_raw.tsv']) if (!fs.existsSync(raw(f))) { console.error('missing ' + raw(f) + ' (run with the dev game up, without --offline)'); process.exit(1); }

const names = new Map();
const counts = {};
function add(rva, kind, name, source, prio) {
  if (!rva || !isFinite(rva)) return;
  name = name.replace(/[\s\t]+/g, '_');
  const cur = names.get(rva);
  if (cur && cur.prio >= prio) return;
  names.set(rva, { kind, name, source, prio });
}

const ue4ss = opt.ue4ss ? rd(opt.ue4ss) : [];
let ue4Base = 0; for (const l of ue4ss) { const m = l.match(/MainExe @ 0x([0-9a-f]+)/i); if (m) ue4Base = hex(m[1]); }
const slotNames = {};
let layoutDone = false;
for (const l of ue4ss) {
  const body = l.replace(/^\[[^\]]*\]\s*/, '');
  if (layoutDone) break;
  const m = body.match(/^([UAF])([A-Za-z0-9_]+)::(\w+) = 0x([0-9A-F]+)$/);
  if (m) {
    let cls = m[1] === 'F' ? 'F' + m[2] : m[2]; if (cls === 'ObjectBase' || cls === 'ObjectBaseUtility') cls = 'Object';
    const off = hex(m[4]); if (off % 8 === 0 && !m[3].startsWith('__vecDelDtor') || off === 0) (slotNames[cls] = slotNames[cls] || {})[off / 8] = m[3] === '__vecDelDtor' ? '__vecDelDtor' : m[3];
  }
  if (/Post-initialization/.test(body)) layoutDone = true;
}
if (ue4Base) {
  const known = [[/Found FName::FName\(wchar_t\*\): 0x([0-9a-f]+)/i, 'FName::FName'], [/Found StaticConstructObject_Internal: 0x([0-9a-f]+)/i, 'StaticConstructObject_Internal'],
    [/Found ConsoleManagerSingleton: 0x([0-9a-f]+)/i, 'ConsoleManagerSingleton'], [/GMalloc address: 0x([0-9a-f]+)/i, 'GMalloc'],
    [/FName::ToString address: 0x([0-9a-f]+)/i, 'FName::ToString'], [/GNatives address: 0x([0-9a-f]+)/i, 'GNatives'],
    [/GUObjectHashTables_Get address: 0x([0-9a-f]+)/i, 'GUObjectHashTables'], [/ProcessLocalScriptFunction address: 0x([0-9a-f]+)/i, 'ProcessLocalScriptFunction'],
    [/GUObjectArray address: 0x([0-9a-f]+)/i, 'GUObjectArray'], [/GameEngine::LoadMap address 0x([0-9a-f]+)/i, 'UGameEngine::LoadMap'],
    [/GameModeBase::InitGameState address 0x([0-9a-f]+)/i, 'AGameModeBase::InitGameState'], [/ProcessEvent address 0x([0-9a-f]+)/i, 'UObject::ProcessEvent'],
    [/ProcessConsoleExec address 0x([0-9a-f]+)/i, 'UObject::ProcessConsoleExec'], [/UStruct::Link address 0x([0-9a-f]+)/i, 'UStruct::Link'],
    [/ProcessInternal address: 0x([0-9a-f]+)/i, 'UObject::ProcessInternal'], [/GameViewportClient::Tick address 0x([0-9a-f]+)/i, 'UGameViewportClient::Tick'],
    [/AActor::Tick address 0x([0-9a-f]+)/i, 'AActor::Tick'], [/AActor::BeginPlay address 0x([0-9a-f]+)/i, 'AActor::BeginPlay'], [/AActor::EndPlay address 0x([0-9a-f]+)/i, 'AActor::EndPlay']];
  for (const l of ue4ss) for (const [re, n] of known) { const m = l.match(re); if (m) { const r = hex(m[1]) - ue4Base; if (r > 0 && r < pe.size) add(r, inText(r) ? 'ue_core' : 'global', n, 'UE4SS.log', 90); } }
}

const execCount = new Map();
const uf = tsv(raw('ufuncs_raw.tsv')).slice(1);
for (const r of uf) { const v = hex(r[4]); if (v) execCount.set(v, (execCount.get(v) || 0) + 1); }
const execName = new Map();
for (const [ow, fn, , fl, r] of uf) {
  const v = hex(r); if (!v || !inText(v)) continue;
  if (execCount.get(v) > 1) continue;
  if (!(hex(fl) & 0x400)) continue;
  execName.set(v, [ow, fn]);
  add(v, 'ufunc_exec', `${ow}::exec${fn}`, 'S2Dev dumpfuncs', 80);
}
{
  const ec = tsv(raw('exec_callees.tsv')).map(([e, , s]) => [hex(e), (s || '').split(',').filter(x => x.startsWith('C:')).map(x => hex(x.slice(2)))]);
  const use = new Map(); for (const [, cs] of ec) for (const c of new Set(cs)) use.set(c, (use.get(c) || 0) + 1);
  const impls = [];
  for (const [ev, cs] of ec) { const c = cs.find(x => use.get(x) < 8); if (c) impls.push([ev, c]); }
  const implUsers = new Map();
  for (const [ev, iv] of impls) { if (!implUsers.has(iv)) implUsers.set(iv, new Set()); implUsers.get(iv).add(ev); }
  for (const [ev, iv] of impls) {
    if (implUsers.get(iv).size > 1 || !inText(iv) || iv === ev) continue;
    const en = execName.get(ev);
    if (en) add(iv, 'ufunc_impl', `${en[0]}::${en[1]}`, 'S2Dev callees+dumpfuncs', 75);
  }
}

const vt = tsv(raw('vtables_raw.tsv')).slice(1).map(([c, s, meta, v, , , slots, cf]) => ({ c, s, meta, v: hex(v), slots: slots ? slots.split(',').map(hex) : [], cf: cf ? cf.split(',').map(x => x.split(':').map(hex)) : [] }));
const byName = new Map(); for (const x of vt) if (x.meta === 'Class' && !byName.has(x.c)) byName.set(x.c, x);
const starts = [...new Set(vt.map(x => x.v))].sort((a, b) => a - b);
for (const x of vt) { const i = starts.indexOf(x.v); const nx = starts[i + 1]; x.cap = nx ? Math.min(x.slots.length, (nx - x.v) / 8) : x.slots.length; x.kids = 0; }
const depth = x => { let d = 0; for (let s = x; s && d < 200; s = byName.get(s.s)) d++; return d; };
const introName = (x, i) => {
  const chain = []; for (let s = x; s && chain.length < 200; s = byName.get(s.s)) chain.push(s);
  for (let k = chain.length - 1; k >= 0; k--) if (chain[k].slots.length > i) { const nm = (slotNames[chain[k].c] || {})[i]; if (nm) return nm; }
  return null;
};
const byDepth = vt.filter(x => x.meta === 'Class').sort((a, b) => depth(b) - depth(a));
for (const x of byDepth) { const sp = byName.get(x.s); if (sp && sp !== x && sp.v !== x.v) { sp.cap = Math.min(sp.cap, x.cap); sp.kids++; } }
for (const x of vt) if (x.meta !== 'Class') { const sp = byName.get(x.s); if (sp && sp.v === x.v) x.cap = sp.cap; }
const slotIdx = new Map(); const noteSlot = (f, i) => { if (!slotIdx.has(f)) slotIdx.set(f, new Set()); slotIdx.get(f).add(i); };
const vfCand = new Map();
const vtSeen = new Set();
for (const x of vt) {
  if (vtSeen.has(x.v)) continue; vtSeen.add(x.v);
  const sup = byName.get(x.s);
  if (x.meta !== 'Class' && sup && sup.v === x.v) continue;
  add(x.v, 'vtable', `${x.c}::vftable`, 'S2Dev dumpvtables', 70);
  const d = depth(x);
  x.slots.slice(0, x.cap).forEach((f, i) => {
    if (sup && sup.cap > i && sup.slots[i] === f) return;
    const nm = introName(x, i) || `vf_${i}`;
    noteSlot(f, i);
    const tail = !x.kids && (!sup || i >= sup.cap);
    if (!vfCand.has(f)) vfCand.set(f, []); vfCand.get(f).push({ name: `${x.c}::${nm}`, d, tail });
  });
}
for (const [f, cands] of vfCand) {
  cands.sort((a, b) => (a.tail - b.tail) || (a.d - b.d)); const c0 = cands[0];
  add(f, 'vfunc', c0.name, 'S2Dev dumpvtables' + (cands.length > 1 ? ` (shared x${cands.length})` : '') + (c0.tail ? ' (leaf tail, unverified)' : ''), c0.tail ? 45 : cands.length > 4 ? 50 : 60);
}

{
  const what = { 0xb0: 'InternalConstructor', 0xb8: 'VTableHelperCtorCaller', 0xc0: 'AddReferencedObjects' };
  const cand = new Map(), ctorOf = new Map();
  for (const x of vt) if (x.meta === 'Class') for (const [o, f] of x.cf) if (what[o]) {
    if (!cand.has(f)) cand.set(f, []); cand.get(f).push({ c: x.c, d: depth(x), o });
    if (o === 0xb0) ctorOf.set(f, x.c);
  }
  for (const [f, cs] of cand) { cs.sort((a, b) => a.d - b.d); add(f, 'uclass_fn', cs[0].c + '::' + what[cs[0].o], 'S2Dev dumpvtables UClass+0x' + cs[0].o.toString(16) + (cs.length > 1 ? ' (shared x' + cs.length + ')' : ''), 55); }
  const ctorUse = new Map();
  const ctorRows = tsv(raw('ctor_callees.tsv')).filter(r => r[2] && r[2].startsWith('J:') && (cand.get(hex(r[0])) || []).length === 1);
  for (const r of ctorRows) { const t = hex(r[2].split(',')[0].slice(2)); ctorUse.set(t, (ctorUse.get(t) || 0) + 1); }
  for (const r of ctorRows) { const t = hex(r[2].split(',')[0].slice(2)), c = ctorOf.get(hex(r[0])); if (ctorUse.get(t) === 1) add(t, 'ctor', c + '::' + c, 'InternalConstructor tail jump', 55); }
}

{
  const fr = tsv(raw('fieldvt_raw.tsv')).slice(1).filter(r => !/^FNone$|</.test(r[0]) && !/</.test(r[1]))
    .map(([c, s, , v, n, , slots]) => ({ c, s, v: hex(v), n: +n, slots: slots ? slots.split(',').map(hex) : [] }));
  const uniq = new Map(); for (const x of fr) if (!uniq.has(x.c)) uniq.set(x.c, x);
  const sup = { FNumericProperty: 'FProperty', FObjectPropertyBase: 'FProperty', FMulticastDelegateProperty: 'FProperty', FProperty: 'FField' };
  for (const x of uniq.values()) sup[x.c] = x.s;
  const chain = c => { const r = []; for (let k = c; k && r.length < 16; k = sup[k]) r.push(k); return r; };
  const minSib = {}; for (const x of uniq.values()) minSib[x.s] = Math.min(minSib[x.s] || 1e9, x.n);
  const at = new Map();
  for (const x of uniq.values()) {
    add(x.v, 'vtable', x.c + '::vftable', 'S2Dev dumpfieldvt', 70);
    x.slots.slice(0, minSib[x.s]).forEach((f, i) => { const k = i + ':' + f; if (!at.has(k)) at.set(k, []); at.get(k).push(x.c); });
  }
  for (const [k, cls] of at) {
    const [i, f] = k.split(':').map(Number); noteSlot(f, i);
    let lca = cls[0]; if (cls.length > 1) { const sets = cls.map(c => chain(c)); lca = sets[0].find(a => sets.every(ch => ch.includes(a))) || 'FField'; }
    const ch = chain(lca); let nm = null; for (let j = ch.length - 1; j >= 0 && !nm; j--) nm = (slotNames[ch[j]] || {})[i] || null;
    add(f, 'vfunc', lca + '::' + (nm || 'vf_' + i), 'S2Dev dumpfieldvt' + (cls.length > 1 ? ' (shared x' + cls.length + ')' : ''), 58);
  }
}

for (const [f, set] of slotIdx) { const cur = names.get(f); if (set.size >= 3 && cur && cur.kind === 'vfunc') names.set(f, { kind: 'vfunc', name: 'vstub_' + f.toString(16), source: 'ICF stub, ' + set.size + ' slot indexes', prio: cur.prio }); }

{
  const { fd, hdr, secs } = pe;
  const off = r => { for (const s of secs) if (r >= s.va && r < s.va + Math.max(s.vs, 1)) return r - s.va + s.raw; return -1; };
  const rdb = (r, n) => { const b = Buffer.alloc(n); fs.readSync(fd, b, 0, n, off(r)); return b; };
  const cstr = r => { const b = rdb(r, 512); return b.toString('latin1', 0, b.indexOf(0)); };
  const dir = i => [hdr.readUInt32LE(pe.opt + 112 + i * 8), hdr.readUInt32LE(pe.opt + 112 + i * 8 + 4)];
  const [er, es] = dir(0);
  if (er && es) {
    const e = rdb(er, 40), nf = e.readUInt32LE(20), nn = e.readUInt32LE(24), af = e.readUInt32LE(28), an = e.readUInt32LE(32), ao = e.readUInt32LE(36);
    const fa = rdb(af, nf * 4), na = rdb(an, nn * 4), oa = rdb(ao, nn * 2);
    for (let i = 0; i < nn; i++) { const f = fa.readUInt32LE(oa.readUInt16LE(i * 2) * 4); if (f < er || f >= er + es) add(f, 'export', cstr(na.readUInt32LE(i * 4)), 'PE exports', 95); }
  }
  const [ir] = dir(1);
  for (let d = ir; ; d += 20) {
    const b = rdb(d, 20); const ilt = b.readUInt32LE(0) || b.readUInt32LE(16), nm = b.readUInt32LE(12), iat = b.readUInt32LE(16); if (!nm) break;
    const dll = cstr(nm).replace(/\.dll$/i, '');
    for (let k = 0; ; k++) {
      const t = rdb(ilt + k * 8, 8).readBigUInt64LE(0); if (!t) break;
      const fn = (t >> 63n) ? `Ordinal_${Number(t & 0xffffn)}` : cstr(Number(t & 0x7fffffffn) + 2);
      add(iat + k * 8, 'import', `__imp_${dll}!${fn}`, 'PE imports', 95);
    }
  }
  fs.closeSync(fd);
}

const rows = [...names.entries()].sort((a, b) => a[0] - b[0]);
const out = ['rva\tkind\tname\tsource'];
for (const [r, v] of rows) { out.push(`${r.toString(16)}\t${v.kind}\t${v.name}\t${v.source}`); counts[v.kind] = (counts[v.kind] || 0) + 1; }
fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT + '.tmp', out.join('\n') + '\n');
fs.renameSync(OUT + '.tmp', OUT);
console.log(OUT + ' rows ' + rows.length + ' ' + JSON.stringify(counts));
