#pragma once

// The tesseract's control panel, docked beside the cube.
//
// Widgets and nothing else, like the kangaroo planet's panel: what the
// controls do and what the readouts show come from the snippet through
// Controls. Every control has an object name beginning with "tesseract", so
// the semantic API can press its buttons and move its sliders too.

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <cmath>
#include <functional>

namespace tesseract_panel {

inline constexpr int planes = 6;
inline const std::array<QString, planes> planeLabels{
    QStringLiteral("xy"), QStringLiteral("xz"), QStringLiteral("xw"),
    QStringLiteral("yz"), QStringLiteral("yw"), QStringLiteral("zw"),
};

struct Snapshot {
    bool installed = false;
    int view = 0;      // 0 perspective, 1 stereographic, 2 slice
    int direction = 3; // 0 cell, 1 face, 2 edge, 3 corner, 4 turning
    bool sweep = true;
    float position = 0.0F; // -1 to 1, where the cut is now
    QString shape;         // what the slice is, in the slice view
    std::array<float, planes> rates{};
    std::array<float, planes> turned{}; // degrees, for the readout
    bool faces = true;
    bool corners = true;
    bool events = true;
};

struct Controls {
    std::function<Snapshot()> snapshot;
    std::function<void(int)> setView;
    std::function<void(int)> setDirection;
    std::function<void(bool)> setSweep;
    std::function<void(float)> setPosition;
    std::function<void(int, float)> setRate;
    std::function<void(bool)> setFaces;
    std::function<void(bool)> setCorners;
    std::function<void(bool)> setEvents;
    std::function<void()> resetCamera;
    std::function<void()> remove;
};

namespace detail {

inline QPushButton* button(const QString& text, const QString& name, const QString& tip)
{
    auto* b = new QPushButton(text);
    b->setObjectName(name);
    b->setToolTip(tip);
    return b;
}

inline void sync(QSlider* s, const int value)
{
    if (!s->isSliderDown() && s->value() != value) {
        const QSignalBlocker blocker(s);
        s->setValue(value);
    }
}

inline void sync(QCheckBox* c, const bool value)
{
    if (c->isChecked() != value) {
        const QSignalBlocker blocker(c);
        c->setChecked(value);
    }
}

inline void sync(QComboBox* c, const int index)
{
    if (c->currentIndex() != index) {
        const QSignalBlocker blocker(c);
        c->setCurrentIndex(index);
    }
}

} // namespace detail

inline QDockWidget* create(QMainWindow* window, Controls controls)
{
    using namespace detail;

    auto* dock = new QDockWidget(QStringLiteral("Tesseract"), window);
    dock->setObjectName(QStringLiteral("tesseractPanel"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* scroll = new QScrollArea(dock);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    // ---- how it is shown ----------------------------------------------------
    auto* viewBox = new QGroupBox(QStringLiteral("View"));
    auto* viewForm = new QFormLayout(viewBox);
    auto* view = new QComboBox;
    view->setObjectName(QStringLiteral("tesseractViewCombo"));
    view->addItems({QStringLiteral("Perspective: a cube within a cube"),
                    QStringLiteral("Stereographic: edges as arcs"), QStringLiteral("Slice: what 3D sees")});
    viewForm->addRow(view);
    auto* faces = new QCheckBox(QStringLiteral("Cells"));
    faces->setObjectName(QStringLiteral("tesseractFacesCheck"));
    faces->setToolTip(QStringLiteral("The eight cells as tinted glass, each its own colour"));
    auto* corners = new QCheckBox(QStringLiteral("Corners"));
    corners->setObjectName(QStringLiteral("tesseractCornersCheck"));
    auto* events = new QCheckBox(QStringLiteral("Events"));
    events->setObjectName(QStringLiteral("tesseractEventsCheck"));
    events->setToolTip(QStringLiteral("A tesseract.slice event each time the slice becomes a different solid"));
    auto* checks = new QHBoxLayout;
    checks->addWidget(faces);
    checks->addWidget(corners);
    checks->addWidget(events);
    viewForm->addRow(checks);
    layout->addWidget(viewBox);

    // ---- the slice ------------------------------------------------------------
    auto* sliceBox = new QGroupBox(QStringLiteral("Slice"));
    auto* sliceForm = new QFormLayout(sliceBox);
    auto* direction = new QComboBox;
    direction->setObjectName(QStringLiteral("tesseractDirectionCombo"));
    direction->addItems({QStringLiteral("Cell first"), QStringLiteral("Face first"), QStringLiteral("Edge first"),
                         QStringLiteral("Corner first"), QStringLiteral("Turning with the rotation")});
    direction->setToolTip(QStringLiteral("Which part of the tesseract meets the cutting space first"));
    sliceForm->addRow(QStringLiteral("Cut"), direction);
    auto* sweep = new QCheckBox(QStringLiteral("Sweep through it"));
    sweep->setObjectName(QStringLiteral("tesseractSweepCheck"));
    sliceForm->addRow(sweep);
    auto* position = new QSlider(Qt::Horizontal);
    position->setObjectName(QStringLiteral("tesseractPositionSlider"));
    position->setRange(-100, 100);
    position->setToolTip(QStringLiteral("Where the cut is, from one end of the tesseract to the other"));
    sliceForm->addRow(QStringLiteral("Where"), position);
    auto* shape = new QLabel;
    shape->setObjectName(QStringLiteral("tesseractShapeLabel"));
    shape->setWordWrap(true);
    sliceForm->addRow(shape);
    layout->addWidget(sliceBox);

    // ---- the rotation ---------------------------------------------------------
    auto* turnBox = new QGroupBox(QStringLiteral("Turning through four dimensions"));
    auto* turnLayout = new QGridLayout(turnBox);
    auto* turnHint = new QLabel(QStringLiteral(
        "Each plane turns at this many times the cube's own spin, so Ctrl++ and Ctrl+- speed it up and "
        "slow it down. Planes with w in them turn the tesseract inside out."));
    turnHint->setWordWrap(true);
    turnLayout->addWidget(turnHint, 0, 0, 1, 3);
    std::array<QSlider*, planes> rates{};
    std::array<QLabel*, planes> rateValues{};
    for (int p = 0; p < planes; ++p) {
        auto* label = new QLabel(planeLabels[p]);
        auto* s = new QSlider(Qt::Horizontal);
        s->setObjectName(QStringLiteral("tesseractRate%1Slider").arg(planeLabels[p].toUpper()));
        s->setRange(-20, 20);
        auto* value = new QLabel;
        value->setMinimumWidth(36);
        turnLayout->addWidget(label, p + 1, 0);
        turnLayout->addWidget(s, p + 1, 1);
        turnLayout->addWidget(value, p + 1, 2);
        rates[p] = s;
        rateValues[p] = value;
    }
    auto* clifford = button(QStringLiteral("Double rotation"), QStringLiteral("tesseractCliffordButton"),
                            QStringLiteral("xw and yz together at the same rate: every point moves, nothing is still"));
    auto* simple = button(QStringLiteral("Through w only"), QStringLiteral("tesseractSimpleButton"),
                          QStringLiteral("xw alone: the cells pass through each other, inner to outer"));
    auto* still = button(QStringLiteral("Hold still"), QStringLiteral("tesseractStillButton"),
                         QStringLiteral("No turning in four dimensions"));
    auto* presets = new QHBoxLayout;
    presets->addWidget(clifford);
    presets->addWidget(simple);
    presets->addWidget(still);
    turnLayout->addLayout(presets, planes + 1, 0, 1, 3);
    auto* turned = new QLabel;
    turned->setObjectName(QStringLiteral("tesseractTurnedLabel"));
    turned->setWordWrap(true);
    turnLayout->addWidget(turned, planes + 2, 0, 1, 3);
    layout->addWidget(turnBox);

    // ---- camera and removal -------------------------------------------------
    auto* cameraBox = new QGroupBox(QStringLiteral("Camera"));
    auto* cameraLayout = new QVBoxLayout(cameraBox);
    auto* resetView = button(QStringLiteral("Reset the camera"), QStringLiteral("tesseractCameraResetButton"),
                             QStringLiteral("Back to the window's own view"));
    auto* hint = new QLabel(QStringLiteral("Drag the cube to orbit, scroll to zoom, double-click to reset."));
    hint->setWordWrap(true);
    cameraLayout->addWidget(resetView);
    cameraLayout->addWidget(hint);
    layout->addWidget(cameraBox);
    auto* remove = button(QStringLiteral("Take the tesseract off"), QStringLiteral("tesseractRemoveButton"),
                          QStringLiteral("Puts the cube back as it was"));
    layout->addWidget(remove);
    layout->addStretch(1);
    scroll->setWidget(body);
    dock->setWidget(scroll);

    // ---- wiring ---------------------------------------------------------------
    QObject::connect(view, &QComboBox::currentIndexChanged, dock, [controls](const int i) { controls.setView(i); });
    QObject::connect(direction, &QComboBox::currentIndexChanged, dock,
                     [controls](const int i) { controls.setDirection(i); });
    QObject::connect(sweep, &QCheckBox::toggled, dock, [controls](const bool on) { controls.setSweep(on); });
    QObject::connect(position, &QSlider::valueChanged, dock,
                     [controls](const int v) { controls.setPosition(static_cast<float>(v) / 100.0F); });
    QObject::connect(faces, &QCheckBox::toggled, dock, [controls](const bool on) { controls.setFaces(on); });
    QObject::connect(corners, &QCheckBox::toggled, dock, [controls](const bool on) { controls.setCorners(on); });
    QObject::connect(events, &QCheckBox::toggled, dock, [controls](const bool on) { controls.setEvents(on); });
    for (int p = 0; p < planes; ++p) {
        QObject::connect(rates[p], &QSlider::valueChanged, dock, [controls, p, rateValues](const int v) {
            rateValues[p]->setText(QString::number(v / 10.0, 'f', 1));
            controls.setRate(p, static_cast<float>(v) / 10.0F);
        });
    }
    const auto preset = [controls](const std::array<float, planes> values) {
        for (int p = 0; p < planes; ++p) {
            controls.setRate(p, values[p]);
        }
    };
    QObject::connect(clifford, &QPushButton::clicked, dock, [preset] { preset({0, 0, 1, 1, 0, 0}); });
    QObject::connect(simple, &QPushButton::clicked, dock, [preset] { preset({0, 0, 1, 0, 0, 0}); });
    QObject::connect(still, &QPushButton::clicked, dock, [preset] { preset({0, 0, 0, 0, 0, 0}); });
    QObject::connect(resetView, &QPushButton::clicked, dock, [controls] { controls.resetCamera(); });
    QObject::connect(remove, &QPushButton::clicked, dock, [controls] { controls.remove(); });

    auto* timer = new QTimer(dock);
    timer->setInterval(250);
    const auto refresh = [=] {
        const Snapshot now = controls.snapshot();
        sync(view, now.view);
        sync(direction, now.direction);
        sync(sweep, now.sweep);
        sync(position, static_cast<int>(std::lround(now.position * 100.0F)));
        sync(faces, now.faces);
        sync(corners, now.corners);
        sync(events, now.events);
        for (int p = 0; p < planes; ++p) {
            sync(rates[p], static_cast<int>(std::lround(now.rates[p] * 10.0F)));
            rateValues[p]->setText(QString::number(now.rates[p], 'f', 1));
        }
        sliceBox->setEnabled(now.view == 2);
        shape->setText(now.view == 2 ? QStringLiteral("Now: %1").arg(now.shape)
                                     : QStringLiteral("Choose the slice view to cut it."));
        QStringList parts;
        for (int p = 0; p < planes; ++p) {
            if (now.rates[p] != 0.0F) {
                parts << QStringLiteral("%1 %2°").arg(planeLabels[p]).arg(std::fmod(now.turned[p], 360.0F), 0, 'f', 0);
            }
        }
        turned->setText(parts.isEmpty() ? QStringLiteral("Held still in four dimensions.")
                                        : QStringLiteral("Turned so far: ") + parts.join(QStringLiteral(", ")));
        if (!now.installed) {
            turned->setText(QStringLiteral("The tesseract is off."));
        }
    };
    QObject::connect(timer, &QTimer::timeout, dock, refresh);
    timer->start();
    refresh();

    window->addDockWidget(Qt::RightDockWidgetArea, dock);
    return dock;
}

} // namespace tesseract_panel
