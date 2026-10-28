// Checks the physics the kangaroo planet runs on, without Qt or a display.
//
// The gravity is a closed form that is easy to get wrong by a sign or a
// swapped argument and still produce a plausible-looking field, so every
// property is checked against something computed a different way: numerical
// integration over the solid, finite differences of the potential, Poisson's
// equation inside and Laplace's outside, the symmetry of the cube, and the
// point-mass limit far away.
//
// The simulation checks after it run the mob for simulated minutes at fixed
// seeds and assert the things that would show on screen as a bug: a kangaroo
// inside the planet, one that stops moving, a planet that gains spin from
// nothing, water that appears or vanishes.

#include "cube_gravity.h"
#include "kangaroo_sim.h"

#include <array>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& what)
{
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    } else {
        std::printf("ok:   %s\n", what.c_str());
    }
}

using cube_gravity::Vec;

// Gauss-Legendre nodes and weights on [-1, 1], computed by Newton iteration on
// the Legendre polynomial so the test carries no table that could itself be
// mistyped.
void gaussLegendre(const int n, std::vector<double>& nodes, std::vector<double>& weights)
{
    nodes.assign(n, 0.0);
    weights.assign(n, 0.0);
    for (int i = 0; i < n; ++i) {
        double x = std::cos(cube_gravity::pi * (i + 0.75) / (n + 0.5));
        for (int iteration = 0; iteration < 100; ++iteration) {
            double p0 = 1.0;
            double p1 = x;
            for (int k = 2; k <= n; ++k) {
                const double p2 = ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / k;
                p0 = p1;
                p1 = p2;
            }
            const double derivative = n * (x * p1 - p0) / (x * x - 1.0);
            const double step = p1 / derivative;
            x -= step;
            if (std::fabs(step) < 1e-15) {
                double q0 = 1.0;
                double q1 = x;
                for (int k = 2; k <= n; ++k) {
                    const double q2 = ((2.0 * k - 1.0) * x * q1 - (k - 1.0) * q0) / k;
                    q0 = q1;
                    q1 = q2;
                }
                const double d = n * (x * q1 - q0) / (x * x - 1.0);
                nodes[i] = x;
                weights[i] = 2.0 / ((1.0 - x * x) * d * d);
                break;
            }
        }
    }
}

// The potential integral and the attraction by brute force: composite
// Gauss-Legendre over the solid, `panels` sub-intervals per axis with `order`
// nodes each. Accurate for field points not too close to the surface, which is
// where the integrand's peak is narrow.
void quadrature(const Vec& p, const int panels, const int order, double& potential, Vec& attraction)
{
    std::vector<double> nodes;
    std::vector<double> weights;
    gaussLegendre(order, nodes, weights);
    std::vector<double> points;
    std::vector<double> pointWeights;
    const double width = 2.0 / panels;
    for (int panel = 0; panel < panels; ++panel) {
        const double centre = -1.0 + width * (panel + 0.5);
        for (int i = 0; i < order; ++i) {
            points.push_back(centre + 0.5 * width * nodes[i]);
            pointWeights.push_back(0.5 * width * weights[i]);
        }
    }
    potential = 0.0;
    attraction = Vec{0.0, 0.0, 0.0};
    const std::size_t count = points.size();
    for (std::size_t i = 0; i < count; ++i) {
        const double dx = points[i] - p[0];
        for (std::size_t j = 0; j < count; ++j) {
            const double dy = points[j] - p[1];
            const double wij = pointWeights[i] * pointWeights[j];
            for (std::size_t k = 0; k < count; ++k) {
                const double dz = points[k] - p[2];
                const double r2 = dx * dx + dy * dy + dz * dz;
                const double r = std::sqrt(r2);
                const double w = wij * pointWeights[k];
                potential += w / r;
                const double w3 = w / (r2 * r);
                attraction[0] += w3 * dx;
                attraction[1] += w3 * dy;
                attraction[2] += w3 * dz;
            }
        }
    }
}

double relativeError(const Vec& a, const Vec& b)
{
    const Vec d{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return cube_gravity::length(d) / std::max(cube_gravity::length(b), 1e-300);
}

std::string format(const char* pattern, const double a, const double b = 0.0, const double c = 0.0)
{
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, pattern, a, b, c);
    return buffer;
}

void gravityAgainstQuadrature()
{
    // Outside, at a range of distances and directions, none on a symmetry line
    // so a swapped component would show.
    const std::array<Vec, 6> points{{
        {1.5, 0.3, -0.2},
        {0.2, 1.3, 0.9},
        {-2.0, 0.7, 1.1},
        {3.0, 2.0, 1.0},
        {0.31, -0.17, 1.25},
        {-1.2, -1.3, -1.4},
    }};
    for (const Vec& p : points) {
        double nExact = cube_gravity::potential(p);
        Vec aExact = cube_gravity::attraction(p);
        double nNumeric = 0.0;
        Vec aNumeric{};
        quadrature(p, 6, 12, nNumeric, aNumeric);
        const double nError = std::fabs(nExact - nNumeric) / nNumeric;
        const double aError = relativeError(aExact, aNumeric);
        check(nError < 1e-7,
              format("potential at (%.2f, %.2f, %.2f) matches quadrature", p[0], p[1], p[2])
                  + format(" (relative error %.1e)", nError));
        check(aError < 1e-7,
              format("attraction at (%.2f, %.2f, %.2f) matches quadrature", p[0], p[1], p[2])
                  + format(" (relative error %.1e)", aError));
    }
}

