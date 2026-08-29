// Drives the cube from whatever the machine is playing.
//
// This snippet uses both of the application's execution seams at once, because
// the two halves of the effect belong to different ones:
//
//   - The cube's own angle, tint and scale are produced by the step function
//     the widget calls once per animation tick. Writing m_tint from a draw hook
//     would achieve nothing: advanceAnimation() overwrites all three from the
//     step's output before the next paintGL() reads them. So the beat-driven
//     punch and colour flash are installed as a dispatch step.
//   - Everything drawn around the cube is a direct connection to
//     frameRendered(), which fires at the end of paintGL() while the context is
//     current and the depth buffer still holds the cube. Rings expanding
//     through the scene are therefore occluded by the cube for free.
//
// The audio itself is captured and analysed on a separate thread; see
// audio_analysis.h for why that thread is never joined.
//
// {"toggle": true} flips it, {"restore": true} takes it off, and the Cube menu
// gets a "Beat visualizer" entry like the other scene snippets.

#include "agent/agent_abi.h"
#include "audio_analysis.h"
#include "cube_widget.h"
#include "scene_toggle.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QMetaObject>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSignalBlocker>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr float pi = 3.14159265358979323846F;

// The shockwave rings lie in the plane perpendicular to the axis the cube
// turns about, so they read as coming off its own motion rather than being
// pasted over the top of it. Normalized here because building that plane's
// basis needs a unit vector, which is not something the widget owes anyone.
const QVector3D spinAxis = QVector3D(cubeSpinAxis[0], cubeSpinAxis[1], cubeSpinAxis[2]).normalized();

constexpr int barCount = audio_analysis::bandCount;

// Radii are given in the vertical half-frame at the cube's depth, which is
// 5.4 * tan(22.5 deg) = 2.237 world units. The horizontal half-frame is that
// times the aspect ratio, so a ring drawn with x scaled by the aspect is a
// circle on screen and uses the width and the height equally. A circle in world
// units would instead be limited by the height and leave the sides empty.
constexpr float ringInnerRadius = 1.42F;
constexpr float barMaxLength = 0.74F;
// Wide enough that the ring reads as separate bars rather than as a filled
// annulus with a bumpy edge.
constexpr float barGapDegrees = 1.05F;

// The cube is +/-1 with a half-diagonal of sqrt(3), which fills 77% of the
// vertical half-frame and leaves nothing to draw a rig in. The visualizer
// reframes the shot: the cube sits at this scale while the effect is on, and
// the kick punch rides on top of it.
constexpr float cubeBaseScale = 0.72F;

// Kick onsets throw a ring; everything else sheds a shell. Splitting them that
// way is what gives the two light temperatures something to mean.
constexpr int maxShockwaves = 3;
constexpr int ringSegments = 72;

constexpr int maxShells = 4;
constexpr float shellFrom = 1.06F;
// At full inflation the shell's half-diagonal is shellTo * cubeBaseScale *
// sqrt(3), which is 2.12 against a 2.24 half-frame, so a shell finishes inside
// the frame. One clipped by the frame edge reads as a cage around the scene
// rather than as a skin the cube shed.
constexpr float shellTo = 1.70F;

constexpr int particleCount = 320;

// The cube rests dimmer than the application draws it, and every onset snaps it
// to full. A beat then lights the cube up, rather than making an already-bright
// cube slightly brighter.
constexpr float tintRest = 0.82F;
constexpr float tintSleep = 0.55F;

// Underdamped, about 4 Hz with a damping ratio of 0.35: the peak lands roughly
// 60 ms after the hit, it dips once below rest, and it has settled by 450 ms.
constexpr float scaleFrequency = 26.0F;
constexpr float scaleStiffness = scaleFrequency * scaleFrequency;
constexpr float scaleDamping = 2.0F * 0.35F * scaleFrequency;

// ---------------------------------------------------------------------------
// Shared between the step function and the draw hook.
//
// Both run on the GUI thread, the step from the animation timer and the draw
// from paintGL(), so plain globals need no synchronisation between them. The
// analyzer is the one thing crossing threads and it has its own mutex.
// ---------------------------------------------------------------------------

std::shared_ptr<audio_analysis::Analyzer> analyzer;

// The step function is a bare C function pointer with no user data, so what it
// needs has to live somewhere it can reach without one.
struct StepDrive {
    // Scale is a spring rather than a decaying envelope. A decaying envelope
    // peaks in the same frame as the hit and only ever falls, which reads as a
    // step; a spring given a velocity overshoots, dips once below rest and
    // settles, which reads as something being struck.
    float scaleOffset = 0.0F;
    float scaleVelocity = 0.0F;

    float tintLevel = tintRest;
    float spinBoost = 0.0F;
    float warmth = 0.0F;
    float sleepiness = 0.0F;
    float silentSeconds = 0.0F;
    std::uint64_t seenBeats = 0;
    std::uint64_t seenKicks = 0;
    bool active = false;
};
StepDrive drive;

struct Shockwave {
    float radius = 0.0F;
    float speed = 0.0F;
    float life = 0.0F;
    float strength = 0.0F;
    float decayRate = 1.3F;
};

// A wireframe copy of the cube, frozen at the angle and scale it had when the
// onset landed, inflating away from it. The parent keeps turning inside, so the
// gap between shell and cube is the time since that beat made visible.
struct Shell {
    float elapsed = 0.0F;
    float lifespan = 0.55F;
    float angleAtHit = 0.0F;
    float scaleAtHit = 1.0F;
    float strength = 0.0F;
    float centroid = 0.5F;
    bool active = false;
};

struct Particle {
    QVector3D position;
    QVector3D velocity;
    float brightness = 0.0F;
    // Its own resting radius, not a shared one. A single target radius for the
    // whole field pulls the seeded volume into one thin shell within seconds,
    // and a shell projects to a bright rim at the edges of frame rather than to
    // dust in a room.
    float homeRadius = 2.5F;
    float phase = 0.0F;
    // Its own rate as well as its own phase. One shared frequency reads as
    // the whole field breathing together rather than as sparkle.
    float twinkleRate = 2.7F;
};

// One vertex format for every triangle this draws. Positions are already in
// view space, so the only transform left is the projection: the bars want to
// face the camera and the rings want to sit in a world-space plane, and doing
// both on the CPU means one buffer and one draw call instead of two of each.
struct Vertex {
    float x, y, z;
    float r, g, b, a;
};

struct PointVertex {
    float x, y, z;
    float r, g, b, a;
    float size;
};

constexpr auto glowVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
uniform mat4 projection;
out vec4 vertexColor;

void main()
{
    vertexColor = inColor;
    gl_Position = projection * vec4(inPosition, 1.0);
}
)glsl";

constexpr auto glowFragmentShader = R"glsl(
#version 330 core
in vec4 vertexColor;
out vec4 fragmentColor;

