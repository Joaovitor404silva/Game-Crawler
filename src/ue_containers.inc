static bool CntRd(uint64_t a, void* o, size_t n) { return SafeRead(o, (uintptr_t)a, n); }

static std::string HexBytes(uintptr_t a, size_t n) {
  std::vector<uint8_t> b(n); if (!SafeRead(b.data(), a, n)) return "<unreadable>";
  std::string s; for (size_t i = 0; i < n; i++) s += Fmt("%02x", b[i]); return s;
}

static void CmdContainer(const std::string& c, const std::vector<std::string>& t, Out& o) {
  if (c == "tset" && t.size() >= 4 && Lower(t[1]) == "raw") {
    uint64_t base = 0, es = 0;
    if (!ParseU64(t[2], base) || !ParseU64(t[3], es) || es == 0 || es > 4096) { o.line("usage: tset raw <addr> <elemsize> [max]"); return; }
    uint32_t mx = t.size() > 4 ? (uint32_t)atoi(t[4].c_str()) : 50;
    uint32_t stride = cnt::SparseStride((uint32_t)es, es >= 8 ? 8 : 4);
    std::vector<uint64_t> el; cnt::SparseInfo si;
    if (!cnt::SparseElems(CntRd, base, cnt::Layout(), stride, mx, el, &si)) { o.f("error: cannot walk sparse array at 0x%llx", (unsigned long long)base); return; }
    o.f("raw set at 0x%llx elem=%llu stride=%u slots=%d free=%d bits=%d data=0x%llx", (unsigned long long)base, (unsigned long long)es, stride, si.slots, si.free, si.bits, (unsigned long long)si.data);
    std::vector<std::string> js;
    for (size_t i = 0; i < el.size(); i++) {
      std::string hx = HexBytes(el[i], (size_t)es);
      o.f("[%zu] 0x%llx %s", i, (unsigned long long)el[i], hx.c_str());
      js.push_back(jo::Obj().num("i", (long long)i).hex("addr", el[i]).str("hex", hx).done());
    }
    JsonSetData(jo::Obj().str("kind", "raw").num("slots", si.slots).num("free", si.free).num("elem_size", (long long)es).raw("items", jo::Arr(js)).done());
    return;
  }
  if (t.size() < 3) { o.line("usage: " + c + " <obj> <Prop.Path> [max=N]" + (c == "tmap" ? " [key=<text>]" : "") + (c == "tset" ? " | tset raw <addr> <elemsize> [max]" : "")); return; }
  uopt::Args ao = uopt::Parse(t, 3);
  uintptr_t obj = ResolveObj(t[1]); if (!obj) { o.line("cannot resolve object " + t[1]); return; }
  uintptr_t p = 0, a = 0; std::string err;
  if (!PropPath(obj, t[2], p, a, err)) {
    if (err.compare(0, 12, "no property ") == 0) err += " (not a UPROPERTY; for a raw TSet use: tset raw <addr> <elemsize> [max])";
    o.line("error: " + err); return;
  }
  std::string ty = PropType(p), want = c == "tarray" ? "ArrayProperty" : c == "tset" ? "SetProperty" : "MapProperty";
  if (ty != want) { o.line("error: " + t[2] + " is " + ty + ", expected " + want); return; }
  uint32_t mx = (uint32_t)ao.Int("max", ao.pos.empty() ? 50 : atoi(ao.pos[0].c_str()));
  std::string keyf = ao.Str("key");
  uintptr_t in = PropInner(p), vp = c == "tmap" ? PropX(p, 1) : 0;
  if (!in || (c == "tmap" && !vp)) { o.line("error: container inner property missing"); return; }
  std::vector<uint64_t> el; int num = 0, cap = 0, slots = 0, nfree = 0; uint32_t es = (uint32_t)PropElem(in), stride = 0, vOff = 0;
  if (c == "tarray") {
    uintptr_t data = RdP(a); num = Rd<int>(a + 8); cap = Rd<int>(a + 12);
    if (num < 0 || num > 5000000 || es == 0) { o.f("error: bad array num=%d elem=%u", num, es); return; }
    for (int i = 0; i < num && (uint32_t)i < (keyf.empty() ? mx : 100000u); i++) el.push_back(data + (uint64_t)i * es);
  } else {
    uint32_t sz = es;
    if (c == "tmap") { vOff = (uint32_t)PropOff(vp); sz = vOff + (uint32_t)PropElem(vp); }
    stride = cnt::SparseStride(sz, sz >= 8 ? 8 : 4);
    cnt::SparseInfo si;
    if (!cnt::SparseElems(CntRd, a, cnt::Layout(), stride, keyf.empty() ? mx : 100000u, el, &si)) { o.f("error: cannot walk sparse array at 0x%llx", (unsigned long long)a); return; }
    slots = si.slots; nfree = si.free; num = slots - nfree; cap = Rd<int>(a + cnt::Layout().dataNum + 4);
  }
  std::string kind = c == "tarray" ? "array" : c == "tset" ? "set" : "map";
  std::string innerDesc = PropType(in) + (PropX(in) && PropType(in) != "EnumProperty" && PropType(in) != "ArrayProperty" ? " " + ObjName(PropX(in)) : "");
  o.f("%s %s elem=%u (%s)%s num=%d max=%d%s", ty.c_str(), PropName(p).c_str(), es, innerDesc.c_str(), c == "tmap" ? (" value=" + PropType(vp) + Fmt("@+0x%x", vOff)).c_str() : "", num, cap, c == "tarray" ? "" : Fmt(" slots=%d free=%d stride=%u", slots, nfree, stride).c_str());
  std::vector<std::string> js; int shown = 0;
  for (size_t i = 0; i < el.size() && shown < (int)mx; i++) {
    std::string val = ValueStr(in, (uintptr_t)el[i], 0, nullptr, ""), rest;
    if (c == "tmap") rest = ValueStr(vp, (uintptr_t)el[i] + vOff, 0, nullptr, "");
    if (!keyf.empty() && val.find(keyf) == std::string::npos) continue;
    std::string line = c == "tmap" ? val + " => " + rest : val;
    o.f("[%zu] 0x%llx %s", i, (unsigned long long)el[i], line.c_str());
    jo::Obj j; j.num("i", (long long)i).hex("addr", el[i]).str("value", val); if (c == "tmap") j.str("map_value", rest);
    js.push_back(j.done()); shown++;
  }
  JsonSetData(jo::Obj().str("prop", PropName(p)).str("kind", kind).num("num", num).num("max", cap).num("elem_size", es).num("slots", slots).num("free", nfree).raw("items", jo::Arr(js)).done());
}

