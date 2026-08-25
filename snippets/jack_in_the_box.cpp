// Opens one side of the cube and puts a spring-loaded head inside it.
//
// paintGL() draws the cube with a hardcoded glDrawArrays(GL_TRIANGLES, 0, 36),
// so a single face cannot be dropped from outside. This snippet keeps a copy of
// the chosen face's six vertices and overwrites them with zeros, which makes
// that face's two triangles rasterize degenerate. The other five faces still
// come from the widget's own buffer and are untouched.
//
// With the face gone the cube is an open shell, and backface culling would let
// the camera see straight through it, so this draws the interior itself: five
// inset inner walls in the widget's own face colors, and a rim around the
// opening so the wall has visible thickness.
//
// The head stays inside until the open side turns toward the camera. The dot
// product of the open face's normal with the direction to the camera drives the
// spring's extension, so it pops out as the side comes around and retracts as
// it leaves. Each full pop emits a jack.popped event.
//
// {"side": "front|back|left|right|top|bottom"} chooses the face, default front.
// {"restore": true} puts the sixth face back.

#include "agent/agent_abi.h"
#include "cube_widget.h"
#include "cube_mesh.h"
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
#include <QQuaternion>
#include <QSignalBlocker>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

constexpr float pi = 3.14159265358979323846F;

// Must match the values paintGL() uses, since this shares the frame.
constexpr float viewDistance = 5.4F;

constexpr float outerHalf = 1.0F;
constexpr float innerHalf = 0.93F;
constexpr float interiorDim = 0.42F;

constexpr float springRadius = 0.42F;
constexpr float tubeRadius = 0.055F;
constexpr float springCoils = 4.5F;
constexpr int springSamples = 120;
constexpr int springRingSegments = 8;
constexpr float retractedLength = 0.55F;
constexpr float popTravel = 2.45F;
constexpr float springTwistTurns = 0.85F;

// The extension does not follow the open side directly. It is a mass on a
// spring pulled toward that target, which is what makes the head overshoot and
// rebound instead of arriving and stopping. Stiffness sets the bounce rate near
// 1.25 Hz; damping is about a seventh of critical, so each swing keeps roughly
// 40% of the last one and the head is still bouncing for about as long as the
// open side stays pointed at the camera.
//
// The overshoot limit only exists to stop a step change in the target throwing
// the head clear of the scene. Clamping kills the swing, so it sits above any
// overshoot this damping actually produces.
constexpr float springStiffness = 62.0F;
constexpr float springDamping = 2.2F;
constexpr float maxOvershoot = 1.75F;
constexpr float maxFrameSeconds = 0.05F;

// How hard the target is allowed to hit the oscillator. Overshoot and ringing
// duration are both set by the damping ratio, so trading one against the other
// there is not possible; easing the target in instead reduces the size of the
// first swing and leaves the decay alone.
constexpr float targetSmoothingSeconds = 0.18F;

constexpr float headRadius = 0.30F;
constexpr float eyeRadius = 0.075F;
constexpr float noseRadius = 0.062F;

// One face of the widget's cube: six vertices at this offset in its buffer, and
// the per-face color those vertices carry.
struct CubeFace {
    QVector3D normal;
    QVector3D color;
    const char* name;
};

