// Turns the cube into a small planet with a mob of kangaroos on it.
//
// The cube becomes the planet itself: its six faces are drawn as six Australian
// landscapes, each keeping the hue of the face it replaces, and a mob of
// kangaroos lives on them. They graze, drink at the billabongs, box, thump
// the ground in alarm, and hop from face to face. The physics is the real
// physics of a uniform cube turning on a fixed axle; kangaroo_sim.h says what
// that means and cube_gravity.h computes it.
//
// Three seams are used, each for what it owns:
//
//   the widget's vertex buffer   the planet draws every face itself, so the
//                                widget's own cube is displaced the way the
//                                other mesh replacements displace it, with the
//                                originals and a claim in the byte stash
//   frameRendered                the scene, drawn while the context is current
//                                and the depth buffer is empty of the cube
//   a host patch binding         the cube's angle. Every push-off and landing
//                                changes the mob's angular momentum about the
//                                axle, the planet takes the opposite change,
//                                and the step function is the only place the
//                                angle can be changed in a way that survives
//                                the next tick
//
// The simulation runs on the widget's own clock, read from the frame hook, so
// pausing the cube pauses the kangaroos and the rate they feel is the rate the
// planet was actually drawn turning at.
//
// Requests:
//   {}                       install, or report what the mob is doing
//   {"restore": true}        take it all off and put the cube back
//   {"toggle": true}         flip it
//   {"status": true}         every kangaroo, the water, the planet's spin
//   {"alarm": true}          someone thumps; the mob stampedes
//   {"box": true}            the two nearest males start a bout
//   {"throw": "Big Red"}     throw that kangaroo; "throwSpeed" sets how hard
//   {"mob": 6, "gravity": 14, "water": 0.05, "seed": 1}
//                            rebuild the scene with these
//   {"inertia": 40}          the planet's moment of inertia; small is lively
//   {"contours": true}       equipotential lines on the ground
//   {"events": true, "hopEvents": false}
//
// Events: kangaroo.flung, kangaroo.returned, kangaroo.alarm,
// kangaroo.bout.started, kangaroo.bout.ended, kangaroo.drink, kangaroo.joey,
// and kangaroo.hop when hopEvents is on.

#include "agent/agent_abi.h"
#include "cube_mesh.h"
#include "cube_widget.h"
#include "kangaroo_sim.h"
#include "scene_toggle.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QSignalBlocker>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using kangaroo::V3;

// ---------------------------------------------------------------------------
// Shared GLSL
// ---------------------------------------------------------------------------

// The six faces, numbered as kangaroo_math.h numbers them, and the landscape
// each one is. A face keeps the hue the widget gave it, so the planet is still
// recognisably the cube:
//
//   0 +x  magenta  pink salt lakes, the kind Western Australia has
//   1 -x  green    eucalypt woodland
//   2 +y  yellow   Mitchell grass plains
//   3 -y  blue     coastal heath and blue saltbush
//   4 +z  red      the red centre, dunes and spinifex
//   5 -z  cyan     a salt lake crust, white with turquoise water
constexpr auto glslCommon = R"glsl(
void faceFrame(int face, out vec3 n, out vec3 u, out vec3 v)
{
    int axis = face / 2;
    float s = (face % 2 == 0) ? 1.0 : -1.0;
    n = vec3(0.0); u = vec3(0.0); v = vec3(0.0);
    n[axis] = s;
    u[(axis + 1) % 3] = 1.0;
    v[(axis + 2) % 3] = s;
}

const vec3 sandA[6] = vec3[6](vec3(0.80, 0.60, 0.52), vec3(0.44, 0.40, 0.24), vec3(0.80, 0.68, 0.38),
                              vec3(0.47, 0.55, 0.53), vec3(0.80, 0.37, 0.17), vec3(0.90, 0.90, 0.86));
const vec3 sandB[6] = vec3[6](vec3(0.70, 0.47, 0.44), vec3(0.33, 0.30, 0.16), vec3(0.66, 0.54, 0.27),
                              vec3(0.36, 0.44, 0.44), vec3(0.60, 0.24, 0.10), vec3(0.76, 0.80, 0.80));
const vec3 accent[6] = vec3[6](vec3(0.93, 0.80, 0.78), vec3(0.24, 0.32, 0.14), vec3(0.88, 0.80, 0.52),
                               vec3(0.62, 0.70, 0.68), vec3(0.58, 0.58, 0.32), vec3(0.98, 0.98, 0.96));
const vec3 waterTint[6] = vec3[6](vec3(0.95, 0.44, 0.64), vec3(0.22, 0.36, 0.26), vec3(0.28, 0.50, 0.64),
                                  vec3(0.12, 0.30, 0.56), vec3(0.34, 0.47, 0.54), vec3(0.30, 0.78, 0.80));

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 w = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), w.x),
               mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), w.x), w.y);
}

float fbm(vec2 p)
{
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amplitude * valueNoise(p);
        p = p * 2.03 + vec2(17.1, 3.7);
        amplitude *= 0.5;
    }
    return sum;
}

// How much sunlight reaches a point whose outward normal is n: the planet
// shades its own far side.
float daylight(vec3 n, vec3 sun)
{
    return smoothstep(-0.18, 0.22, dot(n, sun));
}
)glsl";

// The Newtonian potential of the cube, the same closed form as
// cube_gravity.h, evaluated per pixel so the ground can show its own
// equipotentials.
constexpr auto glslPotential = R"glsl(
float lnPlus(float z, float r, float xy2)
{
    if (z >= 0.0) {
        return (r + z > 0.0) ? log(r + z) : 0.0;
    }
    if (xy2 <= 0.0) {
        return 0.0;
    }
    return log(xy2 / (r - z));
}

float xAtan(float x, float y, float z, float r)
{
    if (x == 0.0 || r == 0.0) {
        return 0.0;
    }
    return x * atan((y * z) / (x * r));
}

float potentialTerm(float x, float y, float z)
{
    float x2 = x * x;
    float y2 = y * y;
    float z2 = z * z;
    float r = sqrt(x2 + y2 + z2);
    return x * y * lnPlus(z, r, x2 + y2) + y * z * lnPlus(x, r, y2 + z2) + z * x * lnPlus(y, r, z2 + x2)
        - 0.5 * x * xAtan(x, y, z, r) - 0.5 * y * xAtan(y, z, x, r) - 0.5 * z * xAtan(z, x, y, r);
}

float cubePotential(vec3 p)
{
    float sum = 0.0;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                float x = (i == 0 ? -1.0 : 1.0) - p.x;
                float y = (j == 0 ? -1.0 : 1.0) - p.y;
                float z = (k == 0 ? -1.0 : 1.0) - p.z;
                float s = ((i + j + k) % 2 == 1) ? -1.0 : 1.0;
                sum += s * potentialTerm(x, y, z);
            }
        }
    }
    return -sum;
}
)glsl";

// ---------------------------------------------------------------------------
// Ground
// ---------------------------------------------------------------------------

constexpr auto terrainVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in float inFace;
uniform mat4 mvp;
out vec3 bodyPosition;
flat out int faceIndex;

void main()
{
    bodyPosition = inPosition;
    faceIndex = int(inFace + 0.5);
    gl_Position = mvp * vec4(inPosition, 1.0);
}
)glsl";

constexpr auto terrainFragmentShader = R"glsl(
in vec3 bodyPosition;
flat in int faceIndex;
uniform vec3 sunBody;
uniform vec3 tint;
uniform float omega;
uniform vec3 axle;
uniform float gravityScale;
uniform float contourSpacing;
uniform float contourStrength;
uniform float time;
uniform sampler2D depthMap;
uniform vec4 shadowPosition[16];
uniform vec4 shadowShape[16];
uniform int shadowCount;
uniform vec4 printPosition[64];
uniform vec4 printShape[64];
uniform int printCount;
out vec4 fragmentColor;

float depthAt(int face, vec2 st)
{
    vec2 grid = (st + 1.0) * 0.5 * 40.0 + 0.5;
    vec2 uv = vec2(grid.x / 41.0, (float(face) * 41.0 + grid.y) / 246.0);
    return texture(depthMap, uv).r;
}

float segment(vec2 p, vec2 a, vec2 b, float r)
{
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

// A crack network for the salt crust: distance to the nearest cell border.
float cracks(vec2 p)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    float first = 8.0;
    float second = 8.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 offset = vec2(float(i), float(j));
            vec2 point = offset + vec2(hash12(cell + offset), hash12(cell + offset + 31.7)) - f;
            float d = dot(point, point);
            if (d < first) {
                second = first;
                first = d;
            } else if (d < second) {
                second = d;
            }
        }
    }
    return sqrt(second) - sqrt(first);
}

void main()
{
    vec3 n;
    vec3 u;
    vec3 v;
    faceFrame(faceIndex, n, u, v);
    vec3 p = bodyPosition;
    vec2 st = vec2(dot(p, u), dot(p, v));
    vec2 q = st * 6.0 + vec2(float(faceIndex) * 7.3, float(faceIndex) * 3.1);

    float grain = fbm(q);
    vec3 colour = mix(sandB[faceIndex], sandA[faceIndex], smoothstep(0.25, 0.75, grain));
    float bump = 0.0;
    if (faceIndex == 4) {
        // Dunes: long parallel ridges with a lit and a shaded side.
        float dune = sin(dot(st, vec2(0.8, 0.6)) * 26.0 + fbm(st * 3.0) * 5.0);
        colour *= 0.86 + 0.16 * dune;
        bump = dune * 0.18;
        float spinifex = smoothstep(0.78, 0.84, valueNoise(st * 34.0));
        colour = mix(colour, accent[faceIndex], spinifex * 0.8);
    } else if (faceIndex == 5) {
        float crack = cracks(st * 9.0);
        colour = mix(colour * 0.8, colour, smoothstep(0.02, 0.09, crack));
    } else if (faceIndex == 2) {
        float streak = fbm(vec2(st.x * 3.0, st.y * 26.0));
        colour = mix(colour, accent[faceIndex], smoothstep(0.55, 0.75, streak) * 0.6);
    } else {
        float speckle = smoothstep(0.72, 0.8, valueNoise(st * 40.0 + 9.0));
        colour = mix(colour, accent[faceIndex], speckle * 0.6);
    }

    // Wet ground under the water, and a damp band just above its shore. The
    // map is the water's height over the ground, negative on dry land.
    float depth = depthAt(faceIndex, st);
    float damp = smoothstep(-0.012, 0.0, depth);
    colour = mix(colour, colour * vec3(0.62, 0.6, 0.6), max(damp * 0.6, step(0.0005, depth) * 0.75));

    // Equipotentials: lines on which the felt "height" is constant. On a
    // cube they are rings round each face centre, because the centre is the
    // bottom of a bowl, and they slide toward the far side of a face as the
    // spin rises.
    if (contourStrength > 0.0 && depth < 0.0005) {
        vec3 off = p - axle * dot(p, axle);
        float phi = -gravityScale * cubePotential(p) - 0.5 * omega * omega * dot(off, off);
        float f = phi / contourSpacing;
        float line = 1.0 - clamp(abs(fract(f + 0.5) - 0.5) / max(fwidth(f), 1e-4) * 0.9, 0.0, 1.0);
        colour = mix(colour, colour * 0.55, line * contourStrength);
    }

    // Footprints, fading as they age.
    vec3 right;
    for (int i = 0; i < printCount; ++i) {
        vec3 rel = p - printPosition[i].xyz;
        if (dot(rel, rel) > 0.02) {
            continue;
        }
        vec3 along = printShape[i].xyz;
        right = cross(along, n);
        float size = fract(printShape[i].w);
        int kind = int(printShape[i].w);
        vec2 local = vec2(dot(rel, along), dot(rel, right)) / size;
        float d;
        if (kind == 0) {
            // A hind foot: long and narrow, with the big fourth toe ahead.
            d = min(segment(local, vec2(-0.1, 0.0), vec2(0.07, 0.0), 0.022),
                    segment(local, vec2(0.07, 0.0), vec2(0.13, 0.004), 0.014));
        } else if (kind == 1) {
            d = segment(local, vec2(-0.25, 0.0), vec2(0.05, 0.0), 0.016);
        } else {
            d = length(local) - 0.02;
        }
        float fade = 1.0 - smoothstep(6.0, 14.0, printPosition[i].w);
        float mark = 1.0 - smoothstep(-0.004, 0.004, d);
        colour = mix(colour, colour * 0.58, mark * fade);
    }

    // Shadows of the mob, softer and wider the higher an animal is.
    float shade = 1.0;
    for (int i = 0; i < shadowCount; ++i) {
        vec3 rel = p - shadowPosition[i].xyz;
        float radius = shadowPosition[i].w;
        vec3 along = shadowShape[i].xyz;
        float a = dot(rel, along);
        float b = length(rel - along * a);
        float d = length(vec2(a / 1.7, b));
        shade *= 1.0 - shadowShape[i].w * (1.0 - smoothstep(0.25 * radius, radius, d));
    }

    vec3 normal = normalize(n + (u * cos(bump) - v * sin(bump)) * 0.04 * bump);
    float sun = max(dot(normal, sunBody), 0.0);
    float day = daylight(n, sunBody);
    vec3 lit = colour * (0.22 + 0.86 * sun * shade * day) + colour * vec3(0.05, 0.06, 0.1) * (1.0 - day);
    fragmentColor = vec4(clamp(lit * tint, 0.0, 1.0), 1.0);
}
)glsl";

