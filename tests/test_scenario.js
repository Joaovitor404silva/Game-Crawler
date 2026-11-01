const fs = require('fs');
const f = process.argv[2];
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('FAIL ' + m); } };
const recs = fs.readFileSync(f, 'utf8').split('\n').filter(x => x.trim()).map(l => JSON.parse(l));
const fns = recs.find(r => r.step === 'fns');
const num = h => parseInt(h, 16);
const rva = h => num(h) - num(fns.base);

for (const r of recs) {
  if (r.text && r.json) {
    const volatile = /^(mark|ver)$/.test(r.cmd.split(' ')[0]);
    if (!volatile) ok(r.json.lines.join('\n') === r.text.replace(/\n$/, ''), 'json lines == text for ' + r.cmd);
    ok(typeof r.json.ok === 'boolean' && r.json.cmd === r.cmd.split(' ')[0] && typeof r.json.port === 'number' && typeof r.json.ms === 'number', 'envelope ' + r.cmd);
  }
}
const by = (step, cmdPrefix) => recs.filter(r => r.step === step && r.cmd && r.cmd.startsWith(cmdPrefix));

const ver = by('basic', 'ver')[0].json;
ok(ver.ok && /^[0-9a-f]{32}$/.test(ver.data.core_md5) && ver.data.port === 27999 && !/\s/.test(ver.data.loader_md5), 'ver structure');
const rd = by('basic', 'rd 0x7ff')[0].json;
ok(rd.data.len === 32 && rd.data.hex.length === 64 && rd.data.complete === true, 'rd structure');
const rd2 = by('basic', 'rd 0x10')[0].json;
ok(rd2.data.len === 8, 'rd len 8');
ok(by('basic', 'nosuchcommand')[0].json.ok === false, 'unknown command ok=false');
ok(by('basic', 'ping')[0].json.ok === true, 'ping');
ok(by('basic', 'hooks')[0].json.data.hooks.length === 0, 'hooks empty');

const aggs = by('hook', 'agg top=5');
const a1 = aggs[0].json.data.sources[0];
ok(a1.hits === 400 && a1.groups === 2, 'agg 400 hits 2 groups: ' + JSON.stringify([a1.hits, a1.groups]));
const g300 = a1.top.find(g => g.hits === 300), g100 = a1.top.find(g => g.hits === 100);
ok(g300 && g100, 'two groups 300/100');
ok(rva(g300.frames[0]) === rva(fns.HotFn) && rva(g300.frames[1]) - rva(fns.HotCallerA) < 0x40 && rva(g300.frames[1]) >= rva(fns.HotCallerA), 'g300 caller is HotCallerA');
ok(rva(g100.frames[1]) >= rva(fns.HotCallerB) && rva(g100.frames[1]) - rva(fns.HotCallerB) < 0x40, 'g100 caller is HotCallerB');
const a2 = aggs[1].json.data.sources[0];
ok(a2.groups >= 4 && a2.hits >= 400400, 'flood aggregated: ' + a2.hits);
const fl = recs.find(r => r.step === 'flood');
ok(fl.newLogLines < 100, 'flood log lines ' + fl.newLogLines);
const filt = recs.find(r => r.step === 'filter');
ok(filt.r5 === 77 && filt.r7 === 77 && filt.r6 === 7, 'filtered ret hook');
const hk = by('hook', 'hooks').pop().json.data.hooks[0];
ok(hk.hits === 2 && hk.agg === true && hk.filtered === 1 && hk.mode.startsWith('ret'), 'hooks agg fields ' + JSON.stringify(hk));
ok(recs.find(r => r.step === 'unhooked').r === 6, 'unhooked passthrough');

const wr = by('watch', 'watchw result')[0].json.data;
ok(wr.writes === 300 && wr.groups === 2 && wr.overflow === 0, 'watchw counts ' + wr.writes);
const w200 = wr.writers.find(w => w.hits === 200), w100 = wr.writers.find(w => w.hits === 100);
ok(w200 && w100, 'watchw writer groups');
ok(rva(w200.frames[0]) >= rva(fns.WriterA) && rva(w200.frames[0]) - rva(fns.WriterA) < 0x30, 'writer A top frame');
ok(rva(w200.frames[1]) >= rva(fns.CallerOne) && rva(w200.frames[1]) - rva(fns.CallerOne) < 0x30, 'writer A caller CallerOne');
ok(rva(w100.frames[0]) >= rva(fns.WriterB) && rva(w100.frames[0]) - rva(fns.WriterB) < 0x30, 'writer B top frame');
ok(rva(w100.frames[1]) >= rva(fns.CallerTwo) && rva(w100.frames[1]) - rva(fns.CallerTwo) < 0x30, 'writer B caller CallerTwo');
ok(w200.old === '0x0' && w200.new === '0xa' && w100.new === '0x3e9', 'old/new values');
const wstop = by('watch', 'watchw stop')[0].json;
ok(wstop.ok, 'watchw stop');
ok(by('watch', 'bps')[1].json.data.bps.length === 0, 'bps empty after stop');
const big = by('watch', 'watchw 0x').find(r => r.cmd.endsWith(' 64'));
ok(big.json.ok === false, 'size 64 rejected');
const waitw = by('watch', 'watchw 0x').find(r => r.cmd.includes('wait'));
ok(waitw.json.ok && waitw.json.data.active === false && waitw.json.ms >= 900, 'watchw wait released');
const bpsLast = by('watch', 'bps').pop().json.data.bps;
ok(bpsLast.length === 0, 'timed release freed debug registers');

