// Turns each side of the cube into a stuffed pillow panel, in the live process.
//
// Like the Catmull-Clark snippet, this replaces the geometry rather than
// deforming it: paintGL() draws with a hardcoded glDrawArrays(GL_TRIANGLES, 0,
// 36), so a denser mesh cannot go through the widget's own buffer. The 36
// original vertices are saved, overwritten with zeros so that draw rasterizes
// only degenerate triangles, and put back on restore.
//
// Each of the six panels is a grid over (u, v) in [-1, 1]^2, displaced by three
// terms:
//
//   rounding  blends the cube point toward the unit sphere, so the box edges
//             become soft folds instead of knife edges;
//   tuck      pulls the eight corners inward, the way fabric gathers where
//             three panels are sewn together;
//   bulge     lifts the panel along its own normal, strongest at the centre and
//             exactly zero at the border.
//
// Rounding and tuck are functions of the cube point alone and are symmetric
// under the cube's own symmetries, and the bulge vanishes at the border, so two
// panels sharing a seam evaluate to the same curve there and the surface stays
// closed. Their normals differ, which is what makes the seam read as a crease.
//
// Loading the snippet also adds a checkable "Pillow mode" entry to the Cube
// menu, so the effect can be switched from the GUI as well as over the socket.
//
// Request: {"puff", "roundness", "tuck", "resolution", "seam", "stitches",
// "fabric"} to (re)build, {"toggle": true} to flip it, {"restore": true} to put
// the original cube back.

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

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

// Must match the values paintGL() uses, since the mesh shares the frame.
constexpr float viewDistance = 5.4F;
constexpr int originalCubeFloats = 36 * 6;

constexpr float halfPi = 1.57079632679489662F;
constexpr int floatsPerVertex = 11; // position, normal, colour, panel (u, v)

constexpr auto pillowVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inPanel;

uniform mat4 mvp;
uniform mat4 model;
uniform mat3 normalMatrix;

out vec3 shadingNormal;
out vec3 worldPosition;
out vec3 panelColor;
out vec2 panelUv;

void main()
{
    shadingNormal = normalMatrix * inNormal;
    worldPosition = vec3(model * vec4(inPosition, 1.0));
    panelColor = inColor;
    panelUv = inPanel;
    gl_Position = mvp * vec4(inPosition, 1.0);
}
)glsl";

// The widget's own shader is unlit, which reads fine on a cube and tells you
// nothing about a curved surface. The light direction matches the other scene
// snippets so all of them agree on where the light is.
constexpr auto pillowFragmentShader = R"glsl(
#version 330 core
in vec3 shadingNormal;
in vec3 worldPosition;
in vec3 panelColor;
in vec2 panelUv;

uniform vec3 tint;
uniform vec3 cameraPosition;
uniform float seamWidth;
uniform float stitchCount;
uniform float fabric;

out vec4 fragmentColor;