// ---------------------------------------------------------------------------
// Water
// ---------------------------------------------------------------------------

constexpr auto waterVertexShader = R"glsl(
layout(location = 0) in vec2 inGrid;
layout(location = 1) in float inFace;
uniform mat4 mvp;
uniform sampler2D depthMap;
out vec3 bodyPosition;
out vec3 surfaceNormal;
out float waterDepth;
flat out int faceIndex;

float depthAtGrid(int face, vec2 grid)
{
    vec2 g = clamp(grid, vec2(0.0), vec2(40.0)) + 0.5;
    return textureLod(depthMap, vec2(g.x / 41.0, (float(face) * 41.0 + g.y) / 246.0), 0.0).r;
}

void main()
{
    int face = int(inFace + 0.5);
    vec3 n;
    vec3 u;
    vec3 v;
    faceFrame(face, n, u, v);
    vec2 st = inGrid / 40.0 * 2.0 - 1.0;
    float d = depthAtGrid(face, inGrid);
    // The surface is level in the felt gravity, so it tilts relative to the
    // face by the slope of the depth.
    float ds = (depthAtGrid(face, inGrid + vec2(1.0, 0.0)) - depthAtGrid(face, inGrid - vec2(1.0, 0.0))) / 0.1;
    float dt = (depthAtGrid(face, inGrid + vec2(0.0, 1.0)) - depthAtGrid(face, inGrid - vec2(0.0, 1.0))) / 0.1;
    surfaceNormal = normalize(n - u * ds - v * dt);
    vec3 p = n + u * st.x + v * st.y + n * (d > 0.0 ? d : -0.004);
    bodyPosition = p;
    waterDepth = d;
    faceIndex = face;
    gl_Position = mvp * vec4(p, 1.0);
}
)glsl";

constexpr auto waterFragmentShader = R"glsl(
in vec3 bodyPosition;
in vec3 surfaceNormal;
in float waterDepth;
flat in int faceIndex;
uniform vec3 sunBody;
uniform vec3 cameraBody;
uniform vec3 tint;
uniform float time;
uniform vec4 ripples[16];
uniform int rippleCount;
out vec4 fragmentColor;

void main()
{
    if (waterDepth < 0.0006) {
        discard;
    }
    vec3 n;
    vec3 u;
    vec3 v;
    faceFrame(faceIndex, n, u, v);
    vec3 normal = normalize(surfaceNormal);
    // Rings spreading from where something landed or drank.
    for (int i = 0; i < rippleCount; ++i) {
        vec3 rel = bodyPosition - ripples[i].xyz;
        rel -= n * dot(rel, n);
        float r = length(rel);
        float age = ripples[i].w;
        float front = 0.32 * age;
        float envelope = exp(-age * 1.6) * smoothstep(0.09, 0.0, abs(r - front));
        float wave = cos((r - front) * 90.0) * envelope;
        if (r > 1e-4) {
            normal = normalize(normal + (rel / r) * wave * 0.5);
        }
    }
    float shimmer = fbm(vec2(dot(bodyPosition, u), dot(bodyPosition, v)) * 14.0 + time * 0.6);
    normal = normalize(normal + (u * (shimmer - 0.5) + v * (fbm(bodyPosition.zx * 13.0 - time * 0.5) - 0.5)) * 0.12);

    vec3 view = normalize(cameraBody - bodyPosition);
    float fresnel = 0.04 + 0.96 * pow(1.0 - max(dot(normal, view), 0.0), 5.0);
    vec3 reflected = reflect(-view, normal);
    float glint = pow(max(dot(reflected, sunBody), 0.0), 90.0);
    float day = daylight(n, sunBody);
    vec3 deep = waterTint[faceIndex];
    vec3 colour = deep * (0.35 + 0.65 * max(dot(normal, sunBody), 0.0) * day);
    colour = mix(colour, vec3(0.75, 0.85, 0.95) * day, fresnel * 0.5);
    colour += vec3(1.0, 0.95, 0.85) * glint * 1.4 * day;
    float foam = 1.0 - smoothstep(0.0006, 0.005, waterDepth);
    colour = mix(colour, vec3(0.92, 0.94, 0.95) * (0.3 + 0.7 * day), foam * 0.7);
    float alpha = clamp(1.0 - exp(-waterDepth * 70.0), 0.0, 0.92);
    alpha = max(alpha, foam * 0.8);
    fragmentColor = vec4(clamp(colour * tint, 0.0, 1.0), alpha);
}
)glsl";

// ---------------------------------------------------------------------------
// Kangaroos
// ---------------------------------------------------------------------------

constexpr auto kangarooVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inCorner;
uniform mat4 mvp;
uniform vec3 boundsMin;
uniform vec3 boundsMax;
out vec3 localPosition;

void main()
{
    localPosition = mix(boundsMin, boundsMax, inCorner);
    gl_Position = mvp * vec4(localPosition, 1.0);
}
)glsl";

// The body is ray marched: the rounded cones from kangaroo_rig.h blended
// into one surface. Each primitive also carries a bounding sphere, so a
// primitive that cannot touch the current distance is skipped without
// evaluating it; the blend cannot reach further than its radius.
constexpr auto kangarooFragmentShader = R"glsl(
#version 330 core
in vec3 localPosition;
uniform mat4 mvp;
uniform vec3 cameraLocal;
uniform vec3 sunLocal;
uniform vec3 boundsMin;
uniform vec3 boundsMax;
uniform float day;
uniform float grounded;
uniform vec3 furColour;
uniform vec3 paleColour;
uniform vec3 footColour;
uniform vec3 joeyColour;
uniform vec4 primA[40];
uniform vec4 primB[40];
uniform vec4 primBound[40];
uniform vec2 primInfo[40];
uniform int primCount;
out vec4 fragmentColor;

float roundCone(vec3 p, vec3 a, vec3 b, float r1, float r2)
{
    vec3 ba = b - a;
    float l2 = dot(ba, ba);
    float rr = r1 - r2;
    float a2 = l2 - rr * rr;
    if (l2 < 1e-10 || a2 <= 1e-10) {
        return r1 >= r2 ? length(p - a) - r1 : length(p - b) - r2;
    }
    float il2 = 1.0 / l2;
    vec3 pa = p - a;
    float y = dot(pa, ba);
    float z = y - l2;
    vec3 xv = pa * l2 - ba * y;
    float x2 = dot(xv, xv);
    float y2 = y * y * l2;
    float z2 = z * z * l2;
    float k = sign(rr) * rr * rr * x2;
    if (sign(z) * a2 * z2 > k) {
        return sqrt(x2 + z2) * il2 - r2;
    }
    if (sign(y) * a2 * y2 < k) {
        return sqrt(x2 + y2) * il2 - r1;
    }
    return (sqrt(x2 * a2 * il2) + y * rr) * il2 - r1;
}

float mapDistance(vec3 p)
{
    float d = 1e9;
    for (int i = 0; i < primCount; ++i) {
        float k = primInfo[i].y;
        if (length(p - primBound[i].xyz) - primBound[i].w > d + k) {
            continue;
        }
        float di = roundCone(p, primA[i].xyz, primB[i].xyz, primA[i].w, primB[i].w);
        if (k > 0.0) {
            float h = clamp(0.5 + 0.5 * (di - d) / k, 0.0, 1.0);
            d = mix(di, d, h) - k * h * (1.0 - h);
        } else {
            d = min(d, di);
        }
    }
    return d;
}

vec3 materialColour(int m)
{
    if (m == 0) return furColour;
    if (m == 1) return paleColour;
    if (m == 2) return vec3(0.07, 0.055, 0.05);
    if (m == 3) return vec3(0.015, 0.012, 0.01);
    if (m == 4) return footColour;
    return joeyColour;
}

vec3 mapColour(vec3 p, out float eye)
{
    float d = 1e9;
    vec3 colour = vec3(0.0);
    eye = 0.0;
    for (int i = 0; i < primCount; ++i) {
        float di = roundCone(p, primA[i].xyz, primB[i].xyz, primA[i].w, primB[i].w);
        int m = int(primInfo[i].x + 0.5);
        vec3 ci = materialColour(m);
        float ei = (m == 3 || m == 2) ? 1.0 : 0.0;
        float k = primInfo[i].y;
        if (i == 0) {
            d = di;
            colour = ci;
            eye = ei;
        } else if (k > 0.0) {
            float h = clamp(0.5 + 0.5 * (di - d) / k, 0.0, 1.0);
            d = mix(di, d, h) - k * h * (1.0 - h);
            colour = mix(ci, colour, h);
            eye = mix(ei, eye, h);
        } else if (di < d) {
            d = di;
            colour = ci;
            eye = ei;
        }
    }
    return colour;
}

