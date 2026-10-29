// Turns the cube into a tesseract, the four-dimensional cube, and lets it
// turn through the fourth dimension.
//
// The cube's own spin drives the turning: each of the six planes of
// four-dimensional rotation turns at some multiple of the cube's angle, so
// Ctrl++ and Ctrl+- speed the tesseract up and slow it down, and pausing the
// cube holds it still. By default xw and yz turn together, a Clifford double
// rotation, under which every point of the tesseract moves and the cells turn
// inside out in a steady cycle.
//
// It is shown three ways (see tesseract_math.h for the maths):
//
//   perspective     divided by distance along w: the cell nearest the eye in
//                   w is the big outer cube, the furthest the small inner one,
//                   and the six between them are the frustums joining them
//   stereographic   on the three-sphere and projected from its pole, so the
//                   edges are circular arcs and the cells are curved
//   slice           the exact three-dimensional cross-section by a sweeping
//                   hyperplane, cut cell first, face first, edge first,
//                   corner first or turning with the rotation; it grows from
//                   a point, through tetrahedra and prisms, to octahedra and
//                   back
//
// Each of the eight cells has its own colour in every view, the six that
// correspond to the cube's faces keeping those faces' hues, so a slice's face
// says which cell it was cut from.
//
// Like the other mesh replacements, it displaces the widget's cube and claims
// that in the byte stash. A panel docks beside the cube, and dragging the cube
// orbits the camera; see projection_camera.h.
//
// Requests:
//   {}                          install, or report what it is showing
//   {"restore": true}           take it off and put the cube back
//   {"toggle": true}            flip it
//   {"status": true}            the view, the slice, the rotation
//   {"view": "slice"}           perspective, stereographic or slice
//   {"direction": "corner"}     cell, face, edge, corner (or vertex), turning
//   {"sweep": false, "position": 0.25}
//                               hold the cut at a quarter of the way along
//   {"rates": {"xw": 1, "yz": 1}}
//                               turning rates by plane, in units of the cube's
//                               own spin; planes not named keep their rate
//   {"faces": false}, {"corners": false}, {"events": false}
//   {"orbit": [30, -10]}, {"distance": 8}, {"resetCamera": true}
//
// Events: tesseract.slice, each time the slice becomes a different solid.

#include "agent/agent_abi.h"
#include "cube_mesh.h"
#include "cube_widget.h"
#include "projection_camera.h"
#include "scene_toggle.h"
#include "tesseract_math.h"
#include "tesseract_panel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QPointer>
#include <QQuaternion>
#include <QSignalBlocker>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using tesseract::V3;
using tesseract::V4;

// ---------------------------------------------------------------------------
// What is shown and how it turns
// ---------------------------------------------------------------------------

enum class View : int { Perspective = 0, Stereographic = 1, Slice = 2 };
enum class Direction : int { Cell = 0, Face = 1, Edge = 2, Corner = 3, Turning = 4 };

constexpr std::array<const char*, 3> viewNames{"perspective", "stereographic", "slice"};
constexpr std::array<const char*, 5> directionNames{"cell", "face", "edge", "corner", "turning"};

struct Options {
    View view = View::Perspective;
    Direction direction = Direction::Corner;
    bool sweep = true;
    float position = 0.0F; // where the cut is held when not sweeping, -1 to 1
    // Turning rates by plane (xy, xz, xw, yz, yw, zw), in units of the cube's
    // own spin.
    std::array<float, tesseract::planeCount> rates{0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F};
    bool faces = true;
    bool corners = true;
    bool events = true;
};

// The colour of each cell. The six that pin x, y or z keep the hue of the
// cube's face in that direction (magenta for +x, green for -x, yellow for +y,
// blue for -y, red for +z, cyan for -z); the two that pin w, which the cube
// has no face for, are warm white and orange.
constexpr std::array<std::array<float, 3>, tesseract::cellCount> cellColour{{
    {1.00F, 0.25F, 0.95F}, {0.25F, 1.00F, 0.35F}, {1.00F, 0.90F, 0.20F}, {0.25F, 0.45F, 1.00F},
    {1.00F, 0.25F, 0.20F}, {0.20F, 0.95F, 1.00F}, {1.00F, 0.96F, 0.88F}, {1.00F, 0.55F, 0.12F},
}};

// The eye in the perspective view, on the w axis.
constexpr double perspectiveEye = 3.0;

// Where the sweep goes and where it pauses, as fractions of the way from one
// end of the tesseract to the other. Most cuts change shape only at their
// ends and pass straight through. A corner-first cut is an octahedron only at
// exactly the middle and a tetrahedron through four of the tesseract's
// corners only at exactly a quarter from either end, so a sweep that did not
// stop there would show each of them for less than a frame.
struct Stop {
    double at;
    double dwell; // seconds
};

std::vector<Stop> sweepStops(const Direction direction)
{
    if (direction == Direction::Corner) {
        return {{-0.995, 0.4}, {-0.5, 1.4}, {0.0, 1.8}, {0.5, 1.4}, {0.995, 0.4}};
    }
    return {{-0.995, 0.3}, {0.995, 0.3}};
}

// Seconds to travel the whole way from one end to the other.
constexpr double sweepSeconds = 7.0;

// The position after `clock` seconds of sweeping: out through the stops and
// back, easing in and out of each.
double sweepPosition(const Direction direction, const double clock)
{
    const std::vector<Stop> stops = sweepStops(direction);
    std::vector<Stop> path = stops;
    for (int i = static_cast<int>(stops.size()) - 2; i >= 1; --i) {
        path.push_back(stops[i]);
    }
    double cycle = 0.0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Stop& next = path[(i + 1) % path.size()];
        cycle += path[i].dwell + sweepSeconds * std::fabs(next.at - path[i].at) / 1.99;
    }
    double t = std::fmod(clock, cycle);
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (t < path[i].dwell) {
            return path[i].at;
        }
        t -= path[i].dwell;
        const Stop& next = path[(i + 1) % path.size()];
        const double move = sweepSeconds * std::fabs(next.at - path[i].at) / 1.99;
        if (t < move) {
            const double eased = 0.5 - 0.5 * std::cos(tesseract::pi * t / move);
            return path[i].at + (next.at - path[i].at) * eased;
        }
        t -= move;
    }
    return path.front().at;
}

