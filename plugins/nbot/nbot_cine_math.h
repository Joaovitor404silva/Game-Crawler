#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace cine
{
    struct Vec { double x, y, z; };
    struct Rot { double pitch, yaw, roll; };
    struct Quat { double w, x, y, z; };
    struct Key { Vec pos; Rot rot; double fov; double secs; };
    struct Pose { Vec pos; Rot rot; double fov; };

    inline constexpr double kPi = 3.14159265358979323846;
    inline constexpr double kDeg = kPi / 180.0;

    inline double lerpd(double a, double b, double t) { return a + (b - a) * t; }

    inline Vec catmull(Vec p0, Vec p1, Vec p2, Vec p3, double t)
    {
        double t2 = t * t, t3 = t2 * t;
        auto axis = [&](double a, double b, double c, double d) {
            return 0.5 * ((2 * b) + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
        };
        return Vec{axis(p0.x, p1.x, p2.x, p3.x), axis(p0.y, p1.y, p2.y, p3.y), axis(p0.z, p1.z, p2.z, p3.z)};
    }

    inline Quat qmul(Quat a, Quat b)
    {
        return Quat{a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    }

    inline Quat axis_quat(int axis, double deg)
    {
        double h = deg * kDeg * 0.5;
        Quat q{std::cos(h), 0, 0, 0};
        if (axis == 0) q.x = std::sin(h);
        else if (axis == 1) q.y = std::sin(h);
        else q.z = std::sin(h);
        return q;
    }

    inline Quat rot_quat(Rot r)
    {
        return qmul(qmul(axis_quat(2, r.yaw), axis_quat(1, -r.pitch)), axis_quat(0, r.roll));
    }

    inline Vec rotate(Quat q, Vec v)
    {
        Quat p{0, v.x, v.y, v.z};
        Quat c{q.w, -q.x, -q.y, -q.z};
        Quat r = qmul(qmul(q, p), c);
        return Vec{r.x, r.y, r.z};
    }

    inline double dot3(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    inline Rot quat_rot(Quat q)
    {
        Vec f = rotate(q, Vec{1, 0, 0});
        Vec u = rotate(q, Vec{0, 0, 1});
        double pitch = std::asin(std::clamp(f.z, -1.0, 1.0));
        double yaw = std::atan2(f.y, f.x);
        Vec u0{-std::sin(pitch) * std::cos(yaw), -std::sin(pitch) * std::sin(yaw), std::cos(pitch)};
        Vec yv{-std::sin(yaw), std::cos(yaw), 0};
        double roll = std::atan2(-dot3(u, yv), dot3(u, u0));
        return Rot{pitch / kDeg, yaw / kDeg, roll / kDeg};
    }

    inline Quat slerp(Quat a, Quat b, double t)
    {
        double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        if (d < 0)
        {
            b = Quat{-b.w, -b.x, -b.y, -b.z};
            d = -d;
        }
        double wa, wb;
        if (d > 0.9995)
        {
            wa = 1 - t;
            wb = t;
        }
        else
        {
            double th = std::acos(std::clamp(d, -1.0, 1.0));
            double s = std::sin(th);
            wa = std::sin((1 - t) * th) / s;
            wb = std::sin(t * th) / s;
        }
        Quat r{wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z};
        double n = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
        return Quat{r.w / n, r.x / n, r.y / n, r.z / n};
    }

    inline Rot look_at(Vec from, Vec to)
    {
        double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        double h = std::sqrt(dx * dx + dy * dy);
        return Rot{std::atan2(dz, h) / kDeg, std::atan2(dy, dx) / kDeg, 0};
    }

    inline Pose sample(const std::vector<Key>& keys, double t)
    {
        if (keys.empty()) return Pose{Vec{0, 0, 0}, Rot{0, 0, 0}, 90};
        if (keys.size() == 1) return Pose{keys[0].pos, keys[0].rot, keys[0].fov};
        std::vector<double> times(keys.size(), 0.0);
        for (size_t i = 1; i < keys.size(); ++i) times[i] = times[i - 1] + std::max(keys[i].secs, 1e-3);
        if (t <= 0) return Pose{keys[0].pos, keys[0].rot, keys[0].fov};
        if (t >= times.back()) return Pose{keys.back().pos, keys.back().rot, keys.back().fov};
        size_t i = 1;
        while (i < keys.size() - 1 && times[i] < t) ++i;
        size_t a = i - 1;
        double u = (t - times[a]) / (times[i] - times[a]);
        Vec pos = catmull(keys[a == 0 ? 0 : a - 1].pos, keys[a].pos, keys[i].pos, keys[std::min(i + 1, keys.size() - 1)].pos, u);
        Rot rot = quat_rot(slerp(rot_quat(keys[a].rot), rot_quat(keys[i].rot), u));
        return Pose{pos, rot, lerpd(keys[a].fov, keys[i].fov, u)};
    }

    inline Pose orbit_pose(Vec aim, double radius, double height, double deg)
    {
        double a = deg * kDeg;
        Vec pos{aim.x + radius * std::cos(a), aim.y + radius * std::sin(a), aim.z + height};
        return Pose{pos, look_at(pos, aim), 90};
    }
}