void main()
{
    vec3 normal = normalize(shadingNormal);
    vec3 lightDirection = normalize(vec3(0.45, 0.8, 0.6));
    vec3 viewDirection = normalize(cameraPosition - worldPosition);

    // Cloth scatters light under its own surface, so the terminator is soft and
    // the far side never reaches black. A plain Lambert term looks like plastic
    // here.
    float wrapped = dot(normal, lightDirection) * 0.5 + 0.5;
    float diffuse = pow(wrapped, 1.7);
    vec3 halfway = normalize(lightDirection + viewDirection);
    float sheen = pow(max(dot(halfway, normal), 0.0), 12.0) * 0.10;

    // fabric fades the six saturated face colours toward a linen tone.
    float luminance = dot(panelColor, vec3(0.299, 0.587, 0.114));
    vec3 cloth = mix(panelColor, mix(vec3(luminance), vec3(0.86, 0.83, 0.76), 0.35), fabric);
    vec3 shaded = cloth * (0.30 + 0.70 * diffuse) + vec3(sheen);

    // edge is 0 at the sewn border of the panel and 1 at its centre.
    float edge = 1.0 - max(abs(panelUv.x), abs(panelUv.y));

    // Stuffing cannot reach the border, so the cloth there stays slack, sits in
    // shadow from the neighbouring panel, and loses the bulge that faces the
    // light. One wide falloff covers all of that.
    shaded *= 1.0 - 0.30 * (1.0 - smoothstep(0.0, 0.22, edge));

    // The seam itself: a tight fold of the same cloth, not a black gap.
    float fold = 1.0 - smoothstep(seamWidth * 0.55, seamWidth, edge);
    shaded = mix(shaded, cloth * 0.32 * (0.40 + 0.60 * diffuse), fold);

    // Stitching, a dashed thread running parallel to the seam and a little way
    // inside the panel, where it is against cloth instead of inside the fold.
    if (stitchCount > 0.0) {
        float along = abs(panelUv.x) > abs(panelUv.y) ? panelUv.y : panelUv.x;
        float dash = fract(along * stitchCount * 0.5 + 0.25);
        float thread = smoothstep(0.30, 0.36, dash) * (1.0 - smoothstep(0.70, 0.76, dash));
        float line = 1.0 - smoothstep(seamWidth * 0.35, seamWidth * 0.75,
                                      abs(edge - seamWidth * 2.0));
        vec3 threadColor = vec3(0.96, 0.94, 0.88) * (0.30 + 0.70 * diffuse);
        shaded = mix(shaded, threadColor, thread * line * 0.9);
    }

    fragmentColor = vec4(clamp(shaded * tint, 0.0, 1.0), 1.0);
}
)glsl";

// The defaults keep the cube readable as a cube. Pushing puff much past the
// corner radius makes the panels meet at the same distance from the centre as
// the corners do, and the silhouette turns into a ball.
struct Shape {
    float puff = 0.19F;
    float roundness = 0.10F;
    float tuck = 0.12F;
    float seamWidth = 0.045F;
    float stitchCount = 18.0F;
    float fabric = 0.0F;
    int resolution = 56;
};

// A pillow is flatter across the middle of a panel than a cosine dome and turns
// into the seam more sharply, which is what an exponent below one does to the
// profile.
constexpr float bulgeFlatness = 0.8F;
// |x * y * z| is 0 at a panel centre and along every edge midline, and 1 only at
// a corner. Raising it concentrates the gather into the corners themselves.
constexpr float tuckSharpness = 1.6F;

struct Panel {
    QVector3D normal;
    QVector3D tangent;
    QVector3D bitangent;
    QVector3D color;
};

// tangent x bitangent == normal on every panel, so a grid quad wound
// (u, v) -> (u+1, v) -> (u+1, v+1) -> (u, v+1) faces outward and survives the
// widget's GL_BACK culling. The colours are the widget's own six, in its order.
const std::array<Panel, 6> panels{
    Panel{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}},   // front, +z
    Panel{{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {0, 1, 1}}, // back, -z
    Panel{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}},  // left, -x
    Panel{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 1}},  // right, +x
    Panel{{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, {1, 1, 0}},  // top, +y
    Panel{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, 1}},  // bottom, -y
};

struct PillowState {
    // The context the GL objects below belong to. They exist in no other, so a
    // frame drawn by a different context must be left alone.
    QOpenGLContext* owner = nullptr;
    QOpenGLShaderProgram program;
    QOpenGLBuffer vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer indexBuffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject vertexArray;
    QOpenGLFunctions_3_3_Core* gl = nullptr;
    Shape shape;
    int indexCount = 0;
    int triangleCount = 0;
    int pointCount = 0;
    std::vector<float> savedCubeVertices;
};

PillowState* pillow = nullptr;
QMetaObject::Connection drawConnection;
QAction* toggleAction = nullptr;

// Survives an off/on cycle, so switching the effect back on from the menu
// returns the panels the way they were last tuned rather than resetting them.
Shape rememberedShape;