const std::array<CubeFace, 6> cubeFaces{{
    {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, "front"},
    {{0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 1.0F}, "back"},
    {{-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, "left"},
    {{1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 1.0F}, "right"},
    {{0.0F, 1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, "top"},
    {{0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, "bottom"},
}};

constexpr auto jackVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

uniform mat4 mvp;
uniform mat3 normalMatrix;
out vec3 shadingNormal;
out vec3 surfaceColor;

void main()
{
    shadingNormal = normalMatrix * inNormal;
    surfaceColor = inColor;
    gl_Position = mvp * vec4(inPosition, 1.0);
}
)glsl";

// The view transform is a pure translation, so a normal rotated by the model
// matrix is already in view space and (0, 0, 1) points at the camera. That
// second term keeps the inside of the box lit whenever it is being looked into,
// which a fixed light direction alone does not.
constexpr auto jackFragmentShader = R"glsl(
#version 330 core
in vec3 shadingNormal;
in vec3 surfaceColor;
uniform vec3 baseColor;
uniform float ambient;
out vec4 fragmentColor;

void main()
{
    vec3 normal = normalize(shadingNormal);
    vec3 lightDirection = normalize(vec3(0.45, 0.8, 0.6));
    float diffuse = max(dot(normal, lightDirection), 0.0);
    float towardCamera = max(dot(normal, vec3(0.0, 0.0, 1.0)), 0.0);
    float lighting = ambient + 0.62 * diffuse + 0.30 * towardCamera;
    fragmentColor = vec4(clamp(surfaceColor * baseColor * lighting, 0.0, 1.0), 1.0);
}
)glsl";

struct JackState {
    // The context the GL objects below belong to. They exist in no other, so a
    // frame drawn by a different context must be left alone.
    QOpenGLContext* owner = nullptr;
    QOpenGLShaderProgram program;

    QOpenGLBuffer shellBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject shellArray;
    int shellVertexCount = 0;

    QOpenGLBuffer springBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer springIndexBuffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject springArray;
    int springIndexCount = 0;
    std::vector<float> springVertices;

    QOpenGLBuffer sphereBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer sphereIndexBuffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject sphereArray;
    int sphereIndexCount = 0;

    int openFace = 0;
    QQuaternion faceOrientation;
    QByteArray stashKey;

    // Copied from the host, which documents agent_context as valid for the
    // lifetime of the process. The draw hook outlives the invocation.
    void* agentContext = nullptr;
    void (*emitEvent)(void*, const char*, const char*) = nullptr;
    bool wasFullyOut = false;
    unsigned int popCount = 0;

    // Where the head actually is, as opposed to where the open side says it
    // should be. Integrated every frame.
    float extension = 0.0F;
    float extensionVelocity = 0.0F;
    float smoothedTarget = 0.0F;
    float lastElapsedSeconds = 0.0F;
};

JackState* jack = nullptr;
QMetaObject::Connection drawConnection;
QAction* toggleAction = nullptr;
int rememberedFace = 0;

float smoothstep(const float edge0, const float edge1, const float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

void pushVertex(std::vector<float>& out,
                const QVector3D& position,
                const QVector3D& normal,
                const QVector3D& color)
{
    out.insert(out.end(), {position.x(), position.y(), position.z(),
                           normal.x(), normal.y(), normal.z(),
                           color.x(), color.y(), color.z()});
}

// Corners must be given in cyclic order around the quad. Reversing the cycle
// when it winds the wrong way is what keeps every face of this mesh visible
// under the widget's GL_BACK culling without having to hand-order nine quads.
void addQuad(std::vector<float>& out,
             const QVector3D& a,
             QVector3D b,
             const QVector3D& c,
             QVector3D d,
             const QVector3D& normal,
             const QVector3D& color)
{
    if (QVector3D::dotProduct(QVector3D::crossProduct(b - a, c - a), normal) < 0.0F) {
        std::swap(b, d);
    }
    for (const QVector3D& vertex : {a, b, c, a, c, d}) {
        pushVertex(out, vertex, normal, color);
    }
}

// Two unit axes spanning the plane of a face.
void spanningAxes(const QVector3D& normal, QVector3D& u, QVector3D& v)
{
    u = std::abs(normal.z()) > 0.5F ? QVector3D(1.0F, 0.0F, 0.0F) : QVector3D(0.0F, 0.0F, 1.0F);
    v = QVector3D::crossProduct(normal, u);
}

// The interior runs to the outer surface on the open side and stops at the
// inner surface everywhere else, so the walls meet the rim instead of ending
// short of it.
float extentAlong(const QVector3D& direction, const QVector3D& openNormal)
{
    return QVector3D::dotProduct(direction, openNormal) > 0.5F ? outerHalf : innerHalf;
}

// Five inset inner walls plus a rim framing the opening.
void buildShell(std::vector<float>& out, const int openFace)
{
    const QVector3D openNormal = cubeFaces[static_cast<std::size_t>(openFace)].normal;

    for (std::size_t index = 0; index < cubeFaces.size(); ++index) {
        if (static_cast<int>(index) == openFace) {
            continue;
        }
        const CubeFace& face = cubeFaces[index];
        QVector3D u;
        QVector3D v;
        spanningAxes(face.normal, u, v);

        const QVector3D origin = face.normal * innerHalf;
        const QVector3D a = origin - u * extentAlong(-u, openNormal) - v * extentAlong(-v, openNormal);
        const QVector3D b = origin + u * extentAlong(u, openNormal) - v * extentAlong(-v, openNormal);
        const QVector3D c = origin + u * extentAlong(u, openNormal) + v * extentAlong(v, openNormal);
        const QVector3D d = origin - u * extentAlong(-u, openNormal) + v * extentAlong(v, openNormal);
        // Inward normal: this is the wall seen from inside the box.
        addQuad(out, a, b, c, d, -face.normal, face.color * interiorDim);
    }

    QVector3D u;
    QVector3D v;
    spanningAxes(openNormal, u, v);
    const QVector3D origin = openNormal * outerHalf;
    const QVector3D color = cubeFaces[static_cast<std::size_t>(openFace)].color * 0.85F;

    const auto rimQuad = [&](const float uLow, const float uHigh, const float vLow, const float vHigh) {
        addQuad(out,
                origin + u * uLow + v * vLow,
                origin + u * uHigh + v * vLow,
                origin + u * uHigh + v * vHigh,
                origin + u * uLow + v * vHigh,
                openNormal,
                color);
    };
    rimQuad(-outerHalf, outerHalf, innerHalf, outerHalf);
    rimQuad(-outerHalf, outerHalf, -outerHalf, -innerHalf);
    rimQuad(innerHalf, outerHalf, -innerHalf, innerHalf);
    rimQuad(-outerHalf, -innerHalf, -innerHalf, innerHalf);
}

// A helix swept with a circular cross-section, built along +Z. Rebuilt every
// frame because stretching the spring moves every point on it.
void buildSpring(std::vector<float>& out, const float length, const float phase)
{
    out.clear();
    const float baseZ = -innerHalf + 0.06F;
    const float turns = springCoils * 2.0F * pi;

    for (int sample = 0; sample <= springSamples; ++sample) {
        const float s = static_cast<float>(sample) / static_cast<float>(springSamples);
        const float angle = s * turns + phase;
        const QVector3D center(springRadius * std::cos(angle),
                               springRadius * std::sin(angle),
                               baseZ + s * length);

        const QVector3D tangent = QVector3D(-springRadius * std::sin(angle) * turns,
                                            springRadius * std::cos(angle) * turns,
                                            length)
                                      .normalized();
        // (frameU, frameV, tangent) is right-handed, which is what makes the
        // index winding below come out counter-clockwise seen from outside.
        const QVector3D frameU =
            QVector3D::crossProduct(tangent, QVector3D(0.0F, 0.0F, 1.0F)).normalized();
        const QVector3D frameV = QVector3D::crossProduct(tangent, frameU).normalized();

        for (int segment = 0; segment <= springRingSegments; ++segment) {
            const float theta = static_cast<float>(segment) * 2.0F * pi
                / static_cast<float>(springRingSegments);
            const QVector3D normal =
                (frameU * std::cos(theta) + frameV * std::sin(theta)).normalized();
            pushVertex(out, center + normal * tubeRadius, normal, QVector3D(1.0F, 1.0F, 1.0F));
        }
    }
}

void buildSpringIndices(std::vector<unsigned short>& indices)
{
    const int stride = springRingSegments + 1;
    for (int sample = 0; sample < springSamples; ++sample) {
        for (int segment = 0; segment < springRingSegments; ++segment) {
            const auto here = static_cast<unsigned short>(sample * stride + segment);
            const auto next = static_cast<unsigned short>(here + stride);
            indices.insert(indices.end(), {
                here, static_cast<unsigned short>(here + 1),
                static_cast<unsigned short>(next + 1),
                here, static_cast<unsigned short>(next + 1), next,
            });
        }
    }
}

void buildSphere(std::vector<float>& vertices, std::vector<unsigned short>& indices)
{
    constexpr int latitudeBands = 18;
    constexpr int longitudeBands = 26;
    for (int lat = 0; lat <= latitudeBands; ++lat) {
        const float theta = static_cast<float>(lat) * pi / static_cast<float>(latitudeBands);
        for (int lon = 0; lon <= longitudeBands; ++lon) {
            const float phi =
                static_cast<float>(lon) * 2.0F * pi / static_cast<float>(longitudeBands);
            const QVector3D point(std::cos(phi) * std::sin(theta),
                                  std::cos(theta),
                                  std::sin(phi) * std::sin(theta));
            pushVertex(vertices, point, point, QVector3D(1.0F, 1.0F, 1.0F));
        }
    }
    for (int lat = 0; lat < latitudeBands; ++lat) {
        for (int lon = 0; lon < longitudeBands; ++lon) {
            const auto first = static_cast<unsigned short>(lat * (longitudeBands + 1) + lon);
            const auto second = static_cast<unsigned short>(first + longitudeBands + 1);
            indices.insert(indices.end(), {
                first, static_cast<unsigned short>(first + 1), second,
                second, static_cast<unsigned short>(first + 1),
                static_cast<unsigned short>(second + 1),
            });
        }
    }
}

void drawJackInTheBox(CubeWidget* cube)
{
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (jack == nullptr || context == nullptr || context != jack->owner) {
        return;
    }
    QOpenGLFunctions* functions = context->functions();

    // The same model and view paintGL() just used, read from the widget's own
    // private state so the interior tracks the cube exactly.
    QMatrix4x4 rotation;
    rotation.rotate(cube->m_angleDegrees, QVector3D(0.72F, 1.0F, 0.31F));

    QMatrix4x4 model = rotation;
    model.scale(cube->m_scale);

    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, -viewDistance);
    const QMatrix4x4 viewProjection = cube->m_projection * view;

    // How squarely the open side faces the camera, which is what drives the pop.
    const QVector3D openNormal = cubeFaces[static_cast<std::size_t>(jack->openFace)].normal;
    const QVector3D faceNormal = rotation.mapVector(openNormal).normalized();
    const QVector3D faceCenter = QVector3D(0.0F, 0.0F, -viewDistance)
        + rotation.mapVector(openNormal * outerHalf) * cube->m_scale;
    const float facing = QVector3D::dotProduct(faceNormal, (-faceCenter).normalized());
    const float pop = smoothstep(0.12F, 0.62F, facing);

    jack->program.bind();
    jack->program.setUniformValue("normalMatrix", model.normalMatrix());

    // The interior carries the widget's current tint, so it stays part of the
    // same object while a patch animates the cube's color.
    jack->program.setUniformValue("mvp", viewProjection * model);
    jack->program.setUniformValue("baseColor", cube->m_tint);
    jack->program.setUniformValue("ambient", 0.30F);
    jack->shellArray.bind();
    functions->glDrawArrays(GL_TRIANGLES, 0, jack->shellVertexCount);
    jack->shellArray.release();

    QMatrix4x4 jackModel = model;
    jackModel.rotate(jack->faceOrientation);
    const QMatrix4x4 jackNormalSource = jackModel;

    // Integrate the head toward the target instead of snapping to it, so it
    // overshoots the opening and rebounds a few times on the way to rest.
    const float frameSeconds =
        std::clamp(cube->m_elapsedSeconds - jack->lastElapsedSeconds, 0.0F, maxFrameSeconds);
    jack->lastElapsedSeconds = cube->m_elapsedSeconds;
    jack->smoothedTarget +=
        (pop - jack->smoothedTarget) * (1.0F - std::exp(-frameSeconds / targetSmoothingSeconds));
    const float acceleration = (jack->smoothedTarget - jack->extension) * springStiffness
        - jack->extensionVelocity * springDamping;
    jack->extensionVelocity += acceleration * frameSeconds;
    jack->extension += jack->extensionVelocity * frameSeconds;
    // Hitting the bottom of the box ends the motion rather than reflecting it.
    if (jack->extension < 0.0F) {
        jack->extension = 0.0F;
        jack->extensionVelocity = std::max(jack->extensionVelocity, 0.0F);
    } else if (jack->extension > maxOvershoot) {
        jack->extension = maxOvershoot;
        jack->extensionVelocity = std::min(jack->extensionVelocity, 0.0F);
    }

    // The coil twists as it travels and holds still once it stops travelling.
    // Driving the phase from elapsed time instead would keep it turning on the
    // spot at full stretch, which reads as a spinning prop rather than a spring.
    const float length = retractedLength + popTravel * jack->extension;
    buildSpring(jack->springVertices, length, jack->extension * springTwistTurns * 2.0F * pi);
    jack->springBuffer.bind();
    jack->springBuffer.write(0, jack->springVertices.data(),
                             static_cast<int>(jack->springVertices.size() * sizeof(float)));
    jack->springBuffer.release();

    jack->program.setUniformValue("mvp", viewProjection * jackModel);
    jack->program.setUniformValue("normalMatrix", jackNormalSource.normalMatrix());
    jack->program.setUniformValue("baseColor", QVector3D(0.78F, 0.81F, 0.88F));
    jack->program.setUniformValue("ambient", 0.24F);
    jack->springArray.bind();
    functions->glDrawElements(GL_TRIANGLES, jack->springIndexCount, GL_UNSIGNED_SHORT, nullptr);
    jack->springArray.release();

    // The head lags whatever the spring under it is doing, so its tilt comes
    // from that same motion: it leans through the bounce and is exactly still
    // once the spring is.
    const float headZ = -innerHalf + 0.06F + length + headRadius * 0.72F;
    const float wobble = std::clamp(jack->extensionVelocity * 6.5F, -16.0F, 16.0F);

    QMatrix4x4 headFrame = jackModel;
    headFrame.translate(0.0F, 0.0F, headZ);
    headFrame.rotate(wobble, QVector3D(0.0F, 1.0F, 0.0F));

    jack->sphereArray.bind();
    const auto drawSphere =
        [&](const QVector3D& offset, const float radius, const QVector3D& color, const float ambient) {
            QMatrix4x4 sphereModel = headFrame;
            sphereModel.translate(offset);
            sphereModel.scale(radius);
            jack->program.setUniformValue("mvp", viewProjection * sphereModel);
            jack->program.setUniformValue("normalMatrix", sphereModel.normalMatrix());
            jack->program.setUniformValue("baseColor", color);
            jack->program.setUniformValue("ambient", ambient);
            functions->glDrawElements(
                GL_TRIANGLES, jack->sphereIndexCount, GL_UNSIGNED_SHORT, nullptr);
        };

    drawSphere({0.0F, 0.0F, 0.0F}, headRadius, QVector3D(1.0F, 0.78F, 0.30F), 0.26F);
    drawSphere(QVector3D(-0.40F, 0.26F, 0.80F) * headRadius, eyeRadius,
               QVector3D(0.06F, 0.06F, 0.09F), 0.55F);
    drawSphere(QVector3D(0.40F, 0.26F, 0.80F) * headRadius, eyeRadius,
               QVector3D(0.06F, 0.06F, 0.09F), 0.55F);
    drawSphere(QVector3D(0.0F, -0.06F, 0.92F) * headRadius, noseRadius,
               QVector3D(0.95F, 0.24F, 0.26F), 0.34F);
    jack->sphereArray.release();

    jack->program.release();

    // Report the transition rather than the value, so this stays a rare event
    // and not a per-frame stream.
    // Reported off the head's own position, so the event marks it arriving
    // rather than the side merely coming around.
    const bool fullyOut = jack->extension > 0.97F;
    if (fullyOut && !jack->wasFullyOut && jack->emitEvent != nullptr) {
        ++jack->popCount;
        const QJsonObject data{
            {QStringLiteral("side"),
             QString::fromLatin1(cubeFaces[static_cast<std::size_t>(jack->openFace)].name)},
            {QStringLiteral("frameIndex"), static_cast<qint64>(cube->m_frameIndex)},
            {QStringLiteral("pops"), static_cast<int>(jack->popCount)},
        };
        jack->emitEvent(jack->agentContext, "jack.popped",
                        QJsonDocument(data).toJson(QJsonDocument::Compact).constData());
    }
    jack->wasFullyOut = fullyOut;
}

// Needs the widget's OpenGL context to be current.
bool installJack(CubeWidget* cube, const int openFace, const RuntimeAgentHostV1* host, QString& error)
{
    // Both mesh replacements collapse all 36 of the widget's vertices and save
    // them, so either one holding the buffer has to let go before this reads it.
    scene_toggle::turnOff(cube, QStringLiteral("actionCatmullClark"));
    scene_toggle::turnOff(cube, QStringLiteral("actionPillowMode"));

    auto* state = new JackState();
    state->owner = QOpenGLContext::currentContext();
    state->openFace = openFace;
    state->faceOrientation = QQuaternion::rotationTo(
        QVector3D(0.0F, 0.0F, 1.0F), cubeFaces[static_cast<std::size_t>(openFace)].normal);
    state->agentContext = host->agent_context;
    state->emitEvent = host->emit_event_json;

    if (!state->program.addShaderFromSourceCode(QOpenGLShader::Vertex, jackVertexShader)
        || !state->program.addShaderFromSourceCode(QOpenGLShader::Fragment, jackFragmentShader)
        || !state->program.link()) {
        error = state->program.log();
        delete state;
        return false;
    }

    const auto describeAttributes = [&state] {
        state->program.bind();
        state->program.enableAttributeArray(0);
        state->program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 9 * sizeof(float));
        state->program.enableAttributeArray(1);
        state->program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 9 * sizeof(float));
        state->program.enableAttributeArray(2);
        state->program.setAttributeBuffer(2, GL_FLOAT, 6 * sizeof(float), 3, 9 * sizeof(float));
        state->program.release();
    };

    std::vector<float> shellVertices;
    buildShell(shellVertices, openFace);
    state->shellVertexCount = static_cast<int>(shellVertices.size() / 9);
    state->shellArray.create();
    state->shellArray.bind();
    state->shellBuffer.create();
    state->shellBuffer.bind();
    state->shellBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    state->shellBuffer.allocate(shellVertices.data(),
                                static_cast<int>(shellVertices.size() * sizeof(float)));
    describeAttributes();
    state->shellBuffer.release();
    state->shellArray.release();

    buildSpring(state->springVertices, retractedLength, 0.0F);
    std::vector<unsigned short> springIndices;
    buildSpringIndices(springIndices);
    state->springIndexCount = static_cast<int>(springIndices.size());
    state->springArray.create();
    state->springArray.bind();
    state->springBuffer.create();
    state->springBuffer.bind();
    state->springBuffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    state->springBuffer.allocate(state->springVertices.data(),
                                 static_cast<int>(state->springVertices.size() * sizeof(float)));
    state->springIndexBuffer.create();
    state->springIndexBuffer.bind();
    state->springIndexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    state->springIndexBuffer.allocate(
        springIndices.data(), static_cast<int>(springIndices.size() * sizeof(unsigned short)));
    describeAttributes();
    state->springBuffer.release();
    state->springArray.release();

    std::vector<float> sphereVertices;
    std::vector<unsigned short> sphereIndices;
    buildSphere(sphereVertices, sphereIndices);
    state->sphereIndexCount = static_cast<int>(sphereIndices.size());
    state->sphereArray.create();
    state->sphereArray.bind();
    state->sphereBuffer.create();
    state->sphereBuffer.bind();
    state->sphereBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    state->sphereBuffer.allocate(sphereVertices.data(),
                                 static_cast<int>(sphereVertices.size() * sizeof(float)));
    state->sphereIndexBuffer.create();
    state->sphereIndexBuffer.bind();
    state->sphereIndexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    state->sphereIndexBuffer.allocate(
        sphereIndices.data(), static_cast<int>(sphereIndices.size() * sizeof(unsigned short)));
    describeAttributes();
    state->sphereBuffer.release();
    state->sphereArray.release();

    const auto destroyBuffers = [&state] {
        state->shellArray.destroy();
        state->shellBuffer.destroy();
        state->springArray.destroy();
        state->springBuffer.destroy();
        state->springIndexBuffer.destroy();
        state->sphereArray.destroy();
        state->sphereBuffer.destroy();
        state->sphereIndexBuffer.destroy();
    };

    // Keep the chosen face's own six vertices so it can come back, then collapse
    // them to a point. paintGL()'s 36-vertex draw still runs; those two
    // triangles just cover no pixels.
    const int byteOffset = openFace * cubeFloatsPerFace * static_cast<int>(sizeof(float));
    std::vector<float> savedFace(cubeFloatsPerFace);
    cube->m_vertexBuffer.bind();
    const bool readBack = cube->m_vertexBuffer.read(
        byteOffset, savedFace.data(), cubeFloatsPerFace * static_cast<int>(sizeof(float)));
    // All zeros means another mesh replacement already collapsed the cube and
    // holds the only copy of the originals. Saving these would lose the face on
    // restore, so refuse rather than nest. This is also the check that makes the
    // stash deposit below safe, which is why the deposit sits behind it rather
    // than carrying a verification of its own.
    const bool alreadySuppressed = readBack && cube_mesh::anyFaceCollapsed(savedFace);
    if (!readBack || alreadySuppressed) {
        cube->m_vertexBuffer.release();
        destroyBuffers();
        delete state;
        error = readBack
            ? QStringLiteral("this face is already collapsed, so another replacement owns "
                             "its vertices; saving its zeros as the originals would lose the "
                             "face on restore")
            : QStringLiteral("could not read the widget's vertex buffer; refusing to overwrite it");
        return false;
    }
    // The stash holds the only copy, and this module's own restore reads it
    // back from there. Keeping a private duplicate as well would make the bytes
    // other code depends on a copy nobody here exercises, and a saved copy
    // nobody reads is one nobody notices going wrong.
    //
    // The entry means "what the displacement currently in effect displaced",
    // not "the oldest value ever seen", so an install overwrites it. Replaying
    // a first-ever copy over a later legitimate edit would revert that edit
    // silently. Nothing verifies the bytes at this call — the guard above
    // already did, and there is no path to here that skipped it.
    // No null check on the stash callbacks. A host too old to have them presents
    // a shorter struct, so those fields would be whatever sits after it in
    // memory — a test that reads past the end of the struct and passes on
    // garbage. The version check at the top of run() is what rules that host
    // out, and the loader refuses the module before it ever gets here.
    state->stashKey = QStringLiteral("jack_in_the_box/face-%1")
        .arg(QString::fromLatin1(cubeFaces[static_cast<std::size_t>(openFace)].name))
        .toUtf8();
    host->stash_put(host->agent_context,
                    state->stashKey.constData(),
                    savedFace.data(),
                    cubeFloatsPerFace * static_cast<std::int64_t>(sizeof(float)),
                    1);

    const std::vector<float> collapsed(cubeFloatsPerFace, 0.0F);
    cube->m_vertexBuffer.write(byteOffset, collapsed.data(),
                               cubeFloatsPerFace * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();

    jack = state;
    drawConnection = QObject::connect(
        cube, &CubeWidget::frameRendered, cube,
        [cube](qulonglong, float) { drawJackInTheBox(cube); },
        Qt::DirectConnection);
    cube->update();
    return true;
}