void main()
{
    fragmentColor = vertexColor;
}
)glsl";

constexpr auto pointVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inSize;
uniform mat4 projection;
out vec4 vertexColor;

void main()
{
    vertexColor = inColor;
    vec4 clip = projection * vec4(inPosition, 1.0);
    gl_Position = clip;
    gl_PointSize = inSize / max(clip.w, 0.25);
}
)glsl";

constexpr auto pointFragmentShader = R"glsl(
#version 330 core
in vec4 vertexColor;
out vec4 fragmentColor;

void main()
{
    // Round, soft-edged sprite. Squaring the falloff keeps the core bright and
    // the edge from looking like a disc with a hard rim.
    float radius = length(gl_PointCoord - vec2(0.5)) * 2.0;
    float falloff = clamp(1.0 - radius, 0.0, 1.0);
    fragmentColor = vec4(vertexColor.rgb, vertexColor.a * falloff * falloff);
}
)glsl";

struct VizState {
    QOpenGLContext* owner = nullptr;

    QOpenGLShaderProgram glowProgram;
    QOpenGLBuffer glowBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject glowArray;

    QOpenGLShaderProgram pointProgram;
    QOpenGLBuffer pointBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject pointArray;

    std::vector<Vertex> triangles;
    std::vector<PointVertex> points;

    std::array<Shockwave, maxShockwaves> shockwaves{};
    std::array<Shell, maxShells> shells{};
    std::vector<Particle> particles;

    float clock = 0.0F;
    float lastPuff = -10.0F;
    float lastKickAt = -10.0F;
    float roomLight = 0.0F;

    // The step function owns the cube's own state; this is only what the drawn
    // layers need.
    std::uint64_t seenBeats = 0;
    std::uint64_t seenKicks = 0;
    float flash = 0.0F;

    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();

    // The binding the host gave us, and what it displaced. Releasing the
    // binding is what puts things back: the host decides what the entry names
    // afterwards, so a replacement bound after this one is not overwritten.
    std::uint64_t binding = 0;
    void* previousStep = nullptr;

    std::uint64_t framesDrawn = 0;
};

VizState* viz = nullptr;
QMetaObject::Connection drawConnection;
QAction* toggleAction = nullptr;
RuntimeAgentHost hookHost{};
bool hookHostValid = false;
QString lastDeviceName;
bool emitBeatEvents = true;

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

// The cube's six faces are fully saturated primaries and are meant to stay the
// only real colour on screen. So nothing drawn around it gets a hue of its own:
// everything is near-white light at one of two temperatures, warm amber for
// low-frequency events and ice blue for high ones. The scene then reads as the
// cube being lit rather than as more coloured objects competing with it.
QVector3D lightTemperature(const float centroid)
{
    const QVector3D amber(1.00F, 0.78F, 0.54F);
    const QVector3D ice(0.62F, 0.84F, 1.00F);
    const float blend = std::clamp(centroid, 0.0F, 1.0F);
    return amber * (1.0F - blend) + ice * blend;
}

// The same two temperatures for the layers that are drawn dim. Additive light
// at low alpha over near-black loses its hue: both of the pair above land on
// grey, one warm and one cool, and the code stops being legible. Pulling the
// dim end toward saturation restores the distinction, and it is how hot metal
// behaves anyway, so it reads as physics rather than as a correction.
QVector3D ambientTemperature(const float centroid)
{
    const QVector3D amber(1.00F, 0.62F, 0.28F);
    const QVector3D ice(0.45F, 0.75F, 1.00F);
    const float blend = std::clamp(centroid, 0.0F, 1.0F);
    return amber * (1.0F - blend) + ice * blend;
}

// ---------------------------------------------------------------------------
// The cube's own motion, as a dispatch step
// ---------------------------------------------------------------------------

CubeStepOutput audioStep(const CubeStepInput* input) noexcept
{
    CubeStepOutput output{};
    if (input == nullptr) {
        return output;
    }

    float delta = input->delta_seconds;
    delta = delta > 0.0F ? std::min(delta, 0.1F) : 0.0F;

    audio_analysis::Frame frame;
    const bool haveAudio = analyzer != nullptr;
    if (haveAudio) {
        frame = analyzer->snapshot();
    }

    if (haveAudio) {
        // Onsets, not levels. Bass energy is present almost continuously in
        // most material, so driving the scale from it gives a cube that is
        // always slightly wrong and never visibly hits anything. A kick onset
        // is a discrete event, and a spring struck by one reads as rhythm.
        if (frame.kickCount != drive.seenKicks) {
            drive.seenKicks = frame.kickCount;
            // Shaped so a weak hit barely registers and a strong one lands,
            // rather than every hit arriving at much the same size.
            const float shaped = std::pow(std::clamp(frame.kickStrength / 1.5F, 0.0F, 1.0F), 1.4F);
            drive.scaleVelocity += 0.6F + 4.8F * shaped;
        }
        if (frame.beatCount != drive.seenBeats) {
            drive.seenBeats = frame.beatCount;
            const float amount = std::clamp(0.4F + 0.6F * frame.beatStrength, 0.0F, 1.6F);
            // Slight overshoot past full on the hardest hits only.
            drive.tintLevel = std::max(drive.tintLevel, 1.0F + 0.14F * std::max(amount - 1.0F, 0.0F));
            // A velocity kick, never a jump in angle. Snapping the angle reads
            // as a dropped frame; adding to the rate reads as eagerness.
            const float centroidWeight = 0.35F + 0.65F * frame.beatCentroid;
            drive.spinBoost = std::max(drive.spinBoost, 110.0F * std::min(amount, 1.2F) * centroidWeight);
            drive.silentSeconds = 0.0F;
        }
        // Slow, so it reads as the piece having a colour rather than as another
        // thing flickering.
        const float target = std::clamp(frame.bass - frame.treble * 0.8F, -1.0F, 1.0F);
        drive.warmth += (target - drive.warmth) * std::min(1.0F, delta * 1.2F);

        drive.silentSeconds = frame.silent ? drive.silentSeconds + delta : 0.0F;
    }

    // Nothing playing for two seconds dims the cube and lets it slow down. The
    // first onset after that snaps it back, so the wake-up is a moment the
    // scene gets for free rather than one that has to be scripted.
    const float sleepTarget = drive.silentSeconds > 2.0F ? 1.0F : 0.0F;
    drive.sleepiness += (sleepTarget - drive.sleepiness) * std::min(1.0F, delta * 1.4F);

    const auto decay = [delta](float& value, const float perSecond) {
        value *= std::exp(-perSecond * delta);
        if (value < 1.0e-4F) {
            value = 0.0F;
        }
    };
    decay(drive.spinBoost, 2.9F);

    // Spring: the hit set a velocity above, this integrates it.
    drive.scaleVelocity +=
        (-scaleStiffness * drive.scaleOffset - scaleDamping * drive.scaleVelocity) * delta;
    drive.scaleOffset += drive.scaleVelocity * delta;

    const float restingSpin = input->angular_velocity_degrees_per_second
        * (1.0F - 0.4F * drive.sleepiness);
    output.angle_degrees = input->angle_degrees + (restingSpin + drive.spinBoost) * delta;

    output.scale = cubeBaseScale * std::clamp(1.0F + drive.scaleOffset, 0.86F, 1.18F);

    // The tint multiplies the face colours. Resting below 1 is what makes the
    // snap to full on an onset visible at all.
    const float rest = tintRest + (tintSleep - tintRest) * drive.sleepiness;
    drive.tintLevel += (rest - drive.tintLevel) * std::min(1.0F, delta * 4.0F);
    const float level = drive.tintLevel;
    output.tint_r = level * (1.0F + 0.10F * std::max(drive.warmth, 0.0F));
    output.tint_g = level;
    output.tint_b = level * (1.0F + 0.12F * std::max(-drive.warmth, 0.0F));
    return output;
}

