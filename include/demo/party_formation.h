#pragma once

// Acts publish marks: places on their set where visiting performers walk or
// fly. A visiting cast, such as the stage's robot fleet, reads the marks for the
// current frame and keeps its own drawing. The act does not know which
// performers visit, and the cast does not know which act is playing.
//
// Coordinates are the host cube's logical pixels, shared by every full-size
// layer above it. Like the scene compositor, the registry is a named child of
// the cube, so separately loaded modules find the same object.
#include <QObject>
#include <QPointF>
#include <QPointer>

#include <functional>
#include <vector>

namespace party_scene {

struct Mark {
    QPointF ground;          // where a walker touches the floor, or the floor point below a flyer
    double altitude = 0;     // screen pixels between that floor point and a flyer's body
    double facing = 0;       // -1 looks left, 1 looks right
    bool carrying = false;   // the performer shows its prop at this mark
};

struct Formation {
    std::vector<Mark> walkers, flyers;   // performers take marks in order
    double laneY = -1;                   // floor line for performers crossing the set; negative keeps the cast's own path
    double laneAltitude = 0;             // height above laneY for crossing flyers
};

class Formations final : public QObject {
public:
    // Modules built against a different layout refuse the shared object.
    static constexpr int abi = 1;
    using Provider = std::function<Formation()>;
    static Formations* acquire(QObject* host)
    {
        if (auto* existing = host->findChild<QObject*>(QStringLiteral("partyFormations"), Qt::FindDirectChildrenOnly))
            return existing->property("partyFormationAbi").toInt() == abi ? static_cast<Formations*>(existing) : nullptr;
        return new Formations(host);
    }
    // One act provides marks at a time; a newer provider replaces the older one.
    void setProvider(QObject* owner, Provider provider)
    {
        disconnect(destroyed_);
        owner_ = owner; provider_ = std::move(provider);
        destroyed_ = connect(owner, &QObject::destroyed, this, [this, owner] { clear(owner); });
    }
    // QPointer is already null while its object emits destroyed().
    void clear(QObject* owner)
    {
        if (owner_ && owner_ != owner) return;
        disconnect(destroyed_); owner_.clear(); provider_ = {};
    }
    Formation current() const { return owner_ && provider_ ? provider_() : Formation{}; }
    bool active() const { return owner_ && provider_; }
private:
    explicit Formations(QObject* host) : QObject(host)
    {
        setObjectName(QStringLiteral("partyFormations")); setProperty("partyFormationAbi", abi);
    }
    QPointer<QObject> owner_;
    Provider provider_;
    QMetaObject::Connection destroyed_;
};

} // namespace party_scene
