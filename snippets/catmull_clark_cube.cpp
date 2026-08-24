// Replaces the cube with its Catmull-Clark subdivision surface, in the live
// process.
//
// paintGL() draws the cube with a hardcoded glDrawArrays(GL_TRIANGLES, 0, 36),
// so the subdivided mesh cannot be pushed through the widget's own vertex
// buffer: the vertex count is baked into compiled code. Instead this snippet
// keeps a copy of the widget's 36 original vertices, overwrites them with
// zeros so that draw rasterizes only degenerate triangles, and draws the
// subdivided mesh from its own buffers in a frameRendered() hook.
//
// Loading the snippet also adds a checkable "Catmull-Clark" entry to the Cube
// menu. It excludes the pillow, which replaces the same mesh: see
// scene_toggle::turnOff for why the two cannot be installed at once.
//
// Request: {"level": 0..5} to (re)build, {"toggle": true} to flip it,
// {"restore": true} to put the original cube back.

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
#include <map>
#include <utility>
#include <vector>

namespace {

// Must match the values paintGL() uses, since the mesh shares the frame.
constexpr float viewDistance = 5.4F;
constexpr int originalCubeFloats = 36 * 6;

constexpr int maxLevel = 5;
constexpr int defaultLevel = 2;

constexpr auto subdivisionVertexShader = R"glsl(
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

// The widget's own shader is unlit, which reads fine on a cube and terribly on
// a rounded surface, so this one carries a diffuse term. The light direction
// matches the orbiting-sphere snippet so both objects agree on where the light
// is.
constexpr auto subdivisionFragmentShader = R"glsl(
#version 330 core
in vec3 shadingNormal;
in vec3 surfaceColor;
uniform vec3 tint;
out vec4 fragmentColor;

void main()
{
    vec3 lightDirection = normalize(vec3(0.45, 0.8, 0.6));
    float diffuse = max(dot(normalize(shadingNormal), lightDirection), 0.0);
    vec3 shaded = surfaceColor * (0.25 + 0.75 * diffuse);
    fragmentColor = vec4(clamp(shaded * tint, 0.0, 1.0), 1.0);
}
)glsl";

struct Quad {
    std::array<int, 4> corner;
    int color;
};

// Every face is a quad: the control cage is a cube, and Catmull-Clark produces
// quads from anything. That removes the general n-gon bookkeeping.
struct Mesh {
    std::vector<QVector3D> points;
    std::vector<Quad> faces;
};

struct SubdivisionState {
    // The context the GL objects below belong to. They exist in no other, so a
    // frame drawn by a different context must be left alone.
    QOpenGLContext* owner = nullptr;
    QOpenGLShaderProgram program;
    QOpenGLBuffer vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer indexBuffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject vertexArray;
    QOpenGLFunctions_3_3_Core* gl = nullptr;
    int indexCount = 0;
    int quadCount = 0;
    int pointCount = 0;
    int level = 0;
    std::vector<float> savedCubeVertices;
};

SubdivisionState* subdivision = nullptr;
QMetaObject::Connection drawConnection;
QAction* toggleAction = nullptr;

// Survives an off/on cycle, so switching the surface back on from the menu
// returns the level it was last built at.
int rememberedLevel = defaultLevel;

// The six face colors of the widget's own cube, in the same order as the
// control cage below.
const std::array<QVector3D, 6> faceColors{
    QVector3D(1.0F, 0.0F, 0.0F), // front, +z
    QVector3D(0.0F, 1.0F, 1.0F), // back, -z
    QVector3D(0.0F, 1.0F, 0.0F), // left, -x
    QVector3D(1.0F, 0.0F, 1.0F), // right, +x
    QVector3D(1.0F, 1.0F, 0.0F), // top, +y
    QVector3D(0.0F, 0.0F, 1.0F), // bottom, -y
};

