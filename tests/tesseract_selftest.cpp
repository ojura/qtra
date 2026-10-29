// Checks the tesseract's maths without Qt or a display: that its rotations
// stay rotations, that its projections land where they should, and above all
// that its slices are the solids they should be. A slice is a convex
// polyhedron built face by face from the cells it cuts, so every one is
// checked to be closed, convex, outward-facing and made of faces that each
// lie in one cell, at the canonical cuts and at two thousand random ones.

#include "tesseract_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace {

int failures = 0;

void check(const bool condition, const std::string& what)
{
    std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", what.c_str());
    if (!condition) {
        ++failures;
    }
}

std::string format(const char* pattern, const double a = 0.0, const double b = 0.0)
{
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, pattern, a, b);
    return buffer;
}

using tesseract::V3;
using tesseract::V4;

struct Rng {
    std::uint64_t state = 0x853C49E6748FEA9BULL;
    double uniform()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(const double lo, const double hi) { return lo + (hi - lo) * uniform(); }
};

double determinant(const tesseract::M4& r)
{
    // Laplace expansion along the first row, with 3x3 minors.
    const auto minor3 = [&](const int skipColumn) {
        std::array<std::array<double, 3>, 3> m{};
        for (int i = 1; i < 4; ++i) {
            int c = 0;
            for (int j = 0; j < 4; ++j) {
                if (j != skipColumn) {
                    m[i - 1][c++] = r.m[i][j];
                }
            }
        }
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    double d = 0.0;
    for (int j = 0; j < 4; ++j) {
        d += ((j % 2 == 0) ? 1.0 : -1.0) * r.m[0][j] * minor3(j);
    }
    return d;
}

void rotationsStayRotations()
{
    Rng rng;
    double worstOrthogonality = 0.0;
    double worstDeterminant = 0.0;
    for (int trial = 0; trial < 500; ++trial) {
        std::array<double, tesseract::planeCount> angles{};
        for (double& a : angles) {
            a = rng.range(-20.0, 20.0);
        }
        const tesseract::M4 r = tesseract::rotation(angles);
        const tesseract::M4 product = r.transposed() * r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                worstOrthogonality = std::max(worstOrthogonality, std::fabs(product.m[i][j] - (i == j ? 1.0 : 0.0)));
            }
        }
        worstDeterminant = std::max(worstDeterminant, std::fabs(determinant(r) - 1.0));
    }
    check(worstOrthogonality < 1e-12, format("500 rotations stay orthonormal (worst %.1e)", worstOrthogonality));
    check(worstDeterminant < 1e-12, format("and keep orientation, determinant 1 (worst %.1e)", worstDeterminant));

    // A Clifford double rotation moves every point: none of the vertices
    // stays where it was.
    std::array<double, tesseract::planeCount> clifford{};
    clifford[tesseract::XW] = 0.7;
    clifford[tesseract::YZ] = 0.7;
    const tesseract::M4 r = tesseract::rotation(clifford);
    double least = 1e9;
    for (int i = 0; i < tesseract::vertexCount; ++i) {
        least = std::min(least, tesseract::length(r * tesseract::vertex(i) - tesseract::vertex(i)));
    }
    check(least > 0.5, format("a double rotation by 0.7 rad in xw and yz moves every vertex (least %.3f)", least));
}

void polytopeCounts()
{
    std::set<std::pair<int, int>> pairs;
    bool oneAxis = true;
    for (const tesseract::Edge& e : tesseract::edges()) {
        pairs.insert({e.a, e.b});
        const int differ = e.a ^ e.b;
        oneAxis = oneAxis && differ == (1 << e.axis);
    }
    check(pairs.size() == 32 && oneAxis, "32 distinct edges, each joining vertices that differ in one coordinate");
    bool squaresOk = true;
    std::set<std::pair<int, int>> cellPairs;
    for (const tesseract::Square& s : tesseract::squares()) {
        for (int k = 0; k < 4; ++k) {
            const int d = s.corners[k] ^ s.corners[(k + 1) % 4];
            squaresOk = squaresOk && (d & (d - 1)) == 0 && d != 0;
            squaresOk = squaresOk && tesseract::vertexInCell(s.corners[k], s.cellA)
                && tesseract::vertexInCell(s.corners[k], s.cellB);
        }
        cellPairs.insert({std::min(s.cellA, s.cellB), std::max(s.cellA, s.cellB)});
    }
    check(squaresOk && cellPairs.size() == 24,
          "24 squares, each a closed loop of edges lying in the two cells it joins");
}

