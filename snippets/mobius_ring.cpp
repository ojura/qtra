// A rotating Mobius strip encircling the scene: translucent surface, opaque
// rim.
//
// Installed as a third frameRendered() hook, so it draws after the cube and the
// orbiting sphere have written depth. That ordering is what makes the blend
// correct: the opaque things are already in the depth buffer when the
// translucent surface goes down.
//
// The strip has exactly one boundary curve, so the opaque rim is a single band
// running over u in [0, 4pi), not two separate rims. The rim is real
// geometry, not GL_LINE_LOOP: this is a 3.3 core context, where line widths
// above 1 are deprecated and clamp to 1 whatever GL_ALIASED_LINE_WIDTH_RANGE
// claims.
//
// Loading the snippet also adds a checkable "Mobius ring" entry to the Cube
// menu, so the strip can be switched from the GUI as well as over the socket.
//
// Request: {"radius","width","tilt","spin","alpha","edgeInset"} to (re)build,
// {"toggle": true} to flip it, {"restore": true} to remove it.

#include "agent/agent_abi.h"
#include "cube_widget.h"
#include "scene_toggle.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QMatrix4x4>
#include <QMetaObject>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLVertexArrayObject>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr float pi = 3.14159265358979323846F;

constexpr int longitudeSteps = 256;
constexpr int widthSteps = 4;

constexpr float defaultRadius = 2.0F;
constexpr float defaultWidth = 0.35F;
constexpr float defaultTiltDegrees = 60.0F;
constexpr float defaultSpinDegreesPerSecond = 22.0F;
constexpr float defaultAlpha = 0.7F;
constexpr float defaultEdgeInset = 0.05F;

const QVector3D surfaceColor{0.62F, 0.55F, 0.98F};
const QVector3D edgeColor{0.90F, 0.87F, 1.0F};

constexpr auto ringVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

uniform mat4 mvp;
uniform mat3 normalMatrix;
out vec3 shadingNormal;

void main()
{
    shadingNormal = normalMatrix * inNormal;
    gl_Position = mvp * vec4(inPosition, 1.0);
}
)glsl";

// The strip is non-orientable, so there is no consistent front face to light.
// abs() on the diffuse term shades both sides the same way and avoids a black
// half where the normal happens to point away.
constexpr auto ringFragmentShader = R"glsl(
#version 330 core
in vec3 shadingNormal;
uniform vec4 color;
out vec4 fragmentColor;

void main()
{
    vec3 lightDirection = normalize(vec3(0.45, 0.8, 0.6));
    float diffuse = abs(dot(normalize(shadingNormal), lightDirection));
    fragmentColor = vec4(color.rgb * (0.30 + 0.70 * diffuse), color.a);
}
)glsl";

