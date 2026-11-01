'use strict';
const path = require('path');
const file = process.argv[2];
if (!file) { console.error('usage: node check_plugin_mcp.js <plugin>/mcp.js'); process.exit(2); }
const S = (props, req) => ({ type: 'object', properties: props, required: req || [] });
const str = d => ({ type: 'string', description: d }), int = d => ({ type: 'integer', description: d });
const OBJ = str('object');
const sent = [];
const cmd = (...parts) => { sent.push(parts.filter(x => x !== undefined && x !== null && x !== '').join(' ')); return Promise.resolve('ok'); };
const game = line => { sent.push(line); return Promise.resolve('ok'); };
let fail = 0;
const bad = m => { console.log('FAIL ' + m); fail++; };
let tools;
try { tools = require(path.resolve(file))({ S, str, int, OBJ, cmd, game }); } catch (e) { bad('load: ' + e.message); process.exit(1); }
if (!Array.isArray(tools) || !tools.length) bad('mcp.js must return a non-empty array of tool rows');
const names = new Set();
(async () => {
  for (const t of tools || []) {
    const n = t && t[0];
    if (!Array.isArray(t) || t.length !== 4) { bad(`${n}: row must be [name, description, schema, handler]`); continue; }
    if (!/^[a-z][a-z0-9_]{1,40}$/.test(n)) bad(`${n}: name must be lower case letters, digits or _`);
    if (names.has(n)) bad(`${n}: duplicate name`); names.add(n);
    if (typeof t[1] !== 'string' || t[1].length < 10) bad(`${n}: needs a description`);
    if (!t[2] || t[2].type !== 'object' || typeof t[2].properties !== 'object') bad(`${n}: schema must come from S({...})`);
    for (const r of (t[2] && t[2].required) || []) if (!(r in t[2].properties)) bad(`${n}: required "${r}" is not a property`);
    if (typeof t[3] !== 'function') { bad(`${n}: handler must be a function`); continue; }
    const args = {}; for (const [k, v] of Object.entries(t[2].properties)) args[k] = v.type === 'integer' ? 1 : v.type === 'boolean' ? true : v.enum ? v.enum[0] : 'x';
    sent.length = 0;
    try { await t[3](args); } catch (e) { bad(`${n}: handler threw ${e.message}`); continue; }
    if (!sent.length) bad(`${n}: handler sent no command`);
  }
  console.log(`${file}: ${tools ? tools.length : 0} tools, ${fail} problems`);
  process.exit(fail ? 1 : 0);
})();