// Needs the widget's OpenGL context to be current. The note says what happened
// to the face, which is not always "put back".
void removeJack(CubeWidget* cube, const RuntimeAgentHostV1* host, QString& note)
{
    QObject::disconnect(drawConnection);

    const int byteOffset = jack->openFace * cubeFloatsPerFace * static_cast<int>(sizeof(float));
    const auto faceBytes = cubeFloatsPerFace * static_cast<std::int64_t>(sizeof(float));

    std::vector<float> savedFace(cubeFloatsPerFace);
    const std::int64_t stashed = host->stash_get(host->agent_context,
                                                 jack->stashKey.constData(),
                                                 savedFace.data(),
                                                 faceBytes);

    // Replay only onto the displacement this entry describes. If the face is no
    // longer the zeros this module wrote, something else has legitimately
    // changed it since, and writing the saved bytes over that would revert
    // someone else's work. This is the install guard again, run at the other
    // end: it is what makes an entry that persists safe to keep around.
    std::vector<float> current(cubeFloatsPerFace);
    cube->m_vertexBuffer.bind();
    const bool readBack = cube->m_vertexBuffer.read(byteOffset, current.data(),
                                                    static_cast<int>(faceBytes));
    const bool stillCollapsed = readBack && cube_mesh::anyFaceCollapsed(current);

    if (stashed != faceBytes) {
        note = QStringLiteral("the face was not restored: its stash entry is gone or the "
                              "wrong size, so the original vertices are no longer anywhere");
    } else if (!stillCollapsed) {
        note = QStringLiteral("the face was left alone: it no longer holds the zeros this "
                              "module wrote, so something else has changed it since");
    } else {
        cube->m_vertexBuffer.write(byteOffset, savedFace.data(), static_cast<int>(faceBytes));
        note = QStringLiteral("the sixth face is back");
    }
    cube->m_vertexBuffer.release();

    jack->shellArray.destroy();
    jack->shellBuffer.destroy();
    jack->springArray.destroy();
    jack->springBuffer.destroy();
    jack->springIndexBuffer.destroy();
    jack->sphereArray.destroy();
    jack->sphereBuffer.destroy();
    jack->sphereIndexBuffer.destroy();
    delete jack;
    jack = nullptr;
    cube->update();
}