void projections()
{
    const V4 p{0.3, -0.7, 0.5, 0.0};
    const V3 q = tesseract::perspective(p, 3.0);
    check(std::fabs(q.x - 0.3) < 1e-12 && std::fabs(q.y + 0.7) < 1e-12 && std::fabs(q.z - 0.5) < 1e-12,
          "perspective leaves the w = 0 hyperplane at its own size");
    check(tesseract::perspectiveScale({0, 0, 0, 1}, 3.0) > tesseract::perspectiveScale({0, 0, 0, -1}, 3.0),
          "and draws the cell nearer the eye in w larger");
    const V3 origin = tesseract::stereographic({0, 0, 0, -1});
    check(tesseract::length(origin) < 1e-12, "stereographic projection sends the far pole to the origin");
    const V3 equator = tesseract::stereographic({0.6, 0.0, 0.8, 0.0});
    check(std::fabs(tesseract::length(equator) - 1.0) < 1e-12, "and the w = 0 equator to the unit sphere");
    const V4 a = tesseract::normalize(tesseract::vertex(0));
    const V4 b = tesseract::normalize(tesseract::vertex(1));
    const V4 mid = tesseract::slerp(a, b, 0.5);
    check(std::fabs(tesseract::length(mid) - 1.0) < 1e-12
              && std::fabs(tesseract::dot(mid, a) - tesseract::dot(mid, b)) < 1e-12,
          "an arc's midpoint stays on the sphere, equally far from both ends");
}

// Everything a slice must be, whatever it is.
struct Soundness {
    bool closed = true;
    bool euler = true;
    bool convex = true;
    bool outward = true;
    bool inCells = true;
    bool oneFacePerCell = true;
};

Soundness sound(const tesseract::Slice& s)
{
    Soundness r;
    std::map<std::pair<int, int>, int> edgeUses;
    std::set<int> cells;
    V3 centre{};
    const int used = tesseract::usedPoints(s);
    std::vector<bool> counted(s.points.size(), false);
    for (const tesseract::SliceFace& face : s.faces) {
        for (const int id : face.corners) {
            if (!counted[id]) {
                counted[id] = true;
                centre = centre + s.points[id];
            }
        }
    }
    centre = centre * (1.0 / std::max(used, 1));
    for (const tesseract::SliceFace& face : s.faces) {
        r.oneFacePerCell = r.oneFacePerCell && cells.insert(face.cell).second;
        const int axis = tesseract::cellAxis(face.cell);
        // Every corner of a face lies in the cell it is listed for.
        const double sign = tesseract::cellSign(face.cell);
        const std::size_t n = face.corners.size();
        for (std::size_t k = 0; k < n; ++k) {
            const int a = face.corners[k];
            const int b = face.corners[(k + 1) % n];
            edgeUses[{std::min(a, b), std::max(a, b)}] += 1;
            r.inCells = r.inCells && std::fabs(s.points4[a][axis] - sign) < 1e-9;
        }
        // The face's own normal from its winding, and every point of the
        // solid on its inner side.
        V3 normal{};
        for (std::size_t k = 0; k < n; ++k) {
            const V3 p = s.points[face.corners[k]];
            const V3 q = s.points[face.corners[(k + 1) % n]];
            normal = normal + tesseract::cross(p, q);
        }
        normal = tesseract::normalize(normal);
        const V3 onFace = s.points[face.corners[0]];
        r.outward = r.outward && tesseract::dot(normal, onFace - centre) > 0.0;
        for (std::size_t id = 0; id < s.points.size(); ++id) {
            if (counted[id] && tesseract::dot(normal, s.points[id] - onFace) > 1e-9) {
                r.convex = false;
            }
        }
    }
    for (const auto& [edge, uses] : edgeUses) {
        r.closed = r.closed && uses == 2;
    }
    r.euler = used - static_cast<int>(edgeUses.size()) + static_cast<int>(s.faces.size()) == 2;
    return r;
}

bool allSound(const Soundness& r)
{
    return r.closed && r.euler && r.convex && r.outward && r.inCells && r.oneFacePerCell;
}

// The lengths of a slice's edges: their spread says whether it is regular.
std::pair<double, double> edgeRange(const tesseract::Slice& s)
{
    double lo = 1e9;
    double hi = 0.0;
    for (const tesseract::SliceFace& face : s.faces) {
        for (std::size_t k = 0; k < face.corners.size(); ++k) {
            const double l = tesseract::length(s.points[face.corners[k]]
                                               - s.points[face.corners[(k + 1) % face.corners.size()]]);
            lo = std::min(lo, l);
            hi = std::max(hi, l);
        }
    }
    return {lo, hi};
}

