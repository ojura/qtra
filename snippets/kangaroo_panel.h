#pragma once

// A control panel for the kangaroo planet, docked beside the cube.
//
// The panel holds widgets and nothing else. What its buttons do and what its
// readouts show come from the snippet through Controls, so the panel can be
// read without knowing anything about the simulation, and the snippet keeps
// every decision about the planet in one place.
//
// Every control has an object name, so the semantic API can operate the panel
// as it operates the rest of the window: action.trigger on a button's
// objectName clicks it, object.set on a slider's "value" moves it.

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <vector>

namespace kangaroo_panel {

struct Row {
    QString name;
    QString role;
    QString doing;
    QString where;
    int hops = 0;
    int flung = 0;
    int wins = 0;
};

// What the panel shows, taken fresh four times a second.
struct Snapshot {
    bool installed = false;
    float setRate = 0.0F;    // degrees per second, the cube's own setting
    float rate = 0.0F;       // what the planet is turning at
    float fromMusic = 0.0F;  // the share of a displaced step function's change, such as the visualizer's
    float fromMob = 0.0F;    // the mob's pushes
    int flung = 0;
    int returned = 0;
    int alarms = 0;
    int bouts = 0;
    float waterHeld = 1.0F;  // fraction of the planet's water on the ground
    std::vector<Row> rows;

