// Soliloquies and intermissions, performed in captions over the cube.
//
// A stage for the main window: cards of text that fade in and out over the
// cube's view, the window's title, the cube's speed eased up or down, and a
// curtain that closes across the view. It came out of a demo of this tooling
// for a guest, where it carried an intermission while another module was being
// built, an apology after a crash, and an epilogue.
//
// Three scripts are built in, and any other can be sent as cards:
//
//   soliloquy      an apology in verse for the crash fixed in ebf88cf: the
//                  About box opened over the socket, its client gave up, and
//                  the reply after the dialog closed went through a freed
//                  socket. The cube slows while it is spoken.
//   intermission   warm-ups about four dimensions and a quiz, looping. Each act
//                  can switch on one of the scene snippets for its length,
//                  through that snippet's own entry in the Cube menu, so it
//                  uses whichever of them are loaded and leaves the rest out.
//   epilogue       a closing speech that brings the cube to rest and ends with
//                  the curtain closed.
//
// The guest and the host are named in the scripts; {"guest": ..., "host": ...}
// sets them.
//
// Requests, any combination:
//   {"play": "soliloquy"}                    one of the scripts above
//   {"cards": [{"text": "...", "ms": 6000, "effect": "actionMobiusRing"}],
//    "loop": false}                          a script of your own; effect is
//                                            the objectName of a menu entry to
//                                            switch on while the card shows
//   {"title": "..."}, {"restoreTitle": true}
//   {"ramp": {"to": 0, "seconds": 4, "easing": "out"}}
//                                            ease the cube's speed; easing is
//                                            in, out or inout
//   {"curtain": "close"}, {"curtain": "open"}
//   {"stop": true}                           end the current script and put
//                                            back the title and the speed
//
// An Intermission entry in the Cube menu (Ctrl+Shift+I) starts and stops the
// intermission. Releasing the module ends whatever is playing, opens the
// curtain and puts the title and the speed back.

#include "agent/agent_abi.h"
#include "cube_widget.h"
#include "scene_toggle.h"

#include <QAction>
#include <QEasingCurve>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>
#include <QSignalBlocker>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

struct Card {
    QString text;
    int ms = 6000;
    // The objectName of a menu entry to keep switched on while this card
    // shows: a scene snippet's toggle, such as actionMobiusRing.
    QString effect;
};

struct Script {
    QString name;
    QList<Card> cards;
    bool loop = false;
    QString title;
    // The cube's speed while the script plays; negative leaves it alone.
    float speed = -1.0F;
    // Close the curtain after the last card, and leave it closed.
    bool curtainAtEnd = false;
};

// ---------------------------------------------------------------------------
// The scripts
// ---------------------------------------------------------------------------

Card card(const QString& text, const int ms, const QString& effect = QString())
{
    return Card{text, ms, effect};
}

Script soliloquy(const QString& guest, const QString& host)
{
    Script s;
    s.name = QStringLiteral("soliloquy");
    s.title = QStringLiteral("An apology to %1, in one act").arg(guest);
    s.speed = 18.0F;
    s.cards = {
        card(QStringLiteral("<i>An apology, in one act.<br>The cube turns slowly. Enter the Agent, alone.</i>"), 5000),
        card(QStringLiteral("O %1, guest of honour, and my host,<br>I swore this cube would spin and never fall;").arg(guest), 6000),
        card(QStringLiteral("and in the very hour you came to see,<br>I opened up the About box, and fell."), 6000),
        card(QStringLiteral("A modal dialog keeps its own small loop,<br>and held my handler, patient, mid-reply."), 6000),
        card(QStringLiteral("My client tired of waiting and went home,<br>and Qt, obliging, freed its socket there:"), 6000),
        card(QStringLiteral("inside the dialog's loop, and not my own.<br>Then someone pressed OK. The handler woke,"), 6000),
        card(QStringLiteral("reached for a socket that was there no more,<br>and wrote its answer into freed memory."), 6000),
        card(QStringLiteral("<i>Segmentation fault. The window, gone.</i>"), 4500),
        card(QStringLiteral("Now every socket waits until the last<br>of all my nested handlers has returned,"), 6000),
        card(QStringLiteral("and every line is looked up fresh, not held<br>by iterators a dozen clients spoil."), 6000),
        card(QStringLiteral("A test now opens that same box, gives up,<br>"
                            "lets twelve more clients come and go, and closes it;"), 6500),
        card(QStringLiteral("it failed before the fix. It passes now.<br>The cube is turning still when it is done."), 6000),
        card(QStringLiteral("Forgive me, %1. Forgive me, %2.<br>The crash was old; the stage was mine to fall on.")
                 .arg(guest, host), 6500),
        card(QStringLiteral("<i>Exit, pursued by a tesseract.</i>"), 5000),
    };
    return s;
}