QVector3D pillowPoint(const Panel& panel, const float u, const float v, const Shape& shape)
{
    const QVector3D cubePoint = panel.normal + panel.tangent * u + panel.bitangent * v;

    QVector3D point = cubePoint * (1.0F - shape.roundness)
        + cubePoint.normalized() * shape.roundness;

    const float cornerness =
        std::abs(cubePoint.x() * cubePoint.y() * cubePoint.z());
    point *= 1.0F - shape.tuck * std::pow(cornerness, tuckSharpness);

    const float acrossU = std::cos(halfPi * u);
    const float acrossV = std::cos(halfPi * v);
    point += panel.normal * (shape.puff * std::pow(acrossU * acrossV, bulgeFlatness));

    return point;
}

// Finite differences over the same function, with the sample points clamped to
// the panel. At a border that degrades to a one-sided difference, which is what
// picks up how steeply the surface turns into the seam.
QVector3D pillowNormal(const Panel& panel, const float u, const float v, const Shape& shape)
{
    constexpr float step = 1.0F / 512.0F;
    const float uLow = std::max(u - step, -1.0F);
    const float uHigh = std::min(u + step, 1.0F);
    const float vLow = std::max(v - step, -1.0F);
    const float vHigh = std::min(v + step, 1.0F);

    const QVector3D alongU =
        (pillowPoint(panel, uHigh, v, shape) - pillowPoint(panel, uLow, v, shape))
        / (uHigh - uLow);
    const QVector3D alongV =
        (pillowPoint(panel, u, vHigh, shape) - pillowPoint(panel, u, vLow, shape))
        / (vHigh - vLow);

    const QVector3D normal = QVector3D::crossProduct(alongU, alongV);
    return normal.isNull() ? panel.normal : normal.normalized();
}

// Interleaved position + normal + colour + panel coordinates. Each panel keeps
// its own vertices even where they coincide with a neighbour's, so the seam
// gets two normals and shades as a crease rather than a smooth roll.
void buildPillow(const Shape& shape,
                 std::vector<float>& vertices,
                 std::vector<unsigned int>& indices)
{
    const int cells = shape.resolution;
    const int side = cells + 1;
    const auto step = 2.0F / static_cast<float>(cells);

    vertices.reserve(panels.size() * static_cast<std::size_t>(side) * side * floatsPerVertex);
    indices.reserve(panels.size() * static_cast<std::size_t>(cells) * cells * 6);

    for (const Panel& panel : panels) {
        const auto base = static_cast<unsigned int>(vertices.size() / floatsPerVertex);
        for (int j = 0; j < side; ++j) {
            const float v = -1.0F + step * static_cast<float>(j);
            for (int i = 0; i < side; ++i) {
                const float u = -1.0F + step * static_cast<float>(i);
                const QVector3D point = pillowPoint(panel, u, v, shape);
                const QVector3D normal = pillowNormal(panel, u, v, shape);
                vertices.insert(vertices.end(), {
                    point.x(), point.y(), point.z(),
                    normal.x(), normal.y(), normal.z(),
                    panel.color.x(), panel.color.y(), panel.color.z(),
                    u, v,
                });
            }
        }
        for (int j = 0; j < cells; ++j) {
            for (int i = 0; i < cells; ++i) {
                const auto corner = base + static_cast<unsigned int>(j * side + i);
                const auto right = corner + 1;
                const auto diagonal = corner + static_cast<unsigned int>(side) + 1;
                const auto above = corner + static_cast<unsigned int>(side);
                indices.insert(indices.end(),
                               {corner, right, diagonal, corner, diagonal, above});
            }
        }
    }
}

