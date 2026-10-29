#pragma once

// A camera for the cube's widget, which has none of its own.
//
// The widget's view is a fixed translation in paintGL(), and every overlay
// builds its own view from the same constant. What they all share is the
// widget's projection matrix, so the camera is folded into that: drawing with
// projection x camera x (the widget's translation undone) puts the camera
// wherever this says, for the widget's own draw and for every overlay, without
// any of them knowing.
//
// resizeGL() rewrites the projection, so before each use the projection is
// compared with what was last written into it: if it is something else, it is
// the new base. Releasing puts the base back, if what is there is still what
// was written.
//
// A snippet that uses this owns the projection while it is installed, which
// is why only one should at a time; the snippets that do also replace the
// widget's mesh, and the claims that keep those exclusive keep this exclusive
// too.

#include "cube_widget.h"

#include <QEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QObject>
#include <QQuaternion>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace projection_camera {

struct Camera {
    // The view's rotation of the world, applied before the translation back
    // from the eye.
    QQuaternion orbit;
    float distance = cubeViewDistance;

    QMatrix4x4 base;
    QMatrix4x4 written;
    bool haveWritten = false;
    // The full view of the last frame written, translation and rotation, for
    // a scene that needs to know where the eye is.
    QMatrix4x4 view;

    bool dragging = false;
    QPointF lastPointer;
};

inline constexpr float nearest = 2.4F;
inline constexpr float furthest = 30.0F;

// Writes the camera into the widget's projection. `follow` is a further turn
// of the world after the orbit, which is how a camera keeps pace with
// something that moves, such as a planet it rides with.
inline void apply(Camera& camera, CubeWidget* cube, const QQuaternion& follow = QQuaternion())
{
    if (!camera.haveWritten || cube->m_projection != camera.written) {
        camera.base = cube->m_projection;
    }
    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, -camera.distance);
    view.rotate(camera.orbit * follow);
    camera.view = view;
    QMatrix4x4 undo;
    undo.translate(0.0F, 0.0F, cubeViewDistance);
    cube->m_projection = camera.base * view * undo;
    camera.written = cube->m_projection;
    camera.haveWritten = true;
}

// Puts back the projection the widget made, if what is there is still ours.
inline void release(Camera& camera, CubeWidget* cube)
{
    if (camera.haveWritten && cube->m_projection == camera.written) {
        cube->m_projection = camera.base;
    }
    camera.haveWritten = false;
}

// Turns the view by degrees across and up, as dragging does: about the view's
// own vertical and horizontal, so the world follows the pointer however it is
// already turned.
inline void orbit(Camera& camera, const float across, const float up)
{
    camera.orbit = QQuaternion::fromAxisAndAngle(1.0F, 0.0F, 0.0F, up)
        * QQuaternion::fromAxisAndAngle(0.0F, 1.0F, 0.0F, across) * camera.orbit;
    camera.orbit.normalize();
}

inline void setDistance(Camera& camera, const float distance)
{
    camera.distance = std::clamp(distance, nearest, furthest);
}

inline void reset(Camera& camera)
{
    camera.orbit = QQuaternion();
    camera.distance = cubeViewDistance;
}

// Dragging with the left button orbits, the wheel zooms, a double click
// resets. Installed on the cube's widget, which handles no pointer input of
// its own. What each gesture does is the owner's: it is given functions, so
// it can apply the camera its own way and decline when nothing is installed.
class Pointer final : public QObject {
public:
    struct Actions {
        std::function<Camera*()> camera;            // nullptr when nothing is installed
        std::function<void(float, float)> orbit;   // degrees across and up
        std::function<void(float)> zoom;           // a factor on the distance
        std::function<void()> reset;
    };

    Pointer(CubeWidget* cube, Actions actions) : QObject(cube), m_cube(cube), m_actions(std::move(actions)) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        Camera* camera = watched == m_cube ? m_actions.camera() : nullptr;
        if (camera == nullptr) {
            return false;
        }
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) {
                return false;
            }
            camera->dragging = true;
            camera->lastPointer = mouse->position();
            return true;
        }
        case QEvent::MouseMove: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!camera->dragging || (mouse->buttons() & Qt::LeftButton) == 0) {
                return false;
            }
            const QPointF delta = mouse->position() - camera->lastPointer;
            camera->lastPointer = mouse->position();
            m_actions.orbit(static_cast<float>(delta.x()) * 0.4F, static_cast<float>(delta.y()) * 0.4F);
            return true;
        }
        case QEvent::MouseButtonRelease:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && camera->dragging) {
                camera->dragging = false;
                return true;
            }
            return false;
        case QEvent::MouseButtonDblClick:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                m_actions.reset();
                return true;
            }
            return false;
        case QEvent::Wheel: {
            const float steps = static_cast<float>(static_cast<QWheelEvent*>(event)->angleDelta().y()) / 120.0F;
            m_actions.zoom(std::pow(0.9F, steps));
            return true;
        }
        default:
            return false;
        }
    }

private:
    CubeWidget* m_cube;
    Actions m_actions;
};

} // namespace projection_camera
