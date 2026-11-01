#pragma once
#include <string>
#include <vector>
#include <algorithm>

namespace plr {

inline std::string Join(const std::vector<std::string>& names) {
  std::string r;
  for (auto& n : names) { if (n.empty()) continue; if (!r.empty()) r += ","; r += n; }
  return r;
}

inline std::vector<std::string> Split(const std::string& s) {
  std::vector<std::string> r; size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
    std::string n = s.substr(i, j - i);
    if (!n.empty() && std::find(r.begin(), r.end(), n) == r.end()) r.push_back(n);
    i = j + 1;
  }
  return r;
}

inline std::vector<std::string> Missing(const std::vector<std::string>& want, const std::vector<std::string>& loaded) {
  std::vector<std::string> r;
  for (auto& n : want) if (std::find(loaded.begin(), loaded.end(), n) == loaded.end()) r.push_back(n);
  return r;
}

}