void syncToggleAction()
{
    if (toggleAction != nullptr) {
        const QSignalBlocker blocker(toggleAction);
        toggleAction->setChecked(jack != nullptr);
    }
}

// Returns false only when an install was asked for and failed. On removal the
// message carries what became of the face rather than an error.
bool setJackEnabled(CubeWidget* cube,
                    const bool enabled,
                    const RuntimeAgentHostV1* host,
                    QString& message)
{
    if (enabled == (jack != nullptr)) {
        return true;
    }
    bool installed = true;
    if (enabled) {
        installed = installJack(cube, rememberedFace, host, message);
    } else {
        removeJack(cube, host, message);
    }
    syncToggleAction();
    return installed;
}

void ensureToggleAction(CubeWidget* cube, const RuntimeAgentHostV1* host)
{
    if (toggleAction != nullptr) {
        return;
    }
    // Keep the whole host by value rather than picking fields out of it. The
    // callbacks are the host's own functions and agent_context is documented as
    // valid for the life of the process; only invocation_context belongs to the
    // call that is ending, so that is the one field cleared. Rebuilding a
    // partial struct here instead would leave the newer callbacks null while
    // struct_size claimed they were there.
    RuntimeAgentHostV1 menuHost = *host;
    menuHost.invocation_context = nullptr;

    toggleAction = scene_toggle::install(
        cube,
        QStringLiteral("actionJackInTheBox"),
        QStringLiteral("&Jack in the box"),
        QStringLiteral("Ctrl+Shift+J"),
        [cube, menuHost](const bool enabled) {
            // The menu runs on the GUI thread with no current context, and both
            // installing and removing need one, so the work waits for a frame.
            // A caller that already has a context current gets it immediately.
            if (scene_toggle::ownContextIsCurrent(cube)) {
                QString ignored;
                setJackEnabled(cube, enabled, &menuHost, ignored);
                return;
            }
            cube->enqueueRenderCallback([cube, enabled, menuHost] {
                QString deferred;
                setJackEnabled(cube, enabled, &menuHost, deferred);
            });
        });
    syncToggleAction();
}