// The attraction is the gradient of the potential integral. This holds on the
// surface and inside too, where quadrature is weakest, so it covers the points
// a kangaroo actually stands on.
//
// On the surface the density jumps, so the potential's second derivative jumps
// by 4 pi there and a central difference straddling it is off by about
// 4 pi h / |a|. The step is small enough that this stays below the tolerance,
// and large enough that rounding in the difference does too.
void attractionIsGradient()
{
    const std::array<Vec, 7> points{{
        {0.0, 1.0, 0.0},
        {0.53, 1.0, -0.27},
        {0.98, 1.0, 0.4},
        {0.2, -0.3, 0.4},
        {0.99, 0.99, 0.99},
        {1.7, -0.2, 0.05},
        {0.0, 0.0, 0.0},
    }};
    const double h = 1e-7;
    for (const Vec& p : points) {
        Vec gradient{};
        for (int axis = 0; axis < 3; ++axis) {
            Vec plus = p;
            Vec minus = p;
            plus[axis] += h;
            minus[axis] -= h;
            gradient[axis] =
                (cube_gravity::potential(plus) - cube_gravity::potential(minus)) / (2.0 * h);
        }
        const Vec a = cube_gravity::attraction(p);
        const double error = relativeError(a, gradient);
        // At the centre the field is zero, so compare absolutely there.
        const bool centre = p[0] == 0.0 && p[1] == 0.0 && p[2] == 0.0;
        check(centre ? cube_gravity::length(a) < 1e-9 : error < 1e-6,
              format("attraction is the gradient of the potential at (%.2f, %.2f, %.2f)",
                     p[0], p[1], p[2])
                  + format(" (error %.1e)", centre ? cube_gravity::length(a) : error));
    }
}

// Poisson inside: the Laplacian of the potential integral is -4 pi where there
// is mass. Laplace outside: it is zero. Both from the potential alone, so they
// test potentialTerm independently of attractionTerm.
void poissonAndLaplace()
{
    const double h = 1e-3;
    const auto laplacian = [h](const Vec& p) {
        double sum = -6.0 * cube_gravity::potential(p);
        for (int axis = 0; axis < 3; ++axis) {
            Vec plus = p;
            Vec minus = p;
            plus[axis] += h;
            minus[axis] -= h;
            sum += cube_gravity::potential(plus) + cube_gravity::potential(minus);
        }
        return sum / (h * h);
    };
    for (const Vec& p : {Vec{0.1, 0.2, -0.3}, Vec{0.7, -0.6, 0.5}, Vec{0.0, 0.0, 0.0}}) {
        const double value = laplacian(p);
        check(std::fabs(value + 4.0 * cube_gravity::pi) < 1e-3,
              format("Poisson inside at (%.1f, %.1f, %.1f)", p[0], p[1], p[2])
                  + format(": laplacian %.6f, expected -4 pi", value));
    }
    for (const Vec& p : {Vec{1.3, 0.2, -0.3}, Vec{-0.7, 1.6, 1.5}, Vec{2.0, 2.0, 2.0}}) {
        const double value = laplacian(p);
        check(std::fabs(value) < 1e-3,
              format("Laplace outside at (%.1f, %.1f, %.1f)", p[0], p[1], p[2])
                  + format(": laplacian %.6f", value));
    }
}

void symmetryAndFarField()
{
    const auto direction = [](const Vec& v) {
        const double l = cube_gravity::length(v);
        return Vec{v[0] / l, v[1] / l, v[2] / l};
    };
    const Vec face = direction(cube_gravity::attraction(Vec{0.0, 1.0, 0.0}));
    check(std::fabs(face[1] + 1.0) < 1e-12, "at a face centre the attraction is straight down");

    const Vec edge = direction(cube_gravity::attraction(Vec{1.0, 1.0, 0.0}));
    check(std::fabs(edge[0] - edge[1]) < 1e-12 && std::fabs(edge[2]) < 1e-12 && edge[0] < 0.0,
          "at an edge's midpoint it bisects the two faces, 45 degrees to each");

    const Vec corner = direction(cube_gravity::attraction(Vec{1.0, 1.0, 1.0}));
    check(std::fabs(corner[0] - corner[1]) < 1e-12 && std::fabs(corner[1] - corner[2]) < 1e-12
              && corner[0] < 0.0,
          "at a corner it points at the centre");

    // Off the symmetry lines on a face, it tilts toward the face centre.
    const Vec tilted = cube_gravity::attraction(Vec{0.6, 1.0, 0.0});
    check(tilted[0] < 0.0, "part way to an edge, down tilts back toward the face centre");

    for (const double r : {8.0, 20.0, 29.0}) {
        const Vec p{r * 0.48, r * 0.6, r * 0.64};
        const Vec a = cube_gravity::attraction(p);
        const double pointMass = cube_gravity::mass / (r * r);
        const double error = std::fabs(cube_gravity::length(a) - pointMass) / pointMass;
        // The leading correction is the hexadecapole, (1/r)^4 relative.
        check(error < 2.0 / (r * r * r * r),
              format("at distance %.0f it is a point mass to %.1e", r, error));
    }

    const double rInside = 29.999;
    const double rOutside = 30.001;
    const Vec unit{0.48, 0.6, 0.64};
    const Vec inside = cube_gravity::attraction(Vec{rInside * unit[0], rInside * unit[1], rInside * unit[2]});
    const Vec outside = cube_gravity::attraction(Vec{rOutside * unit[0], rOutside * unit[1], rOutside * unit[2]});
    const double jump = std::fabs(cube_gravity::length(inside) * rInside * rInside
                                  - cube_gravity::length(outside) * rOutside * rOutside)
        / cube_gravity::mass;
    check(jump < 1e-6, format("the switch to a point mass at 30 is continuous to %.1e", jump));
}

