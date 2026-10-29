#pragma once

// The tesseract and the ways of seeing it from three dimensions.
//
// A tesseract is the four-dimensional cube: the points whose four coordinates
// all lie in [-1, 1]. It has 16 vertices, 32 edges, 24 square faces and 8
// cubic cells, each cell the set where one coordinate is pinned at +1 or -1.
// Nothing four-dimensional can be drawn directly, so it is shown three ways:
//
//   a perspective projection, which divides by distance along w the way a
//   photograph divides by distance along depth: the cell nearer the eye in w
//   comes out as a big cube, the far one as a small cube inside it, and the
//   other six as the frustums joining them;
//
//   a stereographic projection: the vertices are pushed out onto the
//   three-sphere and projected from one of its poles, which keeps angles and
//   turns the edges into circular arcs;
//
//   a slice: the three-dimensional cross-section cut by a hyperplane, which is
//   what a three-dimensional being would see of a tesseract passing through
//   its space. Cut square to an axis it is a cube; cut corner first it grows
//   from a point to a tetrahedron, is trimmed into a truncated tetrahedron, and
//   is an octahedron at the middle.
//
// Standard library only, so the selftest checks it without Qt or a display.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace tesseract {

inline constexpr double pi = 3.14159265358979323846;

struct V4 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;

    [[nodiscard]] double operator[](const int i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    double& operator[](const int i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    V4 operator+(const V4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    V4 operator-(const V4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    V4 operator*(const double s) const { return {x * s, y * s, z * s, w * s}; }
};

inline double dot(const V4& a, const V4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline double length(const V4& a) { return std::sqrt(dot(a, a)); }
inline V4 normalize(const V4& a)
{
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : V4{0.0, 0.0, 0.0, 1.0};
}

struct V3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(const double s) const { return {x * s, y * s, z * s}; }
};

inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(const V3& a)
{
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : V3{0.0, 0.0, 1.0};
}

// ---------------------------------------------------------------------------
// The polytope
// ---------------------------------------------------------------------------

inline constexpr int vertexCount = 16;
inline constexpr int edgeCount = 32;
inline constexpr int squareCount = 24;
inline constexpr int cellCount = 8;

// Vertex i has coordinate k at +1 where bit k of i is set and at -1 where it
// is clear.
inline V4 vertex(const int i)
{
    return {(i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0, (i & 8) ? 1.0 : -1.0};
}

// An edge joins two vertices that differ in one coordinate, its axis.
struct Edge {
    int a = 0;
    int b = 0;
    int axis = 0;
};

inline const std::array<Edge, edgeCount>& edges()
{
    static const std::array<Edge, edgeCount> all = [] {
        std::array<Edge, edgeCount> list{};
        int n = 0;
        for (int axis = 0; axis < 4; ++axis) {
            for (int i = 0; i < vertexCount; ++i) {
                if ((i & (1 << axis)) == 0) {
                    list[n++] = Edge{i, i | (1 << axis), axis};
                }
            }
        }
        return list;
    }();
    return all;
}

// Cell c pins coordinate c / 2 at +1 when c is even and at -1 when it is odd:
// 0 is x = +1, 1 is x = -1, and so on to 7, w = -1.
inline int cellAxis(const int cell) { return cell / 2; }
inline double cellSign(const int cell) { return (cell % 2 == 0) ? 1.0 : -1.0; }
inline bool vertexInCell(const int vertexIndex, const int cell)
{
    return vertex(vertexIndex)[cellAxis(cell)] == cellSign(cell);
}

// A square face pins two coordinates and lets the other two vary; it is where
// two cells meet. Its corners are listed in order round the square.
struct Square {
    std::array<int, 4> corners{};
    int cellA = 0;
    int cellB = 0;
};

inline const std::array<Square, squareCount>& squares()
{
    static const std::array<Square, squareCount> all = [] {
        std::array<Square, squareCount> list{};
        int n = 0;
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                // p and q are pinned; the free axes are the other two.
                int free0 = -1;
                int free1 = -1;
                for (int k = 0; k < 4; ++k) {
                    if (k != p && k != q) {
                        (free0 < 0 ? free0 : free1) = k;
                    }
                }
                for (int signs = 0; signs < 4; ++signs) {
                    const int base = ((signs & 1) ? (1 << p) : 0) | ((signs & 2) ? (1 << q) : 0);
                    Square s;
                    s.corners = {base, base | (1 << free0), base | (1 << free0) | (1 << free1), base | (1 << free1)};
                    s.cellA = p * 2 + ((signs & 1) ? 0 : 1);
                    s.cellB = q * 2 + ((signs & 2) ? 0 : 1);
                    list[n++] = s;
                }
            }
        }
        return list;
    }();
    return all;
}

// ---------------------------------------------------------------------------
// Turning in four dimensions
// ---------------------------------------------------------------------------

// A rotation in four dimensions turns a plane, not about an axis: the two
// coordinates of the plane mix and the other two stay put. There are six
// coordinate planes.
enum Plane : int { XY = 0, XZ = 1, XW = 2, YZ = 3, YW = 4, ZW = 5 };
inline constexpr int planeCount = 6;
inline constexpr std::array<std::array<int, 2>, planeCount> planeAxes{{{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}}};
inline constexpr std::array<const char*, planeCount> planeNames{"xy", "xz", "xw", "yz", "yw", "zw"};

struct M4 {
    std::array<std::array<double, 4>, 4> m{};

    static M4 identity()
    {
        M4 r;
        for (int i = 0; i < 4; ++i) {
            r.m[i][i] = 1.0;
        }
        return r;
    }
    V4 operator*(const V4& v) const
    {
        V4 r;
        for (int i = 0; i < 4; ++i) {
            r[i] = m[i][0] * v.x + m[i][1] * v.y + m[i][2] * v.z + m[i][3] * v.w;
        }
        return r;
    }
    M4 operator*(const M4& o) const
    {
        M4 r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                for (int k = 0; k < 4; ++k) {
                    r.m[i][j] += m[i][k] * o.m[k][j];
                }
            }
        }
        return r;
    }
    [[nodiscard]] M4 transposed() const
    {
        M4 r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                r.m[i][j] = m[j][i];
            }
        }
        return r;
    }
};

