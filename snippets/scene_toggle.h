#pragma once

// Adds a checkable entry to the running application's Cube menu, so a scene
// snippet can be switched on and off from the GUI as well as over the socket.
//
// The application knows nothing about these snippets and must not: they are
// separate shared objects that may never be loaded. The entry is therefore
// created by the snippet itself once it is in the process, and its handler is
// code inside that module, which is why a loaded snippet is never unloaded.
//
// The action becomes an ordinary member of the application's object tree, so
// object.tree lists it and action.trigger operates it like any other.

#include <QAction>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QString>
#include <QWidget>

#include <functional>
#include <utility>

namespace scene_toggle {

// Whether GL work can be done here and now, meaning the widget's own context is
// current. That holds inside its paintGL callbacks and generally nowhere else.
//
// "Some context is current" is not the same question and is not safe to act on:
// opening a menu can leave an unrelated context current on the GUI thread, and
// objects created there do not exist in the context that draws. The symptom is
// GL_INVALID_OPERATION in glBindVertexArray with a non-gen name, once per frame,
// and an empty scene.
inline bool ownContextIsCurrent(const QOpenGLWidget* widget)
{
    QOpenGLContext* current = QOpenGLContext::currentContext();
    return current != nullptr && current == widget->context();
}

inline QMenu* findMenu(QMainWindow* window, const QString& title)
{
    for (QAction* entry : window->menuBar()->actions()) {
        if (entry->menu() != nullptr && entry->menu()->title() == title) {
            return entry->menu();
        }
    }
    return nullptr;
}

// Returns nullptr only if there is no main window or no Cube menu to add to.
//
// An entry of this name already being there means a previous generation of the
// same snippet created it: reloading a snippet needs a new file path, so the
// new code arrives as a separate module with its own static state while the
// old module stays resident. Rebinding the existing entry to the new handler
// hands it over, so the menu drives the generation that is actually installed.
// Leaving it bound to the old module instead gives an entry whose checkbox
// reports that module's empty state and whose clicks reach nothing on screen.
//
// This assumes the previous generation was restored before the new one
// installed, which is what `agentctl.py reload` does. Loading a second copy on
// top of a live one is a different situation, and the snippets that overwrite
// the widget's vertices refuse it outright.
inline QAction* install(QWidget* widget,
                        const QString& objectName,
                        const QString& text,
                        const QString& shortcut,
                        std::function<void(bool)> onToggled)
{
    auto* window = qobject_cast<QMainWindow*>(widget->window());
    if (window == nullptr) {
        return nullptr;
    }
    if (QAction* existing = window->findChild<QAction*>(objectName)) {
        // A checked entry belongs to a generation that currently holds an
        // install, and handing it to a caller which has not installed anything
        // yet loses it for good: the caller may then refuse — the vertex
        // replacements refuse routinely — and its sync leaves the entry
        // unchecked and wired to a module holding nothing. The live generation
        // is then unreachable from the menu, and turnOff, which acts only on a
        // checked entry, silently stops excluding it.
        //
        // So the entry is only handed over when nothing holds it. A caller that
        // gets nullptr here reports menuToggle false and is driven over the
        // socket; once the previous generation releases and unchecks, the next
        // call takes the entry over.
        if (existing->isChecked()) {
            return nullptr;
        }
        QObject::disconnect(existing, &QAction::toggled, nullptr, nullptr);
        QObject::connect(existing, &QAction::toggled, window, std::move(onToggled));
        return existing;
    }
    QMenu* menu = findMenu(window, QStringLiteral("&Cube"));
    if (menu == nullptr) {
        return nullptr;
    }

    // Keep everything added at runtime below one separator, so the menu shows
    // which entries the application itself shipped.
    const QString separatorName = QStringLiteral("actionSceneSeparator");
    if (window->findChild<QAction*>(separatorName) == nullptr) {
        menu->addSeparator()->setObjectName(separatorName);
    }

    auto* action = new QAction(text, window);
    action->setObjectName(objectName);
    action->setCheckable(true);
    if (!shortcut.isEmpty()) {
        action->setShortcut(QKeySequence(shortcut));
    }
    QObject::connect(action, &QAction::toggled, window, std::move(onToggled));
    menu->addAction(action);
    return action;
}

// Switches another runtime-added entry off, if it is there and on. The two
// snippets that replace the cube's own mesh use this on each other: both save
// the widget's 36 original vertices and then overwrite them, so the second one
// to install would save the first one's zeros and later restore those instead
// of the cube.
//
// Unchecking runs the owning module's handler, which does the removal there and
// then when a context is already current — which it is when this is called from
// inside the other snippet's render callback.
inline void turnOff(QWidget* widget, const QString& objectName)
{
    auto* window = qobject_cast<QMainWindow*>(widget->window());
    if (window == nullptr) {
        return;
    }
    QAction* other = window->findChild<QAction*>(objectName);
    if (other != nullptr && other->isChecked()) {
        other->setChecked(false);
    }
}

} // namespace scene_toggle