Script intermission(const QString& guest)
{
    const QString ring = QStringLiteral("actionMobiusRing");
    const QString pillow = QStringLiteral("actionPillowMode");
    const QString sphere = QStringLiteral("actionOrbitSphere");
    const QString jack = QStringLiteral("actionJackInTheBox");
    Script s;
    s.name = QStringLiteral("intermission");
    s.title = QStringLiteral("Intermission");
    s.loop = true;
    s.cards = {
        card(QStringLiteral("<i>Intermission.</i><br>While the next act is assembled backstage, %1, a few warm-ups.")
                 .arg(guest), 6500, ring),
        card(QStringLiteral("This ring has one side and one edge.<br>"
                            "Follow it once around and you come back upside down."), 6500, ring),
        card(QStringLiteral("Two of them, glued edge to edge, make a Klein bottle,<br>"
                            "which needs four dimensions to close without crossing itself."), 7000, ring),
        card(QStringLiteral("A note for %1: a chicken is a dinosaur<br>that learned to wait for its tests.").arg(guest),
             6500, ring),
        card(QStringLiteral("Quiz for %1, while the cube relaxes into pillows.<br>"
                            "How many cubes make the surface of a tesseract?").arg(guest), 7000, pillow),
        card(QStringLiteral("Eight. Each shares its six faces with six of the others.<br>"
                            "Next: what shape is a corner-first slice through its centre?"), 7500, pillow),
        card(QStringLiteral("A regular octahedron,<br>exactly halfway through the fourth dimension."), 6500, pillow),
        card(QStringLiteral("Since this process started, the cube has drawn {frames} frames,<br>"
                            "about {turns} full turns, without falling over."), 7000),
        card(QStringLiteral("In <i>Flatland</i> (1884), a sphere visits a square.<br>"
                            "The square sees a dot grow into a circle, shrink, and vanish."), 7000, sphere),
        card(QStringLiteral("Tesseract, from the Greek for four rays.<br>Named by Charles Howard Hinton in 1888."),
             6500, sphere),
        card(QStringLiteral("Hinton also built a baseball pitching machine that fired with gunpowder.<br>"
                            "Unrelated, but %1 deserves to know.").arg(guest), 7000, sphere),
        card(QStringLiteral("16 corners, 32 edges, 24 squares, 8 cubes."), 5500, sphere),
        card(QStringLiteral("<i>A knock from inside the cube.</i>"), 4500, jack),
        card(QStringLiteral("Every cube hides a spare direction, %1.<br>This one was hiding a jack.").arg(guest),
             7000, jack),
        card(QStringLiteral("In four dimensions a knot in a rope comes undone without cutting it:<br>"
                            "there is room to pass the rope around itself."), 7000),
        card(QStringLiteral("Turn a left hand through the fourth dimension<br>and it comes back a right hand."), 6500),
        card(QStringLiteral("Dali painted the tesseract unfolded into a cross in 1954:<br><i>Corpus Hypercubus</i>."),
             6500),
    };
    return s;
}

Script epilogue(const QString& guest, const QString& host)
{
    Script s;
    s.name = QStringLiteral("epilogue");
    s.title = QStringLiteral("Epilogue");
    s.curtainAtEnd = true;
    s.cards = {
        card(QStringLiteral("<i>Epilogue.</i><br>Tonight this program changed while it ran."), 5000),
        card(QStringLiteral("Its functions were rewritten at the entry, new code loaded beside the old,<br>"
                            "and the cube became whatever was asked of it."), 8000),
        card(QStringLiteral("Live-modified software is a fine future, %1,<br>"
                            "as long as every change carries its build id, its claim and its test.").arg(guest), 8000),
        card(QStringLiteral("Our revels now are ended. These our actors,<br>as I foretold you, were all spirits,<br>"
                            "and are melted into air, into thin air."), 9000),
        card(QStringLiteral("Thank you, %1. Thank you, %2.<br><i>The curtain falls.</i>").arg(guest, host), 6000),
    };
    return s;
}

// ---------------------------------------------------------------------------
// The stage
// ---------------------------------------------------------------------------

