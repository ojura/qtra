#include "main_window.h"

#include "cube_widget.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QFuture>
#include <QLabel>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QStatusBar>
#include <QThread>
#include <QtConcurrentRun>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_cube(new CubeWidget(this))
{
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("Qt Runtime Agent / Hotpatch Cube"));
    resize(920, 700);
    setCentralWidget(m_cube);

    createMenus();

    m_angleLabel = new QLabel(this);
    m_angleLabel->setObjectName(QStringLiteral("angleStatus"));
    m_speedLabel = new QLabel(this);
    m_speedLabel->setObjectName(QStringLiteral("speedStatus"));
    m_patchLabel = new QLabel(this);
    m_patchLabel->setObjectName(QStringLiteral("patchStatus"));
    m_socketLabel = new QLabel(this);
    m_socketLabel->setObjectName(QStringLiteral("socketStatus"));
    m_socketLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_jobProgress = new QProgressBar(this);
    m_jobProgress->setObjectName(QStringLiteral("jobProgress"));
    m_jobProgress->setRange(0, 100);
    m_jobProgress->setValue(0);
    m_jobProgress->setMaximumWidth(180);
    m_jobProgress->hide();

    statusBar()->addPermanentWidget(m_angleLabel);
    statusBar()->addPermanentWidget(m_speedLabel);
    statusBar()->addPermanentWidget(m_patchLabel);
    statusBar()->addPermanentWidget(m_jobProgress);
    statusBar()->addPermanentWidget(m_socketLabel, 1);

    connect(m_cube, &CubeWidget::frameRendered, this, [this](qulonglong, float) {
        updateStatus();
    });
    connect(m_cube, &CubeWidget::stateChanged, this, &MainWindow::updateStatus);
    connect(m_cube, &CubeWidget::runningChanged, this, [this](const bool running) {
        m_pauseAction->setChecked(!running);
        updateStatus();
    });
    connect(m_cube, &CubeWidget::wireframeChanged,
            m_wireframeAction, &QAction::setChecked);
    connect(m_cube, &CubeWidget::activePatchChanged,
            this, [this] { updateStatus(); });

    connect(&m_jobWatcher, &QFutureWatcher<void>::finished, this, [this] {
        m_jobProgress->setValue(100);
        m_jobProgress->hide();
        m_startJobAction->setEnabled(true);
        m_cancelJobAction->setEnabled(false);
        emit demoJobRunningChanged(false);
        emit demoJobFinished(m_activeOperationId, m_jobOutcome);
        statusBar()->showMessage(
            QStringLiteral("Demo job %1: %2").arg(m_activeOperationId).arg(m_jobOutcome),
            5000);
    });

    updateStatus();
}

MainWindow::~MainWindow()
{
    m_cancelRequested.store(true, std::memory_order_release);
    m_jobWatcher.waitForFinished();
}

void MainWindow::setAgentSocket(const QString& socket)
{
    if (m_agentSocket == socket) {
        return;
    }
    m_agentSocket = socket;
    emit agentSocketChanged(m_agentSocket);
    updateStatus();
}

void MainWindow::startDemoJob()
{
    if (m_jobWatcher.isRunning()) {
        return;
    }

    m_cancelRequested.store(false, std::memory_order_release);
    m_activeOperationId = m_nextOperationId++;
    m_jobOutcome = QStringLiteral("completed");
    m_jobProgress->setValue(0);
    m_jobProgress->show();
    m_startJobAction->setEnabled(false);
    m_cancelJobAction->setEnabled(true);
    emit demoJobRunningChanged(true);
    emit demoJobStarted(m_activeOperationId, QStringLiteral("fake point-cloud indexing"));

    const qulonglong operationId = m_activeOperationId;
    QFuture<void> future = QtConcurrent::run([this, operationId] {
        for (int step = 1; step <= 20; ++step) {
            if (m_cancelRequested.load(std::memory_order_acquire)) {
                QMetaObject::invokeMethod(this, [this] {
                    m_jobOutcome = QStringLiteral("canceled");
                }, Qt::QueuedConnection);
                return;
            }

            // This stands in for TBB/domain work. The completion and progress
            // protocol is the relevant part of the sample.
            QThread::msleep(55);
            const int percent = step * 5;
            QMetaObject::invokeMethod(this, [this, operationId, percent] {
                m_jobProgress->setValue(percent);
                emit demoJobProgress(operationId, percent);
            }, Qt::QueuedConnection);
        }
    });
    m_jobWatcher.setFuture(future);
}

void MainWindow::cancelDemoJob()
{
    if (m_jobWatcher.isRunning()) {
        m_cancelRequested.store(true, std::memory_order_release);
    }
}