struct Band {
    QOpenGLBuffer vertices{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer indices{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject array;
    int indexCount = 0;
};

struct RingState {
    // The context the GL objects below belong to. They exist in no other, so a
    // frame drawn by a different context must be left alone.
    QOpenGLContext* owner = nullptr;
    QOpenGLShaderProgram program;
    Band surface;
    Band edge;
    QOpenGLFunctions_3_3_Core* gl = nullptr;
    float radius = defaultRadius;
    float width = defaultWidth;
    float tiltDegrees = defaultTiltDegrees;
    float spinDegreesPerSecond = defaultSpinDegreesPerSecond;
    float alpha = defaultAlpha;
    float edgeInset = defaultEdgeInset;
};

RingState* ring = nullptr;
QMetaObject::Connection drawConnection;
QAction* toggleAction = nullptr;

// The six shape values, kept outside RingState so that switching the strip off
// and on again brings back the one that was last tuned.
struct RingParams {
    float radius = defaultRadius;
    float width = defaultWidth;
    float tiltDegrees = defaultTiltDegrees;
    float spinDegreesPerSecond = defaultSpinDegreesPerSecond;
    float alpha = defaultAlpha;
    float edgeInset = defaultEdgeInset;
};
RingParams rememberedParams;

// P(u, v) = ((R + v cos(u/2)) cos u, (R + v cos(u/2)) sin u, v sin(u/2))
QVector3D mobiusPoint(const float radius, const float u, const float v)
{
    const float rail = radius + v * std::cos(u / 2.0F);
    return {rail * std::cos(u), rail * std::sin(u), v * std::sin(u / 2.0F)};
}

// Analytic normal from the cross product of the two parameter derivatives.
// Taking it from the formula instead of accumulating face normals means the
// seam at u = 2pi matches exactly, where welded vertices would disagree by a
// sign.
QVector3D mobiusNormal(const float radius, const float u, const float v)
{
    const float halfU = u / 2.0F;
    const float rail = radius + v * std::cos(halfU);
    const float railDerivative = -v / 2.0F * std::sin(halfU);

    const QVector3D dU{
        railDerivative * std::cos(u) - rail * std::sin(u),
        railDerivative * std::sin(u) + rail * std::cos(u),
        v / 2.0F * std::cos(halfU),
    };
    const QVector3D dV{
        std::cos(halfU) * std::cos(u),
        std::cos(halfU) * std::sin(u),
        std::sin(halfU),
    };
    return QVector3D::crossProduct(dU, dV).normalized();
}

void buildBand(const float radius,
               const float uSpan,
               const int uSteps,
               const float vFrom,
               const float vTo,
               const int vSteps,
               std::vector<float>& vertices,
               std::vector<unsigned int>& indices)
{
    for (int i = 0; i <= uSteps; ++i) {
        const float u = uSpan * static_cast<float>(i) / static_cast<float>(uSteps);
        for (int j = 0; j <= vSteps; ++j) {
            const float v = vFrom
                + (vTo - vFrom) * static_cast<float>(j) / static_cast<float>(vSteps);
            const QVector3D point = mobiusPoint(radius, u, v);
            const QVector3D normal = mobiusNormal(radius, u, v);
            vertices.insert(vertices.end(), {
                point.x(), point.y(), point.z(),
                normal.x(), normal.y(), normal.z(),
            });
        }
    }
    for (int i = 0; i < uSteps; ++i) {
        for (int j = 0; j < vSteps; ++j) {
            const auto corner = static_cast<unsigned int>(i * (vSteps + 1) + j);
            const auto next = static_cast<unsigned int>(corner + vSteps + 1);
            indices.insert(indices.end(),
                           {corner, next, corner + 1, corner + 1, next, next + 1});
        }
    }
}

void uploadBand(RingState* state,
                Band& band,
                const std::vector<float>& vertices,
                const std::vector<unsigned int>& indices)
{
    band.array.bind();
    band.vertices.bind();
    band.vertices.allocate(vertices.data(), static_cast<int>(vertices.size() * sizeof(float)));
    band.indices.bind();
    band.indices.allocate(indices.data(),
                          static_cast<int>(indices.size() * sizeof(unsigned int)));

    constexpr int stride = 6 * sizeof(float);
    state->program.bind();
    state->program.enableAttributeArray(0);
    state->program.setAttributeBuffer(0, GL_FLOAT, 0, 3, stride);
    state->program.enableAttributeArray(1);
    state->program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, stride);
    state->program.release();

    band.array.release();
    band.vertices.release();
    band.indexCount = static_cast<int>(indices.size());
}

void uploadGeometry(RingState* state)
{
    const float inset = std::min(state->edgeInset, state->width * 0.45F);
    const float interiorHalfWidth = state->width - inset;

    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    buildBand(state->radius, 2.0F * pi, longitudeSteps,
              -interiorHalfWidth, interiorHalfWidth, widthSteps, vertices, indices);
    uploadBand(state, state->surface, vertices, indices);

    // Two laps of u walk the near rim and then the far one, because they are
    // the same curve. The band butts against the interior and does not overlap
    // it, so there is nothing to z-fight.
    std::vector<float> edgeVertices;
    std::vector<unsigned int> edgeIndices;
    buildBand(state->radius, 4.0F * pi, 2 * longitudeSteps,
              interiorHalfWidth, state->width, 1, edgeVertices, edgeIndices);
    uploadBand(state, state->edge, edgeVertices, edgeIndices);
}

void drawRing(const CubeWidget* cube)
{
    if (ring == nullptr || ring->gl == nullptr
        || QOpenGLContext::currentContext() != ring->owner) {
        return;
    }
    QOpenGLFunctions_3_3_Core* gl = ring->gl;

    QMatrix4x4 model;
    model.rotate(ring->tiltDegrees, QVector3D(1.0F, 0.0F, 0.0F));
    model.rotate(cube->m_elapsedSeconds * ring->spinDegreesPerSecond,
                 QVector3D(0.0F, 0.0F, 1.0F));

    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, -cubeViewDistance);

    ring->program.bind();
    ring->program.setUniformValue("mvp", cube->m_projection * view * model);
    ring->program.setUniformValue("normalMatrix", model.normalMatrix());

    // The strip has no outside, so back-face culling would delete half of it.
    gl->glDisable(GL_CULL_FACE);