const s = by('sample', 'sample c=0x')[0].json;
ok(s.ok && s.data.done && s.data.rows >= 45 && s.data.rows <= 55, 'sampler rows ' + s.data.rows);
const tsv = fs.readFileSync(s.data.file, 'utf8').trim().split('\n');
ok(tsv[0].split('\t').join(',') === 'tick,ms,c,t', 'tsv header ' + tsv[0]);
const cs = tsv.slice(1).map(l => +l.split('\t')[2]);
ok(cs.every((v, i) => i === 0 || v >= cs[i - 1]) && cs[cs.length - 1] > cs[0], 'counter monotonic increasing');
ok(tsv.slice(1).every(l => l.split('\t')[3] === '1100'), 'target column constant');
ok(by('sample', 'sample c=zzz')[0].json.ok === false, 'bad type rejected');
const md = by('memdiff', 'memdiff 0x').find(r => r.cmd.endsWith('hz=100 wait'));
ok(md.json.ok && md.json.data.done && md.json.data.samples >= 50 && md.json.data.runs.length >= 1 && md.json.lines.some(l => /^memdiff m\d+ 0x[0-9a-f]+ len 4 1s @100Hz: \d+ changed runs, \d+ byte changes, \d+ samples/.test(l)), 'memdiff counter changes: ' + JSON.stringify(md.json.data));
ok(md.json.data.runs.every(r => r.len >= 1 && r.changes >= 1), 'memdiff runs well formed');
ok(by('memdiff', 'memdiff list')[0].json.data.memdiffs.length >= 1, 'memdiff list');
ok(by('memdiff', 'memdiff 0 16')[0].json.ok === false && /^error:/.test(by('memdiff', 'memdiff 0 16')[0].json.lines[0]), 'memdiff unreadable is an error');
ok(by('memdiff', 'memdiff 0x').find(r => r.cmd.endsWith('70000 1')).json.ok === false, 'memdiff len cap');
ok(by('memdiff', 'memdiff 0x').find(r => r.cmd.endsWith('auto 1')).json.ok === false, 'memdiff auto needs object');

const dp = recs.filter(r => r.step === 'detour');
const refused = dp.find(r => / agg$/.test(r.cmd));
ok(refused.json.ok === false && refused.json.data.refused === 'detoured' && refused.json.data.owner.length > 0 && /^refused: .* is already detoured by /.test(refused.json.lines[0]), 'hook refused on a detoured target: ' + JSON.stringify(refused.json));
const hwRet = dp.find(r => /hw ret 5$/.test(r.cmd));
ok(hwRet.json.ok === false && hwRet.json.lines[0] === 'error: hw supports log only', 'hw rejects ret');
const hw = dp.find(r => / hw$/.test(r.cmd));
ok(hw.json.ok && /^hook\(hw\) bp\d exec /.test(hw.json.lines[0]) && / agg/.test(hw.json.lines[0]), 'hook hw arms an aggregating exec bp: ' + hw.json.lines[0]);
ok(dp.filter(r => r.cmd === 'bps')[0].json.data.bps.length === 1, 'hw hook occupies one debug register');
ok(dp.filter(r => r.cmd === 'bps').length === 1 && dp.find(r => r.cmd === 'bpc all').json.ok, 'bpc all');
const reHook = dp.find(r => r.cmd.startsWith('hook 0x') && !/ (agg|hw|force|hw ret 5)$/.test(r.cmd));
ok(reHook.json.lines[0].startsWith('already hooked as '), 're-hooking our own detoured target says already hooked: ' + JSON.stringify(reHook.json.lines));
ok(refused.json.lines[0].includes('unknown module'), 'unknown detour owner is stated plainly: ' + refused.json.lines[0]);
const race = recs.find(r => r.step === 'memdiffrace');
ok(race && race.waits === 6 && race.good === 6, 'memdiff and sample waiters survive concurrent churn: ' + JSON.stringify(race));
const hooksD = dp.find(r => r.cmd === 'hooks').json.data.hooks;
ok(hooksD.length === 1 && hooksD[0].detoured_by && hooksD[0].mode === 'log', 'force hook lists detoured_by: ' + JSON.stringify(hooksD));

const st = recs.find(r => r.step === 'stats' && r.cmd === 'stats').json;
const stRow = c => st.data.stats.find(x => x.cmd === c);
ok(st.ok && stRow('rd').count >= 2 && stRow('hook').count >= 4 && stRow('memdiff').count >= 1 && stRow('rd').max_ms >= 0 && /^cmd count total_ms max_ms/.test(st.lines[0]), 'stats counts: ' + JSON.stringify(st.data.stats.slice(0, 3)));
ok(recs.find(r => r.step === 'stats' && r.cmd === 'stats clear').json.lines[0] === 'stats cleared', 'stats clear');

const log = recs.find(r => r.step === 'logpath');
ok(fs.existsSync(log.path) && fs.statSync(log.path).size > 0 && /s2dev_27999\.log$/.test(log.path), 'per-port log exists');

console.log(`scenario: ${pass} pass, ${fail} fail`);
process.exit(fail ? 1 : 0);