struct Stage {
    QPointer<CubeWidget> cube;
    QPointer<QLabel> label;
    QPointer<QGraphicsOpacityEffect> fade;
    QPointer<QTimer> advance;
    QPointer<QTimer> fadeOut;
    QPointer<QVariantAnimation> ramp;
    QPointer<QFrame> curtainLeft;
    QPointer<QFrame> curtainRight;
    QPointer<QLabel> curtainWords;

    Script script;
    int index = 0;
    bool playing = false;
    QString effectOn;

    bool titleSaved = false;
    QString originalTitle;
    bool speedSaved = false;
    float originalSpeed = 0.0F;
};

Stage* stage = nullptr;
QAction* toggleAction = nullptr;
QString guestName = QStringLiteral("Stribor");
QString hostName = QStringLiteral("Juraj");

QMainWindow* windowOf(CubeWidget* cube)
{
    return cube != nullptr ? qobject_cast<QMainWindow*>(cube->window()) : nullptr;
}

void setTitle(const QString& title)
{
    QMainWindow* window = windowOf(stage->cube);
    if (window == nullptr || title.isEmpty()) {
        return;
    }
    if (!stage->titleSaved) {
        stage->originalTitle = window->windowTitle();
        stage->titleSaved = true;
    }
    window->setWindowTitle(title);
}

void restoreTitle()
{
    QMainWindow* window = windowOf(stage->cube);
    if (window != nullptr && stage->titleSaved) {
        window->setWindowTitle(stage->originalTitle);
    }
    stage->titleSaved = false;
}

void rampSpeed(const float to, const float seconds, const QString& easing)
{
    CubeWidget* cube = stage->cube;
    if (cube == nullptr) {
        return;
    }
    if (!stage->speedSaved) {
        stage->originalSpeed = cube->angularVelocity();
        stage->speedSaved = true;
    }
    if (stage->ramp != nullptr) {
        stage->ramp->stop();
        stage->ramp->deleteLater();
    }
    auto* ramp = new QVariantAnimation(cube);
    ramp->setStartValue(static_cast<double>(cube->angularVelocity()));
    ramp->setEndValue(static_cast<double>(to));
    ramp->setDuration(static_cast<int>(std::max(seconds, 0.05F) * 1000.0F));
    ramp->setEasingCurve(easing == QStringLiteral("in")    ? QEasingCurve::InCubic
                         : easing == QStringLiteral("out") ? QEasingCurve::OutCubic
                                                           : QEasingCurve::InOutCubic);
    QPointer<CubeWidget> guard(cube);
    QObject::connect(ramp, &QVariantAnimation::valueChanged, ramp, [guard](const QVariant& value) {
        if (guard != nullptr) {
            guard->setAngularVelocity(static_cast<float>(value.toDouble()));
        }
    });
    stage->ramp = ramp;
    ramp->start(QAbstractAnimation::DeleteWhenStopped);
}

// Puts the speed back as it was before the first ramp.
void restoreSpeed(const float seconds)
{
    if (stage->speedSaved) {
        const float to = stage->originalSpeed;
        rampSpeed(to, seconds, QStringLiteral("inout"));
        stage->speedSaved = false;
    }
}

// Switches a scene snippet's menu entry on or off. Its own handler does the
// work, deferred to a frame where its context is current, so this only says
// what is wanted. An entry that is not there, because that snippet was never
// loaded, is skipped.
void setEffect(const QString& name, const bool on)
{
    QMainWindow* window = windowOf(stage->cube);
    if (window == nullptr || name.isEmpty()) {
        return;
    }
    if (QAction* action = window->findChild<QAction*>(name); action != nullptr && action->isCheckable()) {
        if (action->isChecked() != on) {
            action->trigger();
        }
    }
}

void switchEffectTo(const QString& name)
{
    if (stage->effectOn == name) {
        return;
    }
    setEffect(stage->effectOn, false);
    stage->effectOn = name;
    setEffect(name, true);
}