int faceIndexFromName(const QString& name, bool& recognized)
{
    recognized = true;
    for (std::size_t index = 0; index < cubeFaces.size(); ++index) {
        if (name.compare(QString::fromLatin1(cubeFaces[index].name), Qt::CaseInsensitive) == 0) {
            return static_cast<int>(index);
        }
    }
    recognized = false;
    return 0;
}

// Takes the opening and everything in it back off, putting the widget's own
// face vertices back. Declared in the descriptor so a caller can ask whether
// this module can be released, and get an error rather than silence when it
// cannot.
//
// Destroying the GL objects needs this widget's context current, which is only
// true inside its paint callbacks. Deferring the work to the next frame would
// let this report completion before the face is back, and a handover sequenced
// on that completion would hand the next generation the zeroed vertices. So
// this refuses instead of deferring.
void release(const RuntimeAgentHostV1* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI_V1) {
        return;
    }
    auto* cube = static_cast<CubeWidget*>(
        host->find_qobject(host->agent_context, "cubeView"));
    if (cube == nullptr) {
        host->fail(host->invocation_context, "cubeView was not found");
        return;
    }
    if (jack == nullptr) {
        host->complete_json(host->invocation_context,
                            "{\"removed\":false,\"note\":\"nothing was installed\"}");
        return;
    }
    if (!scene_toggle::ownContextIsCurrent(cube)) {
        host->fail(host->invocation_context,
                   "the widget's OpenGL context is not current; release this module with "
                   "executor=render so the face is back before this reports completion");
        return;
    }

    QString note;
    setJackEnabled(cube, false, host, note);
    const QJsonObject result{
        {QStringLiteral("removed"), true},
        {QStringLiteral("note"), note},
    };
    host->complete_json(host->invocation_context,
                        QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}

