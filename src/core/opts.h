#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace uopt {

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> kv;
  std::set<std::string> flags;
  bool Has(const char* k) const { return kv.count(k) > 0; }
  bool Flag(const char* k) const { return flags.count(k) > 0; }
  std::string Str(const char* k, const std::string& d = "") const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
  long long Int(const char* k, long long d) const { auto it = kv.find(k); return it == kv.end() ? d : atoll(it->second.c_str()); }
  double Dbl(const char* k, double d) const { auto it = kv.find(k); return it == kv.end() ? d : atof(it->second.c_str()); }
};

inline std::string LowerS(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

inline Args Parse(const std::vector<std::string>& t, size_t from) {
  Args a;
  for (size_t i = from; i < t.size(); i++) {
    const std::string& s = t[i]; size_t eq = s.find('=');
    if (eq != std::string::npos && eq > 0) a.kv[LowerS(s.substr(0, eq))] = s.substr(eq + 1);
    else if (!s.empty() && s.find_first_not_of("0123456789") != std::string::npos) a.flags.insert(LowerS(s));
    else a.pos.push_back(s);
  }
  return a;
}

inline bool ParseXyz(const std::string& s, double v[3]) {
  double x, y, z; char tail = 0;
  if (sscanf(s.c_str(), "%lf,%lf,%lf%c", &x, &y, &z, &tail) != 3) return false;
  v[0] = x; v[1] = y; v[2] = z; return true;
}

inline double Dist(const double a[3], const double b[3]) {
  double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return sqrt(dx * dx + dy * dy + dz * dz);
}

struct Row { unsigned long long addr = 0; int idx = 0; bool haveLoc = false; double loc[3] = {}; double dist = 0; int hidden = -1; };

inline void SelectRows(std::vector<Row>& rows, bool byDist, int newN) {
  if (byDist) std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.dist < b.dist; });
  if (newN > 0) {
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.idx > b.idx; });
    if ((int)rows.size() > newN) rows.resize(newN);
  }
}

}