// ---------------------------------------------------------------------------
// Geometry, all emitted in view space
// ---------------------------------------------------------------------------

void addQuad(std::vector<Vertex>& out,
             const QVector3D& a,
             const QVector3D& b,
             const QVector3D& c,
             const QVector3D& d,
             const QVector3D& colour,
             const float alpha)
{
    const auto push = [&out, &colour, alpha](const QVector3D& p) {
        out.push_back(Vertex{p.x(), p.y(), p.z(),
                             colour.x(), colour.y(), colour.z(), alpha});
    };
    push(a);
    push(b);
    push(c);
    push(a);
    push(c);
    push(d);
}

// A quad whose two edges carry different alphas, so it fades across its width.
// addQuad gives every corner the same value, which cannot express a tail.
void addGradientQuad(std::vector<Vertex>& out,
                     const QVector3D& a,
                     const QVector3D& b,
                     const QVector3D& c,
                     const QVector3D& d,
                     const QVector3D& colour,
                     const float alphaAB,
                     const float alphaCD)
{
    const auto push = [&out, &colour](const QVector3D& p, const float alpha) {
        out.push_back(Vertex{p.x(), p.y(), p.z(),
                             colour.x(), colour.y(), colour.z(), alpha});
    };
    push(a, alphaAB);
    push(b, alphaAB);
    push(c, alphaCD);
    push(a, alphaAB);
    push(c, alphaCD);
    push(d, alphaCD);
}

// A line drawn as a camera-facing ribbon, so its width is set in pixels and
// stays readable whatever depth it ends up at.
//
// Each line is laid down twice, a wide faint halo under a narrow bright core.
// That pair is what makes a line look like it is glowing rather than like a
// polygon that happens to be pale, and it costs one extra quad.
void addGlowLine(std::vector<Vertex>& out,
                 const QVector3D& a,
                 const QVector3D& b,
                 const float pixelScale,
                 const QVector3D& colour,
                 const float alpha,
                 const float coreWidth)
{
    const QVector3D along = b - a;
    if (along.lengthSquared() < 1.0e-9F) {
        return;
    }
    const QVector3D direction = along.normalized();
    // The camera sits at the origin in view space, so this points from the
    // middle of the edge back at it.
    const QVector3D midpoint = (a + b) * 0.5F;
    QVector3D across = QVector3D::crossProduct(direction, (-midpoint).normalized());
    if (across.lengthSquared() < 1.0e-6F) {
        across = QVector3D::crossProduct(direction, QVector3D(0.0F, 1.0F, 0.0F));
    }
    across.normalize();

    const float worldPerPixel = pixelScale * std::abs(midpoint.z());
    const auto ribbon = [&](const float pixels, const float weight) {
        const QVector3D offset = across * (0.5F * pixels * worldPerPixel);
        addQuad(out, a - offset, b - offset, b + offset, a + offset, colour, alpha * weight);
    };
    ribbon(coreWidth * 4.3F, 0.17F);
    ribbon(coreWidth, 1.0F);
}

// The shed shells: a wireframe copy of the cube per onset, frozen at the pose
// the cube held when the hit landed and inflating away from it.
void buildShells(std::vector<Vertex>& out, const VizState& state, const float pixelScale)
{
    static const std::array<QVector3D, 8> corners{
        QVector3D(-1.0F, -1.0F, -1.0F), QVector3D(1.0F, -1.0F, -1.0F),
        QVector3D(1.0F, 1.0F, -1.0F),   QVector3D(-1.0F, 1.0F, -1.0F),
        QVector3D(-1.0F, -1.0F, 1.0F),  QVector3D(1.0F, -1.0F, 1.0F),
        QVector3D(1.0F, 1.0F, 1.0F),    QVector3D(-1.0F, 1.0F, 1.0F),
    };
    static const std::array<std::pair<int, int>, 12> edges{{
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    }};

    for (const Shell& shell : state.shells) {
        if (!shell.active) {
            continue;
        }
        const float t = std::clamp(shell.elapsed / shell.lifespan, 0.0F, 1.0F);
        // Fading on a gentler curve than the rings: a shell should still be
        // legible halfway through its life, because that is when the offset
        // from the parent cube is most readable.
        const float fade = std::pow(1.0F - t, 1.5F);
        // Birth alpha stays under 1, because anything above it clamps at blend
        // time and every shell then arrives at the same brightness. Strength
        // goes to line width instead, which has no ceiling in the way.
        const float alpha = fade * (0.5F + 0.5F * std::clamp(shell.strength, 0.0F, 1.0F));
        const float coreWidth = 1.5F + 1.5F * std::clamp(shell.strength, 0.0F, 1.0F);
        const QVector3D colour = lightTemperature(shell.centroid);

        // Two ghosts at earlier points on this shell's own inflation curve,
        // then the shell itself. Inside a single frame it is then visibly
        // leaving the cube, rather than being a box that happens to be larger
        // than it was last frame. The trail is evaluated from the curve that
        // produced it, so it needs no memory of past frames and no framebuffer
        // to accumulate them in.
        constexpr std::array<float, 3> ghostSecondsBack{0.050F, 0.025F, 0.0F};
        constexpr std::array<float, 3> ghostWeight{0.18F, 0.40F, 1.0F};

        for (int pass = 0; pass < 3; ++pass) {
            const float when = std::max(shell.elapsed - ghostSecondsBack[pass], 0.0F);
            const float ghostT = std::clamp(when / shell.lifespan, 0.0F, 1.0F);
            // Ease out: the shell leaps off the cube and then coasts, which is
            // what ties the movement to the instant of the hit rather than
            // spreading it evenly over the shell's life.
            const float eased = 1.0F - std::pow(1.0F - ghostT, 3.0F);
            const float inflate = shellFrom + (shellTo - shellFrom) * eased;

            QMatrix4x4 model;
            model.rotate(shell.angleAtHit, spinAxis);
            model.scale(shell.scaleAtHit * inflate);

            std::array<QVector3D, 8> view{};
            for (int corner = 0; corner < 8; ++corner) {
                const QVector3D world = model.map(corners[corner]);
                view[corner] = QVector3D(world.x(), world.y(), world.z() - cubeViewDistance);
            }
            // The ghosts are thinner as well as fainter, so the trail tapers
            // back toward where the shell came from instead of reading as three
            // separate wireframes.
            const float width = coreWidth * (0.55F + 0.45F * ghostWeight[pass]);
            for (const auto& edge : edges) {
                addGlowLine(out, view[edge.first], view[edge.second], pixelScale, colour,
                            alpha * ghostWeight[pass], width);
            }
        }
    }
}