void run(const RuntimeAgentHostV1* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI_V1) {
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

    const QJsonObject request =
        QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();
    ensureToggleAction(cube, host);

    const auto complete = [host](QJsonObject result) {
        result.insert(QStringLiteral("menuToggle"), toggleAction != nullptr);
        host->complete_json(host->invocation_context,
                            QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    };

    // restore always takes it off; toggle flips whichever way it currently is.
    const bool takeOff = request.value(QStringLiteral("restore")).toBool()
        || request.value(QStringLiteral("remove")).toBool()
        || (request.value(QStringLiteral("toggle")).toBool() && jack != nullptr);
    if (takeOff) {
        if (jack == nullptr) {
            complete(QJsonObject{
                {QStringLiteral("removed"), false},
                {QStringLiteral("note"), QStringLiteral("no open side was installed")},
            });
            return;
        }
        QString note;
        setJackEnabled(cube, false, host, note);
        complete(QJsonObject{
            {QStringLiteral("removed"), true},
            {QStringLiteral("note"), note},
        });
        return;
    }

    int requestedFace = rememberedFace;
    if (request.contains(QStringLiteral("side"))) {
        bool recognized = false;
        requestedFace = faceIndexFromName(request.value(QStringLiteral("side")).toString(),
                                          recognized);
        if (!recognized) {
            host->fail(host->invocation_context,
                       "side must be one of front, back, left, right, top, bottom");
            return;
        }
    }

    // Moving the opening means putting the old face back first, so a re-run with
    // a different side does not leave two holes.
    if (jack != nullptr && requestedFace != jack->openFace) {
        QString note;
        setJackEnabled(cube, false, host, note);
    }
    rememberedFace = requestedFace;

    if (jack != nullptr) {
        complete(QJsonObject{
            {QStringLiteral("installed"), true},
            {QStringLiteral("side"),
             QString::fromLatin1(cubeFaces[static_cast<std::size_t>(jack->openFace)].name)},
            {QStringLiteral("note"), QStringLiteral("the jack in the box was already installed")},
        });
        return;
    }

    QString error;
    if (!setJackEnabled(cube, true, host, error)) {
        host->fail(host->invocation_context, error.toLocal8Bit().constData());
        return;
    }

    complete(QJsonObject{
        {QStringLiteral("installed"), true},
        {QStringLiteral("connectionValid"), static_cast<bool>(drawConnection)},
        {QStringLiteral("side"),
         QString::fromLatin1(cubeFaces[static_cast<std::size_t>(jack->openFace)].name)},
        {QStringLiteral("hollowedVertices"), 6},
        {QStringLiteral("interiorTriangles"), jack->shellVertexCount / 3},
        {QStringLiteral("springTriangles"), jack->springIndexCount / 3},
        {QStringLiteral("headTriangles"), jack->sphereIndexCount / 3},
        {QStringLiteral("event"), QStringLiteral("jack.popped")},
    });
}

const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1,
    sizeof(RuntimeAgentSnippetV1),
    "hollow one side of the cube and hide a jack in the box in it",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1()
{
    return &descriptor;
}