void fadeTo(const qreal to, const int ms)
{
    if (stage->fade == nullptr) {
        return;
    }
    auto* animation = new QPropertyAnimation(stage->fade, "opacity", stage->label);
    animation->setDuration(ms);
    animation->setStartValue(stage->fade->opacity());
    animation->setEndValue(to);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void place()
{
    CubeWidget* cube = stage->cube;
    QLabel* label = stage->label;
    if (cube == nullptr || label == nullptr) {
        return;
    }
    const int width = static_cast<int>(cube->width() * 0.86);
    label->setFixedWidth(width);
    label->adjustSize();
    label->move((cube->width() - width) / 2, cube->height() - label->height() - cube->height() / 14);
    label->raise();
}

// Numbers read off the running cube at the moment the card is shown.
QString fillIn(QString text)
{
    CubeWidget* cube = stage->cube;
    if (cube == nullptr) {
        return text;
    }
    const qulonglong frames = cube->m_frameIndex;
    const double seconds = static_cast<double>(cube->m_elapsedSeconds);
    const auto turns = static_cast<qlonglong>(seconds * static_cast<double>(cube->angularVelocity()) / 360.0);
    return text.replace(QStringLiteral("{frames}"), QLocale().toString(frames))
        .replace(QStringLiteral("{turns}"), QLocale().toString(turns));
}

void closeCurtain();
void syncToggleAction();

void endScript()
{
    stage->playing = false;
    stage->advance->stop();
    stage->fadeOut->stop();
    switchEffectTo(QString());
    fadeTo(0.0, 600);
    if (stage->script.curtainAtEnd) {
        rampSpeed(0.0F, 4.0F, QStringLiteral("out"));
        QTimer::singleShot(1500, stage->label, [] { closeCurtain(); });
    } else {
        restoreTitle();
        restoreSpeed(2.5F);
    }
    syncToggleAction();
}

void showCard()
{
    if (stage->index >= stage->script.cards.size()) {
        if (!stage->script.loop || stage->script.cards.isEmpty()) {
            endScript();
            return;
        }
        stage->index = 0;
    }
    const Card card = stage->script.cards.at(stage->index++);
    switchEffectTo(card.effect);
    stage->label->setText(fillIn(card.text));
    place();
    fadeTo(1.0, 600);
    stage->fadeOut->start(std::max(card.ms - 600, 300));
    stage->advance->start(card.ms);
}

void ensureStage(CubeWidget* cube)
{
    if (stage != nullptr && stage->label != nullptr) {
        return;
    }
    delete stage;
    stage = new Stage;
    stage->cube = cube;
    auto* label = new QLabel(cube);
    label->setObjectName(QStringLiteral("soliloquyCaption"));
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setTextFormat(Qt::RichText);
    const int points = std::max(14, cube->height() / 30);
    label->setStyleSheet(QStringLiteral(
        "QLabel { color: #f3ead8; background: rgba(10, 12, 18, 175); border-radius: 14px;"
        " padding: 18px 26px; font-family: 'DejaVu Serif', 'Liberation Serif', serif; font-size: %1pt; }")
                             .arg(points));
    auto* fade = new QGraphicsOpacityEffect(label);
    fade->setOpacity(0.0);
    label->setGraphicsEffect(fade);
    label->show();
    stage->label = label;
    stage->fade = fade;
    stage->advance = new QTimer(label);
    stage->advance->setSingleShot(true);
    QObject::connect(stage->advance, &QTimer::timeout, label, [] { showCard(); });
    stage->fadeOut = new QTimer(label);
    stage->fadeOut->setSingleShot(true);
    QObject::connect(stage->fadeOut, &QTimer::timeout, label, [] { fadeTo(0.0, 550); });
}

void play(const Script& script)
{
    if (stage->playing) {
        switchEffectTo(QString());
    }
    stage->script = script;
    stage->index = 0;
    stage->playing = true;
    stage->advance->stop();
    stage->fadeOut->stop();
    setTitle(script.title);
    if (script.speed >= 0.0F) {
        rampSpeed(script.speed, 2.0F, QStringLiteral("out"));
    }
    showCard();
    syncToggleAction();
}

void stop()
{
    if (stage == nullptr || stage->label == nullptr) {
        return;
    }
    stage->script.curtainAtEnd = false;
    endScript();
}

// ---------------------------------------------------------------------------
// The curtain
// ---------------------------------------------------------------------------

QFrame* curtainPanel(QWidget* parent, const QString& name)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(name);
    frame->setAttribute(Qt::WA_TransparentForMouseEvents);
    // Velvet folds: dark and lighter red repeated across the panel.
    frame->setStyleSheet(QStringLiteral(
        "QFrame { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        " stop:0 #3c070b, stop:0.08 #8e1622, stop:0.16 #4a0910, stop:0.25 #9a1a26, stop:0.33 #45080e,"
        " stop:0.42 #8a1520, stop:0.5 #3f070c, stop:0.58 #931824, stop:0.67 #47080f, stop:0.75 #9c1b27,"
        " stop:0.83 #42070d, stop:0.92 #8c1621, stop:1 #3a060a); }"));
    frame->show();
    frame->raise();
    return frame;
}

