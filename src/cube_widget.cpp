#include "cube_widget.h"

#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLDebugLogger>
#include <QOpenGLDebugMessage>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

constexpr std::array<float, cubeVertexFloats> cubeVertices{
    // position              // per-face color
    // front
    -1, -1,  1,             1, 0, 0,
     1, -1,  1,             1, 0, 0,
     1,  1,  1,             1, 0, 0,
    -1, -1,  1,             1, 0, 0,
     1,  1,  1,             1, 0, 0,
    -1,  1,  1,             1, 0, 0,
    // back
     1, -1, -1,             0, 1, 1,
    -1, -1, -1,             0, 1, 1,
    -1,  1, -1,             0, 1, 1,
     1, -1, -1,             0, 1, 1,
    -1,  1, -1,             0, 1, 1,
     1,  1, -1,             0, 1, 1,
    // left
    -1, -1, -1,             0, 1, 0,
    -1, -1,  1,             0, 1, 0,
    -1,  1,  1,             0, 1, 0,
    -1, -1, -1,             0, 1, 0,
    -1,  1,  1,             0, 1, 0,
    -1,  1, -1,             0, 1, 0,
    // right
     1, -1,  1,             1, 0, 1,
     1, -1, -1,             1, 0, 1,
     1,  1, -1,             1, 0, 1,
     1, -1,  1,             1, 0, 1,
     1,  1, -1,             1, 0, 1,
     1,  1,  1,             1, 0, 1,
    // top
    -1,  1,  1,             1, 1, 0,
     1,  1,  1,             1, 1, 0,
     1,  1, -1,             1, 1, 0,
    -1,  1,  1,             1, 1, 0,
     1,  1, -1,             1, 1, 0,
    -1,  1, -1,             1, 1, 0,
    // bottom
    -1, -1, -1,             0, 0, 1,
     1, -1, -1,             0, 0, 1,
     1, -1,  1,             0, 0, 1,
    -1, -1, -1,             0, 0, 1,
     1, -1,  1,             0, 0, 1,
    -1, -1,  1,             0, 0, 1,
};

constexpr auto vertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

uniform mat4 mvp;
out vec3 vertexColor;

void main()
{
    vertexColor = inColor;
    gl_Position = mvp * vec4(inPosition, 1.0);
}
)glsl";

constexpr auto fragmentShader = R"glsl(
#version 330 core
in vec3 vertexColor;
uniform vec3 tint;
out vec4 fragmentColor;

void main()
{
    fragmentColor = vec4(clamp(vertexColor * tint, 0.0, 1.0), 1.0);
}
)glsl";

} // namespace

CubeWidget::CubeWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setObjectName(QStringLiteral("cubeView"));
    setMinimumSize(520, 420);
    setFocusPolicy(Qt::StrongFocus);

    m_timer.setObjectName(QStringLiteral("cubeAnimationTimer"));
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, &CubeWidget::advanceAnimation);

    m_renderQueueWatchdog.setObjectName(QStringLiteral("renderQueueWatchdog"));
    m_renderQueueWatchdog.setSingleShot(true);
    m_renderQueueWatchdog.setInterval(100);
    connect(&m_renderQueueWatchdog, &QTimer::timeout,
            this, &CubeWidget::drainRenderQueueIfStalled);

    m_clock.start();
    m_lastTickNanoseconds = m_clock.nsecsElapsed();
    m_timer.start();
}

CubeWidget::~CubeWidget()
{
    if (context() != nullptr) {
        makeCurrent();
        if (m_debugLogger != nullptr) {
            m_debugLogger->stopLogging();
            delete m_debugLogger;
            m_debugLogger = nullptr;
        }
        m_vertexArray.destroy();
        m_vertexBuffer.destroy();
        doneCurrent();
    }
}

void CubeWidget::setAngleDegrees(const float angle)
{
    const float normalized = normalizeAngle(angle);
    if (qFuzzyCompare(m_angleDegrees, normalized)) {
        return;
    }
    m_angleDegrees = normalized;
    emit stateChanged();
    update();
}

void CubeWidget::setAngularVelocity(const float degreesPerSecond)
{
    const float clamped = std::clamp(degreesPerSecond, -1440.0F, 1440.0F);
    if (qFuzzyCompare(m_angularVelocity, clamped)) {
        return;
    }
    m_angularVelocity = clamped;
    emit stateChanged();
}

