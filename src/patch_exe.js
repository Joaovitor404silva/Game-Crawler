const fs = require('fs');
const [,, inp, outp] = process.argv;
if (!inp || !outp) { console.error('usage: node patch_exe.js in.exe out.exe'); process.exit(2); }
if (/steamapps/i.test(outp)) { console.error('refusing to write into the Steam install'); process.exit(2); }
const DLL = 'S2Dev.dll', FN = 'S2DevInit', SECT = '.s2dev';
let b = fs.readFileSync(inp);
const al = (v, a) => Math.ceil(v / a) * a;
function hdr(b) {
  const pe = b.readUInt32LE(0x3c), coff = pe + 4, opt = coff + 20;
  return { pe, coff, opt, ns: b.readUInt16LE(coff + 2), sh: opt + b.readUInt16LE(coff + 16) };
}
function sections(b) {
  const h = hdr(b), r = [];
  for (let i = 0; i < h.ns; i++) { const o = h.sh + i * 40; r.push({ name: b.toString('latin1', o, o + 8).replace(/\0/g, ''), va: b.readUInt32LE(o + 12), vs: b.readUInt32LE(o + 8), raw: b.readUInt32LE(o + 20), rs: b.readUInt32LE(o + 16) }); }
  return r;
}
function rva2off(b, rva) { for (const s of sections(b)) if (rva >= s.va && rva < s.va + Math.max(s.vs, s.rs)) return rva - s.va + s.raw; throw new Error('rva ' + rva.toString(16)); }
function cstr(b, off) { let e = off; while (b[e]) e++; return b.toString('latin1', off, e); }
function importDlls(b) {
  const h = hdr(b), irva = b.readUInt32LE(h.opt + 112 + 8), out = [];
  let o = rva2off(b, irva);
  for (;;) { const nameRva = b.readUInt32LE(o + 12); if (!nameRva && !b.readUInt32LE(o)) break; out.push(cstr(b, rva2off(b, nameRva))); o += 20; }
  return out;
}
if (sections(b).some(s => s.name === SECT)) { console.error('input already patched'); process.exit(3); }
let h = hdr(b);
const fa = b.readUInt32LE(h.opt + 36), sa = b.readUInt32LE(h.opt + 32);
const secOff = b.readUInt32LE(h.opt + 112 + 4 * 8), secSz = b.readUInt32LE(h.opt + 112 + 4 * 8 + 4);
if (secOff && secOff + secSz === b.length) b = b.subarray(0, secOff);
b.writeUInt32LE(0, h.opt + 112 + 32); b.writeUInt32LE(0, h.opt + 112 + 36);
b.writeUInt32LE(0, h.opt + 112 + 88); b.writeUInt32LE(0, h.opt + 112 + 92);
const secs = sections(b), last = secs[secs.length - 1];
const newVa = al(last.va + last.vs, sa), newRaw = al(Math.max(b.length, last.raw + last.rs), fa);
if (h.sh + (h.ns + 1) * 40 > b.readUInt32LE(h.opt + 60)) throw new Error('no room for section header');
const oldIrva = b.readUInt32LE(h.opt + 112 + 8), oldIsz = b.readUInt32LE(h.opt + 112 + 12);
const oldDesc = []; { let o = rva2off(b, oldIrva); for (;;) { const d = b.subarray(o, o + 20); if (d.every(x => x === 0)) break; oldDesc.push(Buffer.from(d)); o += 20; } }
const n = oldDesc.length + 2;
const descSz = n * 20;
let p = al(descSz, 16);
const iltOff = p; p += 16; const iatOff = p; p += 16; const hintOff = p; p += 2 + FN.length + 1; p = al(p, 2);
const nameOff = p; p += DLL.length + 1;
const size = al(p, 16);
const sec = Buffer.alloc(al(size, fa));
oldDesc.forEach((d, i) => d.copy(sec, i * 20));
const od = oldDesc.length * 20;
sec.writeUInt32LE(newVa + iltOff, od); sec.writeUInt32LE(newVa + nameOff, od + 12); sec.writeUInt32LE(newVa + iatOff, od + 16);
sec.writeBigUInt64LE(BigInt(newVa + hintOff), iltOff); sec.writeBigUInt64LE(BigInt(newVa + hintOff), iatOff);
sec.write(FN, hintOff + 2, 'latin1'); sec.write(DLL, nameOff, 'latin1');
const so = h.sh + h.ns * 40; b.fill(0, so, so + 40);
b.write(SECT, so, 'latin1'); b.writeUInt32LE(size, so + 8); b.writeUInt32LE(newVa, so + 12); b.writeUInt32LE(sec.length, so + 16); b.writeUInt32LE(newRaw, so + 20);
b.writeUInt32LE(0xC0000040, so + 36);
b.writeUInt16LE(h.ns + 1, h.coff + 2);
b.writeUInt32LE(al(newVa + size, sa), h.opt + 56);
b.writeUInt32LE(newVa, h.opt + 112 + 8); b.writeUInt32LE(n * 20, h.opt + 112 + 12);
const pad = Buffer.alloc(newRaw - b.length);
b = Buffer.concat([b, pad, sec]);
h = hdr(b); const ck = h.opt + 64; b.writeUInt32LE(0, ck);
let sum = 0; for (let i = 0; i < b.length; i += 2) { if (i === ck || i === ck + 2) continue; sum += (i + 1 < b.length) ? b.readUInt16LE(i) : b[i]; sum = (sum & 0xffff) + (sum >>> 16); }
sum = (sum & 0xffff) + (sum >>> 16); b.writeUInt32LE((sum + b.length) >>> 0, ck);
fs.writeFileSync(outp, b);
const v = fs.readFileSync(outp), dl = importDlls(v);
const ok = dl[dl.length - 1] === DLL && dl.length === oldDesc.length + 1 && sections(v).some(s => s.name === SECT);
console.log(`imports=${dl.length} last=${dl[dl.length - 1]} sectVA=0x${newVa.toString(16)} size=${v.length} verify=${ok ? 'OK' : 'FAIL'}`);
process.exit(ok ? 0 : 1);
