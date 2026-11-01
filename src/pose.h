#pragma once
#include <math.h>
#include <string>
#include <vector>
#include <stdio.h>

namespace pse {

struct Xf { double q[4] = { 0, 0, 0, 1 }; double t[3] = {}; };

inline double Wrap180(double a) {
  while (a > 180.0) a -= 360.0;
  while (a < -180.0) a += 360.0;
  return a;
}

inline void Euler(const double q[4], double e[3]) {
  const double X = q[0], Y = q[1], Z = q[2], W = q[3], R2D = 180.0 / 3.14159265358979323846;
  double st = Z * X - W * Y, yawY = 2.0 * (W * Z + X * Y), yawX = 1.0 - 2.0 * (Y * Y + Z * Z), pitch, yaw = atan2(yawY, yawX) * R2D, roll;
  if (st < -0.4999995) { pitch = -90.0; roll = Wrap180(-yaw - 2.0 * atan2(X, W) * R2D); }
  else if (st > 0.4999995) { pitch = 90.0; roll = Wrap180(yaw - 2.0 * atan2(X, W) * R2D); }
  else { pitch = asin(2.0 * st) * R2D; roll = -atan2(-2.0 * (W * X + Y * Z), 1.0 - 2.0 * (X * X + Y * Y)) * R2D; }
  e[0] = roll; e[1] = pitch; e[2] = yaw;
}

inline double QuatAngle(const double a[4], const double b[4]) {
  double d = fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  if (d > 1.0) d = 1.0;
  return 2.0 * acos(d) * 180.0 / 3.14159265358979323846;
}

inline double Dist3(const double a[3], const double b[3]) {
  double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return sqrt(dx * dx + dy * dy + dz * dz);
}

inline bool TwistByRoll(double handLocalRoll) { return fabs(Wrap180(handLocalRoll)) > 120.0; }
inline bool TwistByRef(double dRotDeg) { return dRotDeg > 35.0; }
inline bool Hover(double gapCm) { return gapCm > 3.0; }

inline std::string BoneFlag(bool isForeArm, bool twistRoll, bool haveRef, double dRotDeg) {
  if (isForeArm && twistRoll) return "TWIST";
  if (haveRef && TwistByRef(dRotDeg)) return "TWIST";
  return "";
}
inline std::string CompFlag(double gapCm, bool isMesh) { return isMesh && Hover(gapCm) ? "HOVER" : ""; }

inline bool IsForeArm(const std::string& bone) { return bone.find("fore_arm") != std::string::npos; }
inline bool IsHand(const std::string& bone) { size_t p = bone.rfind("_hand"); return p != std::string::npos && p + 5 == bone.size(); }
inline std::string SideOf(const std::string& bone) {
  if (bone.find("_l_") != std::string::npos) return "l";
  if (bone.find("_r_") != std::string::npos) return "r";
  return "";
}

inline std::vector<std::string> BoneSet(const std::string& spec) {
  static const char* hands[] = { "jnt_r_hand", "jnt_l_hand", "jnt_r_fore_arm", "jnt_l_fore_arm", "jnt_r_arm", "jnt_l_arm", "jnt_r_weapon", "jnt_item" };
  std::vector<std::string> v;
  if (spec.empty() || spec == "hands") { for (auto* h : hands) v.push_back(h); return v; }
  if (spec == "arms") { for (auto* h : hands) v.push_back(h); v.push_back("jnt_r_shoulder"); v.push_back("jnt_l_shoulder"); return v; }
  size_t s = 0;
  while (s <= spec.size()) {
    size_t e = spec.find(',', s); std::string it = spec.substr(s, e == std::string::npos ? std::string::npos : e - s);
    if (!it.empty()) v.push_back(it);
    if (e == std::string::npos) break; s = e + 1;
  }
  return v;
}

inline bool IsLegBone(const std::string& n) {
  static const char* parts[] = { "thigh", "calf", "foot", "toe" };
  if (n.find("jnt_l_") != 0 && n.find("jnt_r_") != 0) return false;
  for (auto* p : parts) if (n.find(p) != std::string::npos) return true;
  return false;
}

struct Row {
  std::string bone, flag;
  Xf disp, comp, par;
  bool haveRef = false; Xf refComp, refPar, refDisp;
  double dLoc = 0, dRot = 0, roll = 0;
};

inline void Finish(std::vector<Row>& rows) {
  for (auto& r : rows) {
    double e[3]; Euler(r.par.q, e); r.roll = e[0];
    if (r.haveRef) { r.dLoc = Dist3(r.comp.t, r.refComp.t); r.dRot = QuatAngle(r.par.q, r.refPar.q); }
  }
  for (auto& r : rows) {
    bool tw = false;
    if (IsForeArm(r.bone)) {
      for (auto& h : rows) if (IsHand(h.bone) && SideOf(h.bone) == SideOf(r.bone)) { if (TwistByRoll(h.roll)) tw = true; }
    }
    r.flag = BoneFlag(IsForeArm(r.bone), tw, r.haveRef, r.dRot);
  }
}

}
