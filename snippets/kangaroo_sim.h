#pragma once

// A mob of kangaroos living on the cube.
//
// Everything is computed in the cube's own frame, which turns with the cube.
// That frame is not inertial, so a body in it feels the cube's attraction, a
// centrifugal push away from the axle, a Coriolis force sideways to its
// motion, and while the spin rate changes an Euler force as the ground turns
// under it. cube_gravity::SpinningCube supplies all four and nothing here
// adds anything to them, so whatever a kangaroo does in the air follows from
// them: hops that curve, slow-motion hops where the push away from the axle
// nearly cancels gravity, and a planet spun fast enough to throw the mob off.
//
// The animals move in two ways. Hopping is ballistic: a push-off chooses the
// velocity that should carry the hop to its landing point under the local
// forces, and then the flight is integrated under the real ones, so the
// landing lands where physics puts it. Walking, the slow five-legged gait
// with the tail and forepaws planted, never leaves the ground and follows
// the surface over edges like a sheet folded round the cube.
//
// Whatever a kangaroo does to its own momentum it does to the planet's,
// in the opposite sense: the planet turns on a fixed axle, so the component of
// angular momentum along that axle is conserved between it and the mob. Every
// substep the change in each animal's axial angular momentum is added, with
// the sign flipped, to what the planet is owed. The step function collects it
// and turns it into spin.
//
// Water is not simulated as a fluid. Over the time it takes to watch, it sits
// where the effective potential (gravity plus the centrifugal term) is lowest,
// with a level surface on which that potential is constant, so the
// billabongs are found by solving for the one level that holds the planet's
// water. They pool at the face centres at rest and slide away from the axle
// when the planet spins up, which is exactly where real water would go.
//
// Standard library only, so tests drive it without Qt or a display.

#include "cube_gravity.h"
#include "kangaroo_math.h"
#include "kangaroo_rig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace kangaroo {

// ---------------------------------------------------------------------------
// Water
// ---------------------------------------------------------------------------

struct Water {
    static constexpr int cells = 40;
    static constexpr int stride = cells + 1;
    static constexpr float maxDepth = 0.3F;
    // How far below the ground the water's surface is followed where the
    // ground is dry. Only the sign matters there, but a field that carries on
    // past the shore crosses zero along a smooth line, which is where the
    // renderer draws the shore. One clamped to zero at every dry vertex would
    // put the shore on the grid's triangle edges, as a row of teeth.
    static constexpr float dryReach = 0.05F;

    // Per face, per grid vertex: where it is, the static part of its potential
    // and the inward attraction along the face normal. These depend only on
    // the gravity, so they are computed once per gravity setting.
    std::array<std::vector<V3>, faceCount> position;
    std::array<std::vector<float>, faceCount> potential;
    std::array<std::vector<float>, faceCount> inward;
    // The spin's share, which depends on the axle and not on the rate: the
    // squared distance from the axle, and the outward reach of the
    // centrifugal push along the face normal. The rate squares into both.
    std::array<std::vector<float>, faceCount> offSquared;
    std::array<std::vector<float>, faceCount> offNormal;
    // Per-vertex potential and holding force at the current rate, filled once
    // per solve.
    std::array<std::vector<float>, faceCount> phiNow;
    std::array<std::vector<float>, faceCount> holdNow;
    // The water surface's height above the ground at each vertex, negative
    // where the ground is above it: what the level asks for, and what is
    // shown, which follows the target over a second or so instead of jumping.
    // The depth is this where it is positive.
    std::array<std::vector<float>, faceCount> targetHeight;
    std::array<std::vector<float>, faceCount> height;

    float volume = 0.0F;
    float level = 0.0F;
    float preparedFor = -1.0F;
    float solvedFor = -1.0F;
    bool spilled = false;

    [[nodiscard]] static float cellArea() { return (2.0F / cells) * (2.0F / cells); }

    // Vertex weights for integrating over a face: a quarter of a cell at a
    // corner, half along an edge, one inside.
    [[nodiscard]] static float weight(const int i, const int j)
    {
        const float wi = (i == 0 || i == cells) ? 0.5F : 1.0F;
        const float wj = (j == 0 || j == cells) ? 0.5F : 1.0F;
        return wi * wj;
    }

    void prepare(const cube_gravity::SpinningCube& planet, const float restDepth)
    {
        const double k = planet.gravityScale();
        const V3 axle = fromVec(planet.axle);
        for (int face = 0; face < faceCount; ++face) {
            V3 u;
            V3 v;
            faceAxes(face, u, v);
            const V3 n = faceNormal(face);
            position[face].resize(stride * stride);
            potential[face].resize(stride * stride);
            inward[face].resize(stride * stride);
            offSquared[face].resize(stride * stride);
            offNormal[face].resize(stride * stride);
            phiNow[face].resize(stride * stride);
            holdNow[face].resize(stride * stride);
            targetHeight[face].assign(stride * stride, -dryReach);
            height[face].assign(stride * stride, -dryReach);
            for (int j = 0; j < stride; ++j) {
                for (int i = 0; i < stride; ++i) {
                    const float s = -1.0F + 2.0F * static_cast<float>(i) / cells;
                    const float t = -1.0F + 2.0F * static_cast<float>(j) / cells;
                    const V3 p = n + u * s + v * t;
                    const int index = j * stride + i;
                    position[face][index] = p;
                    potential[face][index] = static_cast<float>(-k * cube_gravity::potential(toVec(p)));
                    const V3 a = fromVec(cube_gravity::attraction(toVec(p)));
                    inward[face][index] = static_cast<float>(k) * -dot(a, n);
                    const V3 off = p - axle * dot(p, axle);
                    offSquared[face][index] = dot(off, off);
                    offNormal[face][index] = dot(off, n);
                }
            }
        }
        // The volume is whatever fills the planet at rest to `restDepth` at a
        // face centre, so the parameter means something visible.
        const int centre = (cells / 2) * stride + cells / 2;
        const float restLevel = potential[2][centre] + restDepth * inward[2][centre];
        atRate(0.0F);
        volume = volumeAt(restLevel, nullptr);
        preparedFor = static_cast<float>(planet.surfaceGravity);
        solvedFor = -1.0F;
        solve(0.0F);
        for (int face = 0; face < faceCount; ++face) {
            height[face] = targetHeight[face];
        }
    }

    // The potential and holding force at every vertex for a spin rate.
    void atRate(const float omega)
    {
        const float w2 = omega * omega;
        for (int face = 0; face < faceCount; ++face) {
            for (int index = 0; index < stride * stride; ++index) {
                phiNow[face][index] = potential[face][index] - 0.5F * w2 * offSquared[face][index];
                holdNow[face][index] = inward[face][index] - w2 * offNormal[face][index];
            }
        }
    }

    // The volume held below a level at the rate atRate() was last given, and
    // optionally the surface heights that give it. Where the inward effective
    // gravity is too weak to hold water down the vertex is left dry: that
    // water has nowhere to sit.
    float volumeAt(const float candidate, std::array<std::vector<float>, faceCount>* out) const
    {
        float total = 0.0F;
        for (int face = 0; face < faceCount; ++face) {
            const float* phi = phiNow[face].data();
            const float* hold = holdNow[face].data();
            for (int j = 0; j < stride; ++j) {
                for (int i = 0; i < stride; ++i) {
                    const int index = j * stride + i;
                    float h = -dryReach;
                    if (hold[index] > 0.6F) {
                        h = std::clamp((candidate - phi[index]) / hold[index], -dryReach, maxDepth);
                    }
                    if (out != nullptr) {
                        (*out)[face][index] = h;
                    }
                    total += std::max(h, 0.0F) * weight(i, j);
                }
            }
        }
        return total * cellArea();
    }

    // Finds the one level that holds the planet's water, by bisection: the
    // volume below a level only grows as the level rises. The answer depends
    // only on the rate, so an unchanged rate reuses it.
    void solve(const float omega)
    {
        if (std::fabs(omega - solvedFor) < 2e-3F) {
            return;
        }
        solvedFor = omega;
        atRate(omega);
        float lo = 1e9F;
        float hi = -1e9F;
        for (int face = 0; face < faceCount; ++face) {
            for (const float phi : phiNow[face]) {
                lo = std::min(lo, phi);
                hi = std::max(hi, phi);
            }
        }
        hi += 20.0F;
        for (int iteration = 0; iteration < 40; ++iteration) {
            const float mid = 0.5F * (lo + hi);
            if (volumeAt(mid, nullptr) < volume) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        level = hi;
        const float held = volumeAt(level, &targetHeight);
        // When the planet spins so fast that too little of the surface can
        // hold water, the depth cap is reached and some volume has nowhere to
        // go. It is not lost: the volume is remembered, and the water is back
        // when the spin comes down.
        spilled = held < volume * 0.98F;
    }

    void update(const float omega, const float dt)
    {
        solve(omega);
        const float follow = 1.0F - std::exp(-dt / 0.9F);
        for (int face = 0; face < faceCount; ++face) {
            for (int index = 0; index < stride * stride; ++index) {
                height[face][index] += (targetHeight[face][index] - height[face][index]) * follow;
            }
        }
    }

    // Bilinear depth at a point on a face: zero on dry ground.
    [[nodiscard]] float depthAt(const V3& p, const int face) const
    {
        if (height[face].empty()) {
            return 0.0F;
        }
        V3 u;
        V3 v;
        faceAxes(face, u, v);
        const float s = std::clamp((dot(p, u) + 1.0F) * 0.5F * cells, 0.0F, static_cast<float>(cells) - 1e-3F);
        const float t = std::clamp((dot(p, v) + 1.0F) * 0.5F * cells, 0.0F, static_cast<float>(cells) - 1e-3F);
        const int i = static_cast<int>(s);
        const int j = static_cast<int>(t);
        const float fs = s - static_cast<float>(i);
        const float ft = t - static_cast<float>(j);
        const auto& d = height[face];
        const float a = d[j * stride + i] * (1.0F - fs) + d[j * stride + i + 1] * fs;
        const float b = d[(j + 1) * stride + i] * (1.0F - fs) + d[(j + 1) * stride + i + 1] * fs;
        return std::max(a * (1.0F - ft) + b * ft, 0.0F);
    }

    [[nodiscard]] float faceVolume(const int face) const
    {
        float total = 0.0F;
        for (int j = 0; j < stride; ++j) {
            for (int i = 0; i < stride; ++i) {
                total += std::max(height[face][j * stride + i], 0.0F) * weight(i, j);
            }
        }
        return total * cellArea();
    }

    [[nodiscard]] float totalVolume() const
    {
        float total = 0.0F;
        for (int face = 0; face < faceCount; ++face) {
            total += faceVolume(face);
        }
        return total;
    }

    // The nearest point on a face where the water is shallow enough to stand
    // at and deep enough to drink from, or false if the face is dry.
    bool shoreNear(const V3& from, const int face, V3& shore, V3& towardWater) const
    {
        float best = 1e9F;
        bool found = false;
        V3 u;
        V3 v;
        faceAxes(face, u, v);
        for (int j = 1; j < cells; ++j) {
            for (int i = 1; i < cells; ++i) {
                const int index = j * stride + i;
                const float d = height[face][index];
                if (d > 0.0F) {
                    continue;
                }
                // Dry, with water on some neighbour: that is the shore.
                const float east = height[face][index + 1];
                const float west = height[face][index - 1];
                const float north = height[face][index + stride];
                const float south = height[face][index - stride];
                if (std::max({east, west, north, south}) < 0.004F) {
                    continue;
                }
                const float distance = length(position[face][index] - from);
                if (distance < best) {
                    best = distance;
                    shore = position[face][index];
                    towardWater = normalize(u * (east - west) + v * (north - south), u);
                    found = true;
                }
            }
        }
        return found;
    }
};

// ---------------------------------------------------------------------------
// The scenery the mob uses
// ---------------------------------------------------------------------------

struct Tuft {
    V3 p;
    int face = 0;
    float biomass = 1.0F; // 0 eaten to the ground, 1 full
    float scale = 1.0F;
    float seed = 0.0F;
    V3 up;                // grows against the local gravity
};

struct Tree {
    V3 p;
    int face = 0;
    V3 up;
    float height = 0.4F;
    float seed = 0.0F;
};

struct Sign {
    V3 p;
    int face = 2;
    V3 up;
    V3 facing;
};

struct Footprint {
    V3 p;
    V3 heading;
    int face = 0;
    float age = 0.0F;
    float size = 0.3F;
    int kind = 0; // 0 hind foot, 1 tail drag, 2 forepaw
};

struct Particle {
    V3 p;
    V3 v;
    V3 gravity;
    int face = 0;
    float age = 0.0F;
    float life = 1.0F;
    float size = 0.02F;
    int kind = 0; // 0 dust, 1 water, 2 tuft of fur
};

struct Ripple {
    V3 p;
    int face = 0;
    float age = 0.0F;
    float strength = 1.0F;
};

struct Event {
    std::string name;
    std::string json;
};

// ---------------------------------------------------------------------------
// Kangaroos
// ---------------------------------------------------------------------------

enum class Role { Boomer, Flyer, Buck, Joey };

enum class Activity {
    Rest,
    Graze,
    Travel,
    Drink,
    Groom,
    Box,
    Thump,
    Flee,
    Follow,
    Pouched,   // a joey at foot that has dived into its mother's pouch
    Flung,
    Away,      // gone off the planet; coming back later
};

enum class Gait { Stand, Stance, Flight, Walk };

enum class Move { None, Walk, Hop };

inline const char* activityName(const Activity a)
{
    switch (a) {
    case Activity::Rest: return "resting";
    case Activity::Graze: return "grazing";
    case Activity::Travel: return "travelling";
    case Activity::Drink: return "drinking";
    case Activity::Groom: return "licking its forearms";
    case Activity::Box: return "boxing";
    case Activity::Thump: return "thumping an alarm";
    case Activity::Flee: return "fleeing";
    case Activity::Follow: return "following its mother";
    case Activity::Pouched: return "in the pouch";
    case Activity::Flung: return "flung off the planet";
    case Activity::Away: return "lost in space";
    }
    return "unknown";
}

inline const char* roleName(const Role r)
{
    switch (r) {
    case Role::Boomer: return "boomer";
    case Role::Flyer: return "flyer";
    case Role::Buck: return "buck";
    case Role::Joey: return "joey";
    }
    return "unknown";
}

inline const char* gaitName(const Gait g)
{
    switch (g) {
    case Gait::Stand: return "standing";
    case Gait::Stance: return "stance";
    case Gait::Flight: return "airborne";
    case Gait::Walk: return "walking";
    }
    return "unknown";
}

struct Kangaroo {
    std::string name;
    Role role = Role::Flyer;
    float size = 0.3F;   // standing height in the cube's units
    float mass = 1.0F;
    float tone = 0.0F;   // per-animal shade variation, -1 to 1
    int mother = -1;
    bool pouchJoey = false;

