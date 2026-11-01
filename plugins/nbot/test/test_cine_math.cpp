#include <cmath>
#include <cstdio>
#include <vector>
#include "../nbot_cine_math.h"

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace cine;

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
static bool near3(Vec a, Vec b, double eps = 1e-6) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

int main()
{
    {
        Vec p0{0, 0, 0}, p1{1, 0, 0}, p2{2, 0, 0}, p3{3, 0, 0};
        CHECK(near3(catmull(p0, p1, p2, p3, 0.0), p1), "catmull t=0 should be p1");
        CHECK(near3(catmull(p0, p1, p2, p3, 1.0), p2), "catmull t=1 should be p2");
        CHECK(near3(catmull(p0, p1, p2, p3, 0.5), Vec{1.5, 0, 0}), "catmull midpoint on a straight line");
    }
    {
        Rot in{20, 45, 10};
        Rot out = quat_rot(rot_quat(in));
        CHECK(near(out.pitch, in.pitch) && near(out.yaw, in.yaw) && near(out.roll, in.roll), "rot->quat->rot round trip (%f %f %f)", out.pitch, out.yaw, out.roll);
        Vec f = rotate(rot_quat(Rot{0, 90, 0}), Vec{1, 0, 0});
        CHECK(near3(f, Vec{0, 1, 0}), "yaw 90 should face +y");
        Rot rolled = quat_rot(rot_quat(Rot{0, 0, 30}));
        CHECK(near(rolled.roll, 30) && near(rolled.pitch, 0), "roll-only round trip");
    }
    {
        Quat a = rot_quat(Rot{0, 0, 0});
        Quat b = rot_quat(Rot{0, 90, 0});
        CHECK(near(quat_rot(slerp(a, b, 0.5)).yaw, 45), "slerp halfway yaw 45");
        CHECK(near(quat_rot(slerp(a, b, 0.0)).yaw, 0) && near(quat_rot(slerp(a, b, 1.0)).yaw, 90), "slerp endpoints");
        Quat neg{-b.w, -b.x, -b.y, -b.z};
        CHECK(near(quat_rot(slerp(a, neg, 1.0)).yaw, 90), "slerp takes the short path");
    }
    {
        CHECK(near(look_at(Vec{0, 0, 0}, Vec{10, 0, 0}).yaw, 0), "look at +x yaw 0");
        CHECK(near(look_at(Vec{0, 0, 0}, Vec{0, 10, 0}).yaw, 90), "look at +y yaw 90");
        CHECK(near(look_at(Vec{0, 0, 0}, Vec{0, 0, 10}).pitch, 90), "look at +z pitch 90");
        Rot r3 = look_at(Vec{0, 0, 0}, Vec{-10, 0, -10});
        CHECK(near(r3.pitch, -45) && near(r3.yaw, 180), "look down-back");
    }
    {
        std::vector<Key> keys;
        keys.push_back(Key{Vec{0, 0, 0}, Rot{0, 0, 0}, 90, 0});
        keys.push_back(Key{Vec{100, 0, 0}, Rot{0, 0, 0}, 60, 2});
        keys.push_back(Key{Vec{100, 100, 0}, Rot{0, 90, 0}, 60, 2});
        CHECK(near3(sample(keys, 0.0).pos, Vec{0, 0, 0}) && near(sample(keys, 0.0).fov, 90), "sample at start");
        CHECK(near3(sample(keys, 2.0).pos, Vec{100, 0, 0}), "sample at second key time");
        CHECK(near(sample(keys, 1.0).fov, 75), "fov lerp halfway");
        Pose pe = sample(keys, 99.0);
        CHECK(near3(pe.pos, Vec{100, 100, 0}) && near(pe.rot.yaw, 90), "sample clamps past end");
        CHECK(near3(sample(keys, -5.0).pos, Vec{0, 0, 0}), "sample clamps before start");
    }
    {
        Pose o0 = orbit_pose(Vec{0, 0, 0}, 10, 2, 0);
        CHECK(near3(o0.pos, Vec{10, 0, 2}), "orbit angle 0 on +x");
        Pose o1 = orbit_pose(Vec{0, 0, 0}, 10, 2, 90);
        CHECK(near3(o1.pos, Vec{0, 10, 2}), "orbit angle 90 on +y");
        CHECK(near(o1.rot.yaw, -90), "orbit faces the aim point, yaw %f", o1.rot.yaw);
        CHECK(near(o1.rot.pitch, std::atan2(-2.0, 10.0) / kDeg, 1e-6), "orbit pitch looks at aim");
    }
    std::printf("nbot cine math: %d ok, %d fail\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