vec3 normalAt(vec3 p)
{
    const float e = 0.0015;
    vec2 k = vec2(1.0, -1.0);
    return normalize(k.xyy * mapDistance(p + k.xyy * e) + k.yyx * mapDistance(p + k.yyx * e)
                     + k.yxy * mapDistance(p + k.yxy * e) + k.xxx * mapDistance(p + k.xxx * e));
}

float softShadow(vec3 p, vec3 l)
{
    float lit = 1.0;
    float t = 0.02;
    for (int i = 0; i < 14; ++i) {
        float h = mapDistance(p + l * t);
        lit = min(lit, 10.0 * h / t);
        t += clamp(h, 0.02, 0.12);
        if (lit < 0.05 || t > 1.2) {
            break;
        }
    }
    return clamp(lit, 0.0, 1.0);
}

float occlusion(vec3 p, vec3 n)
{
    float o = 0.0;
    float scale = 1.0;
    for (int i = 1; i <= 4; ++i) {
        float h = 0.025 * float(i);
        o += (h - mapDistance(p + n * h)) * scale;
        scale *= 0.7;
    }
    return clamp(1.0 - 3.0 * o, 0.0, 1.0);
}

void main()
{
    vec3 ro = cameraLocal;
    vec3 rd = normalize(localPosition - cameraLocal);
    vec3 inverse = 1.0 / rd;
    vec3 t0 = (boundsMin - ro) * inverse;
    vec3 t1 = (boundsMax - ro) * inverse;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    float enter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
    float leave = min(min(tmax.x, tmax.y), tmax.z);
    float t = enter;
    bool hit = false;
    vec3 p = ro;
    for (int i = 0; i < 110; ++i) {
        p = ro + rd * t;
        float d = mapDistance(p);
        if (d < 0.0012) {
            hit = true;
            break;
        }
        t += d;
        if (t > leave) {
            break;
        }
    }
    if (!hit) {
        discard;
    }

    vec3 n = normalAt(p);
    float eye;
    vec3 albedo = mapColour(p, eye);
    // A little variation, so the coat does not read as plastic.
    float streak = fract(sin(dot(floor(p * 90.0), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    albedo *= 0.92 + 0.12 * streak * (1.0 - eye);

    vec3 l = normalize(sunLocal);
    float wrap = clamp((dot(n, l) + 0.25) / 1.25, 0.0, 1.0);
    float shadow = softShadow(p + n * 0.004, l);
    float ao = occlusion(p, n);
    // Contact darkening near the ground the animal stands on.
    float contact = mix(1.0, smoothstep(0.0, 0.12, p.y) * 0.5 + 0.5, grounded);
    vec3 sky = vec3(0.55, 0.62, 0.75);
    vec3 bounce = vec3(0.55, 0.38, 0.28);
    float upness = n.y * 0.5 + 0.5;
    vec3 ambient = mix(bounce, sky, upness) * 0.32 * ao * contact;
    vec3 view = normalize(ro - p);
    float rim = pow(1.0 - max(dot(n, view), 0.0), 3.0) * 0.35 * (1.0 - eye);
    vec3 colour = albedo * (ambient * (0.4 + 0.6 * day) + vec3(1.0, 0.95, 0.86) * wrap * shadow * day * 1.05);
    colour += albedo * rim * (0.3 + 0.7 * day);
    vec3 h = normalize(l + view);
    colour += vec3(0.9) * pow(max(dot(n, h), 0.0), 60.0) * eye * day * shadow;

    vec4 clip = mvp * vec4(p, 1.0);
    gl_FragDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);
    fragmentColor = vec4(clamp(colour, 0.0, 1.0), 1.0);
}
)glsl";

// ---------------------------------------------------------------------------
// Trees, the road sign, grass
// ---------------------------------------------------------------------------

constexpr auto propsVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColour;
layout(location = 3) in vec3 inPattern;
uniform mat4 mvp;
uniform float time;
out vec3 bodyPosition;
out vec3 bodyNormal;
out vec3 colour;
out vec3 pattern;

void main()
{
    vec3 p = inPosition;
    // Canopy sways a little; trunks, posts and signs do not.
    if (inPattern.z > 0.5) {
        p += vec3(sin(time * 1.3 + p.y * 9.0), 0.0, cos(time * 1.1 + p.x * 9.0)) * 0.004;
    }
    bodyPosition = p;
    bodyNormal = inNormal;
    colour = inColour;
    pattern = inPattern;
    gl_Position = mvp * vec4(p, 1.0);
}
)glsl";

constexpr auto propsFragmentShader = R"glsl(
in vec3 bodyPosition;
in vec3 bodyNormal;
in vec3 colour;
in vec3 pattern;
uniform vec3 sunBody;
uniform vec3 surfaceUp;
uniform vec4 signShape[32];
uniform vec2 signRadius[32];
uniform int signCount;
out vec4 fragmentColor;

float unevenCapsule(vec2 p, vec2 pa, vec2 pb, float ra, float rb)
{
    p -= pa;
    pb -= pa;
    float h = dot(pb, pb);
    if (h < 1e-8) {
        return length(p) - ra;
    }
    vec2 q = vec2(dot(p, vec2(pb.y, -pb.x)), dot(p, pb)) / h;
    q.x = abs(q.x);
    float b = ra - rb;
    if (h <= b * b) {
        return length(p) - max(ra, rb);
    }
    vec2 c = vec2(sqrt(h - b * b), b);
    float k = c.x * q.y - c.y * q.x;
    float m = dot(c, q);
    float n = dot(q, q);
    if (k < 0.0) return sqrt(h * n) - ra;
    if (k > c.x) return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;
    return m - ra;
}

void main()
{
    vec3 albedo = colour;
    if (pattern.z < -0.5) {
        // The face of the sign: a yellow diamond with a black border and a
        // leaping kangaroo, drawn from the same rig as the mob.
        vec2 uv = pattern.xy;
        float diamond = abs(uv.x) + abs(uv.y);
        albedo = vec3(0.98, 0.78, 0.08);
        if (diamond > 0.84 && diamond < 0.92) {
            albedo = vec3(0.04);
        }
        vec2 rig = vec2(uv.x * 0.62 - 0.12, uv.y * 0.62 + 0.52);
        float d = 1e9;
        for (int i = 0; i < signCount; ++i) {
            d = min(d, unevenCapsule(rig, signShape[i].xy, signShape[i].zw, signRadius[i].x, signRadius[i].y));
        }
        if (d < 0.0 && diamond < 0.84) {
            albedo = vec3(0.04);
        }
    }
    vec3 n = normalize(bodyNormal);
    float sun = max(dot(n, sunBody), 0.0);
    float day = daylight(surfaceUp, sunBody) * 0.85 + 0.15 * daylight(n, sunBody);
    vec3 lit = albedo * (0.25 + 0.85 * sun * day) + albedo * vec3(0.04, 0.05, 0.08) * (1.0 - day);
    fragmentColor = vec4(clamp(lit, 0.0, 1.0), 1.0);
}
)glsl";

constexpr auto grassVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inBase;
layout(location = 1) in vec3 inOffset;
layout(location = 2) in vec4 inColourHeight;
layout(location = 3) in vec2 inSwayFace;
uniform mat4 mvp;
uniform float time;
out vec3 colour;
out float tipness;
flat out int faceIndex;

void main()
{
    float height = inColourHeight.w;
    vec3 p = inBase + inOffset * height;
    float tip = clamp(height, 0.0, 1.0) * step(0.5, length(inOffset) * 20.0);
    p += vec3(sin(time * 2.1 + inSwayFace.x * 6.0), cos(time * 1.7 + inSwayFace.x * 5.0),
              sin(time * 1.9 + inSwayFace.x * 4.0)) * 0.004 * tip;
    colour = inColourHeight.rgb;
    tipness = tip;
    faceIndex = int(inSwayFace.y + 0.5);
    gl_Position = mvp * vec4(p, 1.0);
}
)glsl";

constexpr auto grassFragmentShader = R"glsl(
in vec3 colour;
in float tipness;
flat in int faceIndex;
uniform vec3 sunBody;
out vec4 fragmentColor;

void main()
{
    vec3 n;
    vec3 u;
    vec3 v;
    faceFrame(faceIndex, n, u, v);
    float day = daylight(n, sunBody);
    float sun = max(dot(n, sunBody), 0.0);
    vec3 c = colour * (0.75 + 0.35 * tipness);
    vec3 lit = c * (0.3 + 0.8 * sun * day) + c * vec3(0.04, 0.05, 0.08) * (1.0 - day);
    fragmentColor = vec4(clamp(lit, 0.0, 1.0), 1.0);
}
)glsl";

// ---------------------------------------------------------------------------
// Dust and spray
// ---------------------------------------------------------------------------

constexpr auto particleVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColour;
layout(location = 2) in float inSize;
uniform mat4 mvp;
uniform float pixelsPerUnit;
out vec4 colour;

void main()
{
    gl_Position = mvp * vec4(inPosition, 1.0);
    gl_PointSize = max(1.5, inSize * pixelsPerUnit / gl_Position.w);
    colour = inColour;
}
)glsl";

constexpr auto particleFragmentShader = R"glsl(
#version 330 core
in vec4 colour;
out vec4 fragmentColor;

void main()
{
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r = dot(c, c);
    if (r > 1.0) {
        discard;
    }
    fragmentColor = vec4(colour.rgb, colour.a * (1.0 - r));
}
)glsl";

// The same palettes as the shaders, for colouring particles on the CPU.
constexpr std::array<std::array<float, 3>, 6> dustColour{{
    {0.80F, 0.60F, 0.52F}, {0.44F, 0.40F, 0.24F}, {0.80F, 0.68F, 0.38F},
    {0.47F, 0.55F, 0.53F}, {0.80F, 0.37F, 0.17F}, {0.90F, 0.90F, 0.86F},
}};
constexpr std::array<std::array<float, 3>, 6> sprayColour{{
    {1.00F, 0.70F, 0.82F}, {0.55F, 0.68F, 0.58F}, {0.70F, 0.85F, 0.95F},
    {0.60F, 0.75F, 0.95F}, {0.72F, 0.82F, 0.88F}, {0.70F, 0.96F, 0.98F},
}};
constexpr std::array<std::array<float, 3>, 6> grassColour{{
    {0.66F, 0.60F, 0.40F}, {0.36F, 0.46F, 0.20F}, {0.84F, 0.72F, 0.36F},
    {0.42F, 0.56F, 0.50F}, {0.60F, 0.62F, 0.34F}, {0.56F, 0.62F, 0.52F},
}};

// Where the sun is, in the view's frame: high, to the left and behind the
// camera, so the faces turned toward the viewer are the lit ones.
const V3 sunInView = kangaroo::normalize(V3{-0.42F, 0.62F, 0.66F});

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Options {
    kangaroo::Settings settings;
    bool contours = true;
    bool events = true;
};

struct GlScene {
    QOpenGLContext* owner = nullptr;
    QOpenGLFunctions_3_3_Core* gl = nullptr;