void CubeWidget::setRunning(const bool running)
{
    if (running == m_timer.isActive()) {
        return;
    }
    if (running) {
        m_lastTickNanoseconds = m_clock.nsecsElapsed();
        m_timer.start();
    } else {
        m_timer.stop();
    }
    emit runningChanged(running);
    emit stateChanged();
}

void CubeWidget::setWireframe(const bool wireframe)
{
    if (m_wireframe == wireframe) {
        return;
    }
    m_wireframe = wireframe;
    emit wireframeChanged(wireframe);
    emit stateChanged();
    update();
}

void CubeWidget::installDispatchStep(CubeStepFunctionV1 function, const QString& patchName)
{
    if (function == nullptr) {
        function = &cube_step_builtin_v1;
    }
    m_stepFunction.store(function, std::memory_order_release);
    setActivePatchLabel(patchName.isEmpty() ? QStringLiteral("dispatch patch") : patchName);
}

void CubeWidget::resetDispatchStep()
{
    m_stepFunction.store(&cube_step_builtin_v1, std::memory_order_release);
    setActivePatchLabel(QStringLiteral("builtin"));
}

void CubeWidget::setActivePatchLabel(const QString& label)
{
    if (m_activePatch == label) {
        return;
    }
    m_activePatch = label;
    emit activePatchChanged(m_activePatch);
    emit stateChanged();
}

void CubeWidget::enqueueRenderCallback(std::function<void()> callback)
{
    if (!callback) {
        return;
    }
    {
        QMutexLocker lock(&m_renderQueueMutex);
        m_renderQueue.emplace_back(std::move(callback));
    }

    const auto request = [this] {
        update();
        m_renderQueueWatchdog.start();
    };
    if (QThread::currentThread() == thread()) {
        request();
    } else {
        QMetaObject::invokeMethod(this, request, Qt::QueuedConnection);
    }
}

// update() only asks for a repaint, and a window the compositor has stopped
// sending frame callbacks to, whether covered, unfocused or on another
// workspace, may never get one. Queued render work would then sit unfinished for as long
// as the window stays hidden, which the caller sees as its request timing out
// even though the snippet loaded correctly.
//
// grabFramebuffer() renders through an FBO whatever the compositor is doing, so
// one forced frame drains the whole queue. It only runs when a repaint has
// failed to arrive on its own, so a visible window never reaches this.
void CubeWidget::drainRenderQueueIfStalled()
{
    bool pending = false;
    {
        QMutexLocker lock(&m_renderQueueMutex);
        pending = !m_renderQueue.empty();
    }
    if (!pending || !isValid()) {
        return;
    }
    (void)grabFramebuffer();
}

bool CubeWidget::captureFramebuffer(const QString& path, QString* error)
{
    if (!isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("the OpenGL widget does not yet have a valid context");
        }
        return false;
    }

    const QImage image = grabFramebuffer();
    if (image.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("grabFramebuffer returned a null image");
        }
        return false;
    }
    if (!image.save(path, "PNG")) {
        if (error != nullptr) {
            *error = QStringLiteral("could not save PNG to %1").arg(path);
        }
        return false;
    }
    return true;
}

void CubeWidget::resetCube()
{
    m_angleDegrees = 24.0F;
    m_angularVelocity = 75.0F;
    m_tint = QVector3D(1.0F, 1.0F, 1.0F);
    m_scale = 1.0F;
    emit stateChanged();
    update();
}

void CubeWidget::increaseSpeed()
{
    setAngularVelocity(m_angularVelocity * 1.35F);
}

void CubeWidget::decreaseSpeed()
{
    setAngularVelocity(m_angularVelocity / 1.35F);
}

void CubeWidget::toggleRunning()
{
    setRunning(!isRunning());
}