// The numbers the scene is tuned with, printed so a change to them shows in the
// test log and not only on screen.
void reportProfile()
{
    const double faceCentre = cube_gravity::faceCentreAttraction();
    const double edge = cube_gravity::length(cube_gravity::attraction(Vec{1.0, 1.0, 0.0}));
    const double corner = cube_gravity::length(cube_gravity::attraction(Vec{1.0, 1.0, 1.0}));
    const double sphere = cube_gravity::mass; // a point mass at distance 1
    std::printf("profile: attraction at face centre %.6f, edge midpoint %.6f, corner %.6f;"
                " a point mass of the same size at distance 1 would give %.6f\n",
                faceCentre, edge, corner, sphere);
    const double rise = cube_gravity::potential(Vec{0.0, 1.0, 0.0})
        - cube_gravity::potential(Vec{1.0, 1.0, 0.0});
    std::printf("profile: walking from a face centre to an edge climbs %.6f in potential,"
                " the same as %.4f half-sides of height at face-centre gravity\n",
                rise, rise / faceCentre);
}

// ---------------------------------------------------------------------------
// The rig
// ---------------------------------------------------------------------------

void rigStaysWhole()
{
    kangaroo::Rng rng(99);
    int broken = 0;
    int unbounded = 0;
    float lowest = 1.0F;
    for (int trial = 0; trial < 4000; ++trial) {
        kangaroo::Pose pose;
        pose.crouch = rng.uniform();
        pose.heelLift = rng.uniform();
        pose.airborne = rng.uniform();
        pose.legSwing = rng.range(-1.0F, 1.0F);
        pose.legExtend = rng.uniform();
        pose.bodyPitch = rng.range(-0.4F, 1.0F);
        pose.tailLift = rng.range(0.0F, 1.2F);
        pose.headPitch = rng.range(-0.5F, 1.6F);
        pose.headYaw = rng.range(-0.8F, 0.8F);
        pose.armReachLeft = rng.uniform();
        pose.armReachRight = rng.uniform();
        pose.forepawsDown = rng.uniform();
        pose.rear = rng.uniform();
        pose.kick = rng.uniform();
        pose.lick = rng.uniform();
        pose.joeyPeek = rng.uniform();
        pose.pouchFull = rng.uniform();
        pose.juvenile = rng.uniform();
        pose.feltUp = kangaroo::normalize(kangaroo::V3{rng.range(-0.5F, 0.5F), 1.0F, rng.range(-0.5F, 0.5F)});
        const kangaroo::Rig rig = kangaroo::buildRig(pose);
        for (int i = 0; i < rig.count; ++i) {
            const kangaroo::Primitive& p = rig.primitives[i];
            if (!kangaroo::finite(p.a) || !kangaroo::finite(p.b) || !std::isfinite(p.ra)
                || !std::isfinite(p.rb) || p.ra < 0.0F || p.rb < 0.0F) {
                ++broken;
                break;
            }
            for (int axis = 0; axis < 3; ++axis) {
                if (p.a[axis] - p.ra < rig.boundsMin[axis] || p.a[axis] + p.ra > rig.boundsMax[axis]
                    || p.b[axis] - p.rb < rig.boundsMin[axis] || p.b[axis] + p.rb > rig.boundsMax[axis]) {
                    ++unbounded;
                }
            }
        }
        if (pose.airborne == 0.0F || trial % 7 == 0) {
            pose.airborne = 0.0F;
            pose.kick = 0.0F;
            const kangaroo::Rig grounded = kangaroo::buildRig(pose);
            lowest = std::min({lowest, grounded.toeLeft.y, grounded.toeRight.y});
        }
    }
    check(broken == 0, "4000 random poses build without NaN or negative radii");
    check(unbounded == 0, "every primitive lies inside the bounds the ray marcher is given");
    check(lowest > -0.02F, format("on the ground the toes never sink below it (lowest %.3f)", lowest));

    kangaroo::Pose rest;
    const kangaroo::Rig standing = kangaroo::buildRig(rest);
    check(std::fabs(standing.toeLeft.y - 0.014F) < 1e-4F && std::fabs(standing.toeRight.y - 0.014F) < 1e-4F,
          "standing, both toes rest on the ground");
    check(standing.boundsMax.y > 0.95F && standing.boundsMax.y < 1.2F,
          format("standing height with ears is about one unit (%.2f)", standing.boundsMax.y));
    kangaroo::Pose push;
    push.airborne = 1.0F;
    push.legSwing = -1.0F;
    push.legExtend = 1.0F;
    const kangaroo::Rig pushOff = kangaroo::buildRig(push);
    check(std::min(pushOff.toeLeft.y, pushOff.heelLeft.y) > 0.0F
              && std::min(pushOff.toeLeft.y, pushOff.heelLeft.y) < 0.03F,
          "at push-off the body is raised so the extended foot just meets the ground");
}

// ---------------------------------------------------------------------------
// Walking over the surface
// ---------------------------------------------------------------------------

