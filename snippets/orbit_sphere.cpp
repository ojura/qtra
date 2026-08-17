// Draws a shaded sphere orbiting the cube, installed into the live process.
//
// paintGL() emits frameRendered() after the cube is drawn and after polygon
// mode is restored, while the context is still current and the depth buffer
// still holds the cube. A direct connection to that signal is therefore a
// per-frame draw hook that gets correct occlusion for free.

#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QMetaObject>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QVector3D>

#include <cmath>
#include <vector>

namespace {

constexpr float pi = 3.14159265358979323846F;

constexpr int latitudeBands = 24;
constexpr int longitudeBands = 36;
constexpr float sphereRadius = 0.34F;
constexpr float orbitRadius = 2.85F;
constexpr float orbitDegreesPerSecond = 95.0F;
constexpr float orbitTiltDegrees = 22.0F;

// Must match the values paintGL() uses, since the sphere shares the frame.
constexpr float viewDistance = 5.4F;

constexpr auto sphereVertexShader = R"glsl(
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

constexpr auto sphereFragmentShader = R"glsl(
#version 330 core
in vec3 shadingNormal;
uniform vec3 baseColor;
out vec4 fragmentColor;

void main()
{
    vec3 lightDirection = normalize(vec3(0.45, 0.8, 0.6));
    float diffuse = max(dot(normalize(shadingNormal), lightDirection), 0.0);
    fragmentColor = vec4(baseColor * (0.22 + 0.78 * diffuse), 1.0);
}
)glsl";

struct OrbitState {
    QOpenGLShaderProgram program;
    QOpenGLBuffer vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer indexBuffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject vertexArray;
    int indexCount = 0;
};

OrbitState* orbit = nullptr;
QMetaObject::Connection drawConnection;

// Interleaved position + normal. For a unit sphere the two are identical, but
// keeping both means the model matrix can scale non-uniformly later.
void buildSphere(std::vector<float>& vertices, std::vector<unsigned short>& indices)
{
    for (int lat = 0; lat <= latitudeBands; ++lat) {
        const float theta = static_cast<float>(lat) * pi / static_cast<float>(latitudeBands);
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (int lon = 0; lon <= longitudeBands; ++lon) {
            const float phi =
                static_cast<float>(lon) * 2.0F * pi / static_cast<float>(longitudeBands);
            const float x = std::cos(phi) * sinTheta;
            const float y = cosTheta;
            const float z = std::sin(phi) * sinTheta;
            vertices.insert(vertices.end(), {x, y, z, x, y, z});
        }
    }

    // Counter-clockwise seen from outside, so the widget's GL_BACK culling keeps
    // the outer surface.
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

void drawOrbitingSphere(const CubeWidget* cube)
{
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (orbit == nullptr || context == nullptr) {
        return;
    }

    QMatrix4x4 model;
    model.rotate(orbitTiltDegrees, QVector3D(0.0F, 0.0F, 1.0F));
    model.rotate(cube->m_elapsedSeconds * orbitDegreesPerSecond, QVector3D(0.0F, 1.0F, 0.0F));
    model.translate(orbitRadius, 0.0F, 0.0F);
    model.scale(sphereRadius);

    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, -viewDistance);

    orbit->program.bind();
    orbit->program.setUniformValue("mvp", cube->m_projection * view * model);
    orbit->program.setUniformValue("normalMatrix", model.normalMatrix());
    orbit->program.setUniformValue("baseColor", QVector3D(1.0F, 0.74F, 0.26F));
    orbit->vertexArray.bind();
    context->functions()->glDrawElements(
        GL_TRIANGLES, orbit->indexCount, GL_UNSIGNED_SHORT, nullptr);
    orbit->vertexArray.release();
    orbit->program.release();
}

void removeSphere(CubeWidget* cube, const RuntimeAgentHostV1* host)
{
    if (orbit == nullptr) {
        const QJsonObject result{
            {QStringLiteral("removed"), false},
            {QStringLiteral("note"), QStringLiteral("no sphere was installed")},
        };
        host->complete_json(host->invocation_context,
                            QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        return;
    }

    QObject::disconnect(drawConnection);
    orbit->vertexArray.destroy();
    orbit->vertexBuffer.destroy();
    orbit->indexBuffer.destroy();
    delete orbit;
    orbit = nullptr;
    cube->update();

    const QJsonObject result{{QStringLiteral("removed"), true}};
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
    if (request.value(QStringLiteral("restore")).toBool()
        || request.value(QStringLiteral("remove")).toBool()) {
        removeSphere(cube, host);
        return;
    }

    if (orbit != nullptr) {
        const QJsonObject already{
            {QStringLiteral("installed"), true},
            {QStringLiteral("note"), QStringLiteral("the orbiting sphere was already running")},
        };
        host->complete_json(host->invocation_context,
                            QJsonDocument(already).toJson(QJsonDocument::Compact).constData());
        return;
    }

    auto* state = new OrbitState();
    if (!state->program.addShaderFromSourceCode(QOpenGLShader::Vertex, sphereVertexShader)
        || !state->program.addShaderFromSourceCode(QOpenGLShader::Fragment, sphereFragmentShader)
        || !state->program.link()) {
        const QByteArray log = state->program.log().toLocal8Bit();
        delete state;
        host->fail(host->invocation_context, log.constData());
        return;
    }

    std::vector<float> vertices;
    std::vector<unsigned short> indices;
    buildSphere(vertices, indices);
    state->indexCount = static_cast<int>(indices.size());

    state->vertexArray.create();
    state->vertexArray.bind();

    state->vertexBuffer.create();
    state->vertexBuffer.bind();
    state->vertexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    state->vertexBuffer.allocate(vertices.data(),
                                 static_cast<int>(vertices.size() * sizeof(float)));

    state->indexBuffer.create();
    state->indexBuffer.bind();
    state->indexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    state->indexBuffer.allocate(indices.data(),
                                static_cast<int>(indices.size() * sizeof(unsigned short)));

    state->program.bind();
    state->program.enableAttributeArray(0);
    state->program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
    state->program.enableAttributeArray(1);
    state->program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
    state->program.release();

    state->vertexArray.release();
    state->vertexBuffer.release();

    orbit = state;
    drawConnection = QObject::connect(
        cube, &CubeWidget::frameRendered, cube,
        [cube](qulonglong, float) { drawOrbitingSphere(cube); },
        Qt::DirectConnection);

    const QJsonObject result{
        {QStringLiteral("installed"), true},
        {QStringLiteral("connectionValid"), static_cast<bool>(drawConnection)},
        {QStringLiteral("vertices"), static_cast<qint64>(vertices.size() / 6)},
        {QStringLiteral("indices"), state->indexCount},
        {QStringLiteral("orbitRadius"), orbitRadius},
        {QStringLiteral("sphereRadius"), sphereRadius},
        {QStringLiteral("orbitDegreesPerSecond"), orbitDegreesPerSecond},
    };
    host->complete_json(host->invocation_context,
                        QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}

const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1,
    sizeof(RuntimeAgentSnippetV1),
    "draw a sphere orbiting the cube",
    &run,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1()
{
    return &descriptor;
}