void uploadShape(PillowState* state, const Shape& shape)
{
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    buildPillow(shape, vertices, indices);

    state->vertexArray.bind();

    state->vertexBuffer.bind();
    state->vertexBuffer.allocate(vertices.data(),
                                 static_cast<int>(vertices.size() * sizeof(float)));
    state->indexBuffer.bind();
    state->indexBuffer.allocate(indices.data(),
                                static_cast<int>(indices.size() * sizeof(unsigned int)));

    state->program.bind();
    constexpr int stride = floatsPerVertex * sizeof(float);
    state->program.enableAttributeArray(0);
    state->program.setAttributeBuffer(0, GL_FLOAT, 0, 3, stride);
    state->program.enableAttributeArray(1);
    state->program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, stride);
    state->program.enableAttributeArray(2);
    state->program.setAttributeBuffer(2, GL_FLOAT, 6 * sizeof(float), 3, stride);
    state->program.enableAttributeArray(3);
    state->program.setAttributeBuffer(3, GL_FLOAT, 9 * sizeof(float), 2, stride);
    state->program.release();

    state->vertexArray.release();
    state->vertexBuffer.release();

    state->shape = shape;
    state->indexCount = static_cast<int>(indices.size());
    state->triangleCount = state->indexCount / 3;
    state->pointCount = static_cast<int>(vertices.size() / floatsPerVertex);
}