tesseract::Slice cut(const V4& direction, const double fraction)
{
    const V4 n = tesseract::normalize(direction);
    return tesseract::slice(n, fraction * tesseract::reach(n), tesseract::hyperplaneBasis(n));
}

void canonicalSlices()
{
    const V4 cell{0, 0, 0, 1};
    const V4 face{0, 0, 1, 1};
    const V4 edge{0, 1, 1, 1};
    const V4 corner{1, 1, 1, 1};

    struct Case {
        const char* what;
        V4 direction;
        double fraction;
        const char* name;
        bool regular;
    };
    const std::array<Case, 10> cases{{
        {"cell first, anywhere inside", cell, 0.3, "cube", true},
        {"face first, through the middle", face, 0.0, "square prism", false},
        {"face first, near the end", face, 0.85, "square prism", false},
        {"edge first, through the middle", edge, 0.0, "hexagonal prism", false},
        {"edge first, near the end", edge, 0.8, "triangular prism", false},
        {"corner first, near the end", corner, 0.75, "tetrahedron", true},
        {"corner first, through four corners", corner, 0.5, "tetrahedron", true},
        {"corner first, halfway from the middle to the four-corner cut", corner, 0.25, "truncated tetrahedron", false},
        {"corner first, through the middle", corner, 0.0, "octahedron", true},
        {"corner first, from the other side", corner, -0.75, "tetrahedron", true},
    }};
    for (const Case& c : cases) {
        const tesseract::Slice s = cut(c.direction, c.fraction);
        const std::string name = tesseract::nameOf(s);
        const auto [lo, hi] = edgeRange(s);
        const bool regularOk = !c.regular || hi - lo < 1e-9;
        check(name == c.name && regularOk && allSound(sound(s)),
              std::string(c.what) + ": " + name + (c.regular ? format(", edges %.4f to %.4f", lo, hi) : ""));
    }

    // Halfway from the middle to the four-corner cut, the truncated
    // tetrahedron is the Archimedean one, with every edge the same length: the
    // cut trims a third off each edge of the tetrahedron it would otherwise be.
    const tesseract::Slice quarter = cut(corner, 0.25);
    const auto [lo, hi] = edgeRange(quarter);
    check(hi - lo < 1e-9, format("and there every edge is the same length, the Archimedean one (%.4f to %.4f)", lo, hi));

    const tesseract::Slice middle = cut(corner, 0.0);
    check(tesseract::usedPoints(middle) == 6,
          "the octahedron's six corners are six of the tesseract's own vertices, each counted once");
    // Through the middle face first, the hyperplane holds two of the
    // tesseract's squares whole, and each is one face of the solid, not two.
    const tesseract::Slice prism = cut(face, 0.0);
    check(prism.faces.size() == 6 && tesseract::usedPoints(prism) == 8,
          format("a cut containing whole squares counts each once: %.0f faces, %.0f corners",
                 static_cast<double>(prism.faces.size()), tesseract::usedPoints(prism)));
    const tesseract::Slice outside = cut(corner, 1.01);
    check(!outside.solid() && tesseract::nameOf(outside) == "nothing", "a cut beyond the last vertex finds nothing");
}

void randomSlicesAreSound()
{
    Rng rng;
    int solids = 0;
    int unsound = 0;
    std::map<std::string, int> seen;
    for (int trial = 0; trial < 2000; ++trial) {
        const V4 n = tesseract::normalize({rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)});
        const double offset = rng.range(-0.97, 0.97) * tesseract::reach(n);
        const tesseract::Slice s = tesseract::slice(n, offset, tesseract::hyperplaneBasis(n));
        if (!s.solid()) {
            continue;
        }
        ++solids;
        seen[tesseract::nameOf(s)] += 1;
        if (!allSound(sound(s))) {
            ++unsound;
        }
    }
    check(solids > 1900 && unsound == 0,
          format("2000 random cuts: %.0f solids, every one closed, convex, outward-facing and one face per cell",
                 solids));
    std::printf("info: the random cuts were:");
    for (const auto& [name, count] : seen) {
        std::printf(" %s %d,", name.c_str(), count);
    }
    std::printf("\n");
}

} // namespace

int main()
{
    rotationsStayRotations();
    polytopeCounts();
    projections();
    canonicalSlices();
    randomSlicesAreSound();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all checks passed\n");
    return EXIT_SUCCESS;
}
