#pragma once

// The gravitational field of a uniform solid cube, in closed form, and what a
// body standing on it feels while it turns about a fixed axle.
//
// A cube is a poor planet in an interesting way. Its attraction points at the
// centre only along the lines through face centres, edges and corners; in
// between it tilts toward the nearest face centre. Measured against gravity a
// flat face is therefore a bowl: its centre is the lowest point, the middle of
// an edge is a ridge where "down" is at 45 degrees to both faces, and a corner
// is a peak. Water collects at the face centres and anything walking toward an
// edge is walking uphill.
//
// The potential of a homogeneous rectangular prism has an exact expression
// (D. Nagy, "The gravitational attraction of a right rectangular prism",
// Geophysics 31, 1966), and so does its attraction. Both are sums over the
// eight corners of a function of the corner's position relative to the field
// point. They hold inside, on and outside the solid, which matters here
// because a kangaroo's feet and a billabong's floor are on the surface itself.
//
// Units: the cube spans [-1, 1] on each axis and G times the density is 1.
// Callers scale the result to whatever surface gravity they want. Standard
// library only, so the tests can check it without Qt or a display.

#include <array>
#include <cmath>

namespace cube_gravity {

using Vec = std::array<double, 3>;

inline constexpr double halfSide = 1.0;
inline constexpr double pi = 3.14159265358979323846;

// The mass of the cube with G times the density set to 1: the volume.
inline constexpr double mass = 8.0 * halfSide * halfSide * halfSide;

namespace detail {

// ln(z + r) where r = sqrt(x^2 + y^2 + z^2) and xy2 = x^2 + y^2.
//
// For negative z the sum cancels catastrophically, so it is rewritten as
// ln((x^2 + y^2) / (r - z)), which is the same number computed without the
// subtraction. When x and y are both zero and z is negative the logarithm
// diverges, but every term that uses it is multiplied by x or y there, so 0 is
// the value of the product and is returned for it.
inline double logPlus(const double z, const double r, const double xy2)
{
    if (z >= 0.0) {
        return r + z > 0.0 ? std::log(r + z) : 0.0;
    }
    if (xy2 <= 0.0) {
        return 0.0;
    }
    return std::log(xy2 / (r - z));
}

// x * atan(y z / (x r)), which tends to 0 as x does even though the atan
// argument diverges.
inline double xAtan(const double x, const double y, const double z, const double r)
{
    if (x == 0.0 || r == 0.0) {
        return 0.0;
    }
    return x * std::atan((y * z) / (x * r));
}

// The corner function whose alternating sum over the eight corners is the
// integral of 1/r over the box.
inline double potentialTerm(const double x, const double y, const double z)
{
    const double x2 = x * x;
    const double y2 = y * y;
    const double z2 = z * z;
    const double r = std::sqrt(x2 + y2 + z2);
    return x * y * logPlus(z, r, x2 + y2)
        + y * z * logPlus(x, r, y2 + z2)
        + z * x * logPlus(y, r, z2 + x2)
        - 0.5 * x * xAtan(x, y, z, r)
        - 0.5 * y * xAtan(y, z, x, r)
        - 0.5 * z * xAtan(z, x, y, r);
}

// The corner function whose alternating sum is the integral of x/r^3, which is
// the x component of the attraction. The y and z components are the same
// function with the arguments rotated.
inline double attractionTerm(const double x, const double y, const double z)
{
    const double x2 = x * x;
    const double y2 = y * y;
    const double z2 = z * z;
    const double r = std::sqrt(x2 + y2 + z2);
    return -(y * logPlus(z, r, x2 + y2) + z * logPlus(y, r, x2 + z2) - xAtan(x, y, z, r));
}

} // namespace detail

// The Newtonian potential integral N(p), the integral of 1/|q - p| over the
// cube. It is positive and largest at the centre. The potential energy per
// unit mass is -N.
inline double potential(const Vec& p)
{
    double sum = 0.0;
    for (int i = 0; i < 2; ++i) {
        const double x = (i == 0 ? -halfSide : halfSide) - p[0];
        for (int j = 0; j < 2; ++j) {
            const double y = (j == 0 ? -halfSide : halfSide) - p[1];
            for (int k = 0; k < 2; ++k) {
                const double z = (k == 0 ? -halfSide : halfSide) - p[2];
                const double sign = ((i + j + k) % 2 == 1) ? -1.0 : 1.0;
                sum += sign * detail::potentialTerm(x, y, z);
            }
        }
    }
    // The upper corner of all three axes carries a plus sign in the
    // alternating sum, and with three axes it is the term where i+j+k = 3. The
    // parity above gives it a minus, so the sum comes out negated.
    return -sum;
}

// The attraction at p: the integral of (q - p)/|q - p|^3 over the cube, which
// is the gradient of N. It points into the cube.
//
// Beyond thirty half-sides the corner sums lose digits to cancellation, and
// the cube's field is a point mass's to better than one part in 10^7 there,
// because a cube has no quadrupole moment: the first correction is the
// hexadecapole, falling off two powers of distance faster.
inline Vec attraction(const Vec& p)
{
    const double distanceSquared = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
    if (distanceSquared > 900.0 * halfSide * halfSide) {
        const double distance = std::sqrt(distanceSquared);
        const double scale = -mass / (distanceSquared * distance);
        return Vec{scale * p[0], scale * p[1], scale * p[2]};
    }

    Vec result{0.0, 0.0, 0.0};
    for (int i = 0; i < 2; ++i) {
        const double x = (i == 0 ? -halfSide : halfSide) - p[0];
        for (int j = 0; j < 2; ++j) {
            const double y = (j == 0 ? -halfSide : halfSide) - p[1];
            for (int k = 0; k < 2; ++k) {
                const double z = (k == 0 ? -halfSide : halfSide) - p[2];
                const double sign = ((i + j + k) % 2 == 1) ? -1.0 : 1.0;
                result[0] += sign * detail::attractionTerm(x, y, z);
                result[1] += sign * detail::attractionTerm(y, z, x);
                result[2] += sign * detail::attractionTerm(z, x, y);
            }
        }
    }
    // The same parity convention as potential(), so the sum is negated here
    // too.
    return Vec{-result[0], -result[1], -result[2]};
}

inline double length(const Vec& v)
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The attraction at the centre of a face. Callers divide their chosen surface
// gravity by this to get the factor that turns attraction() into an
// acceleration.
inline double faceCentreAttraction()
{
    return length(attraction(Vec{0.0, halfSide, 0.0}));
}

// A planet that turns at a rate omega (radians per second) about a fixed unit
// axle through its centre, with its gravity scaled so the attraction at a face
// centre is `surfaceGravity`.
//
// Everything below is in the planet's own frame, the one its surface is still
// in. A body at rest on the surface feels the attraction plus the centrifugal
// term; a moving one also feels Coriolis, and a planet whose rate is changing
// adds the Euler term. Those three are what the kangaroos are integrated under.
struct SpinningCube {
    double surfaceGravity = 16.0;
    Vec axle{0.0, 1.0, 0.0};
    double omega = 0.0;
    double omegaRate = 0.0;