// A mirrored pair of arcs rather than one sweep all the way round: bass sits at
// the bottom and the spectrum climbs both sides to meet at the top. The
// symmetry is what makes it look arranged instead of merely circular.
void buildSpectrumRing(std::vector<Vertex>& out,
                       const audio_analysis::Frame& frame,
                       const float flash,
                       const float aspect)
{
    // Each side sweeps a full half-circle: bass at the bottom, climbing both
    // ways to meet at the top. The mirroring is what makes it read as arranged
    // rather than as a strip bent into a circle.
    constexpr float span = 180.0F;
    constexpr float step = span / static_cast<float>(barCount);
    constexpr float halfWidth = 0.5F * step - 0.5F * barGapDegrees;

    for (int band = 0; band < barCount; ++band) {
        const float position = static_cast<float>(band) / static_cast<float>(barCount - 1);
        const float level = frame.bands[band];

        // A floor under the length so the rig still has a shape in silence.
        // Straight normalized dB leaves most of a busy mix bunched near the
        // top of the range, so every bar ends up the same length. The curve
        // pushes the quiet bands back down and leaves the peaks alone.
        const float shaped = std::pow(level, 1.7F);
        const float length = 0.04F + barMaxLength * shaped;
        const float outerRadius = ringInnerRadius + length;

        // Position along the spectrum picks the temperature, so the bass end
        // of the ring is amber and the treble end ice, matching what an onset
        // from that part of the spectrum will throw.
        const QVector3D colour = ambientTemperature(position);
        // Held dim, and barely flashing. This is the largest continuous element
        // on screen, and the beat belongs to the events: a rig that brightens on
        // every onset competes with the shell being born and leaves nothing on
        // screen looking sharp.
        const float alpha = std::clamp(0.10F + 0.90F * shaped, 0.0F, 1.0F)
            * (1.0F + 0.20F * std::min(flash, 1.0F)) * 0.38F;

        const auto point = [aspect](const float degrees, const float radius) {
            const float angle = degrees * pi / 180.0F;
            return QVector3D(std::cos(angle) * radius * aspect,
                             std::sin(angle) * radius,
                             -cubeViewDistance);
        };

        for (const int side : {-1, 1}) {
            const float centre = -90.0F + static_cast<float>(side) * position * span;
            addQuad(out,
                    point(centre - halfWidth, ringInnerRadius),
                    point(centre + halfWidth, ringInnerRadius),
                    point(centre + halfWidth, outerRadius),
                    point(centre - halfWidth, outerRadius),
                    colour,
                    alpha);
        }
    }
}

// Rings in the plane the cube turns in, expanding outward through the scene.
// They are drawn in world space and then moved into view space by hand, which
// is what lets the cube occlude the half of each ring that is behind it.
void buildShockwaves(std::vector<Vertex>& out, const VizState& state)
{
    QVector3D u = QVector3D::crossProduct(spinAxis, QVector3D(0.0F, 0.0F, 1.0F));
    if (u.lengthSquared() < 1.0e-4F) {
        u = QVector3D::crossProduct(spinAxis, QVector3D(0.0F, 1.0F, 0.0F));
    }
    u.normalize();
    const QVector3D v = QVector3D::crossProduct(spinAxis, u).normalized();

    for (const Shockwave& wave : state.shockwaves) {
        if (wave.life <= 0.0F) {
            continue;
        }
        // Thins as it grows and fades on a curve rather than linearly, so it
        // leaves the frame instead of switching off in it.
        const float fade = wave.life * wave.life;
        const float thickness = (0.006F + 0.022F * wave.life) * (0.5F + 0.5F * wave.strength);
        const float alpha = 1.15F * fade * std::clamp(wave.strength, 0.3F, 1.4F);
        // Rings belong to kicks, so they are always the warm temperature.
        const QVector3D colour = lightTemperature(0.10F);

        for (int segment = 0; segment < ringSegments; ++segment) {
            const float a0 = 2.0F * pi * static_cast<float>(segment)
                / static_cast<float>(ringSegments);
            const float a1 = 2.0F * pi * static_cast<float>(segment + 1)
                / static_cast<float>(ringSegments);

            const auto point = [&](const float angle, const float radius) {
                const QVector3D world = (u * std::cos(angle) + v * std::sin(angle)) * radius;
                return QVector3D(world.x(), world.y(), world.z() - cubeViewDistance);
            };

            // The tail trails inward, from where the ring was about an eighth
            // of a second ago up to the ring itself, fading to nothing at the
            // far end. A still frame cannot tell an expanding ring from a
            // painted ellipse; with the tail the direction is in the shape, and
            // it comes from the speed already being integrated rather than from
            // any record of past frames.
            const float tailInner = std::max(wave.radius - wave.speed * 0.12F, 0.05F);
            addGradientQuad(out,
                            point(a0, tailInner),
                            point(a1, tailInner),
                            point(a1, wave.radius - thickness),
                            point(a0, wave.radius - thickness),
                            colour,
                            0.0F,
                            alpha * 0.5F);

            addQuad(out,
                    point(a0, wave.radius - thickness),
                    point(a1, wave.radius - thickness),
                    point(a1, wave.radius + thickness),
                    point(a0, wave.radius + thickness),
                    colour,
                    alpha);
        }
    }
}