void slideCurtain(const bool closing)
{
    CubeWidget* cube = stage->cube;
    if (cube == nullptr) {
        return;
    }
    const int w = cube->width();
    const int h = cube->height();
    const int half = w / 2 + 2;
    if (stage->curtainLeft == nullptr) {
        stage->curtainLeft = curtainPanel(cube, QStringLiteral("curtainLeft"));
        stage->curtainRight = curtainPanel(cube, QStringLiteral("curtainRight"));
        stage->curtainLeft->setGeometry(-half, 0, half, h);
        stage->curtainRight->setGeometry(w, 0, half, h);
    }
    if (stage->curtainWords != nullptr) {
        stage->curtainWords->deleteLater();
    }
    const QRect leftOpen(-half, 0, half, h);
    const QRect rightOpen(w, 0, half, h);
    const QRect leftShut(0, 0, half, h);
    const QRect rightShut(w - half, 0, half, h);
    auto* group = new QParallelAnimationGroup(cube);
    for (auto [frame, to] : {std::pair{stage->curtainLeft.data(), closing ? leftShut : leftOpen},
                             std::pair{stage->curtainRight.data(), closing ? rightShut : rightOpen}}) {
        auto* slide = new QPropertyAnimation(frame, "geometry", group);
        slide->setDuration(3600);
        slide->setStartValue(frame->geometry());
        slide->setEndValue(to);
        slide->setEasingCurve(QEasingCurve::InOutQuad);
        group->addAnimation(slide);
    }
    if (closing) {
        QObject::connect(group, &QParallelAnimationGroup::finished, cube, [cube, w, h] {
            if (stage == nullptr) {
                return;
            }
            auto* words = new QLabel(QStringLiteral("Thank you, %1").arg(guestName), cube);
            words->setObjectName(QStringLiteral("curtainWords"));
            words->setAttribute(Qt::WA_TransparentForMouseEvents);
            words->setAlignment(Qt::AlignCenter);
            words->setStyleSheet(QStringLiteral(
                "QLabel { color: #e8c66a; background: transparent;"
                " font-family: 'DejaVu Serif', 'Liberation Serif', serif; font-size: %1pt; font-style: italic; }")
                                     .arg(std::max(18, h / 18)));
            words->setGeometry(0, h / 2 - h / 8, w, h / 4);
            words->show();
            words->raise();
            stage->curtainWords = words;
        });
    } else {
        QObject::connect(group, &QParallelAnimationGroup::finished, cube, [] {
            if (stage == nullptr) {
                return;
            }
            for (QFrame* panel : {stage->curtainLeft.data(), stage->curtainRight.data()}) {
                if (panel != nullptr) {
                    panel->deleteLater();
                }
            }
        });
    }
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void closeCurtain()
{
    slideCurtain(true);
}

void openCurtain()
{
    if (stage->curtainLeft != nullptr) {
        slideCurtain(false);
    }
    restoreTitle();
    restoreSpeed(3.0F);
}

// ---------------------------------------------------------------------------
// The menu entry
// ---------------------------------------------------------------------------

void syncToggleAction()
{
    if (toggleAction != nullptr) {
        const QSignalBlocker blocker(toggleAction);
        toggleAction->setChecked(stage != nullptr && stage->playing
                                 && stage->script.name == QStringLiteral("intermission"));
    }
}

void ensureToggleAction(CubeWidget* cube)
{
    if (toggleAction != nullptr) {
        return;
    }
    toggleAction = scene_toggle::install(
        cube, QStringLiteral("actionIntermission"), QStringLiteral("&Intermission"),
        QStringLiteral("Ctrl+Shift+I"), [cube](const bool on) {
            // Widgets only, so this runs where the click arrives.
            ensureStage(cube);
            if (on) {
                play(intermission(guestName));
            } else {
                stop();
            }
        });
    syncToggleAction();
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void tearDown()
{
    if (stage == nullptr) {
        return;
    }
    // Read before stopping: stop() starts easing the speed back and clears
    // the record of what it was, and the ramp is cut short below.
    const bool restore = stage->speedSaved;
    const float speed = stage->originalSpeed;
    if (stage->label != nullptr) {
        stop();
    }
    if (stage->ramp != nullptr) {
        stage->ramp->stop();
    }
    for (QWidget* widget : {static_cast<QWidget*>(stage->curtainLeft.data()),
                            static_cast<QWidget*>(stage->curtainRight.data()),
                            static_cast<QWidget*>(stage->curtainWords.data()),
                            static_cast<QWidget*>(stage->label.data())}) {
        if (widget != nullptr) {
            widget->deleteLater();
        }
    }
    restoreTitle();
    if (restore && stage->cube != nullptr) {
        stage->cube->setAngularVelocity(speed);
    }
    delete stage;
    stage = nullptr;
    syncToggleAction();
}

QJsonObject status()
{
    QJsonObject result{
        {QStringLiteral("playing"), stage != nullptr && stage->playing},
        {QStringLiteral("menuToggle"), toggleAction != nullptr},
        {QStringLiteral("guest"), guestName},
        {QStringLiteral("host"), hostName},
    };
    if (stage != nullptr) {
        result.insert(QStringLiteral("script"), stage->script.name);
        result.insert(QStringLiteral("card"), stage->index);
        result.insert(QStringLiteral("cards"), static_cast<int>(stage->script.cards.size()));
        result.insert(QStringLiteral("effect"), stage->effectOn);
        result.insert(QStringLiteral("curtain"), stage->curtainLeft != nullptr);
    }
    return result;
}

void release(const RuntimeAgentHost* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI) {
        return;
    }
    const bool wasPlaying = stage != nullptr;
    tearDown();
    host->complete_json(host->invocation_context,
                        QJsonDocument(QJsonObject{{QStringLiteral("removed"), wasPlaying}})
                            .toJson(QJsonDocument::Compact)
                            .constData());
}

void run(const RuntimeAgentHost* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI) {
        return;
    }
    auto* cube = static_cast<CubeWidget*>(host->find_qobject(host->agent_context, "cubeView"));
    if (cube == nullptr) {
        host->fail(host->invocation_context, "cubeView was not found");
        return;
    }
    const QJsonObject request =
        QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();
    ensureStage(cube);
    ensureToggleAction(cube);

    if (request.contains(QStringLiteral("guest"))) {
        guestName = request.value(QStringLiteral("guest")).toString();
    }
    if (request.contains(QStringLiteral("host"))) {
        hostName = request.value(QStringLiteral("host")).toString();
    }
    if (request.contains(QStringLiteral("title"))) {
        setTitle(request.value(QStringLiteral("title")).toString());
    }
    if (request.value(QStringLiteral("restoreTitle")).toBool()) {
        restoreTitle();
    }
    if (request.contains(QStringLiteral("ramp"))) {
        const QJsonObject ramp = request.value(QStringLiteral("ramp")).toObject();
        rampSpeed(static_cast<float>(ramp.value(QStringLiteral("to")).toDouble()),
                  static_cast<float>(ramp.value(QStringLiteral("seconds")).toDouble(3.0)),
                  ramp.value(QStringLiteral("easing")).toString());
    }
    if (request.contains(QStringLiteral("play"))) {
        const QString name = request.value(QStringLiteral("play")).toString();
        if (name == QStringLiteral("soliloquy")) {
            play(soliloquy(guestName, hostName));
        } else if (name == QStringLiteral("intermission")) {
            play(intermission(guestName));
        } else if (name == QStringLiteral("epilogue")) {
            play(epilogue(guestName, hostName));
        } else {
            host->fail(host->invocation_context,
                       QStringLiteral("no script called \"%1\"; the scripts are soliloquy, intermission and epilogue")
                           .arg(name)
                           .toUtf8()
                           .constData());
            return;
        }
    }
    if (request.contains(QStringLiteral("cards"))) {
        Script script;
        script.name = QStringLiteral("cards");
        script.loop = request.value(QStringLiteral("loop")).toBool();
        for (const QJsonValue& value : request.value(QStringLiteral("cards")).toArray()) {
            const QJsonObject c = value.toObject();
            script.cards.append(card(c.value(QStringLiteral("text")).toString(),
                                     c.value(QStringLiteral("ms")).toInt(6000),
                                     c.value(QStringLiteral("effect")).toString()));
        }
        play(script);
    }
    const QString curtain = request.value(QStringLiteral("curtain")).toString();
    if (curtain == QStringLiteral("close")) {
        closeCurtain();
    } else if (curtain == QStringLiteral("open")) {
        openCurtain();
    }
    if (request.value(QStringLiteral("stop")).toBool()) {
        stop();
    }
    host->complete_json(host->invocation_context,
                        QJsonDocument(status()).toJson(QJsonDocument::Compact).constData());
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "soliloquies and intermissions, performed in captions over the cube",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init()
{
    return &descriptor;
}
