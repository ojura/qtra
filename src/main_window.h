#pragma once

#include <QFutureWatcher>
#include <QMainWindow>

#include <atomic>
#include <cstdint>

class QAction;
class QLabel;
class QProgressBar;
class CubeWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
    Q_PROPERTY(QString agentSocket READ agentSocket WRITE setAgentSocket NOTIFY agentSocketChanged)
    Q_PROPERTY(bool demoJobRunning READ demoJobRunning NOTIFY demoJobRunningChanged)

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] CubeWidget* cubeWidget() const noexcept { return m_cube; }
    [[nodiscard]] QString agentSocket() const { return m_agentSocket; }
    [[nodiscard]] bool demoJobRunning() const noexcept { return m_jobWatcher.isRunning(); }

    void setAgentSocket(const QString& socket);

public slots:
    void startDemoJob();
    void cancelDemoJob();

signals:
    void agentSocketChanged(const QString& socket);
    void demoJobRunningChanged(bool running);
    void demoJobStarted(qulonglong operationId, const QString& name);
    void demoJobProgress(qulonglong operationId, int percent);
    void demoJobFinished(qulonglong operationId, const QString& outcome);

private:
    void createMenus();
    void updateStatus();
    void showAgentInformation();
    void saveScreenshotFromMenu();

    CubeWidget* m_cube = nullptr;
    QLabel* m_angleLabel = nullptr;
    QLabel* m_speedLabel = nullptr;
    QLabel* m_patchLabel = nullptr;
    QLabel* m_socketLabel = nullptr;
    QProgressBar* m_jobProgress = nullptr;

    QAction* m_pauseAction = nullptr;
    QAction* m_wireframeAction = nullptr;
    QAction* m_startJobAction = nullptr;
    QAction* m_cancelJobAction = nullptr;

    QString m_agentSocket;
    QFutureWatcher<void> m_jobWatcher;
    std::atomic_bool m_cancelRequested{false};
    qulonglong m_nextOperationId = 1;
    qulonglong m_activeOperationId = 0;
    QString m_jobOutcome;
};