    QOpenGLShaderProgram terrain;
    QOpenGLShaderProgram water;
    QOpenGLShaderProgram roo;
    QOpenGLShaderProgram props;
    QOpenGLShaderProgram grass;
    QOpenGLShaderProgram particles;

    GLuint terrainArray = 0;
    GLuint terrainBuffer = 0;
    GLuint waterArray = 0;
    GLuint waterBuffer = 0;
    GLuint waterIndices = 0;
    GLsizei waterIndexCount = 0;
    GLuint depthTexture = 0;
    GLuint boxArray = 0;
    GLuint boxBuffer = 0;
    GLuint propsArray = 0;
    GLuint propsBuffer = 0;
    GLsizei propsCount = 0;
    GLuint grassArray = 0;
    GLuint grassBuffer = 0;
    GLsizei grassCount = 0;
    GLuint particleArray = 0;
    GLuint particleBuffer = 0;

    std::vector<QVector4D> signShape;
    std::vector<QVector2D> signRadius;
};

struct Planet {
    std::unique_ptr<GlScene> gl;
    kangaroo::Mob mob;

    // The widget's own vertices, displaced while the planet is drawn.
    std::vector<float> savedCubeVertices;
    QByteArray stashKey;
    QByteArray claimKey;

    // The binding the host gave us, and the planet's extra spin.
    std::uint64_t binding = 0;
    bool spinFeedback = false;
    QString spinFeedbackNote;
    kangaroo::SpinResponse spin;

    // The widget's clock and angle when the last frame was simulated.
    bool clockSeen = false;
    float lastElapsed = 0.0F;
    float lastAngle = 0.0F;
    float omega = 0.0F;
    // The rate the mob feels, from the step function's own record of each
    // turn; see kangaroo_sim.h for why not the drawn angle.
    kangaroo::RateMeter rate;

    std::uint64_t framesDrawn = 0;
    double simulateMs = 0.0;
    double drawMs = 0.0;
    float grassClock = 0.0F;
};

Planet* planet = nullptr;
Options options;
QMetaObject::Connection drawConnection;
QAction* toggleAction = nullptr;
RuntimeAgentHost hookHost{};
bool hookHostValid = false;

// The step function the binding displaced. Kept after release: a binding made
// after this one was handed this module's step as its own previous, and goes
// on calling it, so it has to keep working with nothing installed.
CubeStepFunction chainedStep = nullptr;

// ---------------------------------------------------------------------------
// The planet's spin, as the replacement step function
// ---------------------------------------------------------------------------

CubeStepOutput kangarooStep(const CubeStepInput* input) noexcept
{
    CubeStepOutput output = chainedStep != nullptr ? chainedStep(input) : cube_step_builtin(input);
    if (input == nullptr || planet == nullptr) {
        return output;
    }
    const float dt = std::clamp(input->delta_seconds, 0.0F, 0.1F);
    // The mob's pushes, as angular impulse about the axle, become spin.
    const double impulse = planet->mob.takeImpulse();
    planet->spin.apply(planet->spinFeedback ? impulse : 0.0, options.settings.planetInertia, dt);
    output.angle_degrees += static_cast<float>(planet->spin.boost * 180.0 / kangaroo::pi) * dt;
    planet->rate.stepped(input->angle_degrees, output.angle_degrees, dt);
    return output;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void pushVertex(std::vector<float>& out, const V3& p, const V3& n, const V3& colour, const V3& pattern)
{
    out.insert(out.end(), {p.x, p.y, p.z, n.x, n.y, n.z, colour.x, colour.y, colour.z,
                           pattern.x, pattern.y, pattern.z});
}

void pushTriangle(std::vector<float>& out, const V3& a, const V3& b, const V3& c, const V3& colour,
                  const V3& pattern = V3{0.0F, 0.0F, 0.0F})
{
    const V3 n = kangaroo::normalize(kangaroo::cross(b - a, c - a));
    pushVertex(out, a, n, colour, pattern);
    pushVertex(out, b, n, colour, pattern);
    pushVertex(out, c, n, colour, pattern);
}

// A tapered tube from a to b, smooth-shaded.
void pushTube(std::vector<float>& out, const V3& a, const V3& b, const float ra, const float rb,
              const V3& colour, const int sides = 7)
{
    const V3 axis = kangaroo::normalize(b - a);
    V3 side = kangaroo::normalize(kangaroo::cross(axis, std::fabs(axis.y) < 0.9F ? V3{0, 1, 0} : V3{1, 0, 0}));
    const V3 other = kangaroo::cross(axis, side);
    for (int i = 0; i < sides; ++i) {
        const float a0 = 2.0F * kangaroo::pi * static_cast<float>(i) / static_cast<float>(sides);
        const float a1 = 2.0F * kangaroo::pi * static_cast<float>(i + 1) / static_cast<float>(sides);
        const V3 n0 = side * std::cos(a0) + other * std::sin(a0);
        const V3 n1 = side * std::cos(a1) + other * std::sin(a1);
        const V3 p00 = a + n0 * ra;
        const V3 p01 = a + n1 * ra;
        const V3 p10 = b + n0 * rb;
        const V3 p11 = b + n1 * rb;
        pushVertex(out, p00, n0, colour, {});
        pushVertex(out, p01, n1, colour, {});
        pushVertex(out, p11, n1, colour, {});
        pushVertex(out, p00, n0, colour, {});
        pushVertex(out, p11, n1, colour, {});
        pushVertex(out, p10, n0, colour, {});
    }
}

// A lumpy blob of foliage: an octahedron subdivided twice and pushed onto a
// sphere, with its radius jittered so it does not read as a ball.
void pushBlob(std::vector<float>& out, const V3& centre, const float radius, const V3& colour, kangaroo::Rng& rng)
{
    std::vector<std::array<V3, 3>> faces;
    const std::array<V3, 6> o{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    const int tri[8][3] = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}};
    for (const auto& t : tri) {
        faces.push_back({o[t[0]], o[t[1]], o[t[2]]});
    }
    for (int level = 0; level < 2; ++level) {
        std::vector<std::array<V3, 3>> finer;
        for (const auto& f : faces) {
            const V3 ab = kangaroo::normalize(f[0] + f[1]);
            const V3 bc = kangaroo::normalize(f[1] + f[2]);
            const V3 ca = kangaroo::normalize(f[2] + f[0]);
            finer.push_back({f[0], ab, ca});
            finer.push_back({ab, f[1], bc});
            finer.push_back({ca, bc, f[2]});
            finer.push_back({ab, bc, ca});
        }
        faces.swap(finer);
    }
    const float wobble = rng.range(0.0F, 10.0F);
    const auto bump = [&](const V3& d) {
        return 1.0F + 0.16F * std::sin(d.x * 7.0F + wobble) * std::sin(d.y * 6.0F + 2.0F * wobble) * std::sin(d.z * 8.0F);
    };
    for (const auto& f : faces) {
        const V3 c = colour * rng.range(0.85F, 1.12F);
        for (const V3& d : f) {
            pushVertex(out, centre + d * (radius * bump(d)), d, c, V3{0.0F, 0.0F, 1.0F});
        }
    }
}

std::vector<float> buildProps(const kangaroo::Mob& mob)
{
    std::vector<float> out;
    kangaroo::Rng rng(mob.settings.seed * 977 + 3);
    for (const kangaroo::Tree& tree : mob.trees) {
        // A gum tree grows against the local gravity, so near an edge it
        // leans away from the face centre's "down".
        const V3 up = tree.up;
        V3 side = kangaroo::normalize(kangaroo::cross(up, kangaroo::faceNormal(tree.face) + V3{0.3F, 0.1F, 0.2F}));
        const V3 trunkTop = tree.p + up * tree.height + side * (0.04F * std::sin(tree.seed * 10.0F));
        const V3 bark{0.86F, 0.84F, 0.78F};
        pushTube(out, tree.p - up * 0.01F, trunkTop, 0.022F, 0.011F, bark);
        const int branches = 3;
        for (int b = 0; b < branches; ++b) {
            const float angle = tree.seed * 6.0F + 2.1F * static_cast<float>(b);
            const V3 out1 = kangaroo::normalize(side * std::cos(angle) + kangaroo::cross(up, side) * std::sin(angle));
            const V3 from = tree.p + up * (tree.height * (0.55F + 0.12F * static_cast<float>(b)));
            const V3 to = from + out1 * (0.09F + 0.03F * static_cast<float>(b)) + up * 0.08F;
            pushTube(out, from, to, 0.009F, 0.005F, bark * 0.94F, 5);
            pushBlob(out, to + up * 0.03F, 0.065F + 0.012F * static_cast<float>(b), V3{0.34F, 0.42F, 0.24F}, rng);
        }
        pushBlob(out, trunkTop + up * 0.04F, 0.095F, V3{0.36F, 0.45F, 0.25F}, rng);
    }

    // The road sign: a post, and a diamond plate facing out over the edge.
    const kangaroo::Sign& sign = mob.sign;
    const V3 up = sign.up;
    const V3 top = sign.p + up * 0.3F;
    pushTube(out, sign.p - up * 0.01F, top, 0.008F, 0.008F, V3{0.62F, 0.63F, 0.64F}, 6);
    const V3 facing = sign.facing;
    const V3 across = kangaroo::normalize(kangaroo::cross(up, facing));
    const V3 centre = top + up * 0.005F + facing * 0.012F;
    const float r = 0.105F;
    const V3 left = centre - across * r;
    const V3 right = centre + across * r;
    const V3 high = centre + up * r;
    const V3 low = centre - up * r;
    const auto face = [&](const V3& p, const float u, const float v) {
        pushVertex(out, p, facing, V3{1, 1, 1}, V3{u, v, -1.0F});
    };
    face(left, -1, 0);
    face(low, 0, -1);
    face(right, 1, 0);
    face(left, -1, 0);
    face(right, 1, 0);
    face(high, 0, 1);
    const V3 back = facing * -0.004F;
    const V3 grey{0.55F, 0.56F, 0.57F};
    pushTriangle(out, left + back, high + back, right + back, grey);
    pushTriangle(out, left + back, right + back, low + back, grey);
    return out;
}