Mesh controlCage()
{
    Mesh cage;
    cage.points = {
        {-1.0F, -1.0F, -1.0F}, { 1.0F, -1.0F, -1.0F},
        { 1.0F,  1.0F, -1.0F}, {-1.0F,  1.0F, -1.0F},
        {-1.0F, -1.0F,  1.0F}, { 1.0F, -1.0F,  1.0F},
        { 1.0F,  1.0F,  1.0F}, {-1.0F,  1.0F,  1.0F},
    };
    // Counter-clockwise seen from outside, so the widget's GL_BACK culling
    // keeps the outer surface.
    cage.faces = {
        {{4, 5, 6, 7}, 0},
        {{1, 0, 3, 2}, 1},
        {{0, 4, 7, 3}, 2},
        {{1, 2, 6, 5}, 3},
        {{3, 7, 6, 2}, 4},
        {{0, 1, 5, 4}, 5},
    };
    return cage;
}

// One Catmull-Clark step on a closed all-quad mesh. Every edge has exactly two
// adjacent faces here, so there is no boundary rule.
Mesh subdivide(const Mesh& in)
{
    const int pointCount = static_cast<int>(in.points.size());
    const int faceCount = static_cast<int>(in.faces.size());

    std::vector<QVector3D> facePoints(faceCount);
    for (int f = 0; f < faceCount; ++f) {
        QVector3D sum;
        for (const int corner : in.faces[f].corner) {
            sum += in.points[corner];
        }
        facePoints[f] = sum / 4.0F;
    }

    struct Edge {
        int a = -1;
        int b = -1;
        int faceA = -1;
        int faceB = -1;
    };
    std::vector<Edge> edges;
    std::map<std::pair<int, int>, int> edgeIds;
    const auto edgeId = [&edges, &edgeIds](const int a, const int b) {
        const std::pair<int, int> key{std::min(a, b), std::max(a, b)};
        const auto existing = edgeIds.find(key);
        if (existing != edgeIds.end()) {
            return existing->second;
        }
        const int id = static_cast<int>(edges.size());
        edges.push_back(Edge{key.first, key.second, -1, -1});
        edgeIds.emplace(key, id);
        return id;
    };

    std::vector<std::array<int, 4>> faceEdges(faceCount);
    for (int f = 0; f < faceCount; ++f) {
        for (int c = 0; c < 4; ++c) {
            const int id = edgeId(in.faces[f].corner[c], in.faces[f].corner[(c + 1) % 4]);
            faceEdges[f][c] = id;
            if (edges[id].faceA < 0) {
                edges[id].faceA = f;
            } else {
                edges[id].faceB = f;
            }
        }
    }

    std::vector<QVector3D> edgePoints(edges.size());
    for (std::size_t e = 0; e < edges.size(); ++e) {
        const Edge& edge = edges[e];
        edgePoints[e] = (in.points[edge.a] + in.points[edge.b]
                         + facePoints[edge.faceA] + facePoints[edge.faceB]) / 4.0F;
    }

    // newV = (F + 2R + (n - 3)V) / n, with F the average adjacent face point, R
    // the average adjacent edge midpoint, and n the valence.
    std::vector<QVector3D> facePointSum(pointCount);
    std::vector<QVector3D> edgeMidpointSum(pointCount);
    std::vector<int> valence(pointCount, 0);
    for (int f = 0; f < faceCount; ++f) {
        for (const int corner : in.faces[f].corner) {
            facePointSum[corner] += facePoints[f];
        }
    }
    for (const Edge& edge : edges) {
        const QVector3D midpoint = (in.points[edge.a] + in.points[edge.b]) / 2.0F;
        edgeMidpointSum[edge.a] += midpoint;
        edgeMidpointSum[edge.b] += midpoint;
        ++valence[edge.a];
        ++valence[edge.b];
    }

    Mesh out;
    out.points.resize(static_cast<std::size_t>(pointCount) + facePoints.size() + edgePoints.size());
    for (int v = 0; v < pointCount; ++v) {
        const auto n = static_cast<float>(valence[v]);
        const QVector3D averageFacePoint = facePointSum[v] / n;
        const QVector3D averageEdgeMidpoint = edgeMidpointSum[v] / n;
        out.points[v] = (averageFacePoint + 2.0F * averageEdgeMidpoint
                         + (n - 3.0F) * in.points[v]) / n;
    }
    const int facePointBase = pointCount;
    const int edgePointBase = pointCount + faceCount;
    for (int f = 0; f < faceCount; ++f) {
        out.points[facePointBase + f] = facePoints[f];
    }
    for (std::size_t e = 0; e < edgePoints.size(); ++e) {
        out.points[edgePointBase + e] = edgePoints[e];
    }

    // Each corner of each face becomes one quad, which preserves the winding.
    out.faces.reserve(static_cast<std::size_t>(faceCount) * 4);
    for (int f = 0; f < faceCount; ++f) {
        for (int c = 0; c < 4; ++c) {
            out.faces.push_back(Quad{{
                in.faces[f].corner[c],
                edgePointBase + faceEdges[f][c],
                facePointBase + f,
                edgePointBase + faceEdges[f][(c + 3) % 4],
            }, in.faces[f].color});
        }
    }
    return out;
}