    V3 p;
    V3 v;
    V3 heading{1.0F, 0.0F, 0.0F};
    V3 up{0.0F, 1.0F, 0.0F};       // smoothed, what the body is drawn against
    int face = 2;

    Gait gait = Gait::Stand;
    float gaitClock = 0.0F;
    float stanceDuration = 0.11F;
    float flightDuration = 0.25F;
    float landingSpeed = 0.0F;
    float walkPhase = 0.0F;
    int takeoffFace = 2;

    Activity activity = Activity::Rest;
    float activityClock = 0.0F;
    float activityDuration = 2.0F;
    Move move = Move::None;
    V3 goal;
    int goalFace = 2;
    float speed = 0.0F;
    bool arrived = true;
    int partner = -1;
    int tuft = -1;
    V3 faceToward;            // when set, turns to face this way once arrived
    bool hasFaceToward = false;
    float boutStrength = 0.0F;
    int kicksLanded = 0;
    float stagger = 0.0F;
    float kickClock = 0.0F;
    float kickPose = 0.0F;    // seconds left of a kick being thrown
    float jabClock = 0.0F;

    V3 tumbleAxis{0.0F, 0.0F, 1.0F};
    float tumbleRate = 0.0F;
    float tumbleAngle = 0.0F;
    float awayClock = 0.0F;

    float joeyPeek = 0.0F;
    float pouchFull = 0.0F;
    float calmClock = 0.0F;
    float earLeft = 0.0F;
    float earRight = 0.0F;
    float earClock = 1.0F;
    float blinkClock = 2.0F;
    float lookYaw = 0.0F;
    float lookClock = 1.0F;

    Pose pose;
    bool teleported = false;

    int hops = 0;
    float distance = 0.0F;
    int flungCount = 0;
    int bouts = 0;
    int wins = 0;
    float drankSeconds = 0.0F;
    float grazedBiomass = 0.0F;

    Rng rng;

    [[nodiscard]] bool present() const
    {
        return activity != Activity::Away && activity != Activity::Pouched;
    }
    [[nodiscard]] bool grounded() const { return gait != Gait::Flight; }
    [[nodiscard]] bool adult() const { return role != Role::Joey; }
    [[nodiscard]] bool male() const { return role == Role::Boomer || role == Role::Buck; }
};

struct Settings {
    int mob = 6;
    float surfaceGravity = 14.0F;
    float waterDepth = 0.05F;
    // The planet's moment of inertia about its axle, in units where the mob's
    // adult female has mass 1. A real cube of rock this size would outweigh a
    // kangaroo thousands of times and not notice it; 40 keeps the planet's
    // answer to a hop near 2 degrees a second and to a stampede near 20, so
    // it can be seen without the mob shaking the whole scene.
    float planetInertia = 40.0F;
    std::uint64_t seed = 1;
    bool hopEvents = false;
};

// How the planet answers the mob: the angular impulse they hand it about the
// axle becomes extra spin, and the motor that drives the cube pulls the rate
// back to what it was set to over a few seconds, the way a driven axle with
// some give would. The step function runs this every tick; the tests run the
// same code with the planet's answer fed back into the mob, which is the loop
// that once ran away.
struct SpinResponse {
    // Radians per second on top of whatever the cube was set to.
    double boost = 0.0;
    double largest = 0.0;
    // How often the limit below was reached. It should never be: the
    // bookkeeping that feeds this is meant to conserve momentum, and the
    // limit is there so a fault in it shows up as a count in the status
    // instead of a planet turning thousands of times a second.
    int limited = 0;

    static constexpr double settleSeconds = 3.0;
    static constexpr double limit = 3.0;

    void apply(const double impulse, const double inertia, const double dt)
    {
        if (std::isfinite(impulse)) {
            boost += impulse / std::max(inertia, 0.05);
        }
        boost *= std::exp(-std::max(dt, 0.0) / settleSeconds);
        if (!std::isfinite(boost)) {
            boost = 0.0;
            ++limited;
        } else if (std::fabs(boost) > limit) {
            boost = std::copysign(limit, boost);
            ++limited;
        }
        largest = std::max(largest, std::fabs(boost));
    }
};

// The rate the planet is turning at, for the simulation to feel.
//
// The step function sees the turn itself: the angle it was given and the one
// it returned. That is the rotation. The drawn angle is not: a reset, a
// property write or a snippet nudging the angle moves it without turning
// anything, and the step function returns its angle modulo 360, so a plain
// difference reads one tick of every revolution as a turn of nearly -360
// degrees. Either mistake hands the mob a rate of thousands of degrees a
// second for one frame, and the centrifugal term at that rate throws every
// one of them off the planet within it.
//
// So turns are unwrapped, the step function's record is used whenever it
// ran, and the drawn angle only when a binding made later stopped calling it.
// A rate far from the last one is believed only if it lasts: a real change of
// spin persists, a jump of the angle is one frame.
struct RateMeter {
    static constexpr float jump = 8.0F;      // radians per second in one frame
    static constexpr int confirmFrames = 3;

    double turnedDegrees = 0.0;
    double seconds = 0.0;
    float rate = 0.0F;
    bool seen = false;
    float pending = 0.0F;
    int pendingFrames = 0;
    std::uint64_t jumpsIgnored = 0;

    static double wrap(const double degrees) { return degrees - 360.0 * std::round(degrees / 360.0); }

    // From the step function, every tick.
    void stepped(const float inputDegrees, const float outputDegrees, const float dt)
    {
        turnedDegrees += wrap(static_cast<double>(outputDegrees) - static_cast<double>(inputDegrees));
        seconds += static_cast<double>(dt);
    }