    [[nodiscard]] double gravityScale() const { return surfaceGravity / faceCentreAttraction(); }

    // The distance-squared from the axle, times omega^2 over two, is the
    // centrifugal potential's magnitude.
    [[nodiscard]] Vec offAxle(const Vec& p) const
    {
        const double along = p[0] * axle[0] + p[1] * axle[1] + p[2] * axle[2];
        return Vec{p[0] - along * axle[0], p[1] - along * axle[1], p[2] - along * axle[2]};
    }

    // Potential energy per unit mass in the turning frame. Water at rest in
    // that frame has a surface on which this is constant.
    [[nodiscard]] double effectivePotential(const Vec& p) const
    {
        const Vec off = offAxle(p);
        const double offSquared = off[0] * off[0] + off[1] * off[1] + off[2] * off[2];
        return -gravityScale() * potential(p) - 0.5 * omega * omega * offSquared;
    }

    // What a body at rest in the turning frame feels: attraction plus the
    // centrifugal push away from the axle.
    [[nodiscard]] Vec effectiveGravity(const Vec& p) const
    {
        const Vec a = attraction(p);
        const Vec off = offAxle(p);
        const double k = gravityScale();
        const double w2 = omega * omega;
        return Vec{k * a[0] + w2 * off[0], k * a[1] + w2 * off[1], k * a[2] + w2 * off[2]};
    }

    // The full acceleration of a body moving at v in the turning frame:
    // effective gravity, Coriolis -2 w x v, and the Euler term -(dw/dt) x p.
    [[nodiscard]] Vec acceleration(const Vec& p, const Vec& v) const
    {
        const Vec g = effectiveGravity(p);
        const Vec w{omega * axle[0], omega * axle[1], omega * axle[2]};
        const Vec dw{omegaRate * axle[0], omegaRate * axle[1], omegaRate * axle[2]};
        const Vec coriolis{
            -2.0 * (w[1] * v[2] - w[2] * v[1]),
            -2.0 * (w[2] * v[0] - w[0] * v[2]),
            -2.0 * (w[0] * v[1] - w[1] * v[0]),
        };
        const Vec euler{
            -(dw[1] * p[2] - dw[2] * p[1]),
            -(dw[2] * p[0] - dw[0] * p[2]),
            -(dw[0] * p[1] - dw[1] * p[0]),
        };
        return Vec{g[0] + coriolis[0] + euler[0],
                   g[1] + coriolis[1] + euler[1],
                   g[2] + coriolis[2] + euler[2]};
    }
};

} // namespace cube_gravity