void walkingFolds()
{
    using kangaroo::V3;
    // From the centre of +y toward +x: one unit reaches the edge, a second
    // unit goes down the +x face.
    const kangaroo::SurfacePoint end = kangaroo::walk({V3{0.0F, 1.0F, 0.0F}, V3{1.0F, 0.0F, 0.0F}, 2}, 2.0F);
    check(end.face == 0 && std::fabs(end.p.x - 1.0F) < 1e-5F && std::fabs(end.p.y) < 1e-4F
              && std::fabs(end.heading.y + 1.0F) < 1e-4F,
          "walking two units from a face centre goes over the edge and halfway down the next face");
    // Round the cube: four units from a face centre come back to it.
    const kangaroo::SurfacePoint round = kangaroo::walk({V3{0.0F, 1.0F, 0.2F}, V3{1.0F, 0.0F, 0.0F}, 2}, 8.0F);
    check(round.face == 2 && kangaroo::length(round.p - V3{0.0F, 1.0F, 0.2F}) < 1e-3F,
          "eight units in a straight line goes once round the cube and back");
    const V3 across = kangaroo::surfaceDirection(V3{0.5F, 1.0F, 0.0F}, 2, V3{1.0F, 0.5F, 0.0F}, 0);
    check(across.x > 0.99F, "the way to a point on the next face is straight toward the shared edge");
    const float d = kangaroo::surfaceDistance(V3{0.5F, 1.0F, 0.0F}, 2, V3{1.0F, 0.5F, 0.0F}, 0);
    check(std::fabs(d - 1.0F) < 1e-5F, format("and the distance unfolds to %.4f, expected 1", d));
}

// ---------------------------------------------------------------------------
// The water
// ---------------------------------------------------------------------------

void waterFindsItsLevel()
{
    kangaroo::Mob mob;
    kangaroo::Settings settings;
    settings.mob = 1;
    const kangaroo::V3 axle{0.72F, 1.0F, 0.31F};
    mob.reset(settings, axle);
    const float rest = mob.water.totalVolume();
    check(std::fabs(rest - mob.water.volume) < 0.01F * mob.water.volume,
          format("at rest the billabongs hold the planet's water (%.5f of %.5f)", rest, mob.water.volume));
    float spread = 0.0F;
    for (int face = 0; face < kangaroo::faceCount; ++face) {
        spread = std::max(spread, std::fabs(mob.water.faceVolume(face) - rest / 6.0F));
    }
    check(spread < 1e-3F * rest, "at rest the six billabongs are equal, by symmetry");
    const float centreDepth = mob.water.depthAt(kangaroo::V3{0.0F, 1.0F, 0.0F}, 2);
    check(std::fabs(centreDepth - settings.waterDepth) < 2e-3F,
          format("and a face centre is %.4f deep, as asked", centreDepth));

    // Spin it up and let it settle.
    std::array<float, 6> before{};
    for (int face = 0; face < 6; ++face) {
        before[face] = mob.water.faceVolume(face);
    }
    const float omega = 2.0F;
    for (int i = 0; i < 600; ++i) {
        mob.water.update(omega, 1.0F / 60.0F);
    }
    const float spun = mob.water.totalVolume();
    check(std::fabs(spun - mob.water.volume) < 0.02F * mob.water.volume,
          format("spun at 2 rad/s the water is all still held (%.5f of %.5f)", spun, mob.water.volume));
    // The axle is mostly along y, so +y and -y are the polar faces. Water
    // moves away from the axle, off them.
    const float polar = mob.water.faceVolume(2) + mob.water.faceVolume(3);
    const float polarBefore = before[2] + before[3];
    check(polar < 0.8F * polarBefore,
          format("and it has run off the polar faces (%.5f, was %.5f)", polar, polarBefore));
    const float equatorial = mob.water.faceVolume(4) + mob.water.faceVolume(5);
    check(equatorial > before[4] + before[5],
          format("onto the faces furthest from the axle (%.5f, was %.5f)", equatorial, before[4] + before[5]));
}

// ---------------------------------------------------------------------------
// The mob
// ---------------------------------------------------------------------------

constexpr float degrees(const float d) { return d * 3.14159265F / 180.0F; }

struct Run {
    int insideViolations = 0;
    int nonFinite = 0;
    int flung = 0;
    int returned = 0;
    int hops = 0;
    int bouts = 0;
    int alarms = 0;
    double impulse = 0.0;
    float minimumHeight = 1e9F;
};

Run runMob(kangaroo::Mob& mob, const float seconds, const float omega)
{
    Run run;
    const float dt = 1.0F / 60.0F;
    const int frames = static_cast<int>(seconds / dt);
    for (int frame = 0; frame < frames; ++frame) {
        mob.advance(dt, omega);
        run.impulse += mob.takeImpulse();
        for (const kangaroo::Kangaroo& k : mob.roos) {
            if (!k.present()) {
                continue;
            }
            if (!kangaroo::finite(k.p) || !kangaroo::finite(k.v)) {
                ++run.nonFinite;
            }
            const float norm = kangaroo::boxNorm(k.p);
            run.minimumHeight = std::min(run.minimumHeight, norm);
            if (norm < 0.999F) {
                ++run.insideViolations;
            }
        }
        mob.events.clear();
    }
    run.flung = mob.flungTotal;
    run.returned = mob.returnedTotal;
    run.bouts = mob.boutsTotal;
    run.alarms = mob.alarmsTotal;
    for (const kangaroo::Kangaroo& k : mob.roos) {
        run.hops += k.hops;
    }
    return run;
}