    float spinShare = 0.5F;
    float inertia = 40.0F;
    float gravity = 14.0F;
    float water = 0.05F;
    int mob = 6;
    int seed = 1;
    bool contours = true;
    bool events = true;
    bool hopEvents = false;
};

struct Controls {
    std::function<Snapshot()> snapshot;
    std::function<void()> alarm;
    std::function<void()> bout;
    std::function<void()> recall;
    std::function<void()> calm;
    std::function<void()> remove;
    // Throws the named kangaroo now, or as soon as it is next on the ground.
    std::function<void(const QString& name, float speed)> throwKangaroo;
    std::function<void(float)> setSpinShare;
    std::function<void(float)> setInertia;
    std::function<void(bool)> setContours;
    std::function<void(bool)> setEvents;
    std::function<void(bool)> setHopEvents;
    std::function<void(int mob, float gravity, float water, int seed)> rebuild;
};

namespace detail {

// The planet's weight runs from 1 to 400 on a logarithmic slider, so the
// lively end has as much travel as the heavy one.
inline int inertiaToSlider(const float inertia)
{
    return static_cast<int>(std::lround(100.0 * std::log(std::max(inertia, 1.0F)) / std::log(400.0)));
}
inline float sliderToInertia(const int value)
{
    return static_cast<float>(std::exp(std::log(400.0) * value / 100.0));
}

inline QPushButton* button(const QString& text, const QString& name, const QString& tip)
{
    auto* b = new QPushButton(text);
    b->setObjectName(name);
    b->setToolTip(tip);
    return b;
}

inline QSlider* slider(const QString& name, const int lo, const int hi)
{
    auto* s = new QSlider(Qt::Horizontal);
    s->setObjectName(name);
    s->setRange(lo, hi);
    return s;
}

// Moves a control to a value the snapshot reports without firing its own
// handler, and leaves it alone while the user is holding it.
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

} // namespace detail

// Builds the panel and docks it on the right of the window. It refreshes
// itself from Controls::snapshot four times a second for as long as it lives.
inline QDockWidget* create(QMainWindow* window, Controls controls)
{
    using namespace detail;

    auto* dock = new QDockWidget(QStringLiteral("Kangaroo planet"), window);
    dock->setObjectName(QStringLiteral("kangarooPanel"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    // Scrolls when the window is shorter than the panel, rather than
    // squeezing the controls into each other.
    auto* scroll = new QScrollArea(dock);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    // ---- the planet's spin ----------------------------------------------
    auto* planetBox = new QGroupBox(QStringLiteral("Planet"));
    auto* planetForm = new QFormLayout(planetBox);
    auto* rate = new QLabel;
    rate->setObjectName(QStringLiteral("kangarooRateLabel"));
    rate->setWordWrap(true);
    planetForm->addRow(rate);
    auto* calm = button(QStringLiteral("Calm the spin to 75°/s"), QStringLiteral("kangarooCalmButton"),
                        QStringLiteral("Sets the cube's speed back to its default, where the mob hops freely"));
    planetForm->addRow(calm);
    auto* share = slider(QStringLiteral("kangarooSpinShareSlider"), 0, 100);
    auto* shareValue = new QLabel;
    auto* shareRow = new QHBoxLayout;
    shareRow->addWidget(share, 1);
    shareRow->addWidget(shareValue);
    share->setToolTip(QStringLiteral("How much of what the beat visualizer, or any step function bound "
                                     "before the planet, does to the spin reaches the planet"));
    planetForm->addRow(QStringLiteral("Visualizer's spin"), shareRow);
    auto* weight = slider(QStringLiteral("kangarooInertiaSlider"), 0, 100);
    auto* weightValue = new QLabel;
    auto* weightRow = new QHBoxLayout;
    weightRow->addWidget(weight, 1);
    weightRow->addWidget(weightValue);
    weight->setToolTip(QStringLiteral("How heavy the planet is against the mob's pushes; light is lively"));
    planetForm->addRow(QStringLiteral("Planet weight"), weightRow);
    layout->addWidget(planetBox);

    // ---- the mob ----------------------------------------------------------
    auto* mobBox = new QGroupBox(QStringLiteral("Mob"));
    auto* mobLayout = new QGridLayout(mobBox);
    auto* alarm = button(QStringLiteral("Thump an alarm"), QStringLiteral("kangarooAlarmButton"),
                         QStringLiteral("The biggest adult thumps the ground and the mob bolts"));
    auto* bout = button(QStringLiteral("Start a bout"), QStringLiteral("kangarooBoutButton"),
                        QStringLiteral("The two nearest males box"));
    auto* recall = button(QStringLiteral("Bring back the lost"), QStringLiteral("kangarooRecallButton"),
                          QStringLiteral("Drops every kangaroo lost in space back onto the planet, if it can hold them"));
    mobLayout->addWidget(alarm, 0, 0);
    mobLayout->addWidget(bout, 0, 1);
    mobLayout->addWidget(recall, 1, 0, 1, 2);
    auto* who = new QComboBox;
    who->setObjectName(QStringLiteral("kangarooThrowCombo"));
    auto* speed = slider(QStringLiteral("kangarooThrowSpeedSlider"), 2, 12);
    speed->setValue(5);
    speed->setToolTip(QStringLiteral("How hard to throw, in units a second; about 6 escapes the planet"));
    auto* speedValue = new QLabel;
    auto* toss = button(QStringLiteral("Throw"), QStringLiteral("kangarooThrowButton"),
                        QStringLiteral("Throws the chosen kangaroo off the ground"));
    auto* throwRow = new QHBoxLayout;
    throwRow->addWidget(who, 1);
    throwRow->addWidget(speed, 1);
    throwRow->addWidget(speedValue);
    throwRow->addWidget(toss);
    mobLayout->addLayout(throwRow, 2, 0, 1, 2);
    auto* table = new QTableWidget(0, 6);
    table->setObjectName(QStringLiteral("kangarooMobTable"));
    table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Doing"), QStringLiteral("Where"),
                                      QStringLiteral("Hops"), QStringLiteral("Flung"), QStringLiteral("Wins")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setMinimumHeight(170);
    mobLayout->addWidget(table, 3, 0, 1, 2);
    auto* tallies = new QLabel;
    tallies->setObjectName(QStringLiteral("kangarooTalliesLabel"));
    tallies->setWordWrap(true);
    mobLayout->addWidget(tallies, 4, 0, 1, 2);
    layout->addWidget(mobBox, 1);

    // ---- the scene ----------------------------------------------------------
    auto* sceneBox = new QGroupBox(QStringLiteral("Scene"));
    auto* sceneForm = new QFormLayout(sceneBox);
    auto* mobSize = new QSpinBox;
    mobSize->setObjectName(QStringLiteral("kangarooMobSpin"));
    mobSize->setRange(1, 12);
    sceneForm->addRow(QStringLiteral("Kangaroos"), mobSize);
    auto* gravity = slider(QStringLiteral("kangarooGravitySlider"), 6, 40);
    auto* gravityValue = new QLabel;
    auto* gravityRow = new QHBoxLayout;
    gravityRow->addWidget(gravity, 1);
    gravityRow->addWidget(gravityValue);
    gravity->setToolTip(QStringLiteral("Gravity at a face centre; stronger holds the mob at faster spins"));
    sceneForm->addRow(QStringLiteral("Gravity"), gravityRow);
    auto* water = slider(QStringLiteral("kangarooWaterSlider"), 0, 150);
    auto* waterValue = new QLabel;
    auto* waterRow = new QHBoxLayout;
    waterRow->addWidget(water, 1);
    waterRow->addWidget(waterValue);
    water->setToolTip(QStringLiteral("How deep the billabongs are at a face centre on a planet at rest"));
    sceneForm->addRow(QStringLiteral("Water"), waterRow);
    auto* seed = new QSpinBox;
    seed->setObjectName(QStringLiteral("kangarooSeedSpin"));
    seed->setRange(0, 99999);
    sceneForm->addRow(QStringLiteral("Seed"), seed);
    auto* rebuild = button(QStringLiteral("Rebuild the scene"), QStringLiteral("kangarooRebuildButton"),
                           QStringLiteral("A new mob and new scenery with the settings above"));
    sceneForm->addRow(rebuild);
    auto* contours = new QCheckBox(QStringLiteral("Equipotential lines"));
    contours->setObjectName(QStringLiteral("kangarooContoursCheck"));
    auto* events = new QCheckBox(QStringLiteral("Events"));
    events->setObjectName(QStringLiteral("kangarooEventsCheck"));
    auto* hopEvents = new QCheckBox(QStringLiteral("An event per hop"));
    hopEvents->setObjectName(QStringLiteral("kangarooHopEventsCheck"));
    auto* checks = new QHBoxLayout;
    checks->addWidget(contours);
    checks->addWidget(events);
    checks->addWidget(hopEvents);
    sceneForm->addRow(checks);
    layout->addWidget(sceneBox);

    auto* remove = button(QStringLiteral("Take the planet off"), QStringLiteral("kangarooRemoveButton"),
                          QStringLiteral("Puts the cube back as it was"));
    layout->addWidget(remove);
    scroll->setWidget(body);
    dock->setWidget(scroll);

    // ---- wiring ------------------------------------------------------------
    const auto showShare = [shareValue](const int v) { shareValue->setText(QStringLiteral("%1%").arg(v)); };
    const auto showWeight = [weightValue](const int v) {
        weightValue->setText(QString::number(sliderToInertia(v), 'f', 0));
    };
    const auto showSpeed = [speedValue](const int v) { speedValue->setText(QString::number(v)); };
    const auto showGravity = [gravityValue](const int v) { gravityValue->setText(QString::number(v)); };
    const auto showWater = [waterValue](const int v) {
        waterValue->setText(QString::number(v / 1000.0, 'f', 3));
    };
    QObject::connect(alarm, &QPushButton::clicked, dock, [controls] { controls.alarm(); });
    QObject::connect(bout, &QPushButton::clicked, dock, [controls] { controls.bout(); });
    QObject::connect(recall, &QPushButton::clicked, dock, [controls] { controls.recall(); });
    QObject::connect(calm, &QPushButton::clicked, dock, [controls] { controls.calm(); });
    QObject::connect(remove, &QPushButton::clicked, dock, [controls] { controls.remove(); });
    QObject::connect(toss, &QPushButton::clicked, dock, [controls, who, speed] {
        controls.throwKangaroo(who->currentText(), static_cast<float>(speed->value()));
    });
    QObject::connect(share, &QSlider::valueChanged, dock, [controls, showShare](const int v) {
        showShare(v);
        controls.setSpinShare(static_cast<float>(v) / 100.0F);
    });
    QObject::connect(weight, &QSlider::valueChanged, dock, [controls, showWeight](const int v) {
        showWeight(v);
        controls.setInertia(sliderToInertia(v));
    });
    QObject::connect(speed, &QSlider::valueChanged, dock, showSpeed);
    QObject::connect(gravity, &QSlider::valueChanged, dock, showGravity);
    QObject::connect(water, &QSlider::valueChanged, dock, showWater);
    QObject::connect(contours, &QCheckBox::toggled, dock, [controls](const bool on) { controls.setContours(on); });
    QObject::connect(events, &QCheckBox::toggled, dock, [controls](const bool on) { controls.setEvents(on); });
    QObject::connect(hopEvents, &QCheckBox::toggled, dock, [controls](const bool on) { controls.setHopEvents(on); });
    QObject::connect(rebuild, &QPushButton::clicked, dock, [controls, mobSize, gravity, water, seed] {
        controls.rebuild(mobSize->value(), static_cast<float>(gravity->value()),
                         static_cast<float>(water->value()) / 1000.0F, seed->value());
    });

    // The scene settings are filled in once and then left to the user until
    // they rebuild, so a half-edited set is not overwritten under them.
    {
        const Snapshot now = controls.snapshot();
        const QSignalBlocker b1(mobSize);
        const QSignalBlocker b2(gravity);
        const QSignalBlocker b3(water);
        const QSignalBlocker b4(seed);
        mobSize->setValue(now.mob);
        gravity->setValue(static_cast<int>(std::lround(now.gravity)));
        water->setValue(static_cast<int>(std::lround(now.water * 1000.0F)));
        seed->setValue(now.seed);
        showGravity(gravity->value());
        showWater(water->value());
        showSpeed(speed->value());
    }

    auto* timer = new QTimer(dock);
    timer->setInterval(250);
    const auto refresh = [=] {
        const Snapshot now = controls.snapshot();
        if (!now.installed) {
            rate->setText(QStringLiteral("The planet is off."));
            return;
        }
        rate->setText(QStringLiteral("Turning at %1°/s: set to %2, visualizer %3%4, mob %5%6")
                          .arg(now.rate, 0, 'f', 0)
                          .arg(now.setRate, 0, 'f', 0)
                          .arg(now.fromMusic >= 0.0F ? QStringLiteral("+") : QString())
                          .arg(now.fromMusic, 0, 'f', 0)
                          .arg(now.fromMob >= 0.0F ? QStringLiteral("+") : QString())
                          .arg(now.fromMob, 0, 'f', 1));
        sync(share, static_cast<int>(std::lround(now.spinShare * 100.0F)));
        showShare(share->value());
        sync(weight, inertiaToSlider(now.inertia));
        if (!weight->isSliderDown()) {
            weightValue->setText(QString::number(now.inertia, 'f', 0));
        }
        sync(contours, now.contours);
        sync(events, now.events);
        sync(hopEvents, now.hopEvents);

        // The names, kept in step with the mob without losing the choice.
        QStringList names;
        for (const Row& row : now.rows) {
            names << row.name;
        }
        QStringList shown;
        for (int i = 0; i < who->count(); ++i) {
            shown << who->itemText(i);
        }
        if (names != shown) {
            const QString chosen = who->currentText();
            const QSignalBlocker blocker(who);
            who->clear();
            who->addItems(names);
            const int index = who->findText(chosen);
            who->setCurrentIndex(index >= 0 ? index : 0);
        }

        table->setRowCount(static_cast<int>(now.rows.size()));
        for (int r = 0; r < static_cast<int>(now.rows.size()); ++r) {
            const Row& row = now.rows[r];
            const QStringList cells{row.name, row.doing, row.where, QString::number(row.hops),
                                    QString::number(row.flung), QString::number(row.wins)};
            for (int c = 0; c < cells.size(); ++c) {
                QTableWidgetItem* item = table->item(r, c);
                if (item == nullptr) {
                    item = new QTableWidgetItem;
                    table->setItem(r, c, item);
                }
                if (item->text() != cells[c]) {
                    item->setText(cells[c]);
                }
                if (c == 0) {
                    item->setToolTip(row.role);
                }
            }
        }
        tallies->setText(QStringLiteral("Flung %1, back %2. Bouts %3. Alarms %4. Water on the ground %5%.")
                             .arg(now.flung)
                             .arg(now.returned)
                             .arg(now.bouts)
                             .arg(now.alarms)
                             .arg(std::lround(now.waterHeld * 100.0F)));
    };
    QObject::connect(timer, &QTimer::timeout, dock, refresh);
    timer->start();
    refresh();

    window->addDockWidget(Qt::RightDockWidgetArea, dock);
    return dock;
}

} // namespace kangaroo_panel