void buildParticles(std::vector<PointVertex>& out,
                    std::vector<Vertex>& streaks,
                    const VizState& state,
                    const audio_analysis::Frame& frame,
                    const float flash,
                    const float pixelScale)
{
    const QVector3D colour = ambientTemperature(0.72F);
    for (const Particle& particle : state.particles) {
        const QVector3D view(particle.position.x(),
                             particle.position.y(),
                             particle.position.z() - cubeViewDistance);
        // Treble drives a shimmer, and every particle carries its own phase, so
        // the field twinkles instead of pulsing as one object. With nothing
        // bright in the mix it sits almost still, which is the point: this is
        // the layer that has to look alive during a quiet passage.
        const float twinkle =
            0.65F
            + 0.35F * std::sin(state.clock * particle.twinkleRate + particle.phase)
                * frame.treble;
        // Point size divides by w, so a particle drifting close to the camera
        // becomes a screen-filling blob. Fading the last couple of units keeps
        // the field in the room instead of in the lens.
        const float nearFade = std::clamp(-view.z() / 2.5F, 0.0F, 1.0F);
        const float alpha = std::clamp(
            particle.brightness * twinkle * nearFade * (0.75F + 0.45F * flash), 0.0F, 1.0F);
        // A particle still carrying puff velocity is drawn as a short streak
        // back along its own velocity rather than as a dot. The puff then reads
        // as the field being pushed outward, which is what it is, instead of as
        // the dots briefly getting brighter.
        const float speed = particle.velocity.length();
        if (speed > 0.55F) {
            const QVector3D from = particle.position - particle.velocity * 0.04F;
            addGlowLine(streaks,
                        QVector3D(from.x(), from.y(), from.z() - cubeViewDistance),
                        view,
                        pixelScale,
                        colour,
                        alpha * 0.8F,
                        1.2F);
        }

        out.push_back(PointVertex{view.x(), view.y(), view.z(),
                                  colour.x(), colour.y(), colour.z(), alpha,
                                  16.0F + 22.0F * particle.brightness});
    }
}

// A radial lift of the background driven by smoothed loudness, drawn as a fan
// at far depth so only pixels the cube did not write accept it. Nobody looks at
// this directly; it is what makes loud and quiet feel different rather than
// only look different, and without it the scene sits on flat black.
void buildRoomLight(std::vector<Vertex>& out, const float intensity, const float aspect)
{
    if (intensity <= 0.002F) {
        return;
    }
    constexpr int segments = 44;
    constexpr float depth = 60.0F;
    // Reaches just past the top and bottom of the frame, so the corners fall
    // outside the fan and stay dark. That is the vignette, for no extra work.
    const float radius = depth * std::tan(22.5F * pi / 180.0F) * 1.08F;
    const QVector3D colour(0.09F, 0.12F, 0.20F);

    for (int segment = 0; segment < segments; ++segment) {
        const auto rim = [&](const int index) {
            const float angle = 2.0F * pi * static_cast<float>(index)
                / static_cast<float>(segments);
            return Vertex{std::cos(angle) * radius * aspect, std::sin(angle) * radius, -depth,
                          colour.x(), colour.y(), colour.z(), 0.0F};
        };
        out.push_back(Vertex{0.0F, 0.0F, -depth,
                             colour.x(), colour.y(), colour.z(), intensity});
        out.push_back(rim(segment));
        out.push_back(rim(segment + 1));
    }
}

// ---------------------------------------------------------------------------
// Per-frame update and draw
// ---------------------------------------------------------------------------

void advance(VizState& state,
             const audio_analysis::Frame& frame,
             const float delta,
             const CubeWidget* cube)
{
    state.clock += delta;

    // Kicks are handled before beats, so the beat below can ask whether one has
    // just fired rather than guessing from the spectrum.
    if (frame.kickCount != state.seenKicks) {
        state.seenKicks = frame.kickCount;
        state.lastKickAt = state.clock;
        Shockwave* slot = &state.shockwaves[0];
        for (Shockwave& candidate : state.shockwaves) {
            if (candidate.life < slot->life) {
                slot = &candidate;
            }
        }
        slot->radius = 0.6F;
        slot->life = 1.0F;
        slot->strength = std::clamp(0.45F + 0.55F * frame.kickStrength, 0.3F, 1.5F);
        // Clamped against the beat interval the same way a shell is, so a ring
        // is gone by about the time the next kick lands. Amber means "a kick
        // just happened" only while one ring at a time is showing it.
        const float interval = frame.bpm > 20.0F ? 60.0F / frame.bpm : 0.8F;
        slot->decayRate = std::clamp(1.0F / (0.9F * interval), 1.0F, 2.6F);
        // Speed follows from the decay, so a ring reaches much the same radius
        // whatever the tempo and fades out before the frame edge instead of
        // crossing it while still bright enough to read as a live event.
        const float deathRadius = 1.85F + 0.30F * std::clamp(frame.kickStrength, 0.0F, 1.5F);
        slot->speed = (deathRadius - slot->radius) * slot->decayRate;
    }

    if (frame.beatCount != state.seenBeats) {
        state.seenBeats = frame.beatCount;
        state.flash = std::min(1.6F, state.flash + 0.5F + 0.45F * frame.beatStrength);

        // The detected beat is published as an ordinary agent event, so a
        // client can subscribe to the analysis without having to draw anything:
        //   agentctl.py events --prefix audio.
        if (emitBeatEvents && hookHostValid) {
            const QJsonObject payload{
                {QStringLiteral("strength"), frame.beatStrength},
                {QStringLiteral("centroid"), frame.beatCentroid},
                {QStringLiteral("bpm"), frame.bpm},
                {QStringLiteral("bass"), frame.bass},
                {QStringLiteral("treble"), frame.treble},
                {QStringLiteral("count"), static_cast<qint64>(frame.beatCount)},
            };
            hookHost.emit_event_json(
                hookHost.agent_context, "audio.beat",
                QJsonDocument(payload).toJson(QJsonDocument::Compact).constData());
        }

        // One event, one gesture. A kick trips both detectors, so a shell here
        // would answer it a second time and teach the eye that the shapes are
        // decoration.
        //
        // The test is whether a kick fired, because a fired kick is exactly what
        // draws the other shape. Where the energy sits answers a different
        // question: an onset can be low and still miss the 30-170 Hz kick band,
        // a tom or a bass stab being the usual case, and that onset needs its
        // shell or nothing is drawn for it at all. The window covers a kick
        // landing an analysis hop either side of the beat, which happens often
        // enough at 94 hops a second.
        if (state.clock - state.lastKickAt > 0.07F) {
            // Oldest slot wins, so a dense passage replaces the shell nearest
            // to finishing rather than refusing to show the new one.
            Shell* slot = &state.shells[0];
            for (Shell& candidate : state.shells) {
                if (!candidate.active) {
                    slot = &candidate;
                    break;
                }
                if (candidate.elapsed / candidate.lifespan > slot->elapsed / slot->lifespan) {
                    slot = &candidate;
                }
            }
            slot->active = true;
            slot->elapsed = 0.0F;
            // Clamped against the estimated beat interval, so shells never pile
            // up at a fast tempo and never leave a gap at a slow one.
            const float interval = frame.bpm > 20.0F ? 60.0F / frame.bpm : 0.55F;
            slot->lifespan = std::clamp(0.85F * interval, 0.26F, 0.78F);
            // Frozen here: the parent goes on turning, and the growing offset
            // between the two is what makes the shell read as a moment past.
            slot->angleAtHit = cube->m_angleDegrees;
            slot->scaleAtHit = cube->m_scale;
            slot->strength = std::clamp(frame.beatStrength, 0.0F, 2.0F);
            slot->centroid = frame.beatCentroid;
        }

        // The one big gesture the dust field gets, and only on the loudest
        // hits. Every beat pushing it would leave it permanently churning and
        // the push would stop meaning anything.
        if (frame.beatStrength > 0.9F && state.clock - state.lastPuff > 1.4F) {
            state.lastPuff = state.clock;
            for (Particle& particle : state.particles) {
                particle.velocity += particle.position.normalized() * 1.5F;
                particle.brightness = std::min(1.0F, particle.brightness + 0.55F);
            }
        }
    }

    // Fast up, slow down, so the room swells with a loud passage and takes its
    // time coming back rather than tracking every bar.
    const float loudTarget = std::clamp(frame.loudness * 1.7F, 0.0F, 1.0F);
    state.roomLight += (loudTarget - state.roomLight)
        * std::min(1.0F, delta * (loudTarget > state.roomLight ? 3.3F : 0.7F));

    state.flash *= std::exp(-7.0F * delta);

    for (Shell& shell : state.shells) {
        if (!shell.active) {
            continue;
        }
        shell.elapsed += delta;
        if (shell.elapsed >= shell.lifespan) {
            shell.active = false;
        }
    }

    for (Shockwave& wave : state.shockwaves) {
        if (wave.life <= 0.0F) {
            continue;
        }
        wave.radius += wave.speed * delta;
        wave.life -= delta * wave.decayRate;
        if (wave.life < 0.0F) {
            wave.life = 0.0F;
        }
    }

    // A weak spring back toward the shell plus heavy drag: the field breathes
    // outward on a hit and drifts home, so quiet passages still have motion in
    // them without anything escaping the frame.
    for (Particle& particle : state.particles) {
        const float radius = particle.position.length();
        const float target = particle.homeRadius;
        if (radius > 1.0e-3F) {
            const QVector3D inward = particle.position / radius;
            particle.velocity -= inward * (radius - target) * 2.2F * delta;
        }
        particle.velocity *= std::exp(-1.9F * delta);
        particle.position += particle.velocity * delta;
        particle.brightness *= std::exp(-1.1F * delta);
        particle.brightness = std::max(particle.brightness, 0.22F);
    }
}