std::vector<float> buildGrass(const kangaroo::Mob& mob)
{
    // Per vertex: base (3), offset (3), colour and height (4), sway seed and
    // face (2).
    std::vector<float> out;
    kangaroo::Rng rng(mob.settings.seed * 31 + 5);
    for (const kangaroo::Tuft& tuft : mob.grass) {
        const auto& c = grassColour[tuft.face];
        const V3 colour{c[0], c[1], c[2]};
        V3 u;
        V3 v;
        kangaroo::faceAxes(tuft.face, u, v);
        const float height = 0.15F + 0.85F * tuft.biomass;
        for (int blade = 0; blade < 7; ++blade) {
            const float angle = rng.range(0.0F, 2.0F * kangaroo::pi);
            const V3 out1 = u * std::cos(angle) + v * std::sin(angle);
            const V3 base = tuft.p + out1 * (0.008F * rng.uniform()) + kangaroo::faceNormal(tuft.face) * 0.001F;
            const V3 across = kangaroo::normalize(kangaroo::cross(out1, tuft.up)) * 0.006F;
            const V3 tip = tuft.up * (0.05F + 0.04F * rng.uniform()) * tuft.scale + out1 * (0.02F * rng.uniform());
            const V3 shade = colour * rng.range(0.85F, 1.1F);
            const auto vertex = [&](const V3& p, const V3& offset) {
                out.insert(out.end(), {p.x, p.y, p.z, offset.x, offset.y, offset.z,
                                       shade.x, shade.y, shade.z, height, tuft.seed, static_cast<float>(tuft.face)});
            };
            vertex(base - across, V3{});
            vertex(base + across, V3{});
            vertex(base, tip);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Setting up and tearing down GL
// ---------------------------------------------------------------------------

bool buildProgram(QOpenGLShaderProgram& program, const char* vertex, const char* fragment,
                  const bool commonInVertex, const bool commonInFragment, const bool potential, QString& error)
{
    const QByteArray header = "#version 330 core\n";
    QByteArray vs = vertex;
    QByteArray fs = fragment;
    if (commonInVertex) {
        vs = header + glslCommon + vs;
    }
    if (commonInFragment) {
        fs = header + glslCommon + (potential ? QByteArray(glslPotential) : QByteArray()) + fs;
    }
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vs)
        || !program.addShaderFromSourceCode(QOpenGLShader::Fragment, fs) || !program.link()) {
        error = program.log();
        return false;
    }
    return true;
}

void attribute(QOpenGLFunctions_3_3_Core* gl, const GLuint index, const GLint size, const GLsizei stride,
               const std::size_t offset)
{
    gl->glEnableVertexAttribArray(index);
    gl->glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride * static_cast<GLsizei>(sizeof(float)),
                              reinterpret_cast<const void*>(offset * sizeof(float)));
}

bool buildScene(GlScene& scene, const kangaroo::Mob& mob, QString& error)
{
    QOpenGLFunctions_3_3_Core* gl = scene.gl;
    if (!buildProgram(scene.terrain, terrainVertexShader, terrainFragmentShader, false, true, true, error)
        || !buildProgram(scene.water, waterVertexShader, waterFragmentShader, true, true, false, error)
        || !buildProgram(scene.roo, kangarooVertexShader, kangarooFragmentShader, false, false, false, error)
        || !buildProgram(scene.props, propsVertexShader, propsFragmentShader, false, true, false, error)
        || !buildProgram(scene.grass, grassVertexShader, grassFragmentShader, false, true, false, error)
        || !buildProgram(scene.particles, particleVertexShader, particleFragmentShader, false, false, false, error)) {
        return false;
    }
    // Ground: one quad per face.
    std::vector<float> ground;
    for (int face = 0; face < kangaroo::faceCount; ++face) {
        V3 u;
        V3 v;
        kangaroo::faceAxes(face, u, v);
        const V3 n = kangaroo::faceNormal(face);
        const V3 c00 = n - u - v;
        const V3 c10 = n + u - v;
        const V3 c11 = n + u + v;
        const V3 c01 = n - u + v;
        for (const V3& p : {c00, c10, c11, c00, c11, c01}) {
            ground.insert(ground.end(), {p.x, p.y, p.z, static_cast<float>(face)});
        }
    }
    gl->glGenVertexArrays(1, &scene.terrainArray);
    gl->glGenBuffers(1, &scene.terrainBuffer);
    gl->glBindVertexArray(scene.terrainArray);
    gl->glBindBuffer(GL_ARRAY_BUFFER, scene.terrainBuffer);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ground.size() * sizeof(float)), ground.data(), GL_STATIC_DRAW);
    attribute(gl, 0, 3, 4, 0);
    attribute(gl, 1, 1, 4, 3);

    // Water: a grid per face, displaced in the vertex shader by the depth map.
    constexpr int stride = kangaroo::Water::stride;
    std::vector<float> grid;
    std::vector<GLuint> indices;
    for (int face = 0; face < kangaroo::faceCount; ++face) {
        const GLuint base = static_cast<GLuint>(grid.size() / 3);
        for (int j = 0; j < stride; ++j) {
            for (int i = 0; i < stride; ++i) {
                grid.insert(grid.end(), {static_cast<float>(i), static_cast<float>(j), static_cast<float>(face)});
            }
        }
        for (int j = 0; j + 1 < stride; ++j) {
            for (int i = 0; i + 1 < stride; ++i) {
                const GLuint a = base + static_cast<GLuint>(j * stride + i);
                const GLuint b = a + 1;
                const GLuint c = a + stride;
                const GLuint d = c + 1;
                indices.insert(indices.end(), {a, b, d, a, d, c});
            }
        }
    }
    gl->glGenVertexArrays(1, &scene.waterArray);
    gl->glGenBuffers(1, &scene.waterBuffer);
    gl->glGenBuffers(1, &scene.waterIndices);
    gl->glBindVertexArray(scene.waterArray);
    gl->glBindBuffer(GL_ARRAY_BUFFER, scene.waterBuffer);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(grid.size() * sizeof(float)), grid.data(), GL_STATIC_DRAW);
    attribute(gl, 0, 2, 3, 0);
    attribute(gl, 1, 1, 3, 2);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene.waterIndices);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(GLuint)),
                     indices.data(), GL_STATIC_DRAW);
    scene.waterIndexCount = static_cast<GLsizei>(indices.size());

    gl->glGenTextures(1, &scene.depthTexture);
    gl->glBindTexture(GL_TEXTURE_2D, scene.depthTexture);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, stride, stride * kangaroo::faceCount, 0, GL_RED, GL_FLOAT, nullptr);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(GL_TEXTURE_2D, 0);

    // The kangaroos' bounding box: a unit cube, front faces counter-clockwise
    // from outside, which the widget's back-face culling keeps.
    const float corners[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                 {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3}, {1, 2, 6, 5}, {3, 7, 6, 2}, {0, 1, 5, 4}};
    std::vector<float> box;
    for (const auto& q : quads) {
        for (const int c : {q[0], q[1], q[2], q[0], q[2], q[3]}) {
            box.insert(box.end(), {corners[c][0], corners[c][1], corners[c][2]});
        }
    }
    gl->glGenVertexArrays(1, &scene.boxArray);
    gl->glGenBuffers(1, &scene.boxBuffer);
    gl->glBindVertexArray(scene.boxArray);
    gl->glBindBuffer(GL_ARRAY_BUFFER, scene.boxBuffer);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(box.size() * sizeof(float)), box.data(), GL_STATIC_DRAW);
    attribute(gl, 0, 3, 3, 0);

    // Trees and the sign.
    const std::vector<float> props = buildProps(mob);
    gl->glGenVertexArrays(1, &scene.propsArray);
    gl->glGenBuffers(1, &scene.propsBuffer);
    gl->glBindVertexArray(scene.propsArray);
    gl->glBindBuffer(GL_ARRAY_BUFFER, scene.propsBuffer);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(props.size() * sizeof(float)), props.data(), GL_STATIC_DRAW);
    attribute(gl, 0, 3, 12, 0);
    attribute(gl, 1, 3, 12, 3);
    attribute(gl, 2, 3, 12, 6);
    attribute(gl, 3, 3, 12, 9);
    scene.propsCount = static_cast<GLsizei>(props.size() / 12);

    gl->glGenVertexArrays(1, &scene.grassArray);
    gl->glGenBuffers(1, &scene.grassBuffer);
    gl->glBindVertexArray(scene.grassArray);
    gl->glBindBuffer(GL_ARRAY_BUFFER, scene.grassBuffer);
    attribute(gl, 0, 3, 12, 0);
    attribute(gl, 1, 3, 12, 3);
    attribute(gl, 2, 4, 12, 6);
    attribute(gl, 3, 2, 12, 10);

    gl->glGenVertexArrays(1, &scene.particleArray);
    gl->glGenBuffers(1, &scene.particleBuffer);
    gl->glBindVertexArray(scene.particleArray);
    gl->glBindBuffer(GL_ARRAY_BUFFER, scene.particleBuffer);
    attribute(gl, 0, 3, 8, 0);
    attribute(gl, 1, 4, 8, 3);
    attribute(gl, 2, 1, 8, 7);

    gl->glBindVertexArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);

    // The sign's kangaroo is the rig in mid-leap, flattened to its profile.
    kangaroo::Pose leap;
    leap.airborne = 1.0F;
    leap.legSwing = -0.2F;
    leap.legExtend = 0.6F;
    leap.bodyPitch = 0.55F;
    leap.tailLift = 1.0F;
    const kangaroo::Rig rig = kangaroo::buildRig(leap);
    scene.signShape.clear();
    scene.signRadius.clear();
    for (int i = 0; i < rig.count && scene.signShape.size() < 32; ++i) {
        const kangaroo::Primitive& p = rig.primitives[i];
        if (p.material == kangaroo::MaterialEye || p.material == kangaroo::MaterialJoey) {
            continue;
        }
        scene.signShape.emplace_back(p.a.x, p.a.y, p.b.x, p.b.y);
        scene.signRadius.emplace_back(p.ra, p.rb);
    }
    return true;
}