inline M4 planeRotation(const Plane plane, const double angle)
{
    M4 r = M4::identity();
    const int i = planeAxes[plane][0];
    const int j = planeAxes[plane][1];
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    r.m[i][i] = c;
    r.m[i][j] = -s;
    r.m[j][i] = s;
    r.m[j][j] = c;
    return r;
}

// The six plane rotations applied in a fixed order. Equal turns in two planes
// that share no axis, such as xw and yz, make a Clifford double rotation:
// every point moves round a great circle at the same rate, and nothing in the
// tesseract is still.
inline M4 rotation(const std::array<double, planeCount>& angles)
{
    M4 r = M4::identity();
    for (int p = 0; p < planeCount; ++p) {
        if (angles[p] != 0.0) {
            r = planeRotation(static_cast<Plane>(p), angles[p]) * r;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Projections into three dimensions
// ---------------------------------------------------------------------------

// From an eye on the w axis at w = eye, looking toward w = -infinity: the
// point is scaled by how far it is from the eye, relative to the w = 0
// hyperplane, which keeps its size.
inline double perspectiveScale(const V4& p, const double eye) { return eye / std::max(eye - p.w, 1e-3); }
inline V3 perspective(const V4& p, const double eye)
{
    const double s = perspectiveScale(p, eye);
    return {p.x * s, p.y * s, p.z * s};
}

// Onto the unit three-sphere, then from its pole at w = +1 to the w = 0
// hyperplane. Angles are kept, so circles stay circles and the arcs of great
// circles that edges become are drawn as circular arcs. Points within `limit`
// of the pole are held at that distance rather than sent to infinity.
inline V3 stereographic(const V4& p, const double limit = 0.04)
{
    const V4 u = normalize(p);
    const double s = 1.0 / std::max(1.0 - u.w, limit);
    return {u.x * s, u.y * s, u.z * s};
}

// The point a fraction t of the way along the great circle from a to b, both
// on the unit three-sphere.
inline V4 slerp(const V4& a, const V4& b, const double t)
{
    const double c = std::clamp(dot(a, b), -1.0, 1.0);
    const double angle = std::acos(c);
    if (angle < 1e-9) {
        return a;
    }
    const double s = std::sin(angle);
    return a * (std::sin((1.0 - t) * angle) / s) + b * (std::sin(t * angle) / s);
}

// ---------------------------------------------------------------------------
// Slices
// ---------------------------------------------------------------------------

// An orthonormal basis for the hyperplane perpendicular to the unit normal n,
// built from the coordinate axes by Gram-Schmidt, leaving out the axis n leans
// on most, so the basis is the same every time for the same normal.
inline std::array<V4, 3> hyperplaneBasis(const V4& n)
{
    int skip = 0;
    for (int k = 1; k < 4; ++k) {
        if (std::fabs(n[k]) > std::fabs(n[skip])) {
            skip = k;
        }
    }
    std::array<V4, 3> basis{};
    int filled = 0;
    for (int k = 0; k < 4 && filled < 3; ++k) {
        if (k == skip) {
            continue;
        }
        V4 e{};
        e[k] = 1.0;
        e = e - n * dot(e, n);
        for (int j = 0; j < filled; ++j) {
            e = e - basis[j] * dot(e, basis[j]);
        }
        basis[filled++] = normalize(e);
    }
    return basis;
}

// A face of a slice: the polygon one cell leaves in the cutting hyperplane,
// its corners in order counter-clockwise seen from outside the solid. Where the
// hyperplane contains a whole square of the tesseract, the face lies in both
// cells that square joins and is listed for the first.
struct SliceFace {
    int cell = 0;
    std::vector<int> corners;
};

struct Slice {
    std::vector<V4> points4; // in the tesseract's own frame
    std::vector<V3> points;  // in the hyperplane's basis
    std::vector<SliceFace> faces;
    int edges = 0;

    [[nodiscard]] bool solid() const { return faces.size() >= 4; }
};

// The furthest any vertex lies along n, so a cut at offsets within plus or
// minus this meets the tesseract.
inline double reach(const V4& n)
{
    return std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z) + std::fabs(n.w);
}

// The cross-section by the hyperplane n . p = offset, with n a unit vector in
// the tesseract's own frame and basis an orthonormal basis of the hyperplane
// for the three-dimensional coordinates.
//
// The cross-section of a convex polytope by a hyperplane is convex, and each
// of its faces is the cut through one cell: a cell is a cube, and its cut is
// the polygon where the hyperplane crosses that cube's edges. So the faces are
// found cell by cell, from the points where the hyperplane crosses the edges,
// and that set of faces is the convex hull of those points. A vertex lying on
// the hyperplane is one point shared by every edge through it, which is what
// keeps a cut through corners, such as the octahedron at the middle of a
// corner-first sweep, from counting each corner several times.
inline Slice slice(const V4& n, const double offset, const std::array<V4, 3>& basis)
{
    Slice result;
    constexpr double onPlane = 1e-9;
    std::array<double, vertexCount> side{};
    for (int i = 0; i < vertexCount; ++i) {
        side[i] = dot(n, vertex(i)) - offset;
    }
    // Point ids: a vertex on the plane maps to one point, a strict crossing on
    // an edge to its own point.
    std::array<int, vertexCount> vertexPoint;
    vertexPoint.fill(-1);
    std::array<int, edgeCount> edgePoint;
    edgePoint.fill(-1);
    const auto addPoint = [&](const V4& p) {
        result.points4.push_back(p);
        result.points.push_back({dot(p, basis[0]), dot(p, basis[1]), dot(p, basis[2])});
        return static_cast<int>(result.points4.size()) - 1;
    };
    for (int i = 0; i < vertexCount; ++i) {
        if (std::fabs(side[i]) <= onPlane) {
            vertexPoint[i] = addPoint(vertex(i));
        }
    }
    const auto& allEdges = edges();
    for (int e = 0; e < edgeCount; ++e) {
        const double sa = side[allEdges[e].a];
        const double sb = side[allEdges[e].b];
        if (std::fabs(sa) <= onPlane || std::fabs(sb) <= onPlane) {
            continue;
        }
        if ((sa < 0.0) != (sb < 0.0)) {
            const double t = sa / (sa - sb);
            edgePoint[e] = addPoint(vertex(allEdges[e].a) + (vertex(allEdges[e].b) - vertex(allEdges[e].a)) * t);
        }
    }

    for (int cell = 0; cell < cellCount; ++cell) {
        const int axis = cellAxis(cell);
        const double sign = cellSign(cell);
        // The cell's outward normal, as it lies in the hyperplane. A cell
        // parallel to the hyperplane has none there: the cut either misses it
        // or is the whole cell, which the other cells' faces then enclose.
        V4 outward{};
        outward[axis] = sign;
        const V4 inPlane = outward - n * dot(outward, n);
        if (length(inPlane) < 1e-9) {
            continue;
        }
        const V3 normal3 = normalize(V3{dot(inPlane, basis[0]), dot(inPlane, basis[1]), dot(inPlane, basis[2])});
        std::vector<int> ids;
        for (int i = 0; i < vertexCount; ++i) {
            if (vertexPoint[i] >= 0 && vertexInCell(i, cell)) {
                ids.push_back(vertexPoint[i]);
            }
        }
        for (int e = 0; e < edgeCount; ++e) {
            if (edgePoint[e] >= 0 && allEdges[e].axis != axis && vertexInCell(allEdges[e].a, cell)) {
                ids.push_back(edgePoint[e]);
            }
        }
        if (ids.size() < 3) {
            continue;
        }
        V3 centre{};
        for (const int id : ids) {
            centre = centre + result.points[id];
        }
        centre = centre * (1.0 / static_cast<double>(ids.size()));
        // Round the centre, counter-clockwise about the outward normal.
        const V3 u = normalize(result.points[ids[0]] - centre);
        const V3 v = cross(normal3, u);
        std::sort(ids.begin(), ids.end(), [&](const int a, const int b) {
            const V3 pa = result.points[a] - centre;
            const V3 pb = result.points[b] - centre;
            return std::atan2(dot(pa, v), dot(pa, u)) < std::atan2(dot(pb, v), dot(pb, u));
        });
        // A cell met only along an edge leaves a polygon with no area.
        double area = 0.0;
        for (std::size_t k = 1; k + 1 < ids.size(); ++k) {
            area += length(cross(result.points[ids[k]] - result.points[ids[0]],
                                 result.points[ids[k + 1]] - result.points[ids[0]]));
        }
        if (area < 1e-9) {
            continue;
        }
        // A hyperplane that contains a whole square of the tesseract gets
        // that square from both cells it joins, facing the same way. It is
        // one face of the solid, kept once, for the first of the two cells.
        std::vector<int> key = ids;
        std::sort(key.begin(), key.end());
        bool seen = false;
        for (const SliceFace& other : result.faces) {
            std::vector<int> otherKey = other.corners;
            std::sort(otherKey.begin(), otherKey.end());
            seen = seen || otherKey == key;
        }
        if (seen) {
            continue;
        }
        result.faces.push_back(SliceFace{cell, ids});
    }
    int corners = 0;
    for (const SliceFace& face : result.faces) {
        corners += static_cast<int>(face.corners.size());
    }
    result.edges = corners / 2;
    return result;
}

// The points a slice's faces use, which is fewer than it found when the plane
// grazed a corner of a cell it does not otherwise cut.
inline int usedPoints(const Slice& s)
{
    std::vector<bool> used(s.points.size(), false);
    for (const SliceFace& face : s.faces) {
        for (const int id : face.corners) {
            used[id] = true;
        }
    }
    return static_cast<int>(std::count(used.begin(), used.end(), true));
}

// What the solid is, by its faces and, where the faces alone do not settle it,
// by its edge lengths.
inline std::string nameOf(const Slice& s)
{
    if (!s.solid()) {
        return s.faces.empty() && s.points.empty() ? "nothing" : "a flat sliver";
    }
    const int v = usedPoints(s);
    const int f = static_cast<int>(s.faces.size());
    std::array<int, 16> sides{};
    for (const SliceFace& face : s.faces) {
        sides[std::min<std::size_t>(face.corners.size(), 15)] += 1;
    }
    const auto edgeLengths = [&](const SliceFace& face) {
        std::vector<double> lengths;
        for (std::size_t k = 0; k < face.corners.size(); ++k) {
            lengths.push_back(length(s.points[face.corners[k]] - s.points[face.corners[(k + 1) % face.corners.size()]]));
        }
        return lengths;
    };
    const auto isSquare = [&](const SliceFace& face) {
        if (face.corners.size() != 4) {
            return false;
        }
        const std::vector<double> l = edgeLengths(face);
        const double d0 = length(s.points[face.corners[0]] - s.points[face.corners[2]]);
        const double d1 = length(s.points[face.corners[1]] - s.points[face.corners[3]]);
        return std::fabs(l[0] - l[1]) < 1e-6 && std::fabs(l[1] - l[2]) < 1e-6 && std::fabs(l[2] - l[3]) < 1e-6
            && std::fabs(d0 - d1) < 1e-6;
    };
    const auto isRectangle = [&](const SliceFace& face) {
        if (face.corners.size() != 4) {
            return false;
        }
        const double d0 = length(s.points[face.corners[0]] - s.points[face.corners[2]]);
        const double d1 = length(s.points[face.corners[1]] - s.points[face.corners[3]]);
        const std::vector<double> l = edgeLengths(face);
        return std::fabs(d0 - d1) < 1e-6 && std::fabs(l[0] - l[2]) < 1e-6 && std::fabs(l[1] - l[3]) < 1e-6;
    };
    if (v == 4 && f == 4 && sides[3] == 4) {
        return "tetrahedron";
    }
    if (v == 6 && f == 8 && sides[3] == 8) {
        return "octahedron";
    }
    if (v == 5 && f == 5 && sides[3] == 4 && sides[4] == 1) {
        return "square pyramid";
    }
    if (v == 6 && f == 5 && sides[3] == 2 && sides[4] == 3) {
        return "triangular prism";
    }
    if (v == 12 && f == 8 && sides[3] == 4 && sides[6] == 4) {
        return "truncated tetrahedron";
    }
    if (v == 12 && f == 8 && sides[6] == 2 && sides[4] == 6) {
        return "hexagonal prism";
    }
    if (v == 8 && f == 6 && sides[4] == 6) {
        int square = 0;
        int rectangle = 0;
        for (const SliceFace& face : s.faces) {
            square += isSquare(face) ? 1 : 0;
            rectangle += isRectangle(face) ? 1 : 0;
        }
        if (square == 6) {
            return "cube";
        }
        if (square == 2 && rectangle == 6) {
            return "square prism";
        }
        if (rectangle == 6) {
            return "cuboid";
        }
        return "hexahedron";
    }
    // Anything else is described by its faces.
    static constexpr std::array<const char*, 9> polygon{"", "", "", "triangle", "quadrilateral", "pentagon",
                                                        "hexagon", "heptagon", "octagon"};
    std::string description = std::to_string(f) + "-faced solid:";
    bool first = true;
    for (int k = 3; k < 16; ++k) {
        if (sides[k] == 0) {
            continue;
        }
        const std::string kind = k < 9 ? polygon[k] : std::to_string(k) + "-gon";
        description += (first ? " " : ", ") + std::to_string(sides[k]) + " " + kind + (sides[k] > 1 ? "s" : "");
        first = false;
    }
    return description;
}

} // namespace tesseract