void drawFrame(CubeWidget* cube)
{
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (viz == nullptr || context == nullptr || context != viz->owner) {
        return;
    }
    VizState& state = *viz;

    const auto now = std::chrono::steady_clock::now();
    float delta = std::chrono::duration<float>(now - state.lastFrame).count();
    state.lastFrame = now;
    delta = std::clamp(delta, 0.0F, 0.1F);

    audio_analysis::Frame frame;
    if (analyzer != nullptr) {
        frame = analyzer->snapshot();
    }
    advance(state, frame, delta, cube);

    // Read the aspect back out of the projection rather than the widget size,
    // so a resized window keeps the ring circular on screen without this having
    // to watch for the resize.
    const float horizontalScale = cube->m_projection(0, 0);
    const float verticalScale = cube->m_projection(1, 1);
    const float aspect = horizontalScale > 1.0e-5F ? verticalScale / horizontalScale : 1.0F;

    QOpenGLFunctions* gl = context->functions();

    // World units per pixel, per unit of depth. Taking the viewport from GL
    // rather than from the widget's size keeps this right under a device pixel
    // ratio other than 1, where the two differ.
    std::array<GLint, 4> viewport{};
    gl->glGetIntegerv(GL_VIEWPORT, viewport.data());
    const float viewportHeight = viewport[3] > 0 ? static_cast<float>(viewport[3]) : 1.0F;
    const float pixelScale = 2.0F * std::tan(22.5F * pi / 180.0F) / viewportHeight;

    state.triangles.clear();
    state.points.clear();
    buildRoomLight(state.triangles, 0.22F + 0.45F * state.roomLight, aspect);
    buildSpectrumRing(state.triangles, frame, state.flash, aspect);
    buildShockwaves(state.triangles, state);
    buildShells(state.triangles, state, pixelScale);
    buildParticles(state.points, state.triangles, state, frame, state.flash, pixelScale);

    // paintGL() leaves blending off, depth writes on and back-face culling on.
    // All three are wrong for additive glow over a scene that is already drawn,
    // and none of them are reset per frame by the widget, so they are put back
    // exactly as they were before this returns.
    GLboolean blendWas = GL_FALSE;
    GLboolean depthMaskWas = GL_TRUE;
    GLboolean cullWas = GL_FALSE;
    gl->glGetBooleanv(GL_BLEND, &blendWas);
    gl->glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMaskWas);
    gl->glGetBooleanv(GL_CULL_FACE, &cullWas);

    gl->glEnable(GL_BLEND);
    // Additive. Overlapping glow accumulates toward white instead of the
    // nearest layer hiding the ones behind it, which is what makes a dense
    // passage look bright rather than merely crowded.
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    // Still depth-tested against the cube, but writing no depth of its own, so
    // the cube occludes the far side of a ring while two rings never occlude
    // each other.
    gl->glDepthMask(GL_FALSE);
    gl->glDisable(GL_CULL_FACE);

    if (!state.triangles.empty()) {
        state.glowProgram.bind();
        state.glowProgram.setUniformValue("projection", cube->m_projection);
        state.glowArray.bind();
        state.glowBuffer.bind();
        state.glowBuffer.allocate(
            state.triangles.data(),
            static_cast<int>(state.triangles.size() * sizeof(Vertex)));
        gl->glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(state.triangles.size()));
        state.glowBuffer.release();
        state.glowArray.release();
        state.glowProgram.release();
    }

    if (!state.points.empty()) {
        gl->glEnable(GL_PROGRAM_POINT_SIZE);
        state.pointProgram.bind();
        state.pointProgram.setUniformValue("projection", cube->m_projection);
        state.pointArray.bind();
        state.pointBuffer.bind();
        state.pointBuffer.allocate(
            state.points.data(),
            static_cast<int>(state.points.size() * sizeof(PointVertex)));
        gl->glDrawArrays(GL_POINTS, 0, static_cast<int>(state.points.size()));
        state.pointBuffer.release();
        state.pointArray.release();
        state.pointProgram.release();
        gl->glDisable(GL_PROGRAM_POINT_SIZE);
    }

    if (blendWas == GL_FALSE) {
        gl->glDisable(GL_BLEND);
    }
    gl->glDepthMask(depthMaskWas);
    if (cullWas == GL_TRUE) {
        gl->glEnable(GL_CULL_FACE);
    }

    ++state.framesDrawn;
}