void destroyScene(GlScene& scene)
{
    QOpenGLFunctions_3_3_Core* gl = scene.gl;
    if (gl == nullptr) {
        return;
    }
    for (GLuint* array : {&scene.terrainArray, &scene.waterArray, &scene.boxArray, &scene.propsArray,
                          &scene.grassArray, &scene.particleArray}) {
        if (*array != 0) {
            gl->glDeleteVertexArrays(1, array);
            *array = 0;
        }
    }
    for (GLuint* buffer : {&scene.terrainBuffer, &scene.waterBuffer, &scene.waterIndices, &scene.boxBuffer,
                           &scene.propsBuffer, &scene.grassBuffer, &scene.particleBuffer}) {
        if (*buffer != 0) {
            gl->glDeleteBuffers(1, buffer);
            *buffer = 0;
        }
    }
    if (scene.depthTexture != 0) {
        gl->glDeleteTextures(1, &scene.depthTexture);
        scene.depthTexture = 0;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

QVector3D q(const V3& v) { return {v.x, v.y, v.z}; }

V3 furFor(const kangaroo::Kangaroo& k)
{
    // Red kangaroos: the males rusty red, the females blue-grey (the
    // "blue flyers"), joeys a sandy tan.
    V3 base;
    switch (k.role) {
    case kangaroo::Role::Boomer: base = V3{0.66F, 0.29F, 0.15F}; break;
    case kangaroo::Role::Buck: base = V3{0.70F, 0.38F, 0.22F}; break;
    case kangaroo::Role::Flyer: base = V3{0.50F, 0.49F, 0.52F}; break;
    case kangaroo::Role::Joey: base = V3{0.74F, 0.55F, 0.40F}; break;
    }
    return base * (1.0F + 0.08F * k.tone);
}

void uploadDynamic(GlScene& scene, const kangaroo::Mob& mob)
{
    QOpenGLFunctions_3_3_Core* gl = scene.gl;
    // The water's height above the ground, one face above the next. It is
    // negative on dry ground, so the shore is where it crosses zero.
    constexpr int stride = kangaroo::Water::stride;
    std::vector<float> depth(static_cast<std::size_t>(stride * stride * kangaroo::faceCount));
    for (int face = 0; face < kangaroo::faceCount; ++face) {
        std::copy(mob.water.height[face].begin(), mob.water.height[face].end(),
                  depth.begin() + face * stride * stride);
    }
    gl->glBindTexture(GL_TEXTURE_2D, scene.depthTexture);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, stride, stride * kangaroo::faceCount, GL_RED, GL_FLOAT, depth.data());
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void uploadGrass(GlScene& scene, const kangaroo::Mob& mob)
{
    const std::vector<float> grass = buildGrass(mob);
    scene.gl->glBindBuffer(GL_ARRAY_BUFFER, scene.grassBuffer);
    scene.gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(grass.size() * sizeof(float)), grass.data(),
                           GL_DYNAMIC_DRAW);
    scene.gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    scene.grassCount = static_cast<GLsizei>(grass.size() / 12);
}

void drawScene(CubeWidget* cube)
{
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (planet == nullptr || planet->gl == nullptr || context == nullptr || context != planet->gl->owner) {
        return;
    }
    GlScene& scene = *planet->gl;
    QOpenGLFunctions_3_3_Core* gl = scene.gl;
    kangaroo::Mob& mob = planet->mob;

    QElapsedTimer timer;
    timer.start();

    // Advance on the widget's own clock. A frame painted without a tick in
    // between (an expose, a resize) moves nothing.
    const float elapsed = cube->m_elapsedSeconds;
    const float angle = cube->m_angleDegrees;
    if (!planet->clockSeen) {
        planet->clockSeen = true;
        planet->lastElapsed = elapsed;
        planet->lastAngle = angle;
    }
    const float dt = elapsed - planet->lastElapsed;
    if (dt > 1e-5F) {
        planet->omega = planet->rate.frame(angle - planet->lastAngle, dt);
        mob.advance(std::min(dt, 0.1F), planet->omega);
        planet->lastElapsed = elapsed;
        planet->lastAngle = angle;
        planet->grassClock += dt;
    }
    if (!planet->spinFeedback) {
        (void)mob.takeImpulse();
    }
    planet->simulateMs = planet->simulateMs * 0.95 + 0.05 * static_cast<double>(timer.nsecsElapsed()) / 1e6;

    if (hookHostValid) {
        for (const kangaroo::Event& event : mob.events) {
            const bool hop = event.name == "kangaroo.hop";
            if (options.events || hop) {
                hookHost.emit_event_json(hookHost.agent_context, event.name.c_str(), event.json.c_str());
            }
        }
    }
    mob.events.clear();

    timer.restart();
    uploadDynamic(scene, mob);
    if (mob.grassChanged && planet->grassClock > 0.25F) {
        uploadGrass(scene, mob);
        mob.grassChanged = false;
        planet->grassClock = 0.0F;
    }

    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, -cubeViewDistance);
    QMatrix4x4 model;
    model.rotate(angle, QVector3D(cubeSpinAxis[0], cubeSpinAxis[1], cubeSpinAxis[2]));
    model.scale(cube->m_scale);
    const QMatrix4x4 viewProjection = cube->m_projection * view;
    const QMatrix4x4 mvp = viewProjection * model;
    // The sun and the camera in the planet's frame.
    QMatrix4x4 rotation;
    rotation.rotate(angle, QVector3D(cubeSpinAxis[0], cubeSpinAxis[1], cubeSpinAxis[2]));
    const QMatrix4x4 unrotate = rotation.transposed();
    const QVector3D sunBodyQ = unrotate.map(q(sunInView)).normalized();
    const V3 sunBody{sunBodyQ.x(), sunBodyQ.y(), sunBodyQ.z()};
    const QVector3D cameraBody = (view * model).inverted().map(QVector3D(0.0F, 0.0F, 0.0F));
    const float time = static_cast<float>(mob.time);
    const QVector3D tint = cube->m_tint;
    const V3 axle = kangaroo::fromVec(mob.planet.axle);

    gl->glEnable(GL_DEPTH_TEST);
    gl->glDepthFunc(GL_LESS);
    gl->glEnable(GL_CULL_FACE);
    gl->glCullFace(GL_BACK);

    // ---- ground -------------------------------------------------------
    if (cube->m_wireframe) {
        gl->glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    scene.terrain.bind();
    scene.terrain.setUniformValue("mvp", mvp);
    scene.terrain.setUniformValue("sunBody", sunBodyQ);
    scene.terrain.setUniformValue("tint", tint);
    scene.terrain.setUniformValue("omega", planet->omega);
    scene.terrain.setUniformValue("axle", q(axle));
    scene.terrain.setUniformValue("gravityScale", static_cast<float>(mob.planet.gravityScale()));
    scene.terrain.setUniformValue("contourSpacing", 0.42F);
    scene.terrain.setUniformValue("contourStrength", options.contours ? 0.32F : 0.0F);
    scene.terrain.setUniformValue("time", time);
    scene.terrain.setUniformValue("depthMap", 0);
    {
        std::vector<QVector4D> shadowPosition;
        std::vector<QVector4D> shadowShape;
        for (const kangaroo::Kangaroo& k : mob.roos) {
            if (!k.present() || shadowPosition.size() >= 16) {
                continue;
            }
            const float height = std::max(0.0F, kangaroo::boxNorm(k.p) - 1.0F);
            if (height > 1.5F) {
                continue;
            }
            const V3 ground = kangaroo::toSurface(k.p);
            const V3 heading = kangaroo::normalize(kangaroo::flatten(k.heading, kangaroo::faceNormal(kangaroo::faceOf(ground))), k.heading);
            const V3 centre = ground - heading * (0.18F * k.size);
            const float radius = k.size * (0.55F + 1.2F * height);
            const float strength = 0.5F * std::exp(-height * 3.0F / std::max(k.size, 0.05F));
            shadowPosition.emplace_back(centre.x, centre.y, centre.z, radius);
            shadowShape.emplace_back(heading.x, heading.y, heading.z, strength);
        }
        scene.terrain.setUniformValueArray("shadowPosition", shadowPosition.data(), static_cast<int>(shadowPosition.size()));
        scene.terrain.setUniformValueArray("shadowShape", shadowShape.data(), static_cast<int>(shadowShape.size()));
        scene.terrain.setUniformValue("shadowCount", static_cast<int>(shadowPosition.size()));

        std::vector<QVector4D> printPosition;
        std::vector<QVector4D> printShape;
        for (const kangaroo::Footprint& f : mob.footprints) {
            printPosition.emplace_back(f.p.x, f.p.y, f.p.z, f.age);
            printShape.emplace_back(f.heading.x, f.heading.y, f.heading.z,
                                    static_cast<float>(f.kind) + std::clamp(f.size, 0.05F, 0.99F));
        }
        scene.terrain.setUniformValueArray("printPosition", printPosition.data(), static_cast<int>(printPosition.size()));
        scene.terrain.setUniformValueArray("printShape", printShape.data(), static_cast<int>(printShape.size()));
        scene.terrain.setUniformValue("printCount", static_cast<int>(printPosition.size()));
    }
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, scene.depthTexture);
    gl->glBindVertexArray(scene.terrainArray);
    gl->glDrawArrays(GL_TRIANGLES, 0, 36);
    scene.terrain.release();
    if (cube->m_wireframe) {
        gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // ---- trees and the sign ---------------------------------------------
    gl->glDisable(GL_CULL_FACE);
    scene.props.bind();
    scene.props.setUniformValue("mvp", mvp);
    scene.props.setUniformValue("time", time);
    scene.props.setUniformValue("sunBody", sunBodyQ);
    scene.props.setUniformValue("surfaceUp", q(mob.sign.up));
    scene.props.setUniformValueArray("signShape", scene.signShape.data(), static_cast<int>(scene.signShape.size()));
    scene.props.setUniformValueArray("signRadius", scene.signRadius.data(), static_cast<int>(scene.signRadius.size()));
    scene.props.setUniformValue("signCount", static_cast<int>(scene.signShape.size()));
    gl->glBindVertexArray(scene.propsArray);
    gl->glDrawArrays(GL_TRIANGLES, 0, scene.propsCount);
    scene.props.release();

    // ---- grass ------------------------------------------------------------
    if (scene.grassCount > 0) {
        scene.grass.bind();
        scene.grass.setUniformValue("mvp", mvp);
        scene.grass.setUniformValue("time", time);
        scene.grass.setUniformValue("sunBody", sunBodyQ);
        gl->glBindVertexArray(scene.grassArray);
        gl->glDrawArrays(GL_TRIANGLES, 0, scene.grassCount);
        scene.grass.release();
    }
    gl->glEnable(GL_CULL_FACE);

    // ---- kangaroos ----------------------------------------------------------
    scene.roo.bind();
    gl->glBindVertexArray(scene.boxArray);
    std::array<QVector4D, kangaroo::maxPrimitives> primA;
    std::array<QVector4D, kangaroo::maxPrimitives> primB;
    std::array<QVector4D, kangaroo::maxPrimitives> primBound;
    std::array<QVector2D, kangaroo::maxPrimitives> primInfo;
    for (const kangaroo::Kangaroo& k : mob.roos) {
        if (!k.present()) {
            continue;
        }
        const kangaroo::Rig rig = kangaroo::buildRig(k.pose);
        for (int i = 0; i < rig.count; ++i) {
            const kangaroo::Primitive& p = rig.primitives[i];
            primA[i] = QVector4D(p.a.x, p.a.y, p.a.z, p.ra);
            primB[i] = QVector4D(p.b.x, p.b.y, p.b.z, p.rb);
            const V3 centre = (p.a + p.b) * 0.5F;
            const float radius = 0.5F * kangaroo::length(p.b - p.a) + std::max(p.ra, p.rb);
            primBound[i] = QVector4D(centre.x, centre.y, centre.z, radius);
            primInfo[i] = QVector2D(static_cast<float>(p.material), p.blend);
        }

        // The kangaroo's frame in the planet's: heading, up, right, scaled to
        // its size, tumbling about its middle if it has been flung.
        const V3 right = kangaroo::cross(k.heading, k.up);
        QMatrix4x4 frame(k.heading.x, k.up.x, right.x, k.p.x,
                         k.heading.y, k.up.y, right.y, k.p.y,
                         k.heading.z, k.up.z, right.z, k.p.z,
                         0.0F, 0.0F, 0.0F, 1.0F);
        if (k.activity == kangaroo::Activity::Flung) {
            const QVector3D centre(0.0F, 0.45F * k.size, 0.0F);
            frame.translate(centre);
            frame.rotate(k.tumbleAngle * 180.0F / kangaroo::pi, q(k.tumbleAxis));
            frame.translate(-centre);
        }
        frame.scale(k.size);
        const QMatrix4x4 local = model * frame;
        const QMatrix4x4 localMvp = viewProjection * local;
        const QVector3D cameraLocal = (view * local).inverted().map(QVector3D(0.0F, 0.0F, 0.0F));
        const QVector3D sunLocal(QVector3D::dotProduct(sunBodyQ, q(k.heading)),
                                 QVector3D::dotProduct(sunBodyQ, q(k.up)),
                                 QVector3D::dotProduct(sunBodyQ, q(right)));
        const float day = std::clamp((kangaroo::dot(kangaroo::normalize(k.p), sunBody) + 0.2F) / 0.45F, 0.0F, 1.0F);
        const V3 fur = furFor(k);

        scene.roo.setUniformValue("mvp", localMvp);
        scene.roo.setUniformValue("cameraLocal", cameraLocal);
        scene.roo.setUniformValue("sunLocal", sunLocal.normalized());
        scene.roo.setUniformValue("boundsMin", q(rig.boundsMin));
        scene.roo.setUniformValue("boundsMax", q(rig.boundsMax));
        scene.roo.setUniformValue("day", day);
        scene.roo.setUniformValue("grounded", k.grounded() ? 1.0F : 0.0F);
        scene.roo.setUniformValue("furColour", q(fur));
        scene.roo.setUniformValue("paleColour", QVector3D(0.92F, 0.86F, 0.76F));
        scene.roo.setUniformValue("footColour", q(fur * 0.55F));
        scene.roo.setUniformValue("joeyColour", QVector3D(0.80F, 0.62F, 0.48F));
        scene.roo.setUniformValueArray("primA", primA.data(), rig.count);
        scene.roo.setUniformValueArray("primB", primB.data(), rig.count);
        scene.roo.setUniformValueArray("primBound", primBound.data(), rig.count);
        scene.roo.setUniformValueArray("primInfo", primInfo.data(), rig.count);
        scene.roo.setUniformValue("primCount", rig.count);
        gl->glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    scene.roo.release();

    // ---- water --------------------------------------------------------------
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    gl->glDepthMask(GL_FALSE);
    gl->glDisable(GL_CULL_FACE);
    scene.water.bind();
    scene.water.setUniformValue("mvp", mvp);
    scene.water.setUniformValue("depthMap", 0);
    scene.water.setUniformValue("sunBody", sunBodyQ);
    scene.water.setUniformValue("cameraBody", cameraBody);
    scene.water.setUniformValue("tint", tint);
    scene.water.setUniformValue("time", time);
    {
        std::vector<QVector4D> ripples;
        for (const kangaroo::Ripple& r : mob.ripples) {
            ripples.emplace_back(r.p.x, r.p.y, r.p.z, r.age);
        }
        scene.water.setUniformValueArray("ripples", ripples.data(), static_cast<int>(ripples.size()));
        scene.water.setUniformValue("rippleCount", static_cast<int>(ripples.size()));
    }
    gl->glBindTexture(GL_TEXTURE_2D, scene.depthTexture);
    gl->glBindVertexArray(scene.waterArray);
    gl->glDrawElements(GL_TRIANGLES, scene.waterIndexCount, GL_UNSIGNED_INT, nullptr);
    scene.water.release();
    gl->glBindTexture(GL_TEXTURE_2D, 0);

    // ---- dust and spray -------------------------------------------------------
    if (!mob.particles.empty()) {
        std::vector<float> points;
        points.reserve(mob.particles.size() * 8);
        for (const kangaroo::Particle& particle : mob.particles) {
            const float life = std::clamp(particle.age / particle.life, 0.0F, 1.0F);
            std::array<float, 3> c{};
            float alpha = 0.0F;
            float size = particle.size;
            if (particle.kind == 1) {
                c = sprayColour[particle.face];
                alpha = 0.85F * (1.0F - life);
            } else if (particle.kind == 2) {
                c = {0.7F, 0.4F, 0.25F};
                alpha = 0.7F * (1.0F - life);
            } else {
                c = dustColour[particle.face];
                alpha = 0.5F * (1.0F - life) * (1.0F - life);
                size *= 1.0F + 1.6F * life;
            }
            points.insert(points.end(), {particle.p.x, particle.p.y, particle.p.z, c[0] * 0.9F, c[1] * 0.9F,
                                         c[2] * 0.9F, alpha, size});
        }
        gl->glBindBuffer(GL_ARRAY_BUFFER, scene.particleBuffer);
        gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(points.size() * sizeof(float)), points.data(),
                         GL_STREAM_DRAW);
        gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
        gl->glEnable(GL_PROGRAM_POINT_SIZE);
        scene.particles.bind();
        scene.particles.setUniformValue("mvp", mvp);
        // Pixels per unit at distance one: the viewport height over the
        // projection's vertical extent.
        GLint viewport[4] = {0, 0, 1, 1};
        gl->glGetIntegerv(GL_VIEWPORT, viewport);
        scene.particles.setUniformValue("pixelsPerUnit",
                                        static_cast<float>(viewport[3]) * 0.5F * cube->m_projection(1, 1));
        gl->glBindVertexArray(scene.particleArray);
        gl->glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(mob.particles.size()));
        scene.particles.release();
        gl->glDisable(GL_PROGRAM_POINT_SIZE);
    }

    // Put back what the widget and the other hooks expect.
    gl->glBindVertexArray(0);
    gl->glDepthMask(GL_TRUE);
    gl->glDisable(GL_BLEND);
    gl->glEnable(GL_CULL_FACE);
    gl->glCullFace(GL_BACK);
    gl->glDepthFunc(GL_LESS);

    ++planet->framesDrawn;
    planet->drawMs = planet->drawMs * 0.95 + 0.05 * static_cast<double>(timer.nsecsElapsed()) / 1e6;
}