    // Rim first, opaque and writing depth, so it occludes the translucent
    // interior behind it. If the interior went first with depth writes off, the
    // far rim would pass the depth test and draw over the near interior.
    ring->program.setUniformValue("color", QVector4D(edgeColor, 1.0F));
    ring->edge.array.bind();
    gl->glDrawElements(GL_TRIANGLES, ring->edge.indexCount, GL_UNSIGNED_INT, nullptr);
    ring->edge.array.release();

    // Translucent interior: depth-tested against everything already drawn, but
    // not writing depth, so the strip's own two halves blend instead of
    // occluding each other by draw order.
    gl->glEnable(GL_BLEND);
    // Leave the destination alpha alone. QOpenGLWidget composites its FBO, and
    // a framebuffer alpha below 1 would make the whole widget translucent.
    gl->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    gl->glDepthMask(GL_FALSE);

    ring->program.setUniformValue("color", QVector4D(surfaceColor, ring->alpha));
    ring->surface.array.bind();
    gl->glDrawElements(GL_TRIANGLES, ring->surface.indexCount, GL_UNSIGNED_INT, nullptr);
    ring->surface.array.release();

    gl->glDepthMask(GL_TRUE);
    gl->glDisable(GL_BLEND);
    gl->glEnable(GL_CULL_FACE);
    ring->program.release();
}

void destroyBand(Band& band)
{
    band.array.destroy();
    band.vertices.destroy();
    band.indices.destroy();
}

// Needs the widget's OpenGL context to be current.
bool installRing(CubeWidget* cube, QString& error)
{
    auto* state = new RingState();
    state->owner = QOpenGLContext::currentContext();
    state->gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(state->owner);
    if (state->gl == nullptr) {
        delete state;
        error = QStringLiteral("no OpenGL 3.3 core function table for this context");
        return false;
    }
    state->gl->initializeOpenGLFunctions();

    if (!state->program.addShaderFromSourceCode(QOpenGLShader::Vertex, ringVertexShader)
        || !state->program.addShaderFromSourceCode(QOpenGLShader::Fragment, ringFragmentShader)
        || !state->program.link()) {
        error = state->program.log();
        delete state;
        return false;
    }

    state->radius = rememberedParams.radius;
    state->width = rememberedParams.width;
    state->tiltDegrees = rememberedParams.tiltDegrees;
    state->spinDegreesPerSecond = rememberedParams.spinDegreesPerSecond;
    state->alpha = rememberedParams.alpha;
    state->edgeInset = rememberedParams.edgeInset;

    state->surface.array.create();
    state->surface.vertices.create();
    state->surface.indices.create();
    state->edge.array.create();
    state->edge.vertices.create();
    state->edge.indices.create();
    uploadGeometry(state);

    ring = state;
    drawConnection = QObject::connect(
        cube, &CubeWidget::frameRendered, cube,
        [cube](qulonglong, float) { drawRing(cube); },
        Qt::DirectConnection);
    cube->update();
    return true;
}

// Needs the widget's OpenGL context to be current.
void removeRing(CubeWidget* cube)
{
    QObject::disconnect(drawConnection);
    destroyBand(ring->surface);
    destroyBand(ring->edge);
    delete ring;
    ring = nullptr;
    cube->update();
}

void syncToggleAction()
{
    if (toggleAction != nullptr) {
        const QSignalBlocker blocker(toggleAction);
        toggleAction->setChecked(ring != nullptr);
    }
}

// Returns false only when an install was asked for and failed.
bool setRingEnabled(CubeWidget* cube, const bool enabled, QString& error)
{
    if (enabled == (ring != nullptr)) {
        return true;
    }
    bool installed = true;
    if (enabled) {
        installed = installRing(cube, error);
    } else {
        removeRing(cube);
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
        cube,
        QStringLiteral("actionMobiusRing"),
        QStringLiteral("&Mobius ring"),
        QStringLiteral("Ctrl+Shift+M"),
        [cube](const bool enabled) {
            // The menu runs on the GUI thread with no current context, and both
            // installing and removing need one, so the work waits for a frame.
            // A caller that already has a context current gets it immediately.
            if (scene_toggle::ownContextIsCurrent(cube)) {
                QString ignored;
                setRingEnabled(cube, enabled, ignored);
                return;
            }
            cube->enqueueRenderCallback([cube, enabled] {
                QString deferred;
                setRingEnabled(cube, enabled, deferred);
            });
        });
    syncToggleAction();
}