// ---------------------------------------------------------------------------
// Install and remove
// ---------------------------------------------------------------------------

bool buildPrograms(VizState& state, QString& error)
{
    if (!state.glowProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, glowVertexShader)
        || !state.glowProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, glowFragmentShader)
        || !state.glowProgram.link()) {
        error = state.glowProgram.log();
        return false;
    }
    if (!state.pointProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, pointVertexShader)
        || !state.pointProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, pointFragmentShader)
        || !state.pointProgram.link()) {
        error = state.pointProgram.log();
        return false;
    }
    return true;
}

void seedParticles(VizState& state)
{
    state.particles.resize(particleCount);
    // A fixed pattern rather than a random one: the field should look the same
    // every time this is switched on, so a screenshot is reproducible.
    for (int i = 0; i < particleCount; ++i) {
        const float index = static_cast<float>(i);
        // Fibonacci sphere, which spreads points evenly without clumping at the
        // poles the way a naive angle pair does.
        const float y = 1.0F - 2.0F * (index + 0.5F) / static_cast<float>(particleCount);
        const float radiusAtY = std::sqrt(std::max(0.0F, 1.0F - y * y));
        const float theta = pi * (1.0F + std::sqrt(5.0F)) * index;
        const float spread = std::fmod(index * 0.61803F, 1.0F);
        const float jitter = 2.2F + 2.3F * spread;
        state.particles[i].position = QVector3D(std::cos(theta) * radiusAtY,
                                                y,
                                                std::sin(theta) * radiusAtY)
            * jitter;
        state.particles[i].velocity = QVector3D();
        state.particles[i].brightness = 0.22F;
        state.particles[i].homeRadius = jitter;
        state.particles[i].phase = spread * 2.0F * pi;
        state.particles[i].twinkleRate = 2.0F + 4.0F * spread;
    }
}

// Needs the widget's OpenGL context to be current.
bool install(CubeWidget* cube, const audio_analysis::Analyzer::Options& options, QString& error)
{
    auto candidate = std::make_shared<audio_analysis::Analyzer>();
    std::string openError;
    if (!candidate->open(options, openError)) {
        error = QString::fromStdString(openError);
        return false;
    }

    auto* state = new VizState();
    state->owner = QOpenGLContext::currentContext();
    if (!buildPrograms(*state, error)) {
        delete state;
        return false;
    }

    state->glowArray.create();
    state->glowArray.bind();
    state->glowBuffer.create();
    state->glowBuffer.bind();
    state->glowBuffer.setUsagePattern(QOpenGLBuffer::StreamDraw);
    state->glowProgram.bind();
    state->glowProgram.enableAttributeArray(0);
    state->glowProgram.setAttributeBuffer(0, GL_FLOAT, 0, 3, sizeof(Vertex));
    state->glowProgram.enableAttributeArray(1);
    state->glowProgram.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 4, sizeof(Vertex));
    state->glowProgram.release();
    state->glowArray.release();
    state->glowBuffer.release();

    state->pointArray.create();
    state->pointArray.bind();
    state->pointBuffer.create();
    state->pointBuffer.bind();
    state->pointBuffer.setUsagePattern(QOpenGLBuffer::StreamDraw);
    state->pointProgram.bind();
    state->pointProgram.enableAttributeArray(0);
    state->pointProgram.setAttributeBuffer(0, GL_FLOAT, 0, 3, sizeof(PointVertex));
    state->pointProgram.enableAttributeArray(1);
    state->pointProgram.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 4, sizeof(PointVertex));
    state->pointProgram.enableAttributeArray(2);
    state->pointProgram.setAttributeBuffer(2, GL_FLOAT, 7 * sizeof(float), 1, sizeof(PointVertex));
    state->pointProgram.release();
    state->pointArray.release();
    state->pointBuffer.release();

    seedParticles(*state);

    analyzer = candidate;
    audio_analysis::Analyzer::spawn(analyzer);
    lastDeviceName = QString::fromStdString(analyzer->device());

    drive = StepDrive{};
    drive.active = true;

    viz = state;

    // Asking the host to make audioStep what the cube's step function reaches.
    // Nothing in the application holds a pointer for this: the host installs
    // whatever redirection the target needs and hands back the binding.
    RuntimeAgentPatchBinding bound{};
    const std::int32_t boundResult = hookHost.patch_bind(
        hookHost.agent_context,
        reinterpret_cast<void*>(&cube_step_builtin),
        reinterpret_cast<void*>(&audioStep),
        &bound);
    if (boundResult != 0) {
        error = QStringLiteral("the host refused to bind the audio step function (%1)")
                    .arg(boundResult);
        state->glowArray.destroy();
        state->glowBuffer.destroy();
        state->pointArray.destroy();
        state->pointBuffer.destroy();
        delete state;
        viz = nullptr;
        analyzer->requestStop();
        analyzer.reset();
        return false;
    }
    state->binding = bound.id;
    state->previousStep = bound.previous;
    drawConnection = QObject::connect(
        cube, &CubeWidget::frameRendered, cube,
        [cube](qulonglong, float) { drawFrame(cube); },
        Qt::DirectConnection);
    cube->update();
    return true;
}

// Needs the widget's OpenGL context to be current.
void remove(CubeWidget* cube)
{
    QObject::disconnect(drawConnection);

    if (analyzer != nullptr) {
        analyzer->requestStop();
        // Dropped rather than joined. The capture thread holds its own
        // reference and tears the stream down when it next wakes; see the note
        // in audio_analysis.h.
        analyzer.reset();
    }

    if (viz != nullptr) {
        if (viz->binding != 0 && hookHostValid) {
            // Releasing rather than restoring a pointer. Something bound after
            // this one stays selected, which a raw store would have destroyed.
            (void)hookHost.patch_unbind(hookHost.agent_context, viz->binding);
            viz->binding = 0;
        }
        viz->glowArray.destroy();
        viz->glowBuffer.destroy();
        viz->pointArray.destroy();
        viz->pointBuffer.destroy();
        delete viz;
        viz = nullptr;
    }

    // The step function is gone, so the last tint and scale it produced would
    // otherwise stay on the cube until something else moved them.
    cube->m_tint = QVector3D(1.0F, 1.0F, 1.0F);
    cube->m_scale = 1.0F;
    drive = StepDrive{};
    cube->update();
}

void syncToggleAction()
{
    if (toggleAction != nullptr) {
        const QSignalBlocker blocker(toggleAction);
        toggleAction->setChecked(viz != nullptr);
    }
}