// ---------------------------------------------------------------------------
// GLSL
// ---------------------------------------------------------------------------

constexpr auto vertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColour;
uniform mat4 mvp;
uniform mat4 model;
out vec3 worldPosition;
out vec3 worldNormal;
out vec4 colour;

void main()
{
    worldPosition = (model * vec4(inPosition, 1.0)).xyz;
    worldNormal = mat3(model) * inNormal;
    colour = inColour;
    gl_Position = mvp * vec4(inPosition, 1.0);
}
)glsl";

// Lit for tubes, corners and the solid slice; unlit and added on top for the
// cells' glass, which then needs no sorting.
constexpr auto fragmentShader = R"glsl(
#version 330 core
in vec3 worldPosition;
in vec3 worldNormal;
in vec4 colour;
uniform vec3 eye;
uniform vec3 light;
uniform vec3 tint;
uniform float lit;
out vec4 fragmentColour;

void main()
{
    vec3 base = colour.rgb * tint;
    if (lit < 0.5) {
        fragmentColour = vec4(base * colour.a, 1.0);
        return;
    }
    vec3 n = normalize(worldNormal);
    vec3 v = normalize(eye - worldPosition);
    if (dot(n, v) < 0.0) {
        n = -n;
    }
    float diffuse = max(dot(n, light), 0.0);
    float wrap = clamp((dot(n, light) + 0.35) / 1.35, 0.0, 1.0);
    vec3 h = normalize(light + v);
    float specular = pow(max(dot(n, h), 0.0), 48.0);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 2.5);
    vec3 shaded = base * (0.18 + 0.55 * wrap + 0.35 * diffuse) + vec3(0.9) * specular * 0.5 + base * rim * 0.35;
    fragmentColour = vec4(clamp(shaded, 0.0, 1.0), colour.a);
}
)glsl";

// ---------------------------------------------------------------------------
// Geometry, built on the CPU each frame
// ---------------------------------------------------------------------------

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b, a;
};

struct Colour {
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;
};

Colour scaled(const std::array<float, 3>& c, const float k, const float alpha = 1.0F)
{
    return {c[0] * k, c[1] * k, c[2] * k, alpha};
}

void push(std::vector<Vertex>& out, const V3& p, const V3& n, const Colour& c)
{
    out.push_back(Vertex{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z),
                         static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z), c.r, c.g, c.b,
                         c.a});
}

// A tube along a polyline, with a radius and colour at each point.
void addTube(std::vector<Vertex>& out, const std::vector<V3>& points, const std::vector<double>& radii,
             const std::vector<Colour>& colours, const int sides = 8)
{
    const std::size_t n = points.size();
    if (n < 2) {
        return;
    }
    std::vector<std::vector<V3>> rings(n);
    std::vector<std::vector<V3>> normals(n);
    // Each ring's "side" is the previous ring's, with its part along the new
    // tangent removed: parallel transport, so a curved tube does not twist.
    // Building each ring from a fixed reference instead turned the rings by
    // up to half a turn between points, and the tube pinched into beads.
    V3 side{};
    for (std::size_t i = 0; i < n; ++i) {
        const V3 tangent = tesseract::normalize(points[std::min(i + 1, n - 1)] - points[i > 0 ? i - 1 : 0]);
        if (i == 0) {
            side = tesseract::cross(tangent, V3{0.37, 0.81, 0.45});
            if (tesseract::length(side) < 1e-6) {
                side = tesseract::cross(tangent, V3{1.0, 0.0, 0.0});
            }
        } else {
            side = side - tangent * tesseract::dot(side, tangent);
        }
        side = tesseract::normalize(side);
        const V3 other = tesseract::cross(tangent, side);
        for (int k = 0; k < sides; ++k) {
            const double a = 2.0 * tesseract::pi * static_cast<double>(k) / sides;
            const V3 dir = side * std::cos(a) + other * std::sin(a);
            normals[i].push_back(dir);
            rings[i].push_back(points[i] + dir * radii[i]);
        }
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        for (int k = 0; k < sides; ++k) {
            const int k1 = (k + 1) % sides;
            push(out, rings[i][k], normals[i][k], colours[i]);
            push(out, rings[i + 1][k], normals[i + 1][k], colours[i + 1]);
            push(out, rings[i + 1][k1], normals[i + 1][k1], colours[i + 1]);
            push(out, rings[i][k], normals[i][k], colours[i]);
            push(out, rings[i + 1][k1], normals[i + 1][k1], colours[i + 1]);
            push(out, rings[i][k1], normals[i][k1], colours[i]);
        }
    }
}

// A unit sphere: an octahedron subdivided twice and pushed out.
const std::vector<std::array<V3, 3>>& unitSphere()
{
    static const std::vector<std::array<V3, 3>> faces = [] {
        std::vector<std::array<V3, 3>> list;
        const std::array<V3, 6> o{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
        const int tri[8][3] = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}};
        for (const auto& t : tri) {
            list.push_back({o[t[0]], o[t[1]], o[t[2]]});
        }
        for (int level = 0; level < 2; ++level) {
            std::vector<std::array<V3, 3>> finer;
            for (const auto& f : list) {
                const V3 ab = tesseract::normalize(f[0] + f[1]);
                const V3 bc = tesseract::normalize(f[1] + f[2]);
                const V3 ca = tesseract::normalize(f[2] + f[0]);
                finer.push_back({f[0], ab, ca});
                finer.push_back({ab, f[1], bc});
                finer.push_back({ca, bc, f[2]});
                finer.push_back({ab, bc, ca});
            }
            list.swap(finer);
        }
        return list;
    }();
    return faces;
}