void mobLivesOnThePlanet()
{
    kangaroo::Mob mob;
    kangaroo::Settings settings;
    settings.mob = 6;
    settings.seed = 3;
    mob.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
    const Run run = runMob(mob, 180.0F, degrees(75.0F));
    check(run.nonFinite == 0, "three simulated minutes at the default spin stay finite");
    check(run.insideViolations == 0,
          format("and nobody is ever inside the planet (lowest box norm %.4f)", run.minimumHeight));
    check(run.flung == 0, "at 75 degrees a second the mob stays on");
    check(run.hops > 150, format("they get about: %.0f hops", run.hops));
    int faces = 0;
    std::array<bool, 6> visited{};
    for (const kangaroo::Kangaroo& k : mob.roos) {
        visited[k.face] = true;
    }
    for (const bool v : visited) {
        faces += v ? 1 : 0;
    }
    std::printf("info: after three minutes the mob is spread over %d face(s); %d bout(s), %d alarm(s),"
                " %.3f axial impulse handed to the planet\n",
                faces, run.bouts, run.alarms, run.impulse);
    float grazed = 0.0F;
    float drank = 0.0F;
    for (const kangaroo::Kangaroo& k : mob.roos) {
        grazed += k.grazedBiomass;
        drank += k.drankSeconds;
    }
    check(grazed > 0.5F, format("they graze (%.2f tufts' worth eaten)", grazed));
    check(drank > 1.0F, format("and drink (%.1f s at the billabongs)", drank));
}

void spinThrowsThemOff()
{
    kangaroo::Mob mob;
    kangaroo::Settings settings;
    settings.mob = 6;
    settings.seed = 5;
    mob.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
    // Four presses of Ctrl++ from 75 degrees a second.
    const Run fast = runMob(mob, 25.0F, degrees(75.0F * 1.35F * 1.35F * 1.35F * 1.35F));
    check(fast.flung >= 3, format("at 249 degrees a second the mob is thrown off (%.0f flung)", fast.flung));
    check(fast.insideViolations == 0, "and nobody is pushed inside the planet on the way");
    const Run calm = runMob(mob, 40.0F, degrees(75.0F));
    check(calm.returned >= 3, format("slowed back down, they come back (%.0f returned)", calm.returned));
    int present = 0;
    for (const kangaroo::Kangaroo& k : mob.roos) {
        present += (k.present() && k.activity != kangaroo::Activity::Flung) ? 1 : 0;
    }
    check(present >= 5, format("and most of the mob is on the ground again (%.0f)", present));
}

// The flight integrator conserves the Jacobi integral, the energy of the
// turning frame: kinetic plus effective potential. Coriolis does no work and
// the rest derives from that potential, so any drift is integration error.
void flightConservesJacobi()
{
    kangaroo::Mob mob;
    kangaroo::Settings settings;
    settings.mob = 1;
    mob.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
    const float omega = degrees(75.0F);
    mob.advance(1.0F / 60.0F, omega);
    mob.advance(1.0F / 60.0F, omega);
    kangaroo::Kangaroo& k = mob.roos[0];
    // Wait for it to be standing, then throw it up and sideways hard enough
    // for a long flight.
    for (int i = 0; i < 600 && !(k.gait == kangaroo::Gait::Stand); ++i) {
        mob.advance(1.0F / 60.0F, omega);
    }
    const kangaroo::V3 n = kangaroo::faceNormal(k.face);
    kangaroo::V3 side;
    kangaroo::V3 other;
    kangaroo::faceAxes(k.face, side, other);
    const double impulseBefore = mob.owedImpulse;
    const double momentumBefore = mob.axialMomentumOf(k);
    check(mob.launch(0, n * 4.0F + side * 1.5F), "a kangaroo can be thrown");
    const double handed = mob.owedImpulse - impulseBefore;
    const double gained = mob.axialMomentumOf(k) - momentumBefore;
    check(std::fabs(handed + gained) < 1e-9,
          format("the planet takes back exactly the axial momentum the throw gave (%.3g)", handed + gained));
    const auto jacobi = [&]() {
        return 0.5 * kangaroo::dot(k.v, k.v) + mob.planet.effectivePotential(kangaroo::toVec(k.p));
    };
    const double start = jacobi();
    double worst = 0.0;
    float airborne = 0.0F;
    // Hold the spin constant: the Euler term would put energy in.
    while (k.gait == kangaroo::Gait::Flight && airborne < 20.0F) {
        mob.advance(1.0F / 60.0F, omega);
        airborne += 1.0F / 60.0F;
        if (k.gait == kangaroo::Gait::Flight && k.present()) {
            worst = std::max(worst, std::fabs(jacobi() - start) / std::fabs(start));
        }
    }
    check(airborne > 1.0F, format("the throw kept it airborne for %.2f s", airborne));
    check(worst < 2e-4, format("and the Jacobi integral drifted by at most %.1e of itself", worst));
}

