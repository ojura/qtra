#pragma once

#include "demo/cube_step_abi.h"

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QTimer>
#include <QVector3D>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

class QOpenGLDebugLogger;

// The layout of the vertex buffer initializeGeometry() fills: six faces, each
// six vertices, each a position and a colour. Snippets that displace part of
// that buffer depend on these numbers exactly, and were restating them — five
// spellings across three files, none of them next to the array they described.
// The widget owns the buffer, so it states the layout.
inline constexpr int cubeFaceCount = 6;
inline constexpr int cubeVerticesPerFace = 6;
inline constexpr int cubeFloatsPerVertex = 6;
inline constexpr int cubeFloatsPerFace = cubeVerticesPerFace * cubeFloatsPerVertex;
inline constexpr int cubeVertexCount = cubeFaceCount * cubeVerticesPerFace;
inline constexpr int cubeVertexFloats = cubeFaceCount * cubeFloatsPerFace;

class CubeWidget final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
    Q_PROPERTY(float angleDegrees READ angleDegrees WRITE setAngleDegrees NOTIFY stateChanged)
    Q_PROPERTY(float angularVelocity READ angularVelocity WRITE setAngularVelocity NOTIFY stateChanged)
    Q_PROPERTY(bool running READ isRunning WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(bool wireframe READ isWireframe WRITE setWireframe NOTIFY wireframeChanged)
    Q_PROPERTY(qulonglong frameIndex READ frameIndex NOTIFY frameIndexChanged)
    Q_PROPERTY(QString activePatch READ activePatch NOTIFY activePatchChanged)
    Q_PROPERTY(QString glVendor READ glVendor NOTIFY glInitialized)
    Q_PROPERTY(QString glRenderer READ glRenderer NOTIFY glInitialized)
    Q_PROPERTY(QString glVersion READ glVersion NOTIFY glInitialized)

public:
    explicit CubeWidget(QWidget* parent = nullptr);
    ~CubeWidget() override;

    [[nodiscard]] float angleDegrees() const noexcept { return m_angleDegrees; }
    [[nodiscard]] float angularVelocity() const noexcept { return m_angularVelocity; }
    [[nodiscard]] bool isRunning() const noexcept { return m_timer.isActive(); }
    [[nodiscard]] bool isWireframe() const noexcept { return m_wireframe; }
    [[nodiscard]] qulonglong frameIndex() const noexcept { return m_frameIndex; }
    [[nodiscard]] QString activePatch() const { return m_activePatch; }
    [[nodiscard]] QString glVendor() const { return m_glVendor; }
    [[nodiscard]] QString glRenderer() const { return m_glRenderer; }
    [[nodiscard]] QString glVersion() const { return m_glVersion; }

    void setAngleDegrees(float angle);
    void setAngularVelocity(float degreesPerSecond);
    void setRunning(bool running);
    void setWireframe(bool wireframe);

    // Portable patch seam: one atomic load and indirect call per animation tick.
    void installDispatchStep(CubeStepFunctionV1 function, const QString& patchName);
    void resetDispatchStep();

    void setActivePatchLabel(const QString& label);

    // Runs the callback from paintGL() while this widget's context is current.
    // Loaded snippet DSOs are deliberately retained for the process lifetime.
    void enqueueRenderCallback(std::function<void()> callback);

    [[nodiscard]] bool captureFramebuffer(const QString& path, QString* error = nullptr);

public slots:
    void resetCube();
    void increaseSpeed();
    void decreaseSpeed();
    void toggleRunning();

signals:
    void stateChanged();
    void runningChanged(bool running);
    void wireframeChanged(bool wireframe);
    void activePatchChanged(const QString& name);
    void glInitialized();
    void glMessage(const QString& message,
                   int severity,
                   int source,
                   int type,
                   quint32 id);
    void frameIndexChanged(qulonglong frameIndex);
    void frameRendered(qulonglong frameIndex, float angleDegrees);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

private slots:
    void advanceAnimation();

private:
    void initializeGeometry();
    void initializeShaders();
    void drainRenderQueueIfStalled();
    static float normalizeAngle(float angle) noexcept;

    QTimer m_timer;
    QTimer m_renderQueueWatchdog;
    QElapsedTimer m_clock;
    qint64 m_lastTickNanoseconds = 0;
    float m_elapsedSeconds = 0.0F;

    // These fields are intentionally interesting targets for -fno-access-control
    // snippets. Changing them directly can bypass signals and invariants.
    float m_angleDegrees = 24.0F;
    float m_angularVelocity = 75.0F;
    bool m_wireframe = false;
    qulonglong m_frameIndex = 0;
    QVector3D m_tint{1.0F, 1.0F, 1.0F};
    float m_scale = 1.0F;
    QString m_activePatch = QStringLiteral("builtin");

    std::atomic<CubeStepFunctionV1> m_stepFunction{&cube_step_builtin_v1};

    QOpenGLShaderProgram m_program;
    QOpenGLDebugLogger* m_debugLogger = nullptr;
    QOpenGLBuffer m_vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject m_vertexArray;
    QMatrix4x4 m_projection;
    QString m_glVendor;
    QString m_glRenderer;
    QString m_glVersion;

    QMutex m_renderQueueMutex;
    std::vector<std::function<void()>> m_renderQueue;
};