void addSphere(std::vector<Vertex>& out, const V3& centre, const double radius, const Colour& c)
{
    for (const auto& f : unitSphere()) {
        for (const V3& d : f) {
            push(out, centre + d * radius, d, c);
        }
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Scene {
    QOpenGLContext* owner = nullptr;
    QOpenGLFunctions_3_3_Core* gl = nullptr;
    QOpenGLShaderProgram program;
    GLuint array = 0;
    GLuint buffer = 0;
};

struct Tesseract {
    std::unique_ptr<Scene> scene;
    projection_camera::Camera camera;

    std::vector<float> savedCubeVertices;
    QByteArray stashKey;
    QByteArray claimKey;

    // How far each plane has turned, in radians, accumulated from the cube's
    // angle a frame at a time so a rate that is not a whole number does not
    // jump when the angle wraps at 360.
    std::array<double, tesseract::planeCount> turned{};
    double tumble = 0.0;
    double sweepClock = 0.0;
    bool clockSeen = false;
    float lastAngle = 0.0F;
    float lastElapsed = 0.0F;

    // The slice as last drawn, and the solid it was last reported as.
    float position = 0.0F;
    std::string shape = "nothing";
    std::string reportedShape;
    double lastReport = -1e9;

    std::uint64_t framesDrawn = 0;
};

Tesseract* state = nullptr;
CubeWidget* stateCube = nullptr;
Options options;
QMetaObject::Connection drawConnection;
QAction* toggleAction = nullptr;
QPointer<QDockWidget> panel;
QPointer<projection_camera::Pointer> cameraPointer;
RuntimeAgentHost hookHost{};
bool hookHostValid = false;

// ---------------------------------------------------------------------------
// The camera
// ---------------------------------------------------------------------------

void applyCamera(CubeWidget* cube)
{
    if (state != nullptr) {
        projection_camera::apply(state->camera, cube);
    }
}

void orbitCamera(CubeWidget* cube, const float across, const float up)
{
    if (state == nullptr) {
        return;
    }
    projection_camera::orbit(state->camera, across, up);
    applyCamera(cube);
    cube->update();
}

void setCameraDistance(CubeWidget* cube, const float distance)
{
    if (state == nullptr) {
        return;
    }
    projection_camera::setDistance(state->camera, distance);
    applyCamera(cube);
    cube->update();
}

void resetCamera(CubeWidget* cube)
{
    if (state == nullptr) {
        return;
    }
    projection_camera::reset(state->camera);
    applyCamera(cube);
    cube->update();
}

// ---------------------------------------------------------------------------
// Building a frame
// ---------------------------------------------------------------------------

V4 directionVector(const Direction direction)
{
    switch (direction) {
    case Direction::Cell: return {0.0, 0.0, 0.0, 1.0};
    case Direction::Face: return {0.0, 0.0, 1.0, 1.0};
    case Direction::Edge: return {0.0, 1.0, 1.0, 1.0};
    case Direction::Corner: return {1.0, 1.0, 1.0, 1.0};
    case Direction::Turning: return {0.0, 0.0, 0.0, 1.0};
    }
    return {0.0, 0.0, 0.0, 1.0};
}

// The perspective view: straight tubes for the edges, thicker and brighter
// nearer the eye in w, and each cell's six squares as tinted glass.
void buildPerspective(const tesseract::M4& r, std::vector<Vertex>& solid, std::vector<Vertex>& glass)
{
    constexpr double scale = 0.62;
    std::array<V3, tesseract::vertexCount> at{};
    std::array<double, tesseract::vertexCount> near{};
    std::array<double, tesseract::vertexCount> size{};
    for (int i = 0; i < tesseract::vertexCount; ++i) {
        const V4 p = r * tesseract::vertex(i);
        at[i] = tesseract::perspective(p, perspectiveEye) * scale;
        near[i] = std::clamp((p.w + 2.0) / 4.0, 0.0, 1.0);
        size[i] = tesseract::perspectiveScale(p, perspectiveEye);
    }
    const std::array<float, 3> edgeTone{0.86F, 0.9F, 1.0F};
    for (const tesseract::Edge& e : tesseract::edges()) {
        const std::vector<V3> points{at[e.a], at[e.b]};
        const std::vector<double> radii{0.018 * size[e.a], 0.018 * size[e.b]};
        const std::vector<Colour> colours{scaled(edgeTone, static_cast<float>(0.35 + 0.65 * near[e.a])),
                                          scaled(edgeTone, static_cast<float>(0.35 + 0.65 * near[e.b]))};
        addTube(solid, points, radii, colours, 8);
    }
    if (options.corners) {
        for (int i = 0; i < tesseract::vertexCount; ++i) {
            addSphere(solid, at[i], 0.04 * size[i], scaled({1.0F, 1.0F, 1.0F}, static_cast<float>(0.45 + 0.55 * near[i])));
        }
    }
    if (options.faces) {
        for (const tesseract::Square& s : tesseract::squares()) {
            const V3 normal = tesseract::normalize(
                tesseract::cross(at[s.corners[1]] - at[s.corners[0]], at[s.corners[2]] - at[s.corners[0]]));
            for (const int cell : {s.cellA, s.cellB}) {
                const Colour c = scaled(cellColour[cell], 1.0F, 0.055F);
                for (const int k : {0, 1, 2, 0, 2, 3}) {
                    push(glass, at[s.corners[k]], normal, c);
                }
            }
        }
    }
}

// The stereographic view: the vertices on the three-sphere, the edges as the
// arcs of great circles between them, the squares as curved patches, all
// projected from the pole, which draws great circles as circular arcs.
void buildStereographic(const tesseract::M4& r, std::vector<Vertex>& solid, std::vector<Vertex>& glass)
{
    constexpr double scale = 0.62;
    constexpr int segments = 16;
    // Near the pole the projection runs off to infinity. An edge reaching
    // past this is left out, and a cell's patch past the smaller limit too:
    // those patches are what the far side of the three-sphere becomes, and
    // drawn they are sheets of glass across the whole window.
    constexpr double tooFar = 3.6;
    constexpr double tooFarForGlass = 2.1;
    const auto project = [&](const V4& u, double& factor) {
        factor = 1.0 / std::max(1.0 - u.w, 0.04);
        return tesseract::stereographic(u) * scale;
    };
    std::array<V4, tesseract::vertexCount> onSphere{};
    for (int i = 0; i < tesseract::vertexCount; ++i) {
        onSphere[i] = tesseract::normalize(r * tesseract::vertex(i));
    }
    const std::array<float, 3> edgeTone{0.86F, 0.9F, 1.0F};
    for (const tesseract::Edge& e : tesseract::edges()) {
        std::vector<V3> points;
        std::vector<double> radii;
        std::vector<Colour> colours;
        bool outOfView = false;
        for (int k = 0; k <= segments; ++k) {
            const V4 u = tesseract::slerp(onSphere[e.a], onSphere[e.b], static_cast<double>(k) / segments);
            double factor = 1.0;
            const V3 q = project(u, factor);
            outOfView = outOfView || tesseract::length(q) > tooFar;
            points.push_back(q);
            // Stereographic projection magnifies by this factor near the
            // pole, so a tube kept in proportion swells there too.
            radii.push_back(std::min(0.012 * factor, 0.08));
            colours.push_back(scaled(edgeTone, static_cast<float>(0.4 + 0.6 * (1.0 - (u.w + 1.0) / 2.0))));
        }
        if (!outOfView) {
            addTube(solid, points, radii, colours, 7);
        }
    }
    if (options.corners) {
        for (int i = 0; i < tesseract::vertexCount; ++i) {
            double factor = 1.0;
            const V3 q = project(onSphere[i], factor);
            if (tesseract::length(q) < tooFar) {
                addSphere(solid, q, std::min(0.03 * factor, 0.18), scaled({1.0F, 1.0F, 1.0F}, 0.9F));
            }
        }
    }
    if (options.faces) {
        constexpr int grid = 6;
        for (const tesseract::Square& s : tesseract::squares()) {
            const std::array<V4, 4> c{r * tesseract::vertex(s.corners[0]), r * tesseract::vertex(s.corners[1]),
                                      r * tesseract::vertex(s.corners[2]), r * tesseract::vertex(s.corners[3])};
            std::array<std::array<V3, grid + 1>, grid + 1> patch{};
            bool outOfView = false;
            for (int i = 0; i <= grid; ++i) {
                for (int j = 0; j <= grid; ++j) {
                    const double a = static_cast<double>(i) / grid;
                    const double b = static_cast<double>(j) / grid;
                    const V4 p = (c[0] * (1 - a) + c[1] * a) * (1 - b) + (c[3] * (1 - a) + c[2] * a) * b;
                    double factor = 1.0;
                    patch[i][j] = project(tesseract::normalize(p), factor);
                    outOfView = outOfView || tesseract::length(patch[i][j]) > tooFarForGlass;
                }
            }
            if (outOfView) {
                continue;
            }
            for (const int cell : {s.cellA, s.cellB}) {
                const Colour colour = scaled(cellColour[cell], 1.0F, 0.05F);
                for (int i = 0; i < grid; ++i) {
                    for (int j = 0; j < grid; ++j) {
                        const V3 n = tesseract::normalize(
                            tesseract::cross(patch[i + 1][j] - patch[i][j], patch[i][j + 1] - patch[i][j]));
                        for (const auto& [di, dj] : std::array<std::pair<int, int>, 6>{
                                 {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}}) {
                            push(glass, patch[i + di][j + dj], n, colour);
                        }
                    }
                }
            }
        }
    }
}

// The slice: the solid itself, each face the colour of the cell it was cut
// from, with its edges and corners picked out.
tesseract::Slice buildSlice(const tesseract::M4& r, const double position, std::vector<Vertex>& solid)
{
    constexpr double scale = 0.62;
    V4 n;
    std::array<V4, 3> basis{};
    if (options.direction == Direction::Turning) {
        // A fixed cut through a turning tesseract: the hyperplane is w = c in
        // the view, which in the tesseract's own frame turns with it, and the
        // view's x, y and z are the coordinates in it, so the solid moves as
        // smoothly as the rotation does.
        const tesseract::M4 back = r.transposed();
        n = back * V4{0.0, 0.0, 0.0, 1.0};
        basis = {back * V4{1.0, 0.0, 0.0, 0.0}, back * V4{0.0, 1.0, 0.0, 0.0}, back * V4{0.0, 0.0, 1.0, 0.0}};
    } else {
        n = tesseract::normalize(directionVector(options.direction));
        basis = tesseract::hyperplaneBasis(n);
    }
    // The ends themselves are a single point; the stops are held just short.
    const double offset = std::clamp(position, -0.995, 0.995) * tesseract::reach(n);
    tesseract::Slice s = tesseract::slice(n, offset, basis);
    std::map<std::pair<int, int>, bool> drawn;
    for (const tesseract::SliceFace& face : s.faces) {
        const V3 a = s.points[face.corners[0]] * scale;
        V3 normal{};
        for (std::size_t k = 0; k < face.corners.size(); ++k) {
            normal = normal
                + tesseract::cross(s.points[face.corners[k]], s.points[face.corners[(k + 1) % face.corners.size()]]);
        }
        normal = tesseract::normalize(normal);
        const Colour colour = scaled(cellColour[face.cell], 0.92F);
        for (std::size_t k = 1; k + 1 < face.corners.size(); ++k) {
            push(solid, a, normal, colour);
            push(solid, s.points[face.corners[k]] * scale, normal, colour);
            push(solid, s.points[face.corners[k + 1]] * scale, normal, colour);
        }
        for (std::size_t k = 0; k < face.corners.size(); ++k) {
            const int p = face.corners[k];
            const int q = face.corners[(k + 1) % face.corners.size()];
            if (drawn[{std::min(p, q), std::max(p, q)}]) {
                continue;
            }
            drawn[{std::min(p, q), std::max(p, q)}] = true;
            const Colour edge{0.1F, 0.1F, 0.12F, 1.0F};
            addTube(solid, {s.points[p] * scale, s.points[q] * scale}, {0.012, 0.012}, {edge, edge}, 6);
        }
    }
    if (options.corners) {
        std::vector<bool> used(s.points.size(), false);
        for (const tesseract::SliceFace& face : s.faces) {
            for (const int id : face.corners) {
                used[id] = true;
            }
        }
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            if (used[i]) {
                addSphere(solid, s.points[i] * scale, 0.026, {0.95F, 0.95F, 0.95F, 1.0F});
            }
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

QJsonObject sliceReport(const tesseract::Slice& s)
{
    return QJsonObject{
        {QStringLiteral("shape"), QString::fromStdString(tesseract::nameOf(s))},
        {QStringLiteral("corners"), tesseract::usedPoints(s)},
        {QStringLiteral("edges"), s.edges},
        {QStringLiteral("faces"), static_cast<int>(s.faces.size())},
        {QStringLiteral("direction"), QString::fromLatin1(directionNames[static_cast<int>(options.direction)])},
        {QStringLiteral("position"), state != nullptr ? state->position : 0.0F},
    };
}

void drawScene(CubeWidget* cube)
{
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (state == nullptr || state->scene == nullptr || context == nullptr || context != state->scene->owner) {
        return;
    }
    Scene& scene = *state->scene;
    QOpenGLFunctions_3_3_Core* gl = scene.gl;

    // Advance on the widget's own clock and angle.
    const float angle = cube->m_angleDegrees;
    const float elapsed = cube->m_elapsedSeconds;
    if (!state->clockSeen) {
        state->clockSeen = true;
        state->lastAngle = angle;
        state->lastElapsed = elapsed;
    }
    double turn = static_cast<double>(angle) - state->lastAngle;
    turn -= 360.0 * std::round(turn / 360.0);
    const double turnRadians = turn * tesseract::pi / 180.0;
    for (int p = 0; p < tesseract::planeCount; ++p) {
        state->turned[p] += options.rates[p] * turnRadians;
    }
    state->tumble += 0.35 * turnRadians;
    state->sweepClock += std::max(0.0F, elapsed - state->lastElapsed);
    state->lastAngle = angle;
    state->lastElapsed = elapsed;
    if (options.sweep) {
        state->position = static_cast<float>(sweepPosition(options.direction, state->sweepClock));
    } else {
        state->position = options.position;
    }

    applyCamera(cube);
    const tesseract::M4 r = tesseract::rotation(state->turned);

    std::vector<Vertex> solid;
    std::vector<Vertex> glass;
    solid.reserve(40000);
    QMatrix4x4 model;
    if (options.view == View::Slice) {
        const tesseract::Slice s = buildSlice(r, state->position, solid);
        state->shape = tesseract::nameOf(s);
        // The solid turns slowly in three dimensions too, unless it is the
        // turning cut, which already moves with the rotation.
        if (options.direction != Direction::Turning) {
            model.rotate(static_cast<float>(state->tumble * 180.0 / tesseract::pi),
                         QVector3D(cubeSpinAxis[0], cubeSpinAxis[1], cubeSpinAxis[2]));
        } else {
            model.rotate(20.0F, 1.0F, 0.0F, 0.0F);
            model.rotate(-30.0F, 0.0F, 1.0F, 0.0F);
        }
        if (state->shape != state->reportedShape && options.events && hookHostValid
            && state->sweepClock - state->lastReport > 0.25) {
            state->reportedShape = state->shape;
            state->lastReport = state->sweepClock;
            hookHost.emit_event_json(hookHost.agent_context, "tesseract.slice",
                                     QJsonDocument(sliceReport(s)).toJson(QJsonDocument::Compact).constData());
        }
    } else {
        // A fixed tilt, so the projection is not seen square on; the turning
        // in four dimensions is the motion.
        model.rotate(18.0F, 1.0F, 0.0F, 0.0F);
        model.rotate(-28.0F, 0.0F, 1.0F, 0.0F);
        if (options.view == View::Perspective) {
            buildPerspective(r, solid, glass);
        } else {
            buildStereographic(r, solid, glass);
        }
    }
    model.scale(cube->m_scale);

    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, -cubeViewDistance);
    const QMatrix4x4 mvp = cube->m_projection * view * model;
    const QVector3D eye = state->camera.view.inverted().map(QVector3D(0.0F, 0.0F, 0.0F));

    std::vector<Vertex> all = std::move(solid);
    const GLsizei solidCount = static_cast<GLsizei>(all.size());
    all.insert(all.end(), glass.begin(), glass.end());
    const GLsizei glassCount = static_cast<GLsizei>(glass.size());

    gl->glBindVertexArray(scene.array);
    gl->glBindBuffer(GL_ARRAY_BUFFER, scene.buffer);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(all.size() * sizeof(Vertex)), all.data(),
                     GL_STREAM_DRAW);
    scene.program.bind();
    scene.program.setUniformValue("mvp", mvp);
    scene.program.setUniformValue("model", model);
    scene.program.setUniformValue("eye", eye);
    scene.program.setUniformValue("light", QVector3D(-0.42F, 0.62F, 0.66F).normalized());
    scene.program.setUniformValue("tint", cube->m_tint);

    gl->glEnable(GL_DEPTH_TEST);
    gl->glDepthFunc(GL_LESS);
    gl->glDisable(GL_CULL_FACE);
    scene.program.setUniformValue("lit", 1.0F);
    gl->glDrawArrays(GL_TRIANGLES, 0, solidCount);
    if (glassCount > 0) {
        // Added, so the glass needs no sorting and a square two cells share
        // shows both their colours.
        gl->glEnable(GL_BLEND);
        gl->glBlendFunc(GL_ONE, GL_ONE);
        gl->glDepthMask(GL_FALSE);
        scene.program.setUniformValue("lit", 0.0F);
        gl->glDrawArrays(GL_TRIANGLES, solidCount, glassCount);
        gl->glDepthMask(GL_TRUE);
        gl->glDisable(GL_BLEND);
    }
    scene.program.release();
    gl->glBindVertexArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glEnable(GL_CULL_FACE);
    gl->glCullFace(GL_BACK);
    ++state->framesDrawn;
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

bool setEnabled(CubeWidget* cube, bool enabled, QString& error);

tesseract_panel::Snapshot snapshot()
{
    tesseract_panel::Snapshot now;
    now.view = static_cast<int>(options.view);
    now.direction = static_cast<int>(options.direction);
    now.sweep = options.sweep;
    now.faces = options.faces;
    now.corners = options.corners;
    now.events = options.events;
    for (int p = 0; p < tesseract::planeCount; ++p) {
        now.rates[p] = options.rates[p];
    }
    if (state == nullptr) {
        now.position = options.position;
        return now;
    }
    now.installed = true;
    now.position = state->position;
    now.shape = QString::fromStdString(state->shape);
    for (int p = 0; p < tesseract::planeCount; ++p) {
        now.turned[p] = static_cast<float>(state->turned[p] * 180.0 / tesseract::pi);
    }
    return now;
}

// Built after the frame that installed the tesseract, not inside it: docking
// a panel resizes the cube's widget, which should not happen from the middle
// of its own paint.
void createPanel(CubeWidget* cube)
{
    if (state == nullptr || panel != nullptr) {
        return;
    }
    auto* window = qobject_cast<QMainWindow*>(cube->window());
    if (window == nullptr) {
        return;
    }
    tesseract_panel::Controls controls;
    controls.snapshot = [] { return snapshot(); };
    controls.setView = [](const int v) { options.view = static_cast<View>(std::clamp(v, 0, 2)); };
    controls.setDirection = [](const int d) { options.direction = static_cast<Direction>(std::clamp(d, 0, 4)); };
    controls.setSweep = [](const bool on) {
        if (!on && state != nullptr) {
            // Holding the sweep keeps the cut where it was.
            options.position = state->position;
        }
        options.sweep = on;
    };
    controls.setPosition = [](const float p) {
        options.position = std::clamp(p, -1.0F, 1.0F);
        options.sweep = false;
    };
    controls.setRate = [](const int plane, const float rate) {
        if (plane >= 0 && plane < tesseract::planeCount) {
            options.rates[plane] = std::clamp(rate, -2.0F, 2.0F);
        }
    };
    controls.setFaces = [](const bool on) { options.faces = on; };
    controls.setCorners = [](const bool on) { options.corners = on; };
    controls.setEvents = [](const bool on) { options.events = on; };
    controls.resetCamera = [cube] { resetCamera(cube); };
    controls.remove = [cube] {
        if (toggleAction != nullptr) {
            toggleAction->setChecked(false);
            return;
        }
        cube->enqueueRenderCallback([cube] {
            QString ignored;
            setEnabled(cube, false, ignored);
        });
    };
    panel = tesseract_panel::create(window, controls);
    QAction* view = panel->toggleViewAction();
    view->setObjectName(QStringLiteral("actionTesseractPanel"));
    view->setText(QStringLiteral("Tesseract p&anel"));
    view->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+H")));
    if (QMenu* menu = scene_toggle::findMenu(window, QStringLiteral("&Cube"))) {
        menu->addAction(view);
    }
}

void destroyPanel()
{
    if (panel != nullptr) {
        // Later, not now: the removal may have come from one of the panel's
        // own buttons, which is still on the stack.
        panel->deleteLater();
        panel = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Install and remove
// ---------------------------------------------------------------------------

// Needs the widget's OpenGL context to be current.
bool install(CubeWidget* cube, QString& error)
{
    const RuntimeAgentHost* host = &hookHost;
    auto fresh = std::make_unique<Tesseract>();
    fresh->scene = std::make_unique<Scene>();
    Scene& scene = *fresh->scene;
    scene.owner = QOpenGLContext::currentContext();
    scene.gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(scene.owner);
    if (scene.gl == nullptr) {
        error = QStringLiteral("this context offers no OpenGL 3.3 core functions");
        return false;
    }
    scene.gl->initializeOpenGLFunctions();

    // The tesseract stands in for the cube, so the cube's mesh is displaced
    // the way the other mesh replacements displace it, and a module that has
    // already displaced it keeps it.
    cube_mesh::Claim claim;
    if (cube_mesh::overlappingClaim(host, 0, cubeVertexFloats, claim)) {
        error = QStringLiteral("floats %1..%2 of the widget's mesh are already displaced by \"%3\"; saving what "
                               "it wrote as the originals would lose them")
                    .arg(claim.offsetFloats)
                    .arg(claim.offsetFloats + claim.lengthFloats)
                    .arg(claim.owner);
        return false;
    }
    fresh->savedCubeVertices.resize(cubeVertexFloats);
    cube->m_vertexBuffer.bind();
    const bool readBack = cube->m_vertexBuffer.read(
        0, fresh->savedCubeVertices.data(), cubeVertexFloats * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();
    if (!readBack) {
        error = QStringLiteral("could not read the widget's vertex buffer; refusing to overwrite it");
        return false;
    }
    if (cube_mesh::anyFaceCollapsed(fresh->savedCubeVertices)) {
        error = QStringLiteral("the cube's mesh is still collapsed although no module claims it, so its original "
                               "vertices are already lost; saving these zeros would record the loss as the "
                               "originals");
        return false;
    }

    if (!scene.program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)
        || !scene.program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)
        || !scene.program.link()) {
        error = scene.program.log();
        return false;
    }
    QOpenGLFunctions_3_3_Core* gl = scene.gl;
    gl->glGenVertexArrays(1, &scene.array);
    gl->glGenBuffers(1, &scene.buffer);
    gl->glBindVertexArray(scene.array);
    gl->glBindBuffer(GL_ARRAY_BUFFER, scene.buffer);
    const auto attribute = [gl](const GLuint index, const GLint size, const std::size_t offset) {
        gl->glEnableVertexAttribArray(index);
        gl->glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                  reinterpret_cast<const void*>(offset * sizeof(float)));
    };
    attribute(0, 3, 0);
    attribute(1, 3, 3);
    attribute(2, 4, 6);
    gl->glBindVertexArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);

    fresh->stashKey = cube_mesh::regionKey(0, cubeVertexFloats);
    fresh->claimKey = cube_mesh::claimKey(0, cubeVertexFloats);
    if (host->stash_put(host->agent_context, fresh->stashKey.constData(), fresh->savedCubeVertices.data(),
                        cubeVertexFloats * static_cast<std::int64_t>(sizeof(float)), 1) < 0) {
        gl->glDeleteVertexArrays(1, &scene.array);
        gl->glDeleteBuffers(1, &scene.buffer);
        error = QStringLiteral("the originals for the cube's mesh could not be recorded: %1 belongs to another "
                               "snippet. Nothing claims the region, so that entry describes a displacement no "
                               "longer in effect and is safe for the driver to stash.drop")
                    .arg(QString::fromUtf8(fresh->stashKey));
        return false;
    }
    const char marker = 1;
    host->stash_put(host->agent_context, fresh->claimKey.constData(), &marker, 1, 1);
    const std::vector<float> collapsed(cubeVertexFloats, 0.0F);
    cube->m_vertexBuffer.bind();
    cube->m_vertexBuffer.write(0, collapsed.data(), cubeVertexFloats * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();

    state = fresh.release();
    stateCube = cube;
    drawConnection = QObject::connect(
        cube, &CubeWidget::frameRendered, cube, [cube](qulonglong, float) { drawScene(cube); },
        Qt::DirectConnection);
    applyCamera(cube);
    if (cameraPointer == nullptr) {
        cameraPointer = new projection_camera::Pointer(
            cube, projection_camera::Pointer::Actions{
                      [] { return state != nullptr ? &state->camera : nullptr; },
                      [cube](const float across, const float up) { orbitCamera(cube, across, up); },
                      [cube](const float factor) {
                          if (state != nullptr) {
                              setCameraDistance(cube, state->camera.distance * factor);
                          }
                      },
                      [cube] { resetCamera(cube); },
                  });
        cube->installEventFilter(cameraPointer);
    }
    QMetaObject::invokeMethod(cube, [cube] { createPanel(cube); }, Qt::QueuedConnection);
    cube->update();
    return true;
}

// Needs the widget's OpenGL context to be current.
void remove(CubeWidget* cube)
{
    QObject::disconnect(drawConnection);
    if (state == nullptr) {
        return;
    }
    // Replay only onto this module's own displacement.
    std::vector<float> current(cubeVertexFloats);
    cube->m_vertexBuffer.bind();
    const bool readBack =
        cube->m_vertexBuffer.read(0, current.data(), cubeVertexFloats * static_cast<int>(sizeof(float)));
    const bool stillOurs = readBack && cube_mesh::anyFaceCollapsed(current);
    if (stillOurs) {
        cube->m_vertexBuffer.write(0, state->savedCubeVertices.data(),
                                   cubeVertexFloats * static_cast<int>(sizeof(float)));
    }
    cube->m_vertexBuffer.release();
    if ((stillOurs || !readBack) && hookHostValid) {
        hookHost.stash_drop(hookHost.agent_context, state->claimKey.constData());
    }

    destroyPanel();
    projection_camera::release(state->camera, cube);
    if (cameraPointer != nullptr) {
        cube->removeEventFilter(cameraPointer);
        cameraPointer->deleteLater();
        cameraPointer = nullptr;
    }
    if (state->scene != nullptr && state->scene->gl != nullptr) {
        state->scene->gl->glDeleteVertexArrays(1, &state->scene->array);
        state->scene->gl->glDeleteBuffers(1, &state->scene->buffer);
    }
    delete state;
    state = nullptr;
    stateCube = nullptr;
    cube->update();
}

void syncToggleAction()
{
    if (toggleAction != nullptr) {
        const QSignalBlocker blocker(toggleAction);
        toggleAction->setChecked(state != nullptr);
    }
}

bool setEnabled(CubeWidget* cube, const bool enabled, QString& error)
{
    if (enabled == (state != nullptr)) {
        return true;
    }
    bool installed = true;
    if (enabled) {
        installed = install(cube, error);
    } else {
        remove(cube);
    }
    syncToggleAction();
    return installed;
}

void ensureToggleAction(CubeWidget* cube)
{
    if (toggleAction != nullptr) {
        return;
    }
    toggleAction = scene_toggle::install(
        cube, QStringLiteral("actionTesseract"), QStringLiteral("&Tesseract"), QStringLiteral("Ctrl+Shift+T"),
        [cube](const bool enabled) {
            if (scene_toggle::ownContextIsCurrent(cube)) {
                QString ignored;
                setEnabled(cube, enabled, ignored);
                return;
            }
            cube->enqueueRenderCallback([cube, enabled] {
                QString deferred;
                if (!setEnabled(cube, enabled, deferred) && hookHostValid) {
                    hookHost.log(hookHost.agent_context, RUNTIME_AGENT_LOG_WARNING, deferred.toLocal8Bit().constData());
                }
                syncToggleAction();
            });
        });
    syncToggleAction();
}

// ---------------------------------------------------------------------------
// Reports and entry points
// ---------------------------------------------------------------------------

QJsonObject summary()
{
    QJsonObject result;
    result.insert(QStringLiteral("installed"), state != nullptr);
    result.insert(QStringLiteral("view"), QString::fromLatin1(viewNames[static_cast<int>(options.view)]));
    result.insert(QStringLiteral("direction"), QString::fromLatin1(directionNames[static_cast<int>(options.direction)]));
    result.insert(QStringLiteral("sweep"), options.sweep);
    QJsonObject rates;
    for (int p = 0; p < tesseract::planeCount; ++p) {
        rates.insert(QString::fromLatin1(tesseract::planeNames[p]), options.rates[p]);
    }
    result.insert(QStringLiteral("rates"), rates);
    result.insert(QStringLiteral("faces"), options.faces);
    result.insert(QStringLiteral("corners"), options.corners);
    if (state != nullptr) {
        result.insert(QStringLiteral("position"), state->position);
        result.insert(QStringLiteral("shape"), QString::fromStdString(state->shape));
        QJsonObject turned;
        for (int p = 0; p < tesseract::planeCount; ++p) {
            turned.insert(QString::fromLatin1(tesseract::planeNames[p]), state->turned[p] * 180.0 / tesseract::pi);
        }
        result.insert(QStringLiteral("turnedDegrees"), turned);
        result.insert(QStringLiteral("framesDrawn"), static_cast<qint64>(state->framesDrawn));
        result.insert(QStringLiteral("cameraDistance"), state->camera.distance);
    }
    return result;
}

void release(const RuntimeAgentHost* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI) {
        return;
    }
    auto* cube = static_cast<CubeWidget*>(host->find_qobject(host->agent_context, "cubeView"));
    if (cube == nullptr) {
        host->fail(host->invocation_context, "cubeView was not found");
        return;
    }
    if (state == nullptr) {
        host->complete_json(host->invocation_context, "{\"removed\":false,\"note\":\"nothing was installed\"}");
        return;
    }
    if (!scene_toggle::ownContextIsCurrent(cube)) {
        host->fail(host->invocation_context,
                   "the widget's OpenGL context is not current; release this module with executor=render so "
                   "the cube is back before this reports completion");
        return;
    }
    const std::uint64_t frames = state->framesDrawn;
    remove(cube);
    syncToggleAction();
    host->complete_json(host->invocation_context,
                        QJsonDocument(QJsonObject{{QStringLiteral("removed"), true},
                                                  {QStringLiteral("framesDrawn"), static_cast<qint64>(frames)}})
                            .toJson(QJsonDocument::Compact)
                            .constData());
}

int indexOf(const QString& name, const std::initializer_list<const char*> names)
{
    int i = 0;
    for (const char* candidate : names) {
        if (name.compare(QLatin1String(candidate), Qt::CaseInsensitive) == 0) {
            return i;
        }
        ++i;
    }
    return -1;
}

void run(const RuntimeAgentHost* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI) {
        return;
    }
    auto* cube = static_cast<CubeWidget*>(host->find_qobject(host->agent_context, "cubeView"));
    if (cube == nullptr) {
        host->fail(host->invocation_context, "cubeView was not found");
        return;
    }
    if (QOpenGLContext::currentContext() == nullptr) {
        host->fail(host->invocation_context, "no OpenGL context is current; use executor=render");
        return;
    }
    if (!hookHostValid) {
        hookHost = *host;
        hookHost.invocation_context = nullptr;
        hookHostValid = true;
    }
    const QJsonObject request =
        QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();
    ensureToggleAction(cube);
    const auto complete = [host](QJsonObject result) {
        result.insert(QStringLiteral("menuToggle"), toggleAction != nullptr);
        host->complete_json(host->invocation_context, QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    };

    // Settings apply whether or not it is installed, and survive being
    // switched off.
    if (request.contains(QStringLiteral("view"))) {
        const int v = indexOf(request.value(QStringLiteral("view")).toString(),
                              {"perspective", "stereographic", "slice"});
        if (v < 0) {
            host->fail(host->invocation_context, "view is one of perspective, stereographic, slice");
            return;
        }
        options.view = static_cast<View>(v);
    }
    if (request.contains(QStringLiteral("direction"))) {
        int d = indexOf(request.value(QStringLiteral("direction")).toString(),
                        {"cell", "face", "edge", "corner", "turning"});
        if (d < 0 && request.value(QStringLiteral("direction")).toString().compare(QStringLiteral("vertex"), Qt::CaseInsensitive) == 0) {
            d = static_cast<int>(Direction::Corner);
        }
        if (d < 0) {
            host->fail(host->invocation_context, "direction is one of cell, face, edge, corner, turning");
            return;
        }
        options.direction = static_cast<Direction>(d);
    }
    if (request.contains(QStringLiteral("position"))) {
        options.position = std::clamp(static_cast<float>(request.value(QStringLiteral("position")).toDouble()), -1.0F, 1.0F);
        options.sweep = false;
    }
    if (request.contains(QStringLiteral("sweep"))) {
        const bool sweep = request.value(QStringLiteral("sweep")).toBool();
        if (!sweep && state != nullptr && !request.contains(QStringLiteral("position"))) {
            options.position = state->position;
        }
        options.sweep = sweep;
    }
    if (request.contains(QStringLiteral("rates"))) {
        const QJsonObject rates = request.value(QStringLiteral("rates")).toObject();
        for (int p = 0; p < tesseract::planeCount; ++p) {
            const QString name = QString::fromLatin1(tesseract::planeNames[p]);
            if (rates.contains(name)) {
                options.rates[p] = std::clamp(static_cast<float>(rates.value(name).toDouble()), -2.0F, 2.0F);
            }
        }
    }
    for (const auto& [key, flag] : {std::pair{"faces", &options.faces}, std::pair{"corners", &options.corners},
                                    std::pair{"events", &options.events}}) {
        if (request.contains(QLatin1String(key))) {
            *flag = request.value(QLatin1String(key)).toBool();
        }
    }

    const bool takeOff = request.value(QStringLiteral("restore")).toBool()
        || request.value(QStringLiteral("remove")).toBool()
        || (request.value(QStringLiteral("toggle")).toBool() && state != nullptr);
    if (takeOff) {
        if (state == nullptr) {
            complete(QJsonObject{{QStringLiteral("removed"), false},
                                 {QStringLiteral("note"), QStringLiteral("no tesseract was installed")}});
            return;
        }
        const std::uint64_t frames = state->framesDrawn;
        remove(cube);
        syncToggleAction();
        complete(QJsonObject{{QStringLiteral("removed"), true},
                             {QStringLiteral("framesDrawn"), static_cast<qint64>(frames)}});
        return;
    }

    if (state == nullptr) {
        QString error;
        if (!setEnabled(cube, true, error)) {
            host->fail(host->invocation_context, error.toLocal8Bit().constData());
            return;
        }
    }
    if (request.value(QStringLiteral("resetCamera")).toBool()) {
        resetCamera(cube);
    }
    if (request.contains(QStringLiteral("orbit"))) {
        const QJsonArray by = request.value(QStringLiteral("orbit")).toArray();
        orbitCamera(cube, static_cast<float>(by.at(0).toDouble()), static_cast<float>(by.at(1).toDouble()));
    }
    if (request.contains(QStringLiteral("distance"))) {
        setCameraDistance(cube, static_cast<float>(request.value(QStringLiteral("distance")).toDouble(cubeViewDistance)));
    }
    cube->update();
    QJsonObject result = summary();
    result.insert(QStringLiteral("connectionValid"), static_cast<bool>(drawConnection));
    complete(result);
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "turn the cube into a tesseract and turn it through the fourth dimension",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init()
{
    return &descriptor;
}