// The planet's answer fed back into the mob, which is how it runs in the
// application and what the open-loop runs above never exercise. The first
// version of the bookkeeping billed the planet for the Euler shove on animals
// in the air, which speeds up a planet that is already speeding up; under a
// beat visualizer's kicks with the spin set to 162 degrees a second it ran
// away to 67,000.
struct LoopResult {
    double largestBoost = 0.0;
    // The mean size of the extra spin over the last twenty seconds. One hop
    // can move it by several degrees a second for a moment, so a single
    // reading says little; the mean says whether it is dying away.
    double settledBoost = 0.0;
    int limited = 0;
    int flung = 0;
    int returned = 0;
    int nonFinite = 0;
};

template <typename SetRate>
LoopResult closedLoop(const std::uint64_t seed, const float seconds, SetRate setRate)
{
    kangaroo::Mob mob;
    kangaroo::Settings settings;
    settings.seed = seed;
    mob.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
    kangaroo::SpinResponse spin;
    kangaroo::RateMeter meter;
    const float dt = 1.0F / 60.0F;
    float angle = 24.0F;
    LoopResult result;
    for (int frame = 0; frame < static_cast<int>(seconds / dt); ++frame) {
        const float t = static_cast<float>(frame) * dt;
        // The step function as the application runs it: the set rate plus
        // the planet's answer, returned modulo 360, which is what the snippet
        // measures the rate from.
        spin.apply(mob.takeImpulse(), settings.planetInertia, dt);
        const float turned = (setRate(t) + static_cast<float>(spin.boost)) * dt * 180.0F / 3.14159265F;
        const float next = std::fmod(angle + turned + 360.0F, 360.0F);
        meter.stepped(angle, next, dt);
        const float omega = meter.frame(next - angle, dt);
        angle = next;
        mob.advance(dt, omega);
        if (t > seconds - 20.0F) {
            result.settledBoost += std::fabs(spin.boost) * dt / 20.0;
        }
        for (const kangaroo::Kangaroo& k : mob.roos) {
            if (!kangaroo::finite(k.p) || !kangaroo::finite(k.v)) {
                ++result.nonFinite;
            }
        }
        mob.events.clear();
    }
    result.largestBoost = spin.largest;
    result.limited = spin.limited;
    result.flung = mob.flungTotal;
    result.returned = mob.returnedTotal;
    return result;
}

// The bound is the largest push a mob can legitimately give: a stampede of
// six animals of mass about 1.2 leaving at up to 3.5 units a second on a
// fast-spinning planet (the hop planner corrects for Coriolis), about 1.3
// from the axle, hands the planet up to about 13 units of angular impulse,
// which against an inertia of 40 is 0.33 rad/s or 19 degrees a second. A
// loop that feeds itself does not stop at any bound, so the checks that tell
// the two apart are the limit never being reached and the extra spin dying
// away once the mob is calm.
void spinFeedbackStaysBounded()
{
    // Set to 162 degrees a second, with a kick of up to 130 on top roughly
    // every half second, decaying as the beat visualizer's does.
    const auto beats = [](const float t) {
        const float period = 0.47F;
        const float since = std::fmod(t, period);
        const int beat = static_cast<int>(t / period);
        const float strength = 110.0F + 20.0F * static_cast<float>(beat % 3) / 2.0F;
        return degrees(162.0F + strength * std::exp(-2.9F * since));
    };
    const LoopResult music = closedLoop(7, 120.0F, beats);
    std::printf("info: under beats the mob was flung %d times and %d came back; largest extra spin %.2f deg/s\n",
                music.flung, music.returned, music.largestBoost * 180.0 / 3.14159265);
    check(music.nonFinite == 0, "two minutes of beats with the planet answering the mob stay finite");
    check(music.limited == 0, "and the extra spin never reaches its limit");
    check(music.largestBoost < degrees(20.0F),
          format("the mob's pushes add at most %.2f deg/s to the planet", music.largestBoost * 180.0 / 3.14159265));

    // Ctrl++ four times, six seconds apart, then back to the default.
    const auto presses = [](const float t) {
        const int step = std::min(static_cast<int>(t / 6.0F), 4);
        const float set = t < 30.0F ? 75.0F * std::pow(1.35F, static_cast<float>(step)) : 75.0F;
        return degrees(set);
    };
    const LoopResult spun = closedLoop(9, 80.0F, presses);
    check(spun.flung >= 3, format("spun up four presses the mob is thrown off (%.0f flung)", spun.flung));
    check(spun.limited == 0 && spun.largestBoost < degrees(20.0F),
          format("and the planet still takes no more than %.2f deg/s from it",
                 spun.largestBoost * 180.0 / 3.14159265));
    check(spun.settledBoost < degrees(3.0F),
          format("once calm, the extra spin averages %.2f deg/s over the last twenty seconds",
                 spun.settledBoost * 180.0 / 3.14159265));
    check(spun.returned >= 3, format("slowed down again, they come back (%.0f)", spun.returned));
}

