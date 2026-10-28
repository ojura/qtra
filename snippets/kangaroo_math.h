#pragma once

// Small vector maths and cube-surface geometry shared by the kangaroo rig, the
// mob simulation and the tests. Standard library only.
//
// The planet is the widget's cube, [-1, 1] on each axis in the cube's own
// frame. Faces are numbered by their outward normal: 0 is +x, 1 is -x, 2 is +y,
// 3 is -y, 4 is +z and 5 is -z. That is not the order the widget's vertex
// buffer lists them in, and nothing here reads that buffer.

#include "cube_gravity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace kangaroo {

struct V3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    constexpr V3() = default;
    constexpr V3(const float xx, const float yy, const float zz) : x(xx), y(yy), z(zz) {}

    constexpr V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr V3 operator-() const { return {-x, -y, -z}; }
    constexpr V3 operator*(const float s) const { return {x * s, y * s, z * s}; }
    constexpr V3 operator/(const float s) const { return {x / s, y / s, z / s}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3& operator-=(const V3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    V3& operator*=(const float s) { x *= s; y *= s; z *= s; return *this; }
    [[nodiscard]] constexpr float operator[](const int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    float& operator[](const int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

inline constexpr V3 operator*(const float s, const V3& v) { return v * s; }
inline constexpr float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline constexpr V3 cross(const V3& a, const V3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const V3& v) { return std::sqrt(dot(v, v)); }
inline V3 normalize(const V3& v, const V3& fallback = V3{0.0F, 1.0F, 0.0F})
{
    const float l = length(v);
    return l > 1e-8F ? v / l : fallback;
}
inline V3 lerp(const V3& a, const V3& b, const float t) { return a + (b - a) * t; }
inline bool finite(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

inline cube_gravity::Vec toVec(const V3& v) { return {v.x, v.y, v.z}; }
inline V3 fromVec(const cube_gravity::Vec& v)
{
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

// Rotates v about a unit axis by angle radians (Rodrigues).
inline V3 rotate(const V3& v, const V3& axis, const float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0F - c));
}

// The component of v perpendicular to unit n.
inline V3 flatten(const V3& v, const V3& n) { return v - n * dot(v, n); }

inline constexpr float pi = 3.14159265358979323846F;

// ---------------------------------------------------------------------------
// The cube's surface
// ---------------------------------------------------------------------------

inline constexpr int faceCount = 6;

inline V3 faceNormal(const int face)
{
    V3 n{};
    n[face / 2] = (face % 2 == 0) ? 1.0F : -1.0F;
    return n;
}

// The face whose plane p is outermost against, which for a point on or near
// the surface is the face it is on.
inline int faceOf(const V3& p)
{
    int axis = 0;
    float best = std::fabs(p.x);
    if (std::fabs(p.y) > best) { axis = 1; best = std::fabs(p.y); }
    if (std::fabs(p.z) > best) { axis = 2; }
    return axis * 2 + (p[axis] < 0.0F ? 1 : 0);
}

// The largest coordinate magnitude: 1 on the surface, below 1 inside.
inline float boxNorm(const V3& p)
{
    return std::max({std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
}

// Puts p on the given face: its own coordinate set to the face plane and the
// other two clamped to the face.
inline V3 onFace(V3 p, const int face)
{
    const int axis = face / 2;
    p[axis] = (face % 2 == 0) ? 1.0F : -1.0F;
    for (int other = 0; other < 3; ++other) {
        if (other != axis) {
            p[other] = std::clamp(p[other], -1.0F, 1.0F);
        }
    }
    return p;
}

inline V3 toSurface(const V3& p) { return onFace(p, faceOf(p)); }

// The two in-plane axes of a face, so a face's own 2D coordinates are
// (dot(p, u), dot(p, v)). Right-handed with the outward normal: u x v = n.
inline void faceAxes(const int face, V3& u, V3& v)
{
    const int axis = face / 2;
    const float s = (face % 2 == 0) ? 1.0F : -1.0F;
    u = V3{};
    v = V3{};
    u[(axis + 1) % 3] = 1.0F;
    v[(axis + 2) % 3] = s;
}

// Moving along the surface. A step that runs off a face continues over the
// edge onto the next one, folded the way a sheet of paper wrapped round the
// cube would fold, and the direction folds with it. This is how a walking
// kangaroo, which never leaves the ground, gets from one face to another.
struct SurfacePoint {
    V3 p;
    V3 heading;
    int face = 0;
};

inline SurfacePoint walk(SurfacePoint at, float distance)
{
    for (int guard = 0; guard < 6 && distance > 1e-7F; ++guard) {
        const V3 n = faceNormal(at.face);
        V3 dir = normalize(flatten(at.heading, n), V3{});
        if (length(dir) < 0.5F) {
            return at;
        }
        // How far along dir before another coordinate reaches +-1.
        float limit = distance;
        int exitAxis = -1;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis == at.face / 2 || std::fabs(dir[axis]) < 1e-7F) {
                continue;
            }
            const float bound = dir[axis] > 0.0F ? 1.0F : -1.0F;
            const float t = (bound - at.p[axis]) / dir[axis];
            if (t >= 0.0F && t < limit) {
                limit = t;
                exitAxis = axis;
            }
        }
        at.p = at.p + dir * limit;
        distance -= limit;
        if (exitAxis < 0) {
            at.p = onFace(at.p, at.face);
            at.heading = dir;
            return at;
        }
        // Over the edge: the component that was carrying us toward the new
        // face's plane now carries us down its side, away from the old face.
        const int newFace = exitAxis * 2 + (dir[exitAxis] > 0.0F ? 0 : 1);
        const V3 newNormal = faceNormal(newFace);
        const float toward = dot(dir, newNormal);
        at.heading = normalize(flatten(dir, newNormal) - n * toward, dir);
        at.face = newFace;
        at.p = onFace(at.p, newFace);
    }
    return at;
}

// How far along dir, within the face, before reaching one of its edges.
inline float distanceToEdge(const V3& p, const V3& dir, const int face)
{
    float limit = 1e9F;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis == face / 2 || std::fabs(dir[axis]) < 1e-7F) {
            continue;
        }
        const float bound = dir[axis] > 0.0F ? 1.0F : -1.0F;
        const float t = (bound - p[axis]) / dir[axis];
        if (t >= 0.0F) {
            limit = std::min(limit, t);
        }
    }
    return limit;
}

// Carries a direction from one face onto another the way walk() folds it over
// their shared edge. Used when a hop lands on a different face from the one it
// left, so the kangaroo keeps facing the way it was going.
inline V3 foldDirection(const V3& dir, const int fromFace, const int toFace)
{
    if (fromFace == toFace) {
        return normalize(flatten(dir, faceNormal(toFace)), dir);
    }
    const V3 oldNormal = faceNormal(fromFace);
    const V3 newNormal = faceNormal(toFace);
    if (std::fabs(dot(oldNormal, newNormal)) > 0.5F) {
        // Opposite faces share no edge; keep what lies in the plane.
        return normalize(flatten(dir, newNormal), V3{1.0F, 0.0F, 0.0F});
    }
    const float toward = dot(dir, newNormal);
    return normalize(flatten(dir, newNormal) - oldNormal * toward, dir);
}

// The direction along the surface from a to b, as the first step of a path
// that crosses at most one edge. For b on the same face it is the straight
// line. For b on a neighbouring face the far face is unfolded into the plane
// of a's face about their shared edge and the line taken to that image, which
// is the shortest path across that edge.
inline V3 surfaceDirection(const V3& a, const int faceA, const V3& b, const int faceB)
{
    const V3 n = faceNormal(faceA);
    if (faceA == faceB) {
        return normalize(flatten(b - a, n), V3{});
    }
    const V3 nb = faceNormal(faceB);
    if (std::fabs(dot(n, nb)) > 0.5F) {
        // The opposite face: head for whichever neighbour is closest to b, by
        // aiming at b's position projected onto this face's plane, pushed out
        // past the edge.
        V3 aim = flatten(b, n);
        if (length(aim) < 1e-3F) {
            aim = flatten(V3{0.37F, 0.81F, 0.45F}, n);
        }
        return normalize(aim * 4.0F - flatten(a, n), V3{});
    }
    // Unfold: b's height above the shared edge (along -n) becomes distance
    // beyond the edge along nb.
    const float depth = 1.0F - dot(b, n);
    const V3 image = flatten(b, n) + n + nb * depth;
    return normalize(flatten(image - a, n), V3{});
}

// A small deterministic generator so a seed reproduces a whole scene.
struct Rng {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;

    explicit Rng(const std::uint64_t seed = 1) : state(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) {}

    std::uint32_t next()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<std::uint32_t>((state * 0x2545F4914F6CDD1DULL) >> 32);
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0F / 16777216.0F); }
    float range(const float lo, const float hi) { return lo + (hi - lo) * uniform(); }
    bool chance(const float p) { return uniform() < p; }
    V3 inDisc(const V3& u, const V3& v, const float radius)
    {
        const float r = radius * std::sqrt(uniform());
        const float a = 2.0F * pi * uniform();
        return u * (r * std::cos(a)) + v * (r * std::sin(a));
    }
};

} // namespace kangaroo