// Interleaved position + normal + color. Normals are accumulated per shared
// point so the surface shades smoothly; corners are then emitted per face so
// each quad keeps its parent face's color and the six color regions stay crisp.
void buildRenderArrays(const Mesh& mesh,
                       std::vector<float>& vertices,
                       std::vector<unsigned int>& indices)
{
    std::vector<QVector3D> normals(mesh.points.size());
    for (const Quad& face : mesh.faces) {
        // Newell's method: the magnitude is proportional to the area, which
        // weights the accumulation the way it should be weighted.
        QVector3D normal;
        for (int c = 0; c < 4; ++c) {
            const QVector3D& current = mesh.points[face.corner[c]];
            const QVector3D& next = mesh.points[face.corner[(c + 1) % 4]];
            normal += QVector3D((current.y() - next.y()) * (current.z() + next.z()),
                                (current.z() - next.z()) * (current.x() + next.x()),
                                (current.x() - next.x()) * (current.y() + next.y()));
        }
        for (const int corner : face.corner) {
            normals[corner] += normal;
        }
    }
    for (QVector3D& normal : normals) {
        normal.normalize();
    }

    vertices.reserve(mesh.faces.size() * 4 * 9);
    indices.reserve(mesh.faces.size() * 6);
    for (const Quad& face : mesh.faces) {
        const auto base = static_cast<unsigned int>(vertices.size() / 9);
        const QVector3D& color = faceColors[static_cast<std::size_t>(face.color)];
        for (const int corner : face.corner) {
            const QVector3D& point = mesh.points[corner];
            const QVector3D& normal = normals[corner];
            vertices.insert(vertices.end(), {
                point.x(), point.y(), point.z(),
                normal.x(), normal.y(), normal.z(),
                color.x(), color.y(), color.z(),
            });
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

void uploadLevel(SubdivisionState* state, const int level)
{
    Mesh mesh = controlCage();
    for (int step = 0; step < level; ++step) {
        mesh = subdivide(mesh);
    }

    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    buildRenderArrays(mesh, vertices, indices);

    state->vertexArray.bind();

    state->vertexBuffer.bind();
    state->vertexBuffer.allocate(vertices.data(),
                                 static_cast<int>(vertices.size() * sizeof(float)));
    state->indexBuffer.bind();
    state->indexBuffer.allocate(indices.data(),
                                static_cast<int>(indices.size() * sizeof(unsigned int)));

    state->program.bind();
    constexpr int stride = 9 * sizeof(float);
    state->program.enableAttributeArray(0);
    state->program.setAttributeBuffer(0, GL_FLOAT, 0, 3, stride);
    state->program.enableAttributeArray(1);
    state->program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, stride);
    state->program.enableAttributeArray(2);
    state->program.setAttributeBuffer(2, GL_FLOAT, 6 * sizeof(float), 3, stride);
    state->program.release();

    state->vertexArray.release();
    state->vertexBuffer.release();

    state->indexCount = static_cast<int>(indices.size());
    state->quadCount = static_cast<int>(mesh.faces.size());
    state->pointCount = static_cast<int>(mesh.points.size());
    state->level = level;
}

void drawSubdividedCube(const CubeWidget* cube)
{
    if (subdivision == nullptr || subdivision->gl == nullptr
        || QOpenGLContext::currentContext() != subdivision->owner) {
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
        subdivision->gl->glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    subdivision->program.bind();
    subdivision->program.setUniformValue("mvp", cube->m_projection * view * model);
    subdivision->program.setUniformValue("normalMatrix", model.normalMatrix());
    subdivision->program.setUniformValue("tint", cube->m_tint);
    subdivision->vertexArray.bind();
    subdivision->gl->glDrawElements(
        GL_TRIANGLES, subdivision->indexCount, GL_UNSIGNED_INT, nullptr);
    subdivision->vertexArray.release();
    subdivision->program.release();

    if (cube->m_wireframe) {
        subdivision->gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

// Needs the widget's OpenGL context to be current.
bool installSubdivision(CubeWidget* cube, QString& error)
{
    // The pillow replaces the same mesh and saves the same 36 vertices, so it
    // has to be off before this one reads them. The jack in the box saves only
    // the face it hollows, but those vertices are part of these 36 and reading
    // its zeros here would restore the hole instead of the face.
    scene_toggle::turnOff(cube, QStringLiteral("actionPillowMode"));
    scene_toggle::turnOff(cube, QStringLiteral("actionJackInTheBox"));

    auto* state = new SubdivisionState();
    state->owner = QOpenGLContext::currentContext();
    state->gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(state->owner);
    if (state->gl == nullptr) {
        delete state;
        error = QStringLiteral("no OpenGL 3.3 core function table for this context");
        return false;
    }
    state->gl->initializeOpenGLFunctions();

    if (!state->program.addShaderFromSourceCode(QOpenGLShader::Vertex, subdivisionVertexShader)
        || !state->program.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                   subdivisionFragmentShader)
        || !state->program.link()) {
        error = state->program.log();
        delete state;
        return false;
    }

    state->vertexArray.create();
    state->vertexBuffer.create();
    state->indexBuffer.create();
    uploadLevel(state, rememberedLevel);

    // Keep the widget's own vertices so the original cube can come back, then
    // collapse them to a point: paintGL()'s 36-vertex draw then rasterizes
    // nothing and the subdivision surface is the only thing in the frame.
    state->savedCubeVertices.resize(originalCubeFloats);
    cube->m_vertexBuffer.bind();
    const bool readBack = cube->m_vertexBuffer.read(
        0, state->savedCubeVertices.data(), originalCubeFloats * static_cast<int>(sizeof(float)));
    const bool alreadySuppressed = readBack
        && std::all_of(state->savedCubeVertices.begin(), state->savedCubeVertices.end(),
                       [](const float value) { return value == 0.0F; });
    if (!readBack || alreadySuppressed) {
        cube->m_vertexBuffer.release();
        state->vertexArray.destroy();
        state->vertexBuffer.destroy();
        state->indexBuffer.destroy();
        delete state;
        error = readBack
            ? QStringLiteral("the widget's vertices are already collapsed, so another mesh "
                             "replacement owns them; saving these zeros would lose the cube")
            : QStringLiteral("could not read the widget's vertex buffer; refusing to overwrite it");
        return false;
    }
    const std::vector<float> collapsed(originalCubeFloats, 0.0F);
    cube->m_vertexBuffer.write(
        0, collapsed.data(), originalCubeFloats * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();

    subdivision = state;
    drawConnection = QObject::connect(
        cube, &CubeWidget::frameRendered, cube,
        [cube](qulonglong, float) { drawSubdividedCube(cube); },
        Qt::DirectConnection);
    cube->update();
    return true;
}

// Needs the widget's OpenGL context to be current.
void removeSubdivision(CubeWidget* cube)
{
    QObject::disconnect(drawConnection);
    cube->m_vertexBuffer.bind();
    cube->m_vertexBuffer.write(0,
                               subdivision->savedCubeVertices.data(),
                               originalCubeFloats * static_cast<int>(sizeof(float)));
    cube->m_vertexBuffer.release();

    subdivision->vertexArray.destroy();
    subdivision->vertexBuffer.destroy();
    subdivision->indexBuffer.destroy();
    delete subdivision;
    subdivision = nullptr;
    cube->update();
}

void syncToggleAction()
{
    if (toggleAction != nullptr) {
        const QSignalBlocker blocker(toggleAction);
        toggleAction->setChecked(subdivision != nullptr);
    }
}

// Returns false only when an install was asked for and failed.
bool setSubdivisionEnabled(CubeWidget* cube, const bool enabled, QString& error)
{
    if (enabled == (subdivision != nullptr)) {
        return true;
    }
    bool installed = true;
    if (enabled) {
        installed = installSubdivision(cube, error);
    } else {
        removeSubdivision(cube);
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
        QStringLiteral("actionCatmullClark"),
        QStringLiteral("&Catmull-Clark"),
        QStringLiteral("Ctrl+Shift+C"),
        [cube](const bool enabled) {
            QString ignored;
            // A menu click arrives on the GUI thread with no current context and
            // has to wait for a frame. Being switched off by the pillow arrives
            // from inside its render callback, where the context is current and
            // the removal has to finish before that install reads the vertices.
            if (scene_toggle::ownContextIsCurrent(cube)) {
                setSubdivisionEnabled(cube, enabled, ignored);
                return;
            }
            cube->enqueueRenderCallback([cube, enabled] {
                QString deferred;
                setSubdivisionEnabled(cube, enabled, deferred);
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
    if (subdivision == nullptr) {
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
    setSubdivisionEnabled(cube, false, error);
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
        || (request.value(QStringLiteral("toggle")).toBool() && subdivision != nullptr);
    if (takeOff) {
        if (subdivision == nullptr) {
            complete(QJsonObject{
                {QStringLiteral("restored"), false},
                {QStringLiteral("note"), QStringLiteral("no subdivision was installed")},
            });
            return;
        }
        QString error;
        setSubdivisionEnabled(cube, false, error);
        complete(QJsonObject{
            {QStringLiteral("restored"), true},
            {QStringLiteral("note"), QStringLiteral("the widget's original 36 vertices are back")},
        });
        return;
    }

    // A level the request leaves out keeps the value it last had, on a rebuild
    // and across an off/on cycle alike.
    rememberedLevel = std::clamp(
        request.value(QStringLiteral("level")).toInt(rememberedLevel), 0, maxLevel);

    // Already running: rebuild at the requested level and keep the hook.
    if (subdivision != nullptr) {
        uploadLevel(subdivision, rememberedLevel);
        cube->update();
        complete(QJsonObject{
            {QStringLiteral("rebuilt"), true},
            {QStringLiteral("level"), subdivision->level},
            {QStringLiteral("quads"), subdivision->quadCount},
            {QStringLiteral("points"), subdivision->pointCount},
            {QStringLiteral("indices"), subdivision->indexCount},
        });
        return;
    }

    QString error;
    if (!setSubdivisionEnabled(cube, true, error)) {
        host->fail(host->invocation_context, error.toLocal8Bit().constData());
        return;
    }

    complete(QJsonObject{
        {QStringLiteral("installed"), true},
        {QStringLiteral("connectionValid"), static_cast<bool>(drawConnection)},
        {QStringLiteral("level"), subdivision->level},
        {QStringLiteral("quads"), subdivision->quadCount},
        {QStringLiteral("points"), subdivision->pointCount},
        {QStringLiteral("indices"), subdivision->indexCount},
        {QStringLiteral("originalCubeSuppressed"), true},
    });
}

const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1,
    sizeof(RuntimeAgentSnippetV1),
    "replace the cube with its Catmull-Clark subdivision surface",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1()
{
    return &descriptor;
}