// The rate the mob feels. A wrap of the step function's angle or a jump of
// the drawn one read as a rate once threw the whole mob off in one frame.
void rateMeterReadsTurnsNotAngles()
{
    kangaroo::RateMeter meter;
    const float tick = 1.0F / 60.0F;
    // Steady at 75 degrees a second, crossing 360 on the way.
    float angle = 358.0F;
    float rate = 0.0F;
    float worst = 0.0F;
    for (int frame = 0; frame < 30; ++frame) {
        const float next = std::fmod(angle + 75.0F * tick, 360.0F);
        meter.stepped(angle, next, tick);
        rate = meter.frame(next - angle, tick);
        worst = std::max(worst, std::fabs(rate - degrees(75.0F)));
        angle = next;
    }
    check(worst < 1e-3F, format("a step that wraps past 360 still reads as 75 deg/s (worst error %.2g rad/s)", worst));

    // Without the step function, a reset moves the drawn angle by 150 degrees
    // in one frame. That is not a turn.
    kangaroo::RateMeter drawn;
    for (int frame = 0; frame < 10; ++frame) {
        rate = drawn.frame(75.0F * tick, tick);
    }
    rate = drawn.frame(150.0F, tick);
    check(std::fabs(rate - degrees(75.0F)) < 1e-3F && drawn.jumpsIgnored == 1,
          "a one-frame jump of the drawn angle is not read as a turn");
    rate = drawn.frame(75.0F * tick, tick);
    check(std::fabs(rate - degrees(75.0F)) < 1e-3F, "and the rate after it is the steady one");

    // A real change that large lasts, and is believed once it has.
    for (int frame = 0; frame < 3; ++frame) {
        rate = drawn.frame(1440.0F * tick, tick);
    }
    check(std::fabs(rate - degrees(1440.0F)) < 1e-2F,
          format("a jump to 1440 deg/s that lasts three frames is believed (%.0f deg/s)", rate * 180.0F / 3.14159265F));
}

void sameSeedSameMob()
{
    kangaroo::Mob a;
    kangaroo::Mob b;
    kangaroo::Settings settings;
    settings.seed = 11;
    a.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
    b.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
    runMob(a, 20.0F, degrees(75.0F));
    runMob(b, 20.0F, degrees(75.0F));
    float gap = 0.0F;
    for (std::size_t i = 0; i < a.roos.size(); ++i) {
        gap = std::max(gap, kangaroo::length(a.roos[i].p - b.roos[i].p));
    }
    check(gap == 0.0F, "the same seed reproduces the same mob exactly");
}

// The hop planner's arc must clear the ground when nothing is in the way.
// When it did not, every ordinary hop became a hop on the spot, gravity's tilt
// toward the face centre carried each one a little further in, and on a
// planet at rest the whole mob ended up standing inside itself at the centres.
void hopsGoWhereTheyAim()
{
    for (const float spin : {0.0F, 75.0F}) {
        kangaroo::Mob mob;
        kangaroo::Settings settings;
        settings.seed = 1;
        mob.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
        double radius = 0.0;
        int samples = 0;
        for (int frame = 0; frame < 60 * 60; ++frame) {
            mob.advance(1.0F / 60.0F, degrees(spin));
            (void)mob.takeImpulse();
            mob.events.clear();
            if (frame > 10 * 60 && frame % 30 == 0) {
                for (const kangaroo::Kangaroo& k : mob.roos) {
                    if (k.present() && k.grounded()) {
                        radius += kangaroo::length(kangaroo::flatten(k.p, kangaroo::faceNormal(k.face)));
                        ++samples;
                    }
                }
            }
        }
        const double share = mob.hopsPlanned > 0
            ? static_cast<double>(mob.hopsOnTheSpot) / mob.hopsPlanned : 1.0;
        check(share < 0.05, format("at %.0f deg/s, %.1f%% of planned hops fell back to hopping on the spot",
                                   spin, 100.0 * share));
        check(samples > 0 && radius / samples > 0.3,
              format("and the mob stays spread over its faces (mean %.2f from a face centre)",
                     samples > 0 ? radius / samples : 0.0));
    }
}

// A lost kangaroo comes back as soon as the planet can hold it, which on a
// fast planet means a drop from just above the faces nearest the axle. When
// they were always dropped from high up, a planet turning at 200 degrees a
// second kept the whole mob in space.
void lostKangaroosComeBackOnAFastPlanet()
{
    kangaroo::Mob mob;
    kangaroo::Settings settings;
    settings.seed = 4;
    mob.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
    for (int frame = 0; frame < 20 * 60; ++frame) {
        mob.advance(1.0F / 60.0F, degrees(260.0F));
        (void)mob.takeImpulse();
        mob.events.clear();
    }
    const int flung = mob.flungTotal;
    for (int frame = 0; frame < 40 * 60; ++frame) {
        mob.advance(1.0F / 60.0F, degrees(200.0F));
        (void)mob.takeImpulse();
        mob.events.clear();
    }
    int home = 0;
    for (const kangaroo::Kangaroo& k : mob.roos) {
        home += (k.present() && k.activity != kangaroo::Activity::Flung) ? 1 : 0;
    }
    check(flung >= 3 && mob.returnedTotal >= 3,
          format("thrown off at 260 deg/s (%.0f), they come back at 200 (%.0f returned)", flung, mob.returnedTotal));
    check(home >= 4, format("and most of the mob is on the planet again (%.0f)", home));
}