// ---------------------------------------------------------------------------
// Install and remove
// ---------------------------------------------------------------------------

V3 spinAxis()
{
    return kangaroo::normalize(V3{cubeSpinAxis[0], cubeSpinAxis[1], cubeSpinAxis[2]});
}

// Needs the widget's OpenGL context to be current.
bool install(CubeWidget* cube, QString& error)
{
    const RuntimeAgentHost* host = &hookHost;
    auto state = std::make_unique<Planet>();
    state->gl = std::make_unique<GlScene>();
    GlScene& scene = *state->gl;
    scene.owner = QOpenGLContext::currentContext();
    scene.gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(scene.owner);
    if (scene.gl == nullptr) {
        error = QStringLiteral("this context offers no OpenGL 3.3 core functions");
        return false;
    }
    scene.gl->initializeOpenGLFunctions();

    // The kangaroos stand on the flat cube. A module that has replaced the
    // cube's mesh is drawing a different shape, and its claim says so.
    cube_mesh::Claim claim;
    if (cube_mesh::overlappingClaim(host, 0, cubeVertexFloats, claim)) {
        error = QStringLiteral("floats %1..%2 of the widget's mesh are displaced by \"%3\"; the "
                               "kangaroos live on the flat cube, so release that first")
                    .arg(claim.offsetFloats)
                    .arg(claim.offsetFloats + claim.lengthFloats)
                    .arg(claim.owner);
        return false;
    }
    state->savedCubeVertices.resize(cubeVertexFloats);
    cube->m_vertexBuffer.bind();
    const bool readBack = cube->m_vertexBuffer.read(
        0, state->savedCubeVertices.data(), cubeVertexFloats * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();
    if (!readBack) {
        error = QStringLiteral("could not read the widget's vertex buffer; refusing to overwrite it");
        return false;
    }
    if (cube_mesh::anyFaceCollapsed(state->savedCubeVertices)) {
        error = QStringLiteral("the cube's mesh is still collapsed although no module claims it, so its "
                               "original vertices are already lost; saving these zeros would record the "
                               "loss as the originals");
        return false;
    }

    state->mob.reset(options.settings, spinAxis());
    if (!buildScene(scene, state->mob, error)) {
        destroyScene(scene);
        return false;
    }
    uploadGrass(scene, state->mob);
    state->mob.grassChanged = false;

    state->stashKey = cube_mesh::regionKey(0, cubeVertexFloats);
    state->claimKey = cube_mesh::claimKey(0, cubeVertexFloats);
    if (host->stash_put(host->agent_context, state->stashKey.constData(), state->savedCubeVertices.data(),
                        cubeVertexFloats * static_cast<std::int64_t>(sizeof(float)), 1) < 0) {
        destroyScene(scene);
        error = QStringLiteral("the originals for the cube's mesh could not be recorded: %1 belongs to "
                               "another snippet. Nothing claims the region, so that entry describes a "
                               "displacement no longer in effect and is safe for the driver to stash.drop")
                    .arg(QString::fromUtf8(state->stashKey));
        return false;
    }
    const char marker = 1;
    host->stash_put(host->agent_context, state->claimKey.constData(), &marker, 1, 1);
    const std::vector<float> collapsed(cubeVertexFloats, 0.0F);
    cube->m_vertexBuffer.bind();
    cube->m_vertexBuffer.write(0, collapsed.data(), cubeVertexFloats * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();

    // The planet's spin. Refusal here is not fatal: the mob lives without it,
    // and the result says why the planet does not answer their pushes.
    RuntimeAgentPatchBinding bound{};
    const std::int32_t boundResult = hookHost.patch_bind(
        hookHost.agent_context, reinterpret_cast<void*>(&cube_step_builtin),
        reinterpret_cast<void*>(&kangarooStep), 0, &bound);
    if (boundResult == RUNTIME_AGENT_PATCH_OK) {
        state->binding = bound.id;
        chainedStep = reinterpret_cast<CubeStepFunction>(bound.previous);
        state->spinFeedback = true;
    } else {
        state->spinFeedback = false;
        state->spinFeedbackNote = QStringLiteral("the host refused to bind the step function (%1), so the "
                                                 "planet does not answer the mob's pushes")
                                      .arg(boundResult);
    }

    planet = state.release();
    drawConnection = QObject::connect(
        cube, &CubeWidget::frameRendered, cube, [cube](qulonglong, float) { drawScene(cube); },
        Qt::DirectConnection);
    cube->update();
    return true;
}

// Needs the widget's OpenGL context to be current.
void remove(CubeWidget* cube)
{
    QObject::disconnect(drawConnection);
    if (planet == nullptr) {
        return;
    }
    if (planet->binding != 0 && hookHostValid) {
        // Releasing, not restoring a pointer: a binding made after this one
        // stays selected.
        (void)hookHost.patch_unbind(hookHost.agent_context, planet->binding);
        planet->binding = 0;
    }

    // Replay only onto this module's own displacement.
    std::vector<float> current(cubeVertexFloats);
    cube->m_vertexBuffer.bind();
    const bool readBack =
        cube->m_vertexBuffer.read(0, current.data(), cubeVertexFloats * static_cast<int>(sizeof(float)));
    const bool stillOurs = readBack && cube_mesh::anyFaceCollapsed(current);
    if (stillOurs) {
        cube->m_vertexBuffer.write(0, planet->savedCubeVertices.data(),
                                   cubeVertexFloats * static_cast<int>(sizeof(float)));
    }
    cube->m_vertexBuffer.release();
    if ((stillOurs || !readBack) && hookHostValid) {
        hookHost.stash_drop(hookHost.agent_context, planet->claimKey.constData());
    }

    destroyScene(*planet->gl);
    delete planet;
    planet = nullptr;
    cube->update();
}

void syncToggleAction()
{
    if (toggleAction != nullptr) {
        const QSignalBlocker blocker(toggleAction);
        toggleAction->setChecked(planet != nullptr);
    }
}

bool setEnabled(CubeWidget* cube, const bool enabled, QString& error)
{
    if (enabled == (planet != nullptr)) {
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
        cube, QStringLiteral("actionKangarooPlanet"), QStringLiteral("&Kangaroo planet"),
        QStringLiteral("Ctrl+Shift+K"), [cube](const bool enabled) {
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
// Reports
// ---------------------------------------------------------------------------

QJsonArray vector(const V3& v)
{
    return QJsonArray{static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

QJsonObject summary(const bool detailed)
{
    QJsonObject result;
    if (planet == nullptr) {
        result.insert(QStringLiteral("installed"), false);
        return result;
    }
    const kangaroo::Mob& mob = planet->mob;
    result.insert(QStringLiteral("installed"), true);
    result.insert(QStringLiteral("mob"), static_cast<int>(mob.roos.size()));
    result.insert(QStringLiteral("omegaDegreesPerSecond"), planet->omega * 180.0 / kangaroo::pi);
    result.insert(QStringLiteral("spinFeedback"), planet->spinFeedback);
    if (!planet->spinFeedback) {
        result.insert(QStringLiteral("spinFeedbackNote"), planet->spinFeedbackNote);
    }
    result.insert(QStringLiteral("spinBoostDegreesPerSecond"), planet->spin.boost * 180.0 / kangaroo::pi);
    result.insert(QStringLiteral("largestSpinBoostDegreesPerSecond"), planet->spin.largest * 180.0 / kangaroo::pi);
    result.insert(QStringLiteral("spinBoostLimited"), planet->spin.limited);
    result.insert(QStringLiteral("angleJumpsIgnored"), static_cast<qint64>(planet->rate.jumpsIgnored));
    result.insert(QStringLiteral("binding"), static_cast<qint64>(planet->binding));
    result.insert(QStringLiteral("surfaceGravity"), mob.settings.surfaceGravity);
    result.insert(QStringLiteral("planetInertia"), mob.settings.planetInertia);
    result.insert(QStringLiteral("seed"), static_cast<qint64>(mob.settings.seed));
    result.insert(QStringLiteral("flung"), mob.flungTotal);
    result.insert(QStringLiteral("returned"), mob.returnedTotal);
    result.insert(QStringLiteral("bouts"), mob.boutsTotal);
    result.insert(QStringLiteral("alarms"), mob.alarmsTotal);
    result.insert(QStringLiteral("hopsPlanned"), mob.hopsPlanned);
    result.insert(QStringLiteral("hopsOnTheSpot"), mob.hopsOnTheSpot);
    result.insert(QStringLiteral("simulatedSeconds"), mob.time);
    result.insert(QStringLiteral("framesDrawn"), static_cast<qint64>(planet->framesDrawn));
    result.insert(QStringLiteral("simulateMs"), planet->simulateMs);
    result.insert(QStringLiteral("submitMs"), planet->drawMs);
    result.insert(QStringLiteral("contours"), options.contours);

    QJsonArray faces;
    for (int face = 0; face < kangaroo::faceCount; ++face) {
        faces.append(static_cast<double>(mob.water.faceVolume(face)));
    }
    QJsonObject water{
        {QStringLiteral("volume"), static_cast<double>(mob.water.volume)},
        {QStringLiteral("held"), static_cast<double>(mob.water.totalVolume())},
        {QStringLiteral("perFace"), faces},
        {QStringLiteral("spilled"), mob.water.spilled},
    };
    result.insert(QStringLiteral("water"), water);

    QJsonArray roos;
    for (const kangaroo::Kangaroo& k : mob.roos) {
        QJsonObject entry{
            {QStringLiteral("name"), QString::fromStdString(k.name)},
            {QStringLiteral("role"), QString::fromLatin1(kangaroo::roleName(k.role))},
            {QStringLiteral("doing"), QString::fromLatin1(kangaroo::activityName(k.activity))},
            {QStringLiteral("face"), k.face},
            {QStringLiteral("hops"), k.hops},
        };
        if (detailed) {
            entry.insert(QStringLiteral("gait"), QString::fromLatin1(kangaroo::gaitName(k.gait)));
            entry.insert(QStringLiteral("position"), vector(k.p));
            entry.insert(QStringLiteral("altitude"), static_cast<double>(kangaroo::boxNorm(k.p) - 1.0F));
            entry.insert(QStringLiteral("speed"), static_cast<double>(kangaroo::length(k.v)));
            entry.insert(QStringLiteral("distance"), static_cast<double>(k.distance));
            entry.insert(QStringLiteral("flung"), k.flungCount);
            entry.insert(QStringLiteral("bouts"), k.bouts);
            entry.insert(QStringLiteral("wins"), k.wins);
            entry.insert(QStringLiteral("drankSeconds"), static_cast<double>(k.drankSeconds));
            entry.insert(QStringLiteral("grazed"), static_cast<double>(k.grazedBiomass));
            entry.insert(QStringLiteral("size"), static_cast<double>(k.size));
            if (k.pouchJoey) {
                entry.insert(QStringLiteral("pouchJoey"), true);
            }
        }
        roos.append(entry);
    }
    result.insert(QStringLiteral("kangaroos"), roos);
    return result;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

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
    if (planet == nullptr) {
        host->complete_json(host->invocation_context, "{\"removed\":false,\"note\":\"nothing was installed\"}");
        return;
    }
    // GL objects and the cube's vertices both need the context current, and
    // a handover is sequenced on this completing, so it refuses rather than
    // deferring.
    if (!scene_toggle::ownContextIsCurrent(cube)) {
        host->fail(host->invocation_context,
                   "the widget's OpenGL context is not current; release this module with executor=render "
                   "so the cube is back before this reports completion");
        return;
    }
    const std::uint64_t frames = planet->framesDrawn;
    remove(cube);
    syncToggleAction();
    host->complete_json(host->invocation_context,
                        QJsonDocument(QJsonObject{{QStringLiteral("removed"), true},
                                                  {QStringLiteral("framesDrawn"), static_cast<qint64>(frames)}})
                            .toJson(QJsonDocument::Compact)
                            .constData());
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

    // Settings. Ones that change the scene rebuild the mob; the rest apply
    // as they are.
    bool rebuild = false;
    const auto number = [&request](const char* key, const double fallback) {
        return request.value(QLatin1String(key)).toDouble(fallback);
    };
    if (request.contains(QStringLiteral("mob"))) {
        options.settings.mob = std::clamp(static_cast<int>(number("mob", 6)), 1, 12);
        rebuild = true;
    }
    if (request.contains(QStringLiteral("gravity"))) {
        options.settings.surfaceGravity = std::clamp(static_cast<float>(number("gravity", 14.0)), 2.0F, 60.0F);
        rebuild = true;
    }
    if (request.contains(QStringLiteral("water"))) {
        options.settings.waterDepth = std::clamp(static_cast<float>(number("water", 0.05)), 0.0F, 0.2F);
        rebuild = true;
    }
    if (request.contains(QStringLiteral("seed"))) {
        options.settings.seed = static_cast<std::uint64_t>(std::max(0.0, number("seed", 1.0)));
        rebuild = true;
    }
    if (request.contains(QStringLiteral("inertia"))) {
        options.settings.planetInertia = std::clamp(static_cast<float>(number("inertia", 40.0)), 0.05F, 1e6F);
        if (planet != nullptr) {
            planet->mob.settings.planetInertia = options.settings.planetInertia;
        }
    }
    if (request.contains(QStringLiteral("contours"))) {
        options.contours = request.value(QStringLiteral("contours")).toBool();
    }
    if (request.contains(QStringLiteral("events"))) {
        options.events = request.value(QStringLiteral("events")).toBool();
    }
    if (request.contains(QStringLiteral("hopEvents"))) {
        options.settings.hopEvents = request.value(QStringLiteral("hopEvents")).toBool();
        if (planet != nullptr) {
            planet->mob.settings.hopEvents = options.settings.hopEvents;
        }
    }

    const bool takeOff = request.value(QStringLiteral("restore")).toBool()
        || request.value(QStringLiteral("remove")).toBool()
        || (request.value(QStringLiteral("toggle")).toBool() && planet != nullptr);
    if (takeOff) {
        if (planet == nullptr) {
            complete(QJsonObject{{QStringLiteral("removed"), false},
                                 {QStringLiteral("note"), QStringLiteral("no planet was installed")}});
            return;
        }
        const std::uint64_t frames = planet->framesDrawn;
        remove(cube);
        syncToggleAction();
        complete(QJsonObject{{QStringLiteral("removed"), true},
                             {QStringLiteral("framesDrawn"), static_cast<qint64>(frames)}});
        return;
    }

    if (planet != nullptr) {
        QJsonObject result;
        if (rebuild) {
            // A new mob and new scenery; the GL scene is rebuilt with them.
            planet->mob.reset(options.settings, spinAxis());
            destroyScene(*planet->gl);
            QString error;
            if (!buildScene(*planet->gl, planet->mob, error)) {
                host->fail(host->invocation_context, error.toLocal8Bit().constData());
                return;
            }
            uploadGrass(*planet->gl, planet->mob);
            planet->clockSeen = false;
            result.insert(QStringLiteral("rebuilt"), true);
        }
        if (request.value(QStringLiteral("alarm")).toBool()) {
            result.insert(QStringLiteral("alarm"), planet->mob.raiseAlarm("asked to over the socket"));
        }
        if (request.value(QStringLiteral("box")).toBool()) {
            result.insert(QStringLiteral("bout"), planet->mob.startBout());
        }
        if (request.contains(QStringLiteral("throw"))) {
            const QString name = request.value(QStringLiteral("throw")).toString();
            const float speed = static_cast<float>(number("throwSpeed", 4.5));
            bool thrown = false;
            for (std::size_t i = 0; i < planet->mob.roos.size(); ++i) {
                kangaroo::Kangaroo& k = planet->mob.roos[i];
                if (QString::fromStdString(k.name).compare(name, Qt::CaseInsensitive) == 0) {
                    const V3 n = kangaroo::faceNormal(k.face);
                    thrown = planet->mob.launch(static_cast<int>(i), n * speed + k.heading * (0.35F * speed));
                }
            }
            result.insert(QStringLiteral("thrown"), thrown);
        }
        const QJsonObject status = summary(request.value(QStringLiteral("status")).toBool());
        for (auto it = status.begin(); it != status.end(); ++it) {
            result.insert(it.key(), it.value());
        }
        complete(result);
        return;
    }

    QString error;
    if (!setEnabled(cube, true, error)) {
        host->fail(host->invocation_context, error.toLocal8Bit().constData());
        return;
    }
    QJsonObject result = summary(false);
    result.insert(QStringLiteral("connectionValid"), static_cast<bool>(drawConnection));
    complete(result);
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "turn the cube into a planet with a mob of kangaroos on it",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init()
{
    return &descriptor;
}