void applyRequest(RingState* state, const QJsonObject& request)
{
    const auto number = [&request](const QString& key, const float fallback) {
        return static_cast<float>(request.value(key).toDouble(static_cast<double>(fallback)));
    };
    state->radius = std::clamp(number(QStringLiteral("radius"), state->radius), 0.5F, 12.0F);
    state->width = std::clamp(number(QStringLiteral("width"), state->width), 0.02F, 3.0F);
    state->tiltDegrees = number(QStringLiteral("tilt"), state->tiltDegrees);
    state->spinDegreesPerSecond = number(QStringLiteral("spin"), state->spinDegreesPerSecond);
    state->alpha = std::clamp(number(QStringLiteral("alpha"), state->alpha), 0.0F, 1.0F);
    state->edgeInset = std::clamp(
        number(QStringLiteral("edgeInset"), state->edgeInset), 0.002F, 1.0F);

    rememberedParams = RingParams{
        state->radius, state->width, state->tiltDegrees,
        state->spinDegreesPerSecond, state->alpha, state->edgeInset,
    };
}

QJsonObject describe(const RingState* state)
{
    return QJsonObject{
        {QStringLiteral("radius"), state->radius},
        {QStringLiteral("width"), state->width},
        {QStringLiteral("tilt"), state->tiltDegrees},
        {QStringLiteral("spin"), state->spinDegreesPerSecond},
        {QStringLiteral("alpha"), state->alpha},
        {QStringLiteral("edgeInset"), std::min(state->edgeInset, state->width * 0.45F)},
        {QStringLiteral("surfaceIndices"), state->surface.indexCount},
        {QStringLiteral("edgeIndices"), state->edge.indexCount},
    };
}

// Undo whatever run() installed. Declared in the descriptor so a caller can ask
// whether this module can be released and get an answer, instead of sending a
// payload and hoping something acted on it.
//
// Tearing the GL objects down needs this widget's context current, which holds
// only inside its paint callbacks. Deferring to the next frame would report
// completion before the scene is actually back, and a handover sequenced on
// that completion would hand the next generation state this one still owns. So
// this refuses; it does not defer.
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
    if (ring == nullptr) {
        host->complete_json(host->invocation_context,
                            "{\"removed\":false,\"note\":\"nothing was installed\"}");
        return;
    }
    if (!scene_toggle::ownContextIsCurrent(cube)) {
        host->fail(host->invocation_context,
                   "the widget's OpenGL context is not current; release this module with "
                   "executor=render so the scene is back before this reports completion");
        return;
    }

    QString error;
    setRingEnabled(cube, false, error);
    host->complete_json(host->invocation_context, "{\"removed\":true}");
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
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        host->fail(host->invocation_context,
                   "no OpenGL context is current; use executor=render");
        return;
    }

    const QJsonObject request =
        QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();
    ensureToggleAction(cube);

    const auto complete = [host](QJsonObject result) {
        result.insert(QStringLiteral("menuToggle"), toggleAction != nullptr);
        host->complete_json(host->invocation_context,
                            QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    };

    // restore always takes it off; toggle flips whichever way it currently is.
    const bool takeOff = request.value(QStringLiteral("restore")).toBool()
        || request.value(QStringLiteral("remove")).toBool()
        || (request.value(QStringLiteral("toggle")).toBool() && ring != nullptr);
    if (takeOff) {
        if (ring == nullptr) {
            complete(QJsonObject{
                {QStringLiteral("removed"), false},
                {QStringLiteral("note"), QStringLiteral("no ring was installed")},
            });
            return;
        }
        QString error;
        setRingEnabled(cube, false, error);
        complete(QJsonObject{{QStringLiteral("removed"), true}});
        return;
    }

    if (ring != nullptr) {
        applyRequest(ring, request);
        uploadGeometry(ring);
        cube->update();
        QJsonObject result = describe(ring);
        result.insert(QStringLiteral("rebuilt"), true);
        complete(result);
        return;
    }

    QString error;
    if (!setRingEnabled(cube, true, error)) {
        host->fail(host->invocation_context, error.toLocal8Bit().constData());
        return;
    }
    // The strip is up with the parameters it last had; this request may change
    // them, which only costs a rebuild of the two bands.
    applyRequest(ring, request);
    uploadGeometry(ring);
    cube->update();

    QJsonObject result = describe(ring);
    result.insert(QStringLiteral("installed"), true);
    result.insert(QStringLiteral("connectionValid"), static_cast<bool>(drawConnection));
    complete(result);
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "rotating translucent Mobius strip around the scene",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init()
{
    return &descriptor;
}