// The application runs under the user's locale. Under one that writes a
// decimal comma, printf-formatted numbers made every event that carried one
// invalid JSON, and the agent passed it on as a raw string.
void eventsAreJsonUnderAnyLocale()
{
    const char* chosen = nullptr;
    for (const char* name : {"hr_HR.UTF-8", "hr_HR.utf8", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "ru_RU.UTF-8"}) {
        if (std::setlocale(LC_ALL, name) != nullptr) {
            chosen = name;
            break;
        }
    }
    if (chosen == nullptr) {
        std::printf("info: no locale with a decimal comma is installed, so events were not checked under one\n");
        return;
    }
    kangaroo::Mob mob;
    kangaroo::Settings settings;
    settings.mob = 2;
    mob.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
    for (int frame = 0; frame < 120 && mob.roos[0].gait != kangaroo::Gait::Stand; ++frame) {
        mob.advance(1.0F / 60.0F, degrees(75.0F));
    }
    const kangaroo::V3 n = kangaroo::faceNormal(mob.roos[0].face);
    mob.launch(0, n * 9.0F + mob.roos[0].heading * 2.0F);
    bool flung = false;
    bool commaInNumber = false;
    for (int frame = 0; frame < 60 * 15; ++frame) {
        mob.advance(1.0F / 60.0F, degrees(75.0F));
        (void)mob.takeImpulse();
        for (const kangaroo::Event& event : mob.events) {
            flung = flung || event.name == "kangaroo.flung";
            const std::string& json = event.json;
            for (std::size_t i = 1; i + 1 < json.size(); ++i) {
                if (json[i] == ',' && std::isdigit(static_cast<unsigned char>(json[i - 1]))
                    && std::isdigit(static_cast<unsigned char>(json[i + 1]))) {
                    commaInNumber = true;
                }
            }
        }
        mob.events.clear();
    }
    std::setlocale(LC_ALL, "C");
    check(flung, format("a kangaroo thrown at 9 units a second is flung", 0.0));
    check(!commaInNumber, std::string("under ") + chosen + ", the events' numbers still use a decimal point");
}

// The beat visualizer bound before the planet, as the application runs them:
// it kicks the rate on every beat, and the planet passes on a share of that
// through a short lag. The planet should still speed up with the music and
// show the beats, without the mob being startled on every one.
void musicReachesThePlanetGently()
{
    const auto song = [](const float share, const float attack, int& flung, int& alarms, float& mean, float& peak) {
        kangaroo::Mob mob;
        kangaroo::Settings settings;
        settings.seed = 7;
        mob.reset(settings, kangaroo::V3{0.72F, 1.0F, 0.31F});
        kangaroo::SpinResponse spin;
        kangaroo::RateMeter meter;
        kangaroo::InheritedSpin inherited;
        const float dt = 1.0F / 60.0F;
        const float set = 162.0F;
        float angle = 24.0F;
        double sum = 0.0;
        peak = 0.0F;
        const int frames = 120 * 60;
        for (int frame = 0; frame < frames; ++frame) {
            const float t = static_cast<float>(frame) * dt;
            const float since = std::fmod(t, 0.27F);
            const int beat = static_cast<int>(t / 0.27F);
            const float kick = (110.0F + 10.0F * static_cast<float>(beat % 3)) * std::exp(-2.9F * since);
            const float displaced = std::fmod(angle + (set + kick) * dt + 360.0F, 360.0F);
            float turned = inherited.turn(angle, displaced, set, dt, share, attack);
            spin.apply(mob.takeImpulse(), settings.planetInertia, dt);
            turned += static_cast<float>(spin.boost) * dt * 180.0F / 3.14159265F;
            const float next = std::fmod(angle + turned + 360.0F, 360.0F);
            meter.stepped(angle, next, dt);
            const float omega = meter.frame(next - angle, dt) * 180.0F / 3.14159265F;
            angle = next;
            mob.advance(dt, omega * 3.14159265F / 180.0F);
            mob.events.clear();
            if (frame > 10 * 60) {
                sum += omega;
                peak = std::max(peak, omega);
            }
        }
        mean = static_cast<float>(sum / (frames - 10 * 60 - 1));
        flung = mob.flungTotal;
        alarms = mob.alarmsTotal;
    };
    int flung = 0;
    int alarms = 0;
    float mean = 0.0F;
    float peak = 0.0F;
    song(kangaroo::InheritedSpin::defaultShare, kangaroo::InheritedSpin::attackSeconds, flung, alarms, mean, peak);
    std::printf("info: a song at 162 deg/s with the default share: mean %.1f, peak %.1f deg/s, %d flung, %d alarms\n",
                mean, peak, flung, alarms);
    check(mean > 185.0F, format("the music still speeds the planet up (mean %.1f deg/s, set 162)", mean));
    check(peak - mean > 5.0F, format("and each beat still shows, as a surge of %.1f deg/s", peak - mean));
    check(alarms <= 4 && flung <= 10,
          format("without startling the mob on every beat (%.0f alarms, %.0f flung in two minutes)", alarms, flung));
    int fullFlung = 0;
    int fullAlarms = 0;
    song(1.0F, 0.0F, fullFlung, fullAlarms, mean, peak);
    check(fullAlarms >= 10,
          format("passed on whole and at once, the kicks do startle it (%.0f alarms), so the check above means something",
                 fullAlarms));
}

void simulationChecks()
{
    rigStaysWhole();
    walkingFolds();
    waterFindsItsLevel();
    flightConservesJacobi();
    spinFeedbackStaysBounded();
    rateMeterReadsTurnsNotAngles();
    hopsGoWhereTheyAim();
    lostKangaroosComeBackOnAFastPlanet();
    eventsAreJsonUnderAnyLocale();
    musicReachesThePlanetGently();
    sameSeedSameMob();
    mobLivesOnThePlanet();
    spinThrowsThemOff();
}

} // namespace

int main()
{
    gravityAgainstQuadrature();
    attractionIsGradient();
    poissonAndLaplace();
    symmetryAndFarField();
    reportProfile();
    simulationChecks();

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all checks passed\n");
    return EXIT_SUCCESS;
}