    // Once per drawn frame: the drawn angle's change and the clock's step.
    // Returns the rate to simulate the frame at, in radians per second.
    float frame(const float drawnTurnDegrees, const float dt)
    {
        float measured = rate;
        if (seconds > 1e-6) {
            measured = static_cast<float>(turnedDegrees / seconds) * pi / 180.0F;
        } else if (dt > 1e-6F) {
            measured = static_cast<float>(wrap(drawnTurnDegrees)) * pi / 180.0F / dt;
        }
        turnedDegrees = 0.0;
        seconds = 0.0;
        if (!std::isfinite(measured)) {
            ++jumpsIgnored;
            return rate;
        }
        if (!seen) {
            seen = true;
            rate = measured;
            return rate;
        }
        if (std::fabs(measured - rate) > jump) {
            if (pendingFrames > 0 && std::fabs(measured - pending) < 1.0F) {
                ++pendingFrames;
            } else {
                pending = measured;
                pendingFrames = 1;
            }
            if (pendingFrames >= confirmFrames) {
                rate = measured;
                pendingFrames = 0;
            } else {
                ++jumpsIgnored;
            }
            return rate;
        }
        pendingFrames = 0;
        rate = measured;
        return rate;
    }
};

namespace sim_detail {

inline std::string quote(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

inline std::string number(const double value)
{
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.4g", value);
    return buffer;
}

inline float smoothstep(const float a, const float b, const float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

inline float approach(const float value, const float target, const float rate, const float dt)
{
    return value + (target - value) * (1.0F - std::exp(-rate * dt));
}

} // namespace sim_detail

// Distance along the surface, exact on one face and across one edge, and an
// estimate for opposite faces.
inline float surfaceDistance(const V3& a, const int fa, const V3& b, const int fb)
{
    if (fa == fb) {
        return length(b - a);
    }
    const V3 n = faceNormal(fa);
    const V3 nb = faceNormal(fb);
    if (std::fabs(dot(n, nb)) > 0.5F) {
        return 2.0F + length(flatten(a, n) - flatten(b, nb)) * 0.7F + 1.0F;
    }
    const float depthBelowEdge = 1.0F - dot(b, n);
    const V3 image = flatten(b, n) + n + nb * depthBelowEdge;
    return length(image - a);
}

class Mob {
public:
    Settings settings;
    cube_gravity::SpinningCube planet;
    std::vector<Kangaroo> roos;
    Water water;
    std::vector<Tuft> grass;
    std::vector<Tree> trees;
    Sign sign;
    std::vector<Footprint> footprints;
    std::vector<Particle> particles;
    std::vector<Ripple> ripples;
    std::vector<Event> events;

    double time = 0.0;
    // Axial angular impulse the mob has given back to the planet, waiting for
    // the step function to collect it.
    double owedImpulse = 0.0;
    double totalImpulse = 0.0;
    float alarmCooldown = 0.0F;
    float omegaSlow = 0.0F;
    bool omegaSeen = false;
    int flungTotal = 0;
    int returnedTotal = 0;
    int boutsTotal = 0;
    int alarmsTotal = 0;
    // Push-offs planned, and how many of them found no arc that clears the
    // ground and hopped on the spot instead. The second should be rare: when
    // it is not, every animal drifts down the local slope one hop at a time.
    int hopsPlanned = 0;
    int hopsOnTheSpot = 0;
    float largestSpeed = 0.0F;
    bool grassChanged = true;

    static constexpr float substep = 1.0F / 240.0F;
    static constexpr std::size_t maxFootprints = 64;
    static constexpr std::size_t maxParticles = 480;
    static constexpr std::size_t maxRipples = 16;

    void reset(const Settings& chosen, const V3& axle)
    {
        settings = chosen;
        settings.mob = std::clamp(settings.mob, 1, 12);
        planet = cube_gravity::SpinningCube{};
        planet.surfaceGravity = settings.surfaceGravity;
        const V3 unit = normalize(axle);
        planet.axle = toVec(unit);
        planet.omega = 0.0;
        planet.omegaRate = 0.0;
        roos.clear();
        footprints.clear();
        particles.clear();
        ripples.clear();
        events.clear();
        time = 0.0;
        owedImpulse = 0.0;
        totalImpulse = 0.0;
        omegaSeen = false;
        flungTotal = 0;
        returnedTotal = 0;
        hopsPlanned = 0;
        hopsOnTheSpot = 0;
        boutsTotal = 0;
        alarmsTotal = 0;
        largestSpeed = 0.0F;

        water.prepare(planet, settings.waterDepth);
        Rng rng(settings.seed);
        plantScenery(rng);
        populate(rng);
    }

    // Moves everything on by dt seconds of the planet's own clock. omega is
    // the rate the planet is turning at right now, in radians per second,
    // which the caller measures from the angle it actually drew.
    void advance(const float dt, const float omega)
    {
        if (dt <= 0.0F) {
            return;
        }
        // The rate is taken to change evenly across the frame, from what it was
        // at the last one to what it is now, and each substep is integrated at
        // its own share of that ramp. The Euler term is a real effect, not
        // noise to filter: when the spin changes, the ground turns under
        // anything in the air, and in this frame that is a shove. Its
        // integral over the frame is exactly the change.
        const double start = omegaSeen ? planet.omega : static_cast<double>(omega);
        planet.omegaRate = (omega - start) / dt;
        if (!omegaSeen) {
            omegaSlow = omega;
            omegaSeen = true;
        }
        noticeSpinChange(omega, dt);

        const int steps = std::clamp(static_cast<int>(std::ceil(dt / substep)), 1, 48);
        const float h = dt / static_cast<float>(steps);
        for (int s = 0; s < steps; ++s) {
            const double rateStart = start + planet.omegaRate * (static_cast<double>(s) * h);
            const double rateEnd = start + planet.omegaRate * (static_cast<double>(s + 1) * h);
            planet.omega = 0.5 * (rateStart + rateEnd);
            for (std::size_t i = 0; i < roos.size(); ++i) {
                stepKangaroo(static_cast<int>(i), h, rateStart, rateEnd);
            }
            time += h;
        }
        planet.omega = omega;
        for (std::size_t i = 0; i < roos.size(); ++i) {
            think(static_cast<int>(i), dt);
            animate(roos[i], dt);
        }
        separate(dt);
        water.update(omega, dt);
        ageEffects(dt);
        regrow(dt);
        alarmCooldown = std::max(0.0F, alarmCooldown - dt);
    }

    double takeImpulse()
    {
        const double owed = owedImpulse;
        owedImpulse = 0.0;
        return owed;
    }

    // ------------------------------------------------------------------
    // Requests from outside
    // ------------------------------------------------------------------

    bool raiseAlarm(const char* reason)
    {
        int by = -1;
        float best = -1.0F;
        for (std::size_t i = 0; i < roos.size(); ++i) {
            const Kangaroo& k = roos[i];
            if (k.present() && k.grounded() && k.adult() && k.activity != Activity::Flung
                && k.size > best) {
                best = k.size;
                by = static_cast<int>(i);
            }
        }
        if (by < 0) {
            return false;
        }
        alarmCooldown = 0.0F;
        startThump(by, reason);
        return true;
    }

    bool startBout()
    {
        int a = -1;
        int b = -1;
        float best = 1e9F;
        for (std::size_t i = 0; i < roos.size(); ++i) {
            for (std::size_t j = i + 1; j < roos.size(); ++j) {
                const Kangaroo& ki = roos[i];
                const Kangaroo& kj = roos[j];
                if (!ki.male() || !kj.male() || !ki.present() || !kj.present()
                    || ki.activity == Activity::Box || kj.activity == Activity::Box
                    || ki.activity == Activity::Flung || kj.activity == Activity::Flung) {
                    continue;
                }
                const float d = surfaceDistance(ki.p, ki.face, kj.p, kj.face);
                if (d < best) {
                    best = d;
                    a = static_cast<int>(i);
                    b = static_cast<int>(j);
                }
            }
        }
        if (a < 0) {
            return false;
        }
        beginBout(a, b);
        return true;
    }

    // Throws a kangaroo from where it stands with the given velocity in the
    // planet's frame. The planet takes the reaction like any push-off.
    bool launch(const int index, const V3& velocity)
    {
        if (index < 0 || index >= static_cast<int>(roos.size())) {
            return false;
        }
        Kangaroo& k = roos[index];
        if (!k.present() || !k.grounded() || !finite(velocity)) {
            return false;
        }
        const double before = axialMomentum(k, planet.omega);
        startActivity(k, Activity::Rest, 2.0F);
        k.move = Move::None;
        k.arrived = true;
        k.gait = Gait::Flight;
        k.gaitClock = 0.0F;
        k.takeoffFace = k.face;
        k.v = velocity;
        const float hold = std::max(grip(k.p, k.face), 0.5F);
        k.flightDuration = std::max(0.2F, 2.0F * dot(velocity, faceNormal(k.face)) / hold);
        const double after = axialMomentum(k, planet.omega);
        owedImpulse -= after - before;
        totalImpulse += std::fabs(after - before);
        return true;
    }

    // Puts a kangaroo back on the planet, whatever it was doing.
    void recall(Kangaroo& k)
    {
        if (k.activity == Activity::Away || k.activity == Activity::Flung) {
            k.awayClock = 1e3F;
            k.activity = Activity::Away;
        }
    }

    [[nodiscard]] double axialMomentumOf(const Kangaroo& k) const { return axialMomentum(k, planet.omega); }

private:
    // ------------------------------------------------------------------
    // Setting the scene
    // ------------------------------------------------------------------

    [[nodiscard]] V3 staticUp(const V3& p) const
    {
        const V3 a = fromVec(cube_gravity::attraction(toVec(p)));
        return normalize(-a);
    }

    void plantScenery(Rng& rng)
    {
        grass.clear();
        trees.clear();
        // How many tufts each face carries, by what the face is: the salt
        // lake on -z is nearly bare, the grassland on +y thick.
        const std::array<int, faceCount> tuftsPerFace{38, 52, 70, 34, 40, 14};
        for (int face = 0; face < faceCount; ++face) {
            V3 u;
            V3 v;
            faceAxes(face, u, v);
            const V3 n = faceNormal(face);
            int placed = 0;
            for (int attempt = 0; attempt < 600 && placed < tuftsPerFace[face]; ++attempt) {
                const V3 p = n + u * rng.range(-0.93F, 0.93F) + v * rng.range(-0.93F, 0.93F);
                if (water.depthAt(p, face) > 0.0F) {
                    continue;
                }
                bool crowded = false;
                for (const Tuft& other : grass) {
                    if (other.face == face && length(other.p - p) < 0.07F) {
                        crowded = true;
                        break;
                    }
                }
                if (crowded) {
                    continue;
                }
                grass.push_back(Tuft{p, face, 1.0F, rng.range(0.7F, 1.3F), rng.uniform(), staticUp(p)});
                ++placed;
            }
        }
        // Gum trees: the woodland face has a stand of them, the salt lake
        // none, the rest one or two, all clear of the water.
        const std::array<int, faceCount> treesPerFace{1, 3, 1, 2, 1, 0};
        for (int face = 0; face < faceCount; ++face) {
            V3 u;
            V3 v;
            faceAxes(face, u, v);
            const V3 n = faceNormal(face);
            for (int t = 0; t < treesPerFace[face]; ++t) {
                const float angle = rng.range(0.0F, 2.0F * pi);
                const float radius = rng.range(0.6F, 0.8F);
                const V3 p = n + u * (radius * std::cos(angle)) + v * (radius * std::sin(angle));
                trees.push_back(Tree{p, face, staticUp(p), rng.range(0.42F, 0.6F), rng.uniform()});
            }
        }
        // A road sign on the grassland, near the edge it faces.
        sign.face = 2;
        sign.p = V3{0.55F, 1.0F, 0.78F};
        sign.up = staticUp(sign.p);
        sign.facing = normalize(flatten(V3{0.35F, 0.0F, 1.0F}, sign.up));
    }

    void populate(Rng& rng)
    {
        struct Member {
            const char* name;
            Role role;
            float size;
            bool pouchJoey;
            int mother; // index into this table
        };
        static constexpr std::array<Member, 12> members{{
            {"Big Red", Role::Boomer, 0.37F, false, -1},
            {"Matilda", Role::Flyer, 0.30F, true, -1},
            {"Banjo", Role::Buck, 0.315F, false, -1},
            {"Wattle", Role::Flyer, 0.29F, false, -1},
            {"Dusty", Role::Joey, 0.16F, false, 3},
            {"Jarrah", Role::Buck, 0.32F, false, -1},
            {"Coolabah", Role::Flyer, 0.29F, true, -1},
            {"Bindi", Role::Flyer, 0.28F, false, -1},
            {"Mallee", Role::Buck, 0.30F, false, -1},
            {"Pip", Role::Joey, 0.15F, false, 7},
            {"Yarra", Role::Flyer, 0.285F, false, -1},
            {"Tully", Role::Buck, 0.305F, false, -1},
        }};
        // The mob starts together on the red desert face, which the camera
        // sees first.
        const int home = 4;
        V3 u;
        V3 v;
        faceAxes(home, u, v);
        const V3 n = faceNormal(home);
        for (int i = 0; i < settings.mob; ++i) {
            const Member& m = members[i];
            Kangaroo k;
            k.name = m.name;
            k.role = m.role;
            k.size = m.size;
            k.mass = (m.size / 0.3F) * (m.size / 0.3F) * (m.size / 0.3F);
            k.pouchJoey = m.pouchJoey;
            k.mother = (m.mother >= 0 && m.mother < settings.mob) ? m.mother : -1;
            if (m.role == Role::Joey && k.mother < 0) {
                // A joey whose mother is not in this mob stays home.
                continue;
            }
            k.rng = Rng(settings.seed * 131 + static_cast<std::uint64_t>(i) * 7919 + 17);
            k.tone = rng.range(-1.0F, 1.0F);
            const float angle = 2.0F * pi * static_cast<float>(i) / static_cast<float>(settings.mob)
                + rng.range(-0.3F, 0.3F);
            const float radius = rng.range(0.5F, 0.72F);
            k.p = onFace(n + u * (radius * std::cos(angle)) + v * (radius * std::sin(angle)), home);
            k.face = home;
            k.heading = normalize(u * -std::sin(angle) + v * std::cos(angle));
            k.up = n;
            k.goal = k.p;
            k.goalFace = home;
            k.activityDuration = k.rng.range(1.0F, 3.0F);
            k.pouchFull = k.pouchJoey ? 1.0F : 0.0F;
            roos.push_back(k);
        }
        // Mothers of a joey at foot are recorded by index after any joeys
        // without a mother were skipped, so remap.
        for (Kangaroo& k : roos) {
            if (k.role == Role::Joey) {
                const std::string motherName = members[k.mother].name;
                k.mother = -1;
                for (std::size_t j = 0; j < roos.size(); ++j) {
                    if (roos[j].name == motherName) {
                        k.mother = static_cast<int>(j);
                    }
                }
                if (k.mother >= 0) {
                    k.activity = Activity::Follow;
                    const Kangaroo& mum = roos[k.mother];
                    k.p = walk(SurfacePoint{mum.p, -mum.heading, mum.face}, 0.18F).p;
                    k.face = faceOf(k.p);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Forces, frames and momentum
    // ------------------------------------------------------------------

    [[nodiscard]] V3 effectiveGravity(const V3& p) const { return fromVec(planet.effectiveGravity(toVec(p))); }

    [[nodiscard]] V3 acceleration(const V3& p, const V3& v) const
    {
        return fromVec(planet.acceleration(toVec(p), toVec(v)));
    }

    // How hard the ground at p holds things down: the effective gravity's
    // component into the face.
    [[nodiscard]] float grip(const V3& p, const int face) const
    {
        return -dot(effectiveGravity(p), faceNormal(face));
    }

    // Axial angular momentum in the inertial frame, m (r x (v + w x r)) . axle,
    // for a velocity v measured in a frame turning at `rate`.
    //
    // The rate has to be the one v was measured against. Evaluated at any
    // other, the Euler shove a changing spin gives an airborne body looks like
    // momentum it gained, the planet is billed the opposite, and since that
    // bill speeds a planet up when it is already speeding up, the loop runs
    // away. It did: 67,000 degrees a second, with the mob flung into space.
    [[nodiscard]] double axialMomentum(const Kangaroo& k, const double rate) const
    {
        const V3 axle = fromVec(planet.axle);
        const V3 w = axle * static_cast<float>(rate);
        const V3 inertial = k.v + cross(w, k.p);
        return static_cast<double>(k.mass) * dot(cross(k.p, inertial), axle);
    }

    // ------------------------------------------------------------------
    // One substep of one kangaroo's body
    // ------------------------------------------------------------------

    // rateStart and rateEnd are the planet's spin at the two ends of the
    // substep.
    void stepKangaroo(const int index, const float h, const double rateStart, const double rateEnd)
    {
        Kangaroo& k = roos[index];
        if (!k.present()) {
            if (k.activity == Activity::Pouched && k.mother >= 0) {
                const Kangaroo& mum = roos[k.mother];
                k.p = mum.p;
                k.face = mum.face;
                k.v = mum.v;
            }
            return;
        }
        k.teleported = false;
        // An animal on the ground is carried round by the planet: that part of
        // its momentum is the planet's own and the motor's business, so it is
        // measured at the substep's closing rate at both ends and a change of
        // spin bills nothing. One in the air is free of the planet, and its
        // momentum is measured at the rate its velocity was taken against at
        // each end, which is what makes the Euler shove cancel.
        const bool airborneAtStart = k.gait == Gait::Flight;
        const double before = axialMomentum(k, airborneAtStart ? rateStart : rateEnd);

        switch (k.gait) {
        case Gait::Flight:
            fly(k, h);
            break;
        case Gait::Stance:
            k.gaitClock += h;
            k.v = V3{};
            if (k.gaitClock >= k.stanceDuration) {
                if (k.move == Move::Hop && !k.arrived) {
                    takeOff(k);
                } else {
                    k.gait = Gait::Stand;
                    k.gaitClock = 0.0F;
                }
            }
            break;
        case Gait::Walk:
            walkStep(k, h);
            break;
        case Gait::Stand:
            k.v = V3{};
            k.gaitClock += h;
            if (k.move == Move::Hop && !k.arrived && k.stagger <= 0.0F) {
                // Gather for the first hop: a short crouch before push-off.
                k.gait = Gait::Stance;
                k.gaitClock = 0.0F;
                k.stanceDuration = 0.12F;
                k.landingSpeed = 0.6F;
            } else if (k.move == Move::Walk && !k.arrived) {
                k.gait = Gait::Walk;
                k.gaitClock = 0.0F;
            }
            break;
        }

        // Ground that no longer holds: the spin has made the effective gravity
        // point out of the face here, and the animal lifts off it.
        if (k.grounded() && grip(k.p, k.face) <= 0.0F) {
            k.gait = Gait::Flight;
            k.gaitClock = 0.0F;
            k.flightDuration = 1e3F;
            k.takeoffFace = k.face;
            k.v = faceNormal(k.face) * 0.05F;
        }

        if (!k.teleported) {
            const double after = axialMomentum(k, rateEnd);
            owedImpulse -= after - before;
            totalImpulse += std::fabs(after - before);
        }
    }

    void fly(Kangaroo& k, const float h)
    {
        k.gaitClock += h;
        const V3 p0 = k.p;
        // Midpoint integration: Coriolis depends on velocity, so the second
        // evaluation sees the velocity it acts on.
        const V3 a1 = acceleration(k.p, k.v);
        const V3 vHalf = k.v + a1 * (0.5F * h);
        const V3 pMid = k.p + k.v * (0.5F * h);
        const V3 a2 = acceleration(pMid, vHalf);
        k.p = k.p + vHalf * h;
        k.v = k.v + a2 * h;

        if (!finite(k.p) || !finite(k.v)) {
            k.p = toSurface(p0);
            k.v = V3{};
            land(k, faceOf(k.p));
            return;
        }

        // Thrown well clear of anything a hop reaches.
        const float altitude = boxNorm(k.p) - 1.0F;
        if (k.activity != Activity::Flung && altitude > 4.0F * k.size
            && dot(k.v, k.p) > 0.0F) {
            fling(k);
        }
        if (k.activity == Activity::Flung) {
            k.tumbleAngle += k.tumbleRate * h;
            if (length(k.p) > 7.0F || k.gaitClock > 9.0F) {
                k.activity = Activity::Away;
                k.awayClock = 0.0F;
                k.teleported = true;
                return;
            }
        }

        if (boxNorm(k.p) < 1.0F) {
            // Find where the path crossed the surface, by bisection along the
            // substep's segment.
            float lo = 0.0F;
            float hi = 1.0F;
            for (int i = 0; i < 18; ++i) {
                const float mid = 0.5F * (lo + hi);
                if (boxNorm(lerp(p0, k.p, mid)) >= 1.0F) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            const V3 contact = lerp(p0, k.p, lo);
            const int face = faceOf(contact);
            const V3 n = faceNormal(face);
            if (grip(contact, face) < 0.5F && dot(k.v, n) < 0.0F) {
                // Ground that cannot hold a body down gives it nothing to
                // grip, so a landing there is a bounce: most of the speed
                // into the face is lost and the speed along it is kept.
                // Landing as if on firm ground would stop all of that
                // sideways speed against the planet at once, which is a shove
                // the planet would feel and the ground could not give.
                k.p = onFace(contact, face) + n * 1e-4F;
                k.v = flatten(k.v, n) - n * (0.3F * dot(k.v, n));
                return;
            }
            k.p = onFace(contact, face);
            land(k, face);
        }
    }

    void land(Kangaroo& k, const int face)
    {
        const V3 n = faceNormal(face);
        k.landingSpeed = std::max(0.0F, -dot(k.v, n));
        const bool returning = k.activity == Activity::Flung;
        if (face != k.face) {
            k.heading = foldDirection(k.heading, k.face, face);
        }
        k.face = face;
        k.heading = normalize(flatten(k.heading, n), V3{n.y, n.z, n.x});
        const float slip = length(flatten(k.v, n));
        k.v = V3{};
        k.gait = Gait::Stance;
        k.gaitClock = 0.0F;
        // A harder landing compresses for longer, as a spring would.
        k.stanceDuration = std::clamp(0.075F + 0.03F * k.landingSpeed, 0.08F, 0.16F) * std::sqrt(k.size / 0.3F);
        ++k.hops;

        const float wet = water.depthAt(k.p, face);
        const V3 right = cross(k.heading, n);
        if (wet > 0.004F) {
            splash(k, wet);
        } else {
            dust(k.p, n, face, 4 + static_cast<int>(3.0F * std::min(k.landingSpeed, 3.0F)), k.size, 0);
            printFeet(k, right);
        }
        if (returning) {
            // It was put back high above the planet with whatever momentum
            // that position gave it, which is a choice made here and not
            // anything the planet did. Billing the planet for catching it
            // would hand it free spin with every return, and returns follow
            // flings, which follow spin.
            k.teleported = true;
            k.activity = Activity::Rest;
            k.activityClock = 0.0F;
            k.activityDuration = 2.5F;
            k.move = Move::None;
            k.arrived = true;
            ++returnedTotal;
            publish("kangaroo.returned",
                 "{\"name\":" + sim_detail::quote(k.name) + ",\"face\":" + std::to_string(face)
                     + ",\"landingSpeed\":" + sim_detail::number(k.landingSpeed) + "}");
            dust(k.p, n, face, 18, k.size * 1.6F, 0);
        }
        if (settings.hopEvents) {
            publish("kangaroo.hop",
                 "{\"name\":" + sim_detail::quote(k.name) + ",\"face\":" + std::to_string(face)
                     + ",\"slip\":" + sim_detail::number(slip)
                     + ",\"landingSpeed\":" + sim_detail::number(k.landingSpeed)
                     + ",\"wet\":" + (wet > 0.004F ? "true" : "false") + "}");
        }
    }

    void fling(Kangaroo& k)
    {
        k.activity = Activity::Flung;
        k.move = Move::None;
        k.tumbleAxis = normalize(V3{k.rng.range(-1.0F, 1.0F), k.rng.range(-1.0F, 1.0F),
                                    k.rng.range(-1.0F, 1.0F)},
                                 V3{0.0F, 0.0F, 1.0F});
        k.tumbleRate = k.rng.range(4.0F, 9.0F);
        ++k.flungCount;
        ++flungTotal;
        const float speed = length(k.v);
        largestSpeed = std::max(largestSpeed, speed);
        publish("kangaroo.flung",
             "{\"name\":" + sim_detail::quote(k.name)
                 + ",\"omegaDegreesPerSecond\":" + sim_detail::number(planet.omega * 180.0 / cube_gravity::pi)
                 + ",\"speed\":" + sim_detail::number(speed)
                 + ",\"fromFace\":" + std::to_string(k.takeoffFace) + "}");
        if (alarmCooldown <= 0.0F) {
            raiseAlarm("a kangaroo was flung off the planet");
        }
    }

    // Choosing the push-off for the next hop: where to land, how high to go,
    // and the velocity that gets there under the local forces.
    void takeOff(Kangaroo& k)
    {
        const V3 n = faceNormal(k.face);
        V3 dir = surfaceDirection(k.p, k.face, k.goal, k.goalFace);
        if (length(dir) < 0.5F) {
            dir = k.heading;
        }
        dir = normalize(flatten(dir + avoidance(k) * 0.8F, n), k.heading);
        // A hop turns the body by at most about 70 degrees.
        const float turn = std::acos(std::clamp(dot(dir, k.heading), -1.0F, 1.0F));
        if (turn > 1.2F) {
            const V3 axis = normalize(cross(k.heading, dir), n);
            dir = normalize(rotate(k.heading, axis, 1.2F), dir);
        }
        k.heading = dir;

        const float remaining = surfaceDistance(k.p, k.face, k.goal, k.goalFace);
        const float wet = water.depthAt(k.p, k.face);
        const float speed = k.speed * (wet > 0.01F ? 0.65F : 1.0F);
        const float cycle = 0.36F * std::sqrt(k.size / 0.3F);
        float stride = std::clamp(speed * cycle, 0.03F, 0.9F);
        if (k.activity != Activity::Flee) {
            stride = std::min(stride, std::max(remaining, 0.03F));
        }
        // An edge is crossed with a short hop from close to it, the way an
        // animal takes a ridge. A long hop across one would have to clear the
        // corner between its take-off and its landing, and the arc that does
        // that is a launch, made where gravity is at its weakest.
        const float toEdge = distanceToEdge(k.p, dir, k.face);
        const float nearEdge = 0.1F * k.size / 0.3F;
        if (stride > toEdge) {
            stride = toEdge > nearEdge ? toEdge - 0.5F * nearEdge : std::min(stride, toEdge + nearEdge);
        }
        stride = std::max(stride, 0.02F);
        const SurfacePoint landing = walk(SurfacePoint{k.p, dir, k.face}, stride);

        const float hold = std::max(grip(k.p, k.face), 0.4F);
        const float baseApex = k.size * (0.2F + 0.16F * std::min(speed, 1.6F));
        float apex = baseApex;
        V3 v0;
        float flight = 0.2F;
        bool cleared = false;
        for (int attempt = 0; attempt < 4 && !cleared; ++attempt) {
            flight = 2.0F * std::sqrt(2.0F * apex / hold);
            V3 planned;
            v0 = aim(k.p, landing.p, flight, n, apex, planned);
            cleared = clears(k.p, v0, planned, flight);
            apex *= 1.4F;
        }
        // Nothing reasonable clears, or the hop would be a throw: hop on the
        // spot instead and let the next one try again.
        ++hopsPlanned;
        if (!cleared || length(v0) > 2.2F + 1.2F * speed) {
            flight = 2.0F * std::sqrt(2.0F * baseApex / hold);
            v0 = n * (0.5F * hold * flight);
            stride = 0.0F;
            ++hopsOnTheSpot;
        }
        k.flightDuration = flight;
        k.v = v0;
        k.gait = Gait::Flight;
        k.gaitClock = 0.0F;
        k.takeoffFace = k.face;
        k.distance += stride;
        dust(k.p, n, k.face, 2, k.size * 0.7F, 0);
    }

    // The velocity that reaches q from p in time t under constant forces,
    // evaluated at the middle of the arc, and the acceleration that was
    // assumed. Coriolis depends on that velocity, so it is iterated a few
    // times.
    [[nodiscard]] V3 aim(const V3& p, const V3& q, const float t, const V3& n, const float apex, V3& planned) const
    {
        V3 v0 = (q - p) / t;
        const V3 middle = (p + q) * 0.5F + n * apex;
        planned = acceleration(middle, v0);
        for (int i = 0; i < 4; ++i) {
            planned = acceleration(middle, v0 + acceleration(middle, v0) * (0.5F * t));
            v0 = (q - p) / t - planned * (0.5F * t);
        }
        return v0;
    }

    // Whether the planned arc stays outside the cube between take-off and
    // landing. A hop across an edge must go over the corner, not through the
    // planet.
    //
    // The arc checked has to be the one aim() solved for, under the same
    // acceleration. Checked under any other it ends somewhere else: with the
    // stronger gravity at the surface it ends just under the ground, every
    // ordinary hop fails, and the vertical hop it falls back to lands a
    // little toward the face centre each time, which is where the whole mob
    // ended up standing on top of itself.
    [[nodiscard]] bool clears(const V3& p, const V3& v0, const V3& a, const float t) const
    {
        for (int i = 1; i < 12; ++i) {
            const float s = t * static_cast<float>(i) / 12.0F;
            const V3 point = p + v0 * s + a * (0.5F * s * s);
            if (boxNorm(point) < 1.0F + 0.003F) {
                return false;
            }
        }
        return true;
    }

    // The slow gait: forepaws and tail planted, hind legs swung forward
    // together, which is how a grazing kangaroo moves. It never leaves the
    // ground, and lurches: almost all the progress is made while the hind
    // legs swing.
    void walkStep(Kangaroo& k, const float h)
    {
        // Walking is only ever on the way somewhere. An activity that changed
        // underneath it (a hunker-down, a bout ending) leaves no reason to,
        // and carrying on would walk at whatever speed the last activity set,
        // which for a flight from danger is a sprint.
        if (k.move != Move::Walk || k.arrived) {
            k.gait = Gait::Stand;
            k.gaitClock = 0.0F;
            k.v = V3{};
            return;
        }
        const float cycle = 0.85F * std::sqrt(k.size / 0.3F);
        k.walkPhase += h / cycle;
        if (k.walkPhase >= 1.0F) {
            k.walkPhase -= 1.0F;
            const V3 n = faceNormal(k.face);
            const V3 right = cross(k.heading, n);
            printFeet(k, right);
            addFootprint(k.p - k.heading * (k.size * 0.75F), k.heading, k.face, k.size, 1);
        }
        const float surge = std::max(0.0F, std::sin(pi * std::clamp((k.walkPhase - 0.3F) / 0.4F, 0.0F, 1.0F)));
        const float pace = k.speed * 2.2F * surge;

        V3 dir = surfaceDirection(k.p, k.face, k.goal, k.goalFace);
        if (length(dir) < 0.5F) {
            dir = k.heading;
        }
        const V3 n = faceNormal(k.face);
        dir = normalize(flatten(dir + avoidance(k) * 0.5F, n), k.heading);
        // Turn gradually.
        const float turnRate = 2.5F * h;
        const float turn = std::acos(std::clamp(dot(dir, k.heading), -1.0F, 1.0F));
        if (turn > turnRate) {
            const V3 axis = normalize(cross(k.heading, dir), n);
            dir = normalize(rotate(k.heading, axis, turnRate), dir);
        }
        k.heading = dir;
        const SurfacePoint next = walk(SurfacePoint{k.p, k.heading, k.face}, pace * h);
        k.v = (next.face == k.face) ? (next.p - k.p) / h : next.heading * pace;
        k.p = next.p;
        k.heading = next.heading;
        k.face = next.face;
        k.distance += pace * h;
        if (surfaceDistance(k.p, k.face, k.goal, k.goalFace) < 0.025F) {
            k.arrived = true;
            k.v = V3{};
            k.gait = Gait::Stand;
        }
    }

    // A push away from neighbours, so a mob does not stack on one spot.
    [[nodiscard]] V3 avoidance(const Kangaroo& k) const
    {
        V3 push{};
        for (const Kangaroo& other : roos) {
            if (&other == &k || !other.present() || other.face != k.face) {
                continue;
            }
            if (k.activity == Activity::Box && &other == &roos[std::max(k.partner, 0)]) {
                continue;
            }
            if (k.role == Role::Joey && &other == &roos[std::max(k.mother, 0)]) {
                continue;
            }
            const V3 away = k.p - other.p;
            const float d = length(away);
            const float reach = 0.55F * (k.size + other.size);
            if (d < reach && d > 1e-4F) {
                push += away * ((reach - d) / (reach * d));
            }
        }
        return push;
    }

    void separate(const float dt)
    {
        for (std::size_t i = 0; i < roos.size(); ++i) {
            Kangaroo& a = roos[i];
            if (!a.present() || a.gait != Gait::Stand) {
                continue;
            }
            for (std::size_t j = 0; j < roos.size(); ++j) {
                const Kangaroo& b = roos[j];
                if (i == j || !b.present() || b.face != a.face || !b.grounded()) {
                    continue;
                }
                if (a.activity == Activity::Box && a.partner == static_cast<int>(j)) {
                    continue;
                }
                // A joey keeps close to its mother but not inside her, except
                // while it is diving for the pouch.
                const bool family = (a.role == Role::Joey && a.mother == static_cast<int>(j))
                    || (b.role == Role::Joey && b.mother == static_cast<int>(i));
                if (family && (a.calmClock < 0.0F || b.calmClock < 0.0F)) {
                    continue;
                }
                const V3 away = a.p - b.p;
                const float d = length(away);
                const float reach = (family ? 0.2F : 0.32F) * (a.size + b.size);
                if (d < reach && d > 1e-4F) {
                    const V3 n = faceNormal(a.face);
                    const V3 shove = normalize(flatten(away, n)) * ((reach - d) * std::min(1.0F, 3.0F * dt));
                    const SurfacePoint moved = walk(SurfacePoint{a.p, shove, a.face}, length(shove));
                    a.p = moved.p;
                    if (moved.face != a.face) {
                        a.heading = foldDirection(a.heading, a.face, moved.face);
                        a.face = moved.face;
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Effects left behind
    // ------------------------------------------------------------------

    void addFootprint(const V3& p, const V3& heading, const int face, const float size, const int kind)
    {
        if (water.depthAt(p, face) > 0.002F || boxNorm(p) > 1.001F) {
            return;
        }
        if (footprints.size() >= maxFootprints) {
            footprints.erase(footprints.begin());
        }
        footprints.push_back(Footprint{toSurface(p), heading, face, 0.0F, size, kind});
    }

    void printFeet(const Kangaroo& k, const V3& right)
    {
        const float spread = 0.085F * k.size * 0.85F;
        const V3 heel = k.p - k.heading * (0.06F * k.size);
        addFootprint(heel + right * spread, k.heading, k.face, k.size, 0);
        addFootprint(heel - right * spread, k.heading, k.face, k.size, 0);
    }

    void dust(const V3& p, const V3& n, const int face, const int count, const float scale, const int kind)
    {
        Rng& rng = roos.empty() ? fallbackRng : roos[0].rng;
        const V3 g = effectiveGravity(p) * 0.06F;
        V3 u;
        V3 v;
        faceAxes(face, u, v);
        for (int i = 0; i < count && particles.size() < maxParticles; ++i) {
            const V3 out = rng.inDisc(u, v, 1.0F);
            Particle particle;
            particle.p = p + n * 0.01F + out * (0.03F * scale);
            particle.v = out * (0.5F * scale) + n * rng.range(0.15F, 0.6F) * scale;
            particle.gravity = g;
            particle.face = face;
            particle.life = rng.range(0.6F, 1.1F);
            particle.size = rng.range(0.035F, 0.06F) * scale / 0.3F;
            particle.kind = kind;
            particles.push_back(particle);
        }
    }

    void splash(Kangaroo& k, const float wet)
    {
        const V3 n = faceNormal(k.face);
        const V3 g = effectiveGravity(k.p);
        V3 u;
        V3 v;
        faceAxes(k.face, u, v);
        const int count = 10 + static_cast<int>(10.0F * std::min(k.landingSpeed, 2.0F));
        for (int i = 0; i < count && particles.size() < maxParticles; ++i) {
            const V3 out = k.rng.inDisc(u, v, 1.0F);
            Particle drop;
            drop.p = k.p + n * (wet + 0.005F) + out * (0.04F * k.size / 0.3F);
            drop.v = out * k.rng.range(0.3F, 0.9F) + n * k.rng.range(0.8F, 1.8F) * std::sqrt(k.size / 0.3F);
            // Water drops fall under the real local gravity, so a splash on a
            // fast-spinning planet hangs and drifts.
            drop.gravity = g;
            drop.face = k.face;
            drop.life = 0.9F;
            drop.size = k.rng.range(0.012F, 0.022F);
            drop.kind = 1;
            particles.push_back(drop);
        }
        addRipple(k.p, k.face, 1.0F);
    }

    void addRipple(const V3& p, const int face, const float strength)
    {
        if (ripples.size() >= maxRipples) {
            ripples.erase(ripples.begin());
        }
        ripples.push_back(Ripple{p, face, 0.0F, strength});
    }

    void ageEffects(const float dt)
    {
        for (Footprint& f : footprints) {
            f.age += dt;
        }
        footprints.erase(std::remove_if(footprints.begin(), footprints.end(),
                                        [](const Footprint& f) { return f.age > 14.0F; }),
                         footprints.end());
        for (Particle& particle : particles) {
            particle.age += dt;
            particle.v += particle.gravity * dt;
            if (particle.kind == 0) {
                particle.v *= std::exp(-3.0F * dt);
            }
            particle.p += particle.v * dt;
            if (particle.kind == 1 && boxNorm(particle.p) < 1.0F) {
                particle.age = particle.life;
            }
        }
        particles.erase(std::remove_if(particles.begin(), particles.end(),
                                       [](const Particle& p) { return p.age >= p.life; }),
                        particles.end());
        for (Ripple& r : ripples) {
            r.age += dt;
        }
        ripples.erase(std::remove_if(ripples.begin(), ripples.end(),
                                     [](const Ripple& r) { return r.age > 2.5F; }),
                      ripples.end());
    }

    void regrow(const float dt)
    {
        for (Tuft& t : grass) {
            if (t.biomass < 1.0F) {
                t.biomass = std::min(1.0F, t.biomass + 0.025F * dt);
            }
        }
    }

    void publish(const std::string& name, const std::string& json)
    {
        if (events.size() < 256) {
            events.push_back(Event{name, json});
        }
    }

    // ------------------------------------------------------------------
    // Deciding what to do
    // ------------------------------------------------------------------

    void noticeSpinChange(const float omega, const float dt)
    {
        // A sudden change in spin startles the mob. The slow average follows
        // over a couple of seconds, so a jump stands out against it.
        const float jump = std::fabs(omega - omegaSlow);
        omegaSlow += (omega - omegaSlow) * (1.0F - std::exp(-dt / 1.5F));
        if (jump > 0.3F && alarmCooldown <= 0.0F) {
            raiseAlarm("the ground lurched");
        }
    }

    void setGoal(Kangaroo& k, const V3& goal, const int goalFace, const Move move, const float speed)
    {
        k.goal = onFace(goal, goalFace);
        k.goalFace = goalFace;
        k.move = move;
        k.speed = speed;
        k.arrived = surfaceDistance(k.p, k.face, k.goal, k.goalFace) < (move == Move::Hop ? 0.05F : 0.025F);
    }

    void startActivity(Kangaroo& k, const Activity activity, const float duration)
    {
        k.activity = activity;
        k.activityClock = 0.0F;
        k.activityDuration = duration;
        k.hasFaceToward = false;
        k.tuft = -1;
        if (activity != Activity::Box) {
            k.partner = -1;
        }
    }

    // A point on k's face some distance away in a random direction, staying
    // inside the face.
    V3 nearbyPoint(Kangaroo& k, const float lo, const float hi)
    {
        V3 u;
        V3 v;
        faceAxes(k.face, u, v);
        const float angle = k.rng.range(0.0F, 2.0F * pi);
        const float r = k.rng.range(lo, hi);
        V3 q = k.p + u * (r * std::cos(angle)) + v * (r * std::sin(angle));
        // Pull back toward the face centre if it would leave the face.
        const V3 n = faceNormal(k.face);
        V3 inPlane = flatten(q, n);
        const float limit = 0.9F;
        for (int axis = 0; axis < 3; ++axis) {
            inPlane[axis] = std::clamp(inPlane[axis], -limit, limit);
        }
        return n + inPlane;
    }

    void think(const int index, const float dt)
    {
        Kangaroo& k = roos[index];
        k.activityClock += dt;
        k.stagger = std::max(0.0F, k.stagger - dt);

        if (k.activity == Activity::Away) {
            k.awayClock += dt;
            if (k.awayClock > 3.0F) {
                bringBack(k);
            }
            return;
        }
        if (k.activity == Activity::Pouched) {
            thinkPouched(index, dt);
            return;
        }
        if (k.activity == Activity::Flung) {
            return;
        }
        if (!k.grounded()) {
            return;
        }

        // A kangaroo standing where the ground barely holds hunkers down
        // instead of hopping, which keeps it on a planet spun close to its
        // limit: a hop there would be a launch.
        const float hold = grip(k.p, k.face);
        if (hold < 0.35F * settings.surfaceGravity && k.activity != Activity::Box) {
            if (k.activity != Activity::Rest || k.move != Move::None) {
                startActivity(k, Activity::Rest, 1.5F);
                k.move = Move::None;
                k.arrived = true;
            }
            return;
        }

        switch (k.activity) {
        case Activity::Rest:
        case Activity::Groom:
            if (k.activityClock > k.activityDuration) {
                choose(index);
            }
            break;
        case Activity::Graze:
            thinkGraze(k, dt);
            break;
        case Activity::Travel:
            if (k.arrived || k.activityClock > k.activityDuration) {
                choose(index);
            }
            break;
        case Activity::Drink:
            thinkDrink(k, dt);
            break;
        case Activity::Box:
            thinkBox(index, dt);
            break;
        case Activity::Thump:
            thinkThump(index, dt);
            break;
        case Activity::Flee:
            if (k.activityClock > k.activityDuration) {
                startActivity(k, Activity::Rest, k.rng.range(1.5F, 3.0F));
                k.move = Move::None;
                k.arrived = true;
            } else if (k.arrived) {
                // Keep running the same way.
                const SurfacePoint ahead = walk(SurfacePoint{k.p, k.heading, k.face}, 0.8F);
                setGoal(k, ahead.p, ahead.face, Move::Hop, k.speed);
            }
            break;
        case Activity::Follow:
            thinkFollow(index, dt);
            break;
        default:
            break;
        }

        // Males that meet sometimes spar.
        if (k.male() && k.activity != Activity::Box && k.activity != Activity::Flee
            && k.activity != Activity::Thump && k.gait == Gait::Stand) {
            for (std::size_t j = 0; j < roos.size(); ++j) {
                Kangaroo& other = roos[j];
                if (static_cast<int>(j) == index || !other.male() || !other.present()
                    || other.activity == Activity::Box || other.activity == Activity::Flee
                    || other.activity == Activity::Flung || !other.grounded()) {
                    continue;
                }
                if (surfaceDistance(k.p, k.face, other.p, other.face) < 0.75F
                    && k.rng.chance(0.06F * dt)) {
                    beginBout(index, static_cast<int>(j));
                    break;
                }
            }
        }
    }

    void choose(const int index)
    {
        Kangaroo& k = roos[index];
        if (k.role == Role::Joey && k.mother >= 0) {
            startActivity(k, Activity::Follow, 4.0F);
            return;
        }
        // The boomer leads; others drift back toward him when they wander.
        const Kangaroo* leader = nullptr;
        for (const Kangaroo& other : roos) {
            if (other.role == Role::Boomer && other.present() && &other != &k) {
                leader = &other;
            }
        }
        const float roll = k.rng.uniform();
        if (leader != nullptr && surfaceDistance(k.p, k.face, leader->p, leader->face) > 1.1F
            && roll < 0.5F) {
            startActivity(k, Activity::Travel, 6.0F);
            const SurfacePoint near = walk(SurfacePoint{leader->p, k.heading, leader->face}, 0.25F);
            setGoal(k, near.p, near.face, Move::Hop, k.rng.range(0.6F, 0.9F));
            return;
        }
        if (roll < 0.36F) {
            startActivity(k, Activity::Graze, k.rng.range(5.0F, 10.0F));
            k.move = Move::None;
            k.arrived = true;
        } else if (roll < 0.56F) {
            startActivity(k, Activity::Travel, 5.0F);
            const V3 goal = nearbyPoint(k, 0.4F, 0.9F);
            setGoal(k, goal, k.face, Move::Hop, k.rng.range(0.45F, 0.9F));
        } else if (roll < 0.68F) {
            // Over an edge to a neighbouring face.
            startActivity(k, Activity::Travel, 7.0F);
            const SurfacePoint far = walk(SurfacePoint{k.p, k.heading, k.face}, k.rng.range(1.0F, 1.6F));
            setGoal(k, far.p, far.face, Move::Hop, k.rng.range(0.7F, 1.0F));
        } else if (roll < 0.8F) {
            startDrink(k);
        } else if (roll < 0.9F) {
            startActivity(k, Activity::Groom, k.rng.range(2.0F, 4.0F));
            k.move = Move::None;
            k.arrived = true;
        } else {
            // Rest, in a tree's shade if one is near.
            startActivity(k, Activity::Rest, k.rng.range(2.5F, 5.0F));
            k.move = Move::None;
            k.arrived = true;
            for (const Tree& tree : trees) {
                if (tree.face == k.face && length(tree.p - k.p) < 0.9F) {
                    const SurfacePoint shade = walk(SurfacePoint{tree.p, k.heading, tree.face}, 0.12F);
                    setGoal(k, shade.p, shade.face, Move::Hop, 0.5F);
                    break;
                }
            }
        }
    }

    void thinkGraze(Kangaroo& k, const float dt)
    {
        if (k.activityClock > k.activityDuration) {
            choose(static_cast<int>(&k - roos.data()));
            return;
        }
        if (k.tuft < 0) {
            // The nearest tuft on this face with something left on it.
            float best = 0.7F;
            for (std::size_t i = 0; i < grass.size(); ++i) {
                const Tuft& t = grass[i];
                if (t.face != k.face || t.biomass < 0.3F) {
                    continue;
                }
                const float d = length(t.p - k.p);
                bool taken = false;
                for (const Kangaroo& other : roos) {
                    if (&other != &k && other.tuft == static_cast<int>(i)) {
                        taken = true;
                    }
                }
                if (!taken && d < best) {
                    best = d;
                    k.tuft = static_cast<int>(i);
                }
            }
            if (k.tuft < 0) {
                // Nothing in reach: wander a little and look again.
                const V3 goal = nearbyPoint(k, 0.15F, 0.3F);
                setGoal(k, goal, k.face, Move::Walk, 0.09F);
                k.tuft = -2;
                return;
            }
            const Tuft& t = grass[k.tuft];
            // Stand just short of it, head over it.
            const V3 approach = normalize(flatten(t.p - k.p, faceNormal(k.face)), k.heading);
            setGoal(k, t.p - approach * (0.45F * k.size), t.face, Move::Walk, 0.09F);
            return;
        }
        if (k.tuft == -2) {
            if (k.arrived) {
                k.tuft = -1;
            }
            return;
        }
        Tuft& t = grass[k.tuft];
        if (k.arrived) {
            k.hasFaceToward = true;
            k.faceToward = t.p;
            const float bite = std::min(t.biomass, 0.22F * dt);
            t.biomass -= bite;
            k.grazedBiomass += bite;
            grassChanged = true;
            if (t.biomass < 0.12F) {
                k.tuft = -1;
            }
        }
    }

    void startDrink(Kangaroo& k)
    {
        V3 shore;
        V3 toward;
        int face = k.face;
        bool found = water.shoreNear(k.p, k.face, shore, toward);
        if (!found) {
            // Try the neighbours.
            for (int f = 0; f < faceCount && !found; ++f) {
                if (f != k.face && std::fabs(dot(faceNormal(f), faceNormal(k.face))) < 0.5F) {
                    found = water.shoreNear(k.p, f, shore, toward);
                    face = f;
                }
            }
        }
        if (!found) {
            startActivity(k, Activity::Graze, 4.0F);
            k.move = Move::None;
            k.arrived = true;
            return;
        }
        startActivity(k, Activity::Drink, 14.0F);
        // Stand back from the shore line by about a head's length.
        const V3 standAt = shore - toward * (0.3F * k.size);
        const bool far = surfaceDistance(k.p, k.face, standAt, face) > 0.35F;
        setGoal(k, standAt, face, far ? Move::Hop : Move::Walk, far ? 0.6F : 0.1F);
        k.hasFaceToward = true;
        k.faceToward = shore + toward * 0.2F;
        k.activityDuration = 14.0F;
        k.tuft = -1;
    }

    void thinkDrink(Kangaroo& k, const float dt)
    {
        if (!k.arrived) {
            if (k.activityClock > k.activityDuration) {
                choose(static_cast<int>(&k - roos.data()));
            }
            return;
        }
        if (k.move != Move::None) {
            // Just arrived: drink for a while from here.
            k.move = Move::None;
            k.activityClock = 0.0F;
            k.activityDuration = k.rng.range(3.0F, 6.0F);
            publish("kangaroo.drink", "{\"name\":" + sim_detail::quote(k.name)
                                       + ",\"face\":" + std::to_string(k.face) + "}");
        }
        k.drankSeconds += dt;
        if (k.rng.chance(1.6F * dt)) {
            const Rig rig = buildRig(k.pose);
            const V3 snout = toPlanet(k, rig.snout);
            addRipple(toSurface(snout), faceOf(snout), 0.35F);
        }
        if (k.activityClock > k.activityDuration) {
            choose(static_cast<int>(&k - roos.data()));
        }
    }

    void beginBout(const int a, const int b)
    {
        Kangaroo& ka = roos[a];
        Kangaroo& kb = roos[b];
        const float stand = 0.32F * (ka.size + kb.size);
        for (auto [self, other] : {std::pair{a, b}, std::pair{b, a}}) {
            Kangaroo& k = roos[self];
            const Kangaroo& o = roos[other];
            startActivity(k, Activity::Box, k.rng.range(5.0F, 8.0F));
            k.partner = other;
            k.kicksLanded = 0;
            k.boutStrength = k.size * 10.0F + k.rng.range(0.0F, 1.5F);
            // Meet halfway, each stopping short of the middle.
            const V3 toward = normalize(flatten(o.p - k.p, faceNormal(k.face)), k.heading);
            const float gap = surfaceDistance(k.p, k.face, o.p, o.face);
            const SurfacePoint meet = walk(SurfacePoint{k.p, toward, k.face}, std::max(0.0F, 0.5F * gap - 0.5F * stand));
            setGoal(k, meet.p, meet.face, gap > 0.6F ? Move::Hop : Move::Walk, gap > 0.6F ? 0.6F : 0.12F);
            k.kickClock = k.rng.range(0.8F, 1.6F);
            k.jabClock = 0.0F;
            ++k.bouts;
        }
        // Both run the same duration, so they finish together.
        kb.activityDuration = ka.activityDuration;
        ++boutsTotal;
        publish("kangaroo.bout.started", "{\"a\":" + sim_detail::quote(ka.name) + ",\"b\":"
                                          + sim_detail::quote(kb.name) + "}");
    }

    void thinkBox(const int index, const float dt)
    {
        Kangaroo& k = roos[index];
        if (k.partner < 0 || !roos[k.partner].present() || roos[k.partner].activity != Activity::Box) {
            startActivity(k, Activity::Rest, 2.0F);
            k.move = Move::None;
            k.arrived = true;
            return;
        }
        Kangaroo& o = roos[k.partner];
        k.hasFaceToward = true;
        k.faceToward = o.p;
        if (!k.arrived) {
            if (k.activityClock > 6.0F) {
                k.arrived = true;
            }
            return;
        }
        k.move = Move::None;
        k.jabClock += dt;
        k.kickClock -= dt;
        k.kickPose = std::max(0.0F, k.kickPose - dt);
        if (k.kickClock <= 0.0F && k.gait == Gait::Stand && o.arrived) {
            // A kick: rocked back on the tail, both feet into the opponent.
            k.kickClock = k.rng.range(1.2F, 2.4F);
            k.kickPose = 0.45F;
            const float gap = surfaceDistance(k.p, k.face, o.p, o.face);
            if (gap < 0.45F * (k.size + o.size) + 0.06F) {
                ++k.kicksLanded;
                o.stagger = 0.35F;
                const V3 n = faceNormal(o.face);
                const V3 push = normalize(flatten(o.p - k.p, n), o.heading);
                const SurfacePoint shoved = walk(SurfacePoint{o.p, push, o.face}, 0.035F * k.size / 0.3F);
                o.p = shoved.p;
                o.face = shoved.face;
                dust(o.p, n, o.face, 5, o.size, 2);
            }
        }
        if (k.activityClock > k.activityDuration && index < k.partner) {
            // Settle it once, from the lower index.
            const float scoreK = k.boutStrength + static_cast<float>(k.kicksLanded);
            const float scoreO = o.boutStrength + static_cast<float>(o.kicksLanded);
            Kangaroo& winner = scoreK >= scoreO ? k : o;
            Kangaroo& loser = scoreK >= scoreO ? o : k;
            ++winner.wins;
            publish("kangaroo.bout.ended",
                 "{\"winner\":" + sim_detail::quote(winner.name) + ",\"loser\":"
                     + sim_detail::quote(loser.name) + ",\"kicks\":["
                     + std::to_string(winner.kicksLanded) + "," + std::to_string(loser.kicksLanded) + "]}");
            startActivity(winner, Activity::Groom, 3.0F);
            winner.move = Move::None;
            winner.arrived = true;
            startActivity(loser, Activity::Flee, 2.5F);
            const V3 away = normalize(flatten(loser.p - winner.p, faceNormal(loser.face)), loser.heading);
            loser.heading = away;
            const SurfacePoint escape = walk(SurfacePoint{loser.p, away, loser.face}, 0.9F);
            setGoal(loser, escape.p, escape.face, Move::Hop, 1.1F);
        }
    }

    void startThump(const int by, const char* reason)
    {
        Kangaroo& k = roos[by];
        startActivity(k, Activity::Thump, 0.8F);
        k.move = Move::None;
        k.arrived = true;
        k.kickClock = 0.0F;
        alarmCooldown = 8.0F;
        ++alarmsTotal;
        publish("kangaroo.alarm", "{\"by\":" + sim_detail::quote(k.name) + ",\"reason\":"
                                   + sim_detail::quote(reason) + "}");
    }

    void thinkThump(const int index, const float dt)
    {
        Kangaroo& k = roos[index];
        const float before = k.activityClock - dt;
        // Two thumps of the hind feet, then everybody runs.
        for (const float beat : {0.28F, 0.56F}) {
            if (before < beat && k.activityClock >= beat) {
                const V3 n = faceNormal(k.face);
                dust(k.p, n, k.face, 10, k.size * 1.2F, 0);
                printFeet(k, cross(k.heading, n));
            }
        }
        if (k.activityClock < k.activityDuration) {
            return;
        }
        for (std::size_t i = 0; i < roos.size(); ++i) {
            Kangaroo& other = roos[i];
            if (!other.present() || other.activity == Activity::Flung) {
                continue;
            }
            if (other.role == Role::Joey && other.mother >= 0) {
                // A joey at foot dives head first into its mother's pouch.
                startActivity(other, Activity::Follow, 6.0F);
                other.calmClock = -1.0F;
                continue;
            }
            startActivity(other, Activity::Flee, other.rng.range(2.5F, 3.5F));
            const V3 n = faceNormal(other.face);
            V3 away = flatten(other.p - k.p, n);
            if (length(away) < 0.05F || other.face != k.face) {
                away = flatten(k.heading, n);
            }
            other.heading = normalize(away, other.heading);
            const SurfacePoint escape = walk(SurfacePoint{other.p, other.heading, other.face}, 0.9F);
            setGoal(other, escape.p, escape.face, Move::Hop, other.rng.range(1.25F, 1.6F));
        }
    }

    void thinkFollow(const int index, const float dt)
    {
        Kangaroo& k = roos[index];
        if (k.mother < 0 || !roos[k.mother].present()) {
            if (k.activityClock > 3.0F) {
                startActivity(k, Activity::Rest, 2.0F);
            }
            return;
        }
        Kangaroo& mum = roos[k.mother];
        const float gap = surfaceDistance(k.p, k.face, mum.p, mum.face);
        if (k.calmClock < 0.0F) {
            // Diving for the pouch.
            if (gap < 0.12F && mum.grounded()) {
                k.activity = Activity::Pouched;
                k.activityClock = 0.0F;
                k.calmClock = 0.0F;
                k.move = Move::None;
                k.gait = Gait::Stand;
                k.teleported = true;
                publish("kangaroo.joey", "{\"name\":" + sim_detail::quote(k.name) + ",\"mother\":"
                                          + sim_detail::quote(mum.name) + ",\"event\":\"dived into the pouch\"}");
                return;
            }
            setGoal(k, mum.p, mum.face, gap > 0.25F ? Move::Hop : Move::Walk, gap > 0.25F ? 1.4F : 0.3F);
            k.arrived = false;
            return;
        }
        // At her side, clear of her body: behind her is her tail, which
        // reaches most of her height back along the ground.
        const SurfacePoint beside =
            walk(SurfacePoint{mum.p, cross(mum.heading, faceNormal(mum.face)), mum.face}, 0.6F * mum.size);
        if (gap > 0.45F) {
            setGoal(k, beside.p, beside.face, Move::Hop, std::max(mum.speed, 0.6F) * 1.15F);
        } else if (gap > 0.3F && k.arrived) {
            setGoal(k, beside.p, beside.face, Move::Walk, 0.12F);
        } else if (k.arrived && k.rng.chance(0.4F * dt)) {
            // Potter about near her.
            const V3 goal = nearbyPoint(k, 0.05F, 0.12F);
            setGoal(k, goal, k.face, Move::Walk, 0.07F);
        }
    }

    void thinkPouched(const int index, const float dt)
    {
        Kangaroo& k = roos[index];
        if (k.mother < 0) {
            return;
        }
        Kangaroo& mum = roos[k.mother];
        mum.pouchFull = std::min(1.0F, mum.pouchFull + 3.0F * dt);
        const bool settled = mum.grounded() && (mum.activity == Activity::Rest || mum.activity == Activity::Graze
                                                || mum.activity == Activity::Groom || mum.activity == Activity::Drink);
        k.calmClock = settled ? k.calmClock + dt : 0.0F;
        if (k.calmClock > 7.0F) {
            // Climb out in front of her.
            const SurfacePoint out = walk(SurfacePoint{mum.p, mum.heading, mum.face}, 0.4F * mum.size);
            k.p = out.p;
            k.face = out.face;
            k.heading = out.heading;
            k.up = faceNormal(k.face);
            k.v = V3{};
            k.gait = Gait::Stand;
            k.teleported = true;
            startActivity(k, Activity::Follow, 4.0F);
            k.calmClock = 0.0F;
            k.move = Move::None;
            k.arrived = true;
            publish("kangaroo.joey", "{\"name\":" + sim_detail::quote(k.name) + ",\"mother\":"
                                      + sim_detail::quote(mum.name) + ",\"event\":\"climbed out\"}");
        }
    }

    void bringBack(Kangaroo& k)
    {
        // Drop in over a face that holds, from as high up as the planet will
        // still pull it down. On a fast planet the centrifugal push grows
        // with distance from the axle faster than gravity falls off, so a drop
        // from high up is thrown straight back out; the drop is then made
        // lower, down to just above the ground, over whichever face holds
        // best. If nothing holds even there, wait: there is nowhere to land.
        std::array<int, faceCount> order{0, 1, 2, 3, 4, 5};
        std::array<float, faceCount> holds{};
        for (int face = 0; face < faceCount; ++face) {
            holds[face] = grip(faceNormal(face), face);
        }
        std::sort(order.begin(), order.end(), [&](const int a, const int b) { return holds[a] > holds[b]; });
        const float pulledEnough = 0.2F * settings.surfaceGravity;
        for (const int face : order) {
            if (holds[face] < 0.45F * settings.surfaceGravity) {
                break;
            }
            V3 u;
            V3 v;
            faceAxes(face, u, v);
            const V3 n = faceNormal(face);
            for (const float height : {1.6F, 1.0F, 0.55F, 0.3F}) {
                const V3 offset = k.rng.inDisc(u, v, 0.4F * height / 1.6F);
                bool pulled = true;
                for (int sample = 0; sample <= 4 && pulled; ++sample) {
                    const V3 point = n * (1.0F + height * static_cast<float>(sample) / 4.0F) + offset;
                    pulled = -dot(effectiveGravity(point), n) >= pulledEnough;
                }
                if (!pulled) {
                    continue;
                }
                k.p = n * (1.0F + height) + offset;
                k.v = V3{};
                k.face = face;
                k.up = n;
                k.heading = normalize(u);
                k.gait = Gait::Flight;
                k.gaitClock = 0.0F;
                k.flightDuration = 1e3F;
                k.activity = Activity::Flung;
                k.tumbleRate = 1.5F;
                k.teleported = true;
                k.awayClock = 0.0F;
                return;
            }
        }
    }

    // ------------------------------------------------------------------
    // Turning the state into a pose
    // ------------------------------------------------------------------

    void animate(Kangaroo& k, const float dt)
    {
        using sim_detail::approach;
        using sim_detail::smoothstep;
        if (!k.present()) {
            return;
        }
        const V3 n = faceNormal(k.face);

        // What the body is drawn against: the face while on it, and in the air
        // the felt gravity, so a kangaroo crossing an edge in flight turns
        // smoothly and one flung off tumbles freely.
        const V3 g = effectiveGravity(k.p);
        V3 upTarget = k.grounded() ? n : normalize(-g, n);
        if (k.activity == Activity::Flung) {
            upTarget = k.up;
        }
        k.up = normalize(lerp(k.up, upTarget, 1.0F - std::exp(-9.0F * dt)), n);
        if (k.hasFaceToward && k.grounded() && k.arrived) {
            const V3 want = normalize(flatten(k.faceToward - k.p, k.up), k.heading);
            k.heading = normalize(lerp(k.heading, want, 1.0F - std::exp(-5.0F * dt)), want);
        }
        k.heading = normalize(flatten(k.heading, k.up), V3{k.up.y, k.up.z, k.up.x});

        Pose target;
        target.pouchFull = std::max(k.pouchJoey ? 1.0F : 0.0F, k.pouchFull);
        target.juvenile = k.role == Role::Joey ? 1.0F : 0.0F;

        switch (k.gait) {
        case Gait::Stand:
            target.crouch = 0.12F;
            target.tailLift = 0.0F;
            break;
        case Gait::Stance: {
            const float u = std::clamp(k.gaitClock / std::max(k.stanceDuration, 1e-3F), 0.0F, 1.0F);
            const float depth = std::clamp(0.35F + 0.3F * k.landingSpeed, 0.35F, 1.0F);
            target.crouch = std::sin(pi * u) * depth;
            target.heelLift = 0.55F + 0.45F * u;
            target.legExtend = smoothstep(0.55F, 1.0F, u);
            target.bodyPitch = 0.3F + 0.15F * target.crouch;
            target.tailLift = 0.45F;
            break;
        }
        case Gait::Flight: {
            const float u = std::clamp(k.gaitClock / std::max(k.flightDuration, 1e-3F), 0.0F, 1.0F);
            target.airborne = 1.0F;
            target.legSwing = -1.0F + 2.0F * smoothstep(0.12F, 0.88F, u);
            target.legExtend = 1.0F - smoothstep(0.0F, 0.4F, u);
            target.heelLift = 1.0F;
            target.bodyPitch = 0.48F - 0.18F * u;
            target.tailLift = 0.8F + 0.2F * target.legSwing;
            break;
        }
        case Gait::Walk: {
            const float ph = k.walkPhase;
            // Forepaws and tail take the weight while the hind legs swing
            // forward together, then the forepaws are lifted and placed ahead.
            const float swing = std::max(0.0F, std::sin(pi * std::clamp((ph - 0.3F) / 0.4F, 0.0F, 1.0F)));
            target.forepawsDown = ph < 0.72F ? 1.0F : 1.0F - smoothstep(0.72F, 0.9F, ph) * 0.8F;
            target.bodyPitch = 0.78F;
            target.crouch = 0.45F;
            target.headPitch = 0.7F;
            target.airborne = 0.55F * swing;
            target.legSwing = -0.2F + 1.0F * swing;
            target.tailLift = 0.0F;
            break;
        }
        }

        const float t = static_cast<float>(time);
        switch (k.activity) {
        case Activity::Graze:
            if (k.arrived && k.gait == Gait::Stand) {
                target.forepawsDown = 1.0F;
                target.bodyPitch = 0.86F;
                target.crouch = 0.55F;
                target.headPitch = 1.35F + 0.08F * std::sin(t * 7.0F + k.tone * 3.0F);
                target.tailLift = 0.0F;
            } else if (k.gait == Gait::Walk) {
                target.headPitch = 1.2F;
            }
            break;
        case Activity::Drink:
            if (k.arrived && k.gait == Gait::Stand) {
                target.forepawsDown = 0.9F;
                target.bodyPitch = 0.82F;
                target.crouch = 0.5F;
                target.headPitch = 1.5F;
            }
            break;
        case Activity::Groom:
            if (k.gait == Gait::Stand) {
                target.lick = 0.7F + 0.3F * std::sin(t * 9.0F);
                target.headPitch = 0.55F;
                target.crouch = 0.2F;
            }
            break;
        case Activity::Box:
            if (k.arrived && k.gait == Gait::Stand) {
                target.rear = 1.0F;
                const float jab = k.jabClock * 4.2F + k.tone;
                target.armReachLeft = std::max(0.0F, std::sin(jab));
                target.armReachRight = std::max(0.0F, -std::sin(jab));
                target.headPitch = -0.25F;
                if (k.kickPose > 0.0F) {
                    // Rocked back onto the tail with both feet thrown forward.
                    target.kick = 1.0F;
                    target.armReachLeft = 0.3F;
                    target.armReachRight = 0.3F;
                }
                if (k.stagger > 0.0F) {
                    target.bodyPitch = -0.25F;
                }
            }
            break;
        case Activity::Thump:
            target.rear = 0.35F;
            target.crouch = 0.2F + 0.6F * (std::fabs(std::sin(k.activityClock * pi / 0.28F)));
            target.headPitch = -0.2F;
            break;
        case Activity::Flee:
            target.earLeft = 0.9F;
            target.earRight = 0.9F;
            break;
        case Activity::Flung: {
            // Flailing.
            target.airborne = 1.0F;
            target.legSwing = std::sin(t * 13.0F + k.tone * 2.0F);
            target.legExtend = 0.5F + 0.5F * std::sin(t * 9.0F);
            target.armReachLeft = 0.5F + 0.5F * std::sin(t * 17.0F);
            target.armReachRight = 0.5F + 0.5F * std::cos(t * 15.0F);
            target.tailLift = 0.6F + 0.5F * std::sin(t * 7.0F);
            target.earLeft = 1.0F;
            target.earRight = 1.0F;
            break;
        }
        default:
            break;
        }

        // Pouch young peek out when their mother is settled.
        const bool settled = k.gait == Gait::Stand
            && (k.activity == Activity::Rest || k.activity == Activity::Graze
                || k.activity == Activity::Drink || k.activity == Activity::Groom);
        k.calmClock = (k.role != Role::Joey) ? (settled ? k.calmClock + dt : 0.0F) : k.calmClock;
        bool youngInside = k.pouchJoey;
        for (const Kangaroo& other : roos) {
            if (other.activity == Activity::Pouched && other.mother == static_cast<int>(&k - roos.data())) {
                youngInside = true;
            }
        }
        if (!youngInside && !k.pouchJoey) {
            k.pouchFull = std::max(0.0F, k.pouchFull - dt);
        }
        const float peekTarget = (youngInside && k.calmClock > 1.2F) ? 1.0F : 0.0F;
        k.joeyPeek = approach(k.joeyPeek, peekTarget, peekTarget > k.joeyPeek ? 1.5F : 8.0F, dt);
        target.joeyPeek = k.joeyPeek;

        // Ears twitch on their own, and the eyes blink.
        k.earClock -= dt;
        if (k.earClock <= 0.0F) {
            k.earClock = k.rng.range(1.2F, 4.5F);
            (k.rng.chance(0.5F) ? k.earLeft : k.earRight) = k.rng.range(-0.8F, 0.9F);
        }
        k.earLeft = approach(k.earLeft, 0.0F, 3.0F, dt);
        k.earRight = approach(k.earRight, 0.0F, 3.0F, dt);
        target.earLeft = std::max(target.earLeft, k.earLeft);
        target.earRight = std::max(target.earRight, k.earRight);
        if (target.earLeft == 0.0F) {
            target.earLeft = k.earLeft;
        }
        if (target.earRight == 0.0F) {
            target.earRight = k.earRight;
        }
        k.blinkClock -= dt;
        if (k.blinkClock <= 0.0F) {
            k.blinkClock = k.rng.range(2.0F, 5.0F);
        }
        target.blink = k.blinkClock < 0.12F ? 1.0F : 0.0F;
        k.lookClock -= dt;
        if (k.lookClock <= 0.0F) {
            k.lookClock = k.rng.range(0.8F, 3.0F);
            k.lookYaw = (k.activity == Activity::Rest || k.activity == Activity::Groom) ? k.rng.range(-0.7F, 0.7F) : 0.0F;
        }
        target.headYaw = k.lookYaw;

        // The felt up in the body's frame, for the lean on slopes.
        const V3 forward = k.heading;
        const V3 right = cross(forward, k.up);
        const V3 felt = normalize(-g, k.up);
        target.feltUp = V3{dot(felt, forward), dot(felt, k.up), dot(felt, right)};
        if (k.activity == Activity::Flung) {
            target.feltUp = V3{0.0F, 1.0F, 0.0F};
        }

        // Follow the target pose. Fast, so the hop cycle stays crisp, but not
        // instant, so a change of activity is a movement and not a jump.
        const float rate = 22.0F;
        Pose& p = k.pose;
        p.crouch = approach(p.crouch, target.crouch, rate, dt);
        p.heelLift = approach(p.heelLift, target.heelLift, rate, dt);
        p.airborne = approach(p.airborne, target.airborne, rate * 1.5F, dt);
        p.legSwing = approach(p.legSwing, target.legSwing, rate, dt);
        p.legExtend = approach(p.legExtend, target.legExtend, rate, dt);
        p.bodyPitch = approach(p.bodyPitch, target.bodyPitch, rate * 0.5F, dt);
        p.tailLift = approach(p.tailLift, target.tailLift, rate * 0.6F, dt);
        p.headPitch = approach(p.headPitch, target.headPitch, rate * 0.35F, dt);
        p.headYaw = approach(p.headYaw, target.headYaw, 4.0F, dt);
        p.armReachLeft = approach(p.armReachLeft, target.armReachLeft, rate, dt);
        p.armReachRight = approach(p.armReachRight, target.armReachRight, rate, dt);
        p.forepawsDown = approach(p.forepawsDown, target.forepawsDown, rate * 0.5F, dt);
        p.rear = approach(p.rear, target.rear, 6.0F, dt);
        p.kick = approach(p.kick, target.kick, 18.0F, dt);
        p.lick = approach(p.lick, target.lick, 8.0F, dt);
        p.earLeft = target.earLeft;
        p.earRight = target.earRight;
        p.blink = target.blink;
        p.joeyPeek = target.joeyPeek;
        p.pouchFull = approach(p.pouchFull, target.pouchFull, 4.0F, dt);
        p.juvenile = target.juvenile;
        p.feltUp = normalize(lerp(p.feltUp, target.feltUp, 1.0F - std::exp(-6.0F * dt)));
    }

public:
    // From the kangaroo's own frame to the planet's: x forward, y up, z right,
    // scaled by its size, around the point it stands on.
    [[nodiscard]] static V3 toPlanet(const Kangaroo& k, const V3& local)
    {
        const V3 right = cross(k.heading, k.up);
        V3 q = local * k.size;
        if (k.activity == Activity::Flung && k.tumbleAngle != 0.0F) {
            const V3 centre{0.0F, 0.45F * k.size, 0.0F};
            q = rotate(q - centre, k.tumbleAxis, k.tumbleAngle) + centre;
        }
        return k.p + k.heading * q.x + k.up * q.y + right * q.z;
    }

private:
    Rng fallbackRng{7};
};

} // namespace kangaroo