void CubeWidget::initializeGL()
{
    initializeOpenGLFunctions();
    const auto glString = [this](const GLenum name) {
        const GLubyte* value = glGetString(name);
        return QString::fromLatin1(
            value != nullptr ? reinterpret_cast<const char*>(value) : "unknown");
    };
    m_glVendor = glString(GL_VENDOR);
    m_glRenderer = glString(GL_RENDERER);
    m_glVersion = glString(GL_VERSION);

    m_debugLogger = new QOpenGLDebugLogger(this);
    if (m_debugLogger->initialize()) {
        connect(m_debugLogger, &QOpenGLDebugLogger::messageLogged,
                this, [this](const QOpenGLDebugMessage& message) {
                    emit glMessage(message.message(),
                                   static_cast<int>(message.severity()),
                                   static_cast<int>(message.source()),
                                   static_cast<int>(message.type()),
                                   message.id());
                });
        m_debugLogger->startLogging(QOpenGLDebugLogger::AsynchronousLogging);
        m_debugLogger->enableMessages();
    } else {
        delete m_debugLogger;
        m_debugLogger = nullptr;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glClearColor(0.055F, 0.065F, 0.085F, 1.0F);

    initializeShaders();
    initializeGeometry();
    emit glInitialized();
}

void CubeWidget::resizeGL(const int width, const int height)
{
    m_projection.setToIdentity();
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0F;
    m_projection.perspective(45.0F, aspect, 0.1F, 100.0F);
}

void CubeWidget::paintGL()
{
    std::vector<std::function<void()>> callbacks;
    {
        QMutexLocker lock(&m_renderQueueMutex);
        callbacks.swap(m_renderQueue);
    }
    for (auto& callback : callbacks) {
        callback();
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, m_wireframe ? GL_LINE : GL_FILL);

    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, -5.4F);

    QMatrix4x4 model;
    model.rotate(m_angleDegrees, QVector3D(0.72F, 1.0F, 0.31F));
    model.scale(m_scale);

    m_program.bind();
    m_program.setUniformValue("mvp", m_projection * view * model);
    m_program.setUniformValue("tint", m_tint);
    m_vertexArray.bind();
    // Still a constant baked into this instruction, which is what makes a face
    // undrawable from outside without rewriting its vertices. It is now the
    // same constant the snippets read, so changing the mesh cannot leave them
    // quietly addressing the old layout.
    glDrawArrays(GL_TRIANGLES, 0, cubeVertexCount);
    m_vertexArray.release();
    m_program.release();

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    emit frameIndexChanged(m_frameIndex);
    emit frameRendered(m_frameIndex, m_angleDegrees);
}

void CubeWidget::advanceAnimation()
{
    const qint64 now = m_clock.nsecsElapsed();
    float delta = static_cast<float>(now - m_lastTickNanoseconds) / 1'000'000'000.0F;
    m_lastTickNanoseconds = now;
    delta = std::clamp(delta, 0.0F, 0.1F);
    m_elapsedSeconds += delta;
    ++m_frameIndex;

    const CubeStepInputV1 input{
        m_angleDegrees,
        m_angularVelocity,
        delta,
        m_elapsedSeconds,
        m_frameIndex,
    };
    const CubeStepFunctionV1 step = m_stepFunction.load(std::memory_order_acquire);
    const CubeStepOutputV1 output = step != nullptr
        ? step(&input)
        : cube_step_builtin_v1(&input);

    m_angleDegrees = normalizeAngle(output.angle_degrees);
    m_tint = QVector3D(output.tint_r, output.tint_g, output.tint_b);
    m_scale = std::clamp(output.scale, 0.05F, 8.0F);

    // Do not emit a high-frequency generic stateChanged signal every frame. The
    // frameRendered event is the explicit high-rate channel and the agent
    // throttles its publication by default.
    update();
}

void CubeWidget::initializeGeometry()
{
    m_vertexArray.create();
    m_vertexArray.bind();

    m_vertexBuffer.create();
    m_vertexBuffer.bind();
    m_vertexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    m_vertexBuffer.allocate(cubeVertices.data(),
                            static_cast<int>(cubeVertices.size() * sizeof(float)));

    m_program.bind();
    m_program.enableAttributeArray(0);
    m_program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
    m_program.enableAttributeArray(1);
    m_program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
    m_program.release();

    m_vertexBuffer.release();
    m_vertexArray.release();
}

void CubeWidget::initializeShaders()
{
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)) {
        qFatal("vertex shader compilation failed: %s", qPrintable(m_program.log()));
    }
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)) {
        qFatal("fragment shader compilation failed: %s", qPrintable(m_program.log()));
    }
    if (!m_program.link()) {
        qFatal("shader link failed: %s", qPrintable(m_program.log()));
    }
}

float CubeWidget::normalizeAngle(float angle) noexcept
{
    angle = std::fmod(angle, 360.0F);
    return angle < 0.0F ? angle + 360.0F : angle;
}