void drawPillowCube(const CubeWidget* cube)
{
    if (pillow == nullptr || pillow->gl == nullptr
        || QOpenGLContext::currentContext() != pillow->owner) {
        return;
    }

    QMatrix4x4 model;
    model.rotate(cube->m_angleDegrees, QVector3D(0.72F, 1.0F, 0.31F));
    model.scale(cube->m_scale);

    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, -viewDistance);

    // paintGL() has already restored GL_FILL by the time frameRendered() is
    // emitted, so the wireframe state has to be reapplied here.
    if (cube->m_wireframe) {
        pillow->gl->glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    pillow->program.bind();
    pillow->program.setUniformValue("mvp", cube->m_projection * view * model);
    pillow->program.setUniformValue("model", model);
    pillow->program.setUniformValue("normalMatrix", model.normalMatrix());
    pillow->program.setUniformValue("tint", cube->m_tint);
    pillow->program.setUniformValue("cameraPosition", QVector3D(0.0F, 0.0F, viewDistance));
    pillow->program.setUniformValue("seamWidth", pillow->shape.seamWidth);
    pillow->program.setUniformValue("stitchCount", pillow->shape.stitchCount);
    pillow->program.setUniformValue("fabric", pillow->shape.fabric);
    pillow->vertexArray.bind();
    pillow->gl->glDrawElements(GL_TRIANGLES, pillow->indexCount, GL_UNSIGNED_INT, nullptr);
    pillow->vertexArray.release();
    pillow->program.release();

    if (cube->m_wireframe) {
        pillow->gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

Shape readShape(const QJsonObject& request, const Shape& current)
{
    const auto number = [&request](const QString& key, const float fallback,
                                   const float low, const float high) {
        return std::clamp(static_cast<float>(request.value(key).toDouble(fallback)), low, high);
    };

    Shape shape;
    shape.puff = number(QStringLiteral("puff"), current.puff, 0.0F, 0.8F);
    shape.roundness = number(QStringLiteral("roundness"), current.roundness, 0.0F, 1.0F);
    shape.tuck = number(QStringLiteral("tuck"), current.tuck, 0.0F, 0.5F);
    shape.seamWidth = number(QStringLiteral("seam"), current.seamWidth, 0.0F, 0.3F);
    shape.stitchCount = number(QStringLiteral("stitches"), current.stitchCount, 0.0F, 64.0F);
    shape.fabric = number(QStringLiteral("fabric"), current.fabric, 0.0F, 1.0F);
    shape.resolution = std::clamp(
        request.value(QStringLiteral("resolution")).toInt(current.resolution), 4, 160);
    return shape;
}

QJsonObject describe(const PillowState* state)
{
    return QJsonObject{
        {QStringLiteral("puff"), state->shape.puff},
        {QStringLiteral("roundness"), state->shape.roundness},
        {QStringLiteral("tuck"), state->shape.tuck},
        {QStringLiteral("seam"), state->shape.seamWidth},
        {QStringLiteral("stitches"), state->shape.stitchCount},
        {QStringLiteral("fabric"), state->shape.fabric},
        {QStringLiteral("resolution"), state->shape.resolution},
        {QStringLiteral("triangles"), state->triangleCount},
        {QStringLiteral("points"), state->pointCount},
    };
}

// A collapsed face is six vertices of zeros in a row. Testing the whole buffer
// for zeros only catches a replacement that took all six faces at once, and
// misses one that took a single face — after which this would save that face's
// zeros as the originals and lose it on restore. Per-face catches both, and
// cannot be done by testing individual floats: the widget's own face colours
// contain zeros.
bool anyFaceCollapsed(const std::vector<float>& vertices)
{
    constexpr int floatsPerFace = 6 * 6;
    const auto total = static_cast<int>(vertices.size());
    for (int start = 0; start + floatsPerFace <= total; start += floatsPerFace) {
        const auto begin = vertices.begin() + start;
        if (std::all_of(begin, begin + floatsPerFace,
                        [](const float value) { return value == 0.0F; })) {
            return true;
        }
    }
    return false;
}

// Needs the widget's OpenGL context to be current.
bool installPillow(CubeWidget* cube, QString& error)
{
    // The Catmull-Clark snippet replaces the same mesh and saves the same 36
    // vertices, so it has to be off before this one reads them. The jack in the
    // box saves only the face it hollows, but those vertices are part of these
    // 36 and reading its zeros here would restore the hole instead of the face.
    scene_toggle::turnOff(cube, QStringLiteral("actionCatmullClark"));
    scene_toggle::turnOff(cube, QStringLiteral("actionJackInTheBox"));

    auto* state = new PillowState();
    state->owner = QOpenGLContext::currentContext();
    state->gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(state->owner);
    if (state->gl == nullptr) {
        delete state;
        error = QStringLiteral("no OpenGL 3.3 core function table for this context");
        return false;
    }
    state->gl->initializeOpenGLFunctions();

    if (!state->program.addShaderFromSourceCode(QOpenGLShader::Vertex, pillowVertexShader)
        || !state->program.addShaderFromSourceCode(QOpenGLShader::Fragment, pillowFragmentShader)
        || !state->program.link()) {
        error = state->program.log();
        delete state;
        return false;
    }

    state->vertexArray.create();
    state->vertexBuffer.create();
    state->indexBuffer.create();
    uploadShape(state, rememberedShape);

    // Keep the widget's own vertices so the original cube can come back, then
    // collapse them to a point: paintGL()'s 36-vertex draw then rasterizes
    // nothing and the pillow is the only thing in the frame.
    state->savedCubeVertices.resize(originalCubeFloats);
    cube->m_vertexBuffer.bind();
    const bool readBack = cube->m_vertexBuffer.read(
        0, state->savedCubeVertices.data(), originalCubeFloats * static_cast<int>(sizeof(float)));
    // All zeros means some other mesh replacement already collapsed them and
    // holds the only copy of the originals. Saving these would lose the cube on
    // restore, so refuse rather than nest.
    const bool alreadySuppressed = readBack && anyFaceCollapsed(state->savedCubeVertices);
    if (!readBack || alreadySuppressed) {
        cube->m_vertexBuffer.release();
        state->vertexArray.destroy();
        state->vertexBuffer.destroy();
        state->indexBuffer.destroy();
        delete state;
        error = readBack
            ? QStringLiteral("a face of the widget's mesh is already collapsed, so another "
                             "replacement owns those vertices; saving its zeros as the "
                             "originals would lose that face on restore")
            : QStringLiteral("could not read the widget's vertex buffer; refusing to overwrite it");
        return false;
    }
    const std::vector<float> collapsed(originalCubeFloats, 0.0F);
    cube->m_vertexBuffer.write(
        0, collapsed.data(), originalCubeFloats * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();

    pillow = state;
    drawConnection = QObject::connect(
        cube, &CubeWidget::frameRendered, cube,
        [cube](qulonglong, float) { drawPillowCube(cube); },
        Qt::DirectConnection);
    cube->update();
    return true;
}

// Needs the widget's OpenGL context to be current.
void removePillow(CubeWidget* cube)
{
    QObject::disconnect(drawConnection);
    cube->m_vertexBuffer.bind();
    cube->m_vertexBuffer.write(0,
                               pillow->savedCubeVertices.data(),
                               originalCubeFloats * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();

    pillow->vertexArray.destroy();
    pillow->vertexBuffer.destroy();
    pillow->indexBuffer.destroy();
    delete pillow;
    pillow = nullptr;
    cube->update();
}

void syncToggleAction()
{
    if (toggleAction != nullptr) {
        const QSignalBlocker blocker(toggleAction);
        toggleAction->setChecked(pillow != nullptr);
    }
}

// Returns false only when an install was asked for and failed.
bool setPillowEnabled(CubeWidget* cube, const bool enabled, QString& error)
{
    if (enabled == (pillow != nullptr)) {
        return true;
    }
    bool installed = true;
    if (enabled) {
        installed = installPillow(cube, error);
    } else {
        removePillow(cube);
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
        QStringLiteral("actionPillowMode"),
        QStringLiteral("P&illow mode"),
        QStringLiteral("Ctrl+Shift+P"),
        [cube](const bool enabled) {
            QString ignored;
            // A menu click arrives on the GUI thread with no current context and
            // has to wait for a frame. Being switched off by the Catmull-Clark
            // snippet arrives from inside its render callback, where the context
            // is current and the removal has to finish before that install reads
            // the vertices.
            if (scene_toggle::ownContextIsCurrent(cube)) {
                setPillowEnabled(cube, enabled, ignored);
                return;
            }
            cube->enqueueRenderCallback([cube, enabled] {
                QString deferred;
                setPillowEnabled(cube, enabled, deferred);
            });
        });
    syncToggleAction();
}

// Undo whatever run() installed. Declared in the descriptor so a caller can ask
// whether this module can be released and get an answer, rather than sending a
// payload and hoping something acted on it.
//
// Tearing the GL objects down needs this widget's context current, which holds
// only inside its paint callbacks. Deferring to the next frame would report
// completion before the scene is actually back, and a handover sequenced on
// that completion would hand the next generation state this one still owns. So
// this refuses rather than defers.
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
    if (pillow == nullptr) {
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
    setPillowEnabled(cube, false, error);
    host->complete_json(host->invocation_context, "{\"removed\":true}");
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
        || (request.value(QStringLiteral("toggle")).toBool() && pillow != nullptr);
    if (takeOff) {
        if (pillow == nullptr) {
            complete(QJsonObject{
                {QStringLiteral("restored"), false},
                {QStringLiteral("note"), QStringLiteral("no pillow was installed")},
            });
            return;
        }
        QString error;
        setPillowEnabled(cube, false, error);
        complete(QJsonObject{
            {QStringLiteral("restored"), true},
            {QStringLiteral("note"), QStringLiteral("the widget's original 36 vertices are back")},
        });
        return;
    }

    // Parameters the request leaves out keep the value they last had, on a
    // rebuild and across an off/on cycle alike.
    rememberedShape = readShape(request, rememberedShape);

    // Already running: rebuild with the requested changes and keep the hook.
    if (pillow != nullptr) {
        if (rememberedShape.resolution != pillow->shape.resolution
            || rememberedShape.puff != pillow->shape.puff
            || rememberedShape.roundness != pillow->shape.roundness
            || rememberedShape.tuck != pillow->shape.tuck) {
            uploadShape(pillow, rememberedShape);
        } else {
            pillow->shape = rememberedShape; // the rest are uniforms, read every frame
        }
        cube->update();
        QJsonObject result = describe(pillow);
        result.insert(QStringLiteral("rebuilt"), true);
        complete(result);
        return;
    }

    QString error;
    if (!setPillowEnabled(cube, true, error)) {
        host->fail(host->invocation_context, error.toLocal8Bit().constData());
        return;
    }

    QJsonObject result = describe(pillow);
    result.insert(QStringLiteral("installed"), true);
    result.insert(QStringLiteral("connectionValid"), static_cast<bool>(drawConnection));
    result.insert(QStringLiteral("originalCubeSuppressed"), true);
    complete(result);
}

const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1,
    sizeof(RuntimeAgentSnippetV1),
    "turn each side of the cube into a stuffed pillow panel",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1()
{
    return &descriptor;
}