bool setEnabled(CubeWidget* cube,
                const bool enabled,
                const audio_analysis::Analyzer::Options& options,
                QString& error)
{
    if (enabled == (viz != nullptr)) {
        return true;
    }
    bool installed = true;
    if (enabled) {
        installed = install(cube, options, error);
    } else {
        remove(cube);
    }
    syncToggleAction();
    return installed;
}

audio_analysis::Analyzer::Options currentOptions;

void ensureToggleAction(CubeWidget* cube)
{
    if (toggleAction != nullptr) {
        return;
    }
    toggleAction = scene_toggle::install(
        cube,
        QStringLiteral("actionBeatVisualizer"),
        QStringLiteral("&Beat visualizer"),
        QStringLiteral("Ctrl+Shift+B"),
        [cube](const bool enabled) {
            // The menu runs on the GUI thread with no current context, and both
            // installing and removing need one, so the work waits for a frame.
            if (scene_toggle::ownContextIsCurrent(cube)) {
                QString ignored;
                setEnabled(cube, enabled, currentOptions, ignored);
                return;
            }
            cube->enqueueRenderCallback([cube, enabled] {
                QString deferred;
                if (!setEnabled(cube, enabled, currentOptions, deferred) && hookHostValid) {
                    hookHost.log(hookHost.agent_context, RUNTIME_AGENT_LOG_WARNING,
                                 deferred.toLocal8Bit().constData());
                }
                syncToggleAction();
            });
        });
    syncToggleAction();
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void release(const RuntimeAgentHost* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI) {
        return;
    }
    auto* cube = static_cast<CubeWidget*>(
        host->find_qobject(host->agent_context, "cubeView"));
    if (cube == nullptr) {
        host->fail(host->invocation_context, "cubeView was not found");
        return;
    }
    if (viz == nullptr) {
        host->complete_json(host->invocation_context,
                            "{\"removed\":false,\"note\":\"nothing was installed\"}");
        return;
    }
    // Tearing down GL objects and putting the step function back both have to
    // have happened before this reports completion, because a handover is
    // sequenced on it. Deferring to the next frame would report done while the
    // cube is still being driven by code the next generation is replacing.
    if (!scene_toggle::ownContextIsCurrent(cube)) {
        host->fail(host->invocation_context,
                   "the widget's OpenGL context is not current; release this module with "
                   "executor=render so the scene is back before this reports completion");
        return;
    }
    const std::uint64_t frames = viz->framesDrawn;
    remove(cube);
    syncToggleAction();
    host->complete_json(
        host->invocation_context,
        QJsonDocument(QJsonObject{
                          {QStringLiteral("removed"), true},
                          {QStringLiteral("framesDrawn"), static_cast<qint64>(frames)},
                      })
            .toJson(QJsonDocument::Compact)
            .constData());
}

void run(const RuntimeAgentHost* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI) {
        return;
    }

    auto* cube = static_cast<CubeWidget*>(
        host->find_qobject(host->agent_context, "cubeView"));
    if (cube == nullptr) {
        host->fail(host->invocation_context, "cubeView was not found");
        return;
    }
    if (QOpenGLContext::currentContext() == nullptr) {
        host->fail(host->invocation_context,
                   "no OpenGL context is current; use executor=render");
        return;
    }

    if (!hookHostValid) {
        // Everything except invocation_context is process-lifetime, so a copy
        // with that one cleared is what the menu handler and the draw hook use
        // after this call returns.
        hookHost = *host;
        hookHost.invocation_context = nullptr;
        hookHostValid = true;
    }

    const QJsonObject request =
        QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();
    ensureToggleAction(cube);

    // Parameters persist across an off/on cycle, so switching the effect back on
    // from the menu reuses whatever it was last given.
    if (request.contains(QStringLiteral("device"))) {
        currentOptions.device = request.value(QStringLiteral("device")).toString().toStdString();
    }
    if (request.contains(QStringLiteral("sensitivity"))) {
        currentOptions.sensitivity = std::clamp(
            static_cast<float>(request.value(QStringLiteral("sensitivity")).toDouble()),
            1.02F, 4.0F);
    }
    if (request.contains(QStringLiteral("dynamicRangeDb"))) {
        currentOptions.dynamicRangeDb = std::clamp(
            static_cast<float>(request.value(QStringLiteral("dynamicRangeDb")).toDouble()),
            20.0F, 100.0F);
    }
    if (request.contains(QStringLiteral("events"))) {
        emitBeatEvents = request.value(QStringLiteral("events")).toBool();
    }

    const auto complete = [host](QJsonObject result) {
        result.insert(QStringLiteral("menuToggle"), toggleAction != nullptr);
        host->complete_json(host->invocation_context,
                            QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    };

    const bool takeOff = request.value(QStringLiteral("restore")).toBool()
        || request.value(QStringLiteral("remove")).toBool()
        || (request.value(QStringLiteral("toggle")).toBool() && viz != nullptr);
    if (takeOff) {
        if (viz == nullptr) {
            complete(QJsonObject{
                {QStringLiteral("removed"), false},
                {QStringLiteral("note"), QStringLiteral("no visualizer was installed")},
            });
            return;
        }
        remove(cube);
        syncToggleAction();
        complete(QJsonObject{{QStringLiteral("removed"), true}});
        return;
    }

    if (viz != nullptr) {
        // Already running. Report what it is doing rather than installing a
        // second copy of a draw hook.
        const audio_analysis::Frame frame =
            analyzer != nullptr ? analyzer->snapshot() : audio_analysis::Frame{};
        complete(QJsonObject{
            {QStringLiteral("installed"), true},
            {QStringLiteral("note"), QStringLiteral("the beat visualizer was already running")},
            {QStringLiteral("device"), lastDeviceName},
            {QStringLiteral("capturing"), analyzer != nullptr && analyzer->running()},
            {QStringLiteral("bpm"), frame.bpm},
            {QStringLiteral("beats"), static_cast<qint64>(frame.beatCount)},
            {QStringLiteral("silent"), frame.silent},
        });
        return;
    }

    QString error;
    if (!setEnabled(cube, true, currentOptions, error)) {
        host->fail(host->invocation_context, error.toLocal8Bit().constData());
        return;
    }

    complete(QJsonObject{
        {QStringLiteral("installed"), true},
        {QStringLiteral("connectionValid"), static_cast<bool>(drawConnection)},
        {QStringLiteral("device"), lastDeviceName},
        {QStringLiteral("binding"), static_cast<qint64>(viz->binding)},
        {QStringLiteral("bands"), audio_analysis::bandCount},
        {QStringLiteral("fftSize"), audio_analysis::fftSize},
        {QStringLiteral("hopSize"), audio_analysis::hopSize},
        {QStringLiteral("particles"), particleCount},
        {QStringLiteral("sensitivity"), currentOptions.sensitivity},
    });
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "drive the cube from the system audio mix",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init()
{
    return &descriptor;
}