void MainWindow::createMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* screenshotAction = fileMenu->addAction(QStringLiteral("Save &Screenshot…"));
    screenshotAction->setObjectName(QStringLiteral("actionSaveScreenshot"));
    screenshotAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(screenshotAction, &QAction::triggered, this, &MainWindow::saveScreenshotFromMenu);
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    quitAction->setObjectName(QStringLiteral("actionQuit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QCoreApplication::quit);

    QMenu* cubeMenu = menuBar()->addMenu(QStringLiteral("&Cube"));
    m_pauseAction = cubeMenu->addAction(QStringLiteral("&Paused"));
    m_pauseAction->setObjectName(QStringLiteral("actionPause"));
    m_pauseAction->setCheckable(true);
    m_pauseAction->setShortcut(Qt::Key_Space);
    connect(m_pauseAction, &QAction::triggered, m_cube, &CubeWidget::toggleRunning);

    QAction* fasterAction = cubeMenu->addAction(QStringLiteral("&Faster"));
    fasterAction->setObjectName(QStringLiteral("actionFaster"));
    fasterAction->setShortcut(QKeySequence(QStringLiteral("Ctrl++")));
    connect(fasterAction, &QAction::triggered, m_cube, &CubeWidget::increaseSpeed);

    QAction* slowerAction = cubeMenu->addAction(QStringLiteral("&Slower"));
    slowerAction->setObjectName(QStringLiteral("actionSlower"));
    slowerAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+-")));
    connect(slowerAction, &QAction::triggered, m_cube, &CubeWidget::decreaseSpeed);

    QAction* resetAction = cubeMenu->addAction(QStringLiteral("&Reset"));
    resetAction->setObjectName(QStringLiteral("actionResetCube"));
    resetAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    connect(resetAction, &QAction::triggered, m_cube, &CubeWidget::resetCube);

    m_wireframeAction = cubeMenu->addAction(QStringLiteral("&Wireframe"));
    m_wireframeAction->setObjectName(QStringLiteral("actionWireframe"));
    m_wireframeAction->setCheckable(true);
    m_wireframeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(m_wireframeAction, &QAction::toggled, m_cube, &CubeWidget::setWireframe);

    QMenu* taskMenu = menuBar()->addMenu(QStringLiteral("&Tasks"));
    m_startJobAction = taskMenu->addAction(QStringLiteral("Run fake point-cloud job"));
    m_startJobAction->setObjectName(QStringLiteral("actionStartDemoJob"));
    connect(m_startJobAction, &QAction::triggered, this, &MainWindow::startDemoJob);
    m_cancelJobAction = taskMenu->addAction(QStringLiteral("Cancel current job"));
    m_cancelJobAction->setObjectName(QStringLiteral("actionCancelDemoJob"));
    m_cancelJobAction->setEnabled(false);
    connect(m_cancelJobAction, &QAction::triggered, this, &MainWindow::cancelDemoJob);

    QMenu* agentMenu = menuBar()->addMenu(QStringLiteral("&Agent"));
    QAction* infoAction = agentMenu->addAction(QStringLiteral("Runtime agent information"));
    infoAction->setObjectName(QStringLiteral("actionAgentInfo"));
    connect(infoAction, &QAction::triggered, this, &MainWindow::showAgentInformation);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("&About"));
    aboutAction->setObjectName(QStringLiteral("actionAbout"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this,
                           QStringLiteral("Qt Runtime Agent Demo"),
                           QStringLiteral(
                               "An optimized Qt 6/OpenGL process with semantic RPC, "
                               "runtime-compiled C++ snippets, and two hotpatch modes."));
    });
}

void MainWindow::updateStatus()
{
    m_angleLabel->setText(QStringLiteral("angle %1°").arg(m_cube->angleDegrees(), 0, 'f', 1));
    m_speedLabel->setText(QStringLiteral("speed %1°/s").arg(m_cube->angularVelocity(), 0, 'f', 1));
    m_patchLabel->setText(QStringLiteral("patch: %1").arg(m_cube->activePatch()));
    m_socketLabel->setText(m_agentSocket.isEmpty()
        ? QStringLiteral("agent disabled")
        : QStringLiteral("socket: %1").arg(m_agentSocket));
}

void MainWindow::showAgentInformation()
{
    QMessageBox::information(
        this,
        QStringLiteral("Runtime agent"),
        m_agentSocket.isEmpty()
            ? QStringLiteral("The runtime agent is disabled for this process.")
            : QStringLiteral(
                  "Unix socket:\n%1\n\n"
                  "Use tools/agentctl.py to inspect objects, trigger actions, load "
                  "snippets, and activate patches.")
                  .arg(m_agentSocket));
}

void MainWindow::saveScreenshotFromMenu()
{
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save cube framebuffer"),
        QStringLiteral("cube.png"),
        QStringLiteral("PNG images (*.png)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    if (!m_cube->captureFramebuffer(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Capture failed"), error);
    }
}