static void CmdLayoutCheck(const std::vector<std::string>& t, Out& o) {
  if (t.size() < 2) { o.line("usage: layoutcheck <Class> [obj]"); return; }
  uintptr_t cls = ResolveStruct(t[1]); if (!cls) { o.line("cannot resolve class " + t[1]); return; }
  uintptr_t obj = 0;
  if (t.size() > 2) { obj = ResolveObj(t[2]); if (!obj) { o.line("cannot resolve object " + t[2]); return; } }
  std::string dir = S2Env("ANOMALY_SDK", S2Path("sdk")), name = ObjName(cls);
  std::map<std::string, std::pair<long, long>> sdk; long sdkSize = -1; bool haveSdk = false;
  for (const std::string& n : { name, t[1] }) {
    FILE* f = fopen((dir + "\\" + n + ".tsv").c_str(), "r"); if (!f) continue;
    haveSdk = true; char buf[512];
    while (fgets(buf, sizeof buf, f)) {
      std::string l = buf; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      size_t a1 = l.find('\t'); if (a1 == std::string::npos) continue;
      size_t a2 = l.find('\t', a1 + 1);
      if (l.rfind("#size", 0) == 0) { sdkSize = strtol(l.c_str() + a1 + 1, nullptr, 0); continue; }
      if (a2 == std::string::npos) continue;
      sdk[l.substr(0, a1)] = { strtol(l.c_str() + a1 + 1, nullptr, 0), strtol(l.c_str() + a2 + 1, nullptr, 0) };
    }
    fclose(f); break;
  }
  if (!haveSdk) o.line("note: no sdk table " + dir + "\\" + name + ".tsv (no generator since the rag was removed; dumpsdk <file> writes every live layout); live checks only");
  int props = 0, bad = 0; std::vector<std::string> js; std::set<std::string> seen;
  o.line("name                           live_off sdk_off size   status");
  ForProps(cls, false, [&](uintptr_t q) {
    std::string pn = PropName(q); int off = PropOff(q), sz = PropElem(q) * std::max(1, PropDim(q)); props++; seen.insert(pn);
    std::string st = "ok", so = "-"; auto it = sdk.find(pn);
    if (haveSdk) {
      if (it == sdk.end()) st = "MISSING";
      else { so = Fmt("0x%lx", it->second.first); if (it->second.first != off) st = "OFF"; else if (it->second.second != sz) st = "SIZE"; }
    }
    if (obj && st == "ok") {
      uint8_t b; if (!SafeRead(&b, obj + off, 1)) st = "UNREADABLE";
      else if (PropType(q) == "ObjectProperty") { uintptr_t pv = RdP(obj + off); if (pv && !IsObj(pv)) st = "BADPTR"; }
    }
    if (st != "ok") bad++;
    o.f("%-30s 0x%-6x %-7s %-6d %s", pn.c_str(), off, so.c_str(), sz, st.c_str());
    js.push_back(jo::Obj().str("name", pn).num("live_off", off).str("sdk_off", so).num("size", sz).str("status", st).done());
    return true;
  });
  for (auto& kv : sdk) if (!seen.count(kv.first)) { bad++; o.f("%-30s %-8s 0x%-5lx %-6ld SDKONLY", kv.first.c_str(), "-", kv.second.first, kv.second.second); js.push_back(jo::Obj().str("name", kv.first).str("status", "SDKONLY").done()); }
  int live = Rd<int>(cls + S_SIZE); std::string cs = Fmt("class size 0x%x", live);
  if (sdkSize >= 0) { cs += sdkSize == live ? Fmt(" == sdk 0x%lx", sdkSize) : Fmt(" != sdk 0x%lx", sdkSize); if (sdkSize != live) bad++; }
  o.f("%d props, %d mismatches, %s", props, bad, cs.c_str());
  JsonSetData(jo::Obj().str("class", name).num("props", props).num("mismatches", bad).num("class_size", live).num("sdk_size", sdkSize).raw("rows", jo::Arr(js)).done());
}
