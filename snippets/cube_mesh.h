#pragma once

// Shared by the snippets that displace part of the widget's vertex buffer.
//
// Each of them zeroes some region of that buffer so the triangles it feeds
// rasterize nothing, and each has to know whether another module is already
// displacing the region it wants. Otherwise it saves that module's zeros as
// the originals, and the face is lost when either one restores.
//
// Two different questions live here, and they are answered in two different
// ways on purpose.
//
// Who owns a region, answered by the records further down. Reading ownership
// out of the vertex values instead would be in-band signalling: zeros are a
// legal vertex value, so such a check has to reason about which legal data
// happens to look like a message, can only work a whole face at a time, and
// leaves a six-vertex displacement invisible to a thirty-six-vertex one's
// guard. A claim says it directly and overlap is arithmetic, so the two sizes
// compare without either having to guess the other's granularity.
//
// Whether a displacement is still in effect, answered by looking at the
// values, which is what the first two functions do. This one belongs in-band:
// it asks what the buffer holds right now, and a record cannot answer that
// because a module can be wrong about what it did. It is the guard a restore
// runs before replaying saved bytes, so that a region something else has since
// changed is left alone rather than reverted.

#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace cube_mesh {

// Whether one face's worth of floats, starting at `start`, is entirely zero.
//
// This cannot be answered per float: the widget's own face colours contain
// zeros, so only a whole face of them means anything.
inline bool faceCollapsedAt(const std::vector<float>& vertices, const int start)
{
    if (start < 0 || start + cubeFloatsPerFace > static_cast<int>(vertices.size())) {
        return false;
    }
    const auto begin = vertices.begin() + start;
    return std::all_of(begin, begin + cubeFloatsPerFace,
                       [](const float value) { return value == 0.0F; });
}

// Whether any face in the given span is collapsed. Testing the whole buffer for
// zeros instead only catches a replacement that took every face, and misses one
// that took a single face, which is the case that loses a face on restore.
inline bool anyFaceCollapsed(const std::vector<float>& vertices)
{
    const auto total = static_cast<int>(vertices.size());
    for (int start = 0; start + cubeFloatsPerFace <= total; start += cubeFloatsPerFace) {
        if (faceCollapsedAt(vertices, start)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Displacement records
//
// The check above reads ownership out of the vertex values, which is in-band
// signalling: zeros are a legal vertex value, so the guard has to reason about
// which legal data happens to look like a message. That is why it must be done
// per face rather than per float, and why a module displacing a sub-face region
// would reopen the problem.
//
// The record below says it out of band instead. Two keys, because the two facts
// have different lifetimes and conflating them is what makes either one wrong:
//
//   cube.vertexBuffer/<offset>+<length>          the displaced originals
//   cube.vertexBuffer/<offset>+<length>/claimed   a displacement is in effect
//
// The bytes persist after release, because deciding a restore actually worked
// takes an observation no module can make about itself, and a restore that
// corrupts rather than restores must not delete the only good copy. The claim
// is dropped on release, because its whole meaning is "right now". If the bytes
// carried both meanings, the first install would block every later one forever.
//
// A claim outlives its module only in one way, and it is not the obvious one.
// The stash lives in this process, so a crash takes the claims, the bytes and
// the displaced buffer together and the next process starts clean, which
// unsafe.crash verifies. What does persist is a claim
// whose module never released successfully: a release that could not recover
// the originals keeps its claim deliberately, and one that is simply never
// called keeps it by default.
//
// Such a claim blocks its region until someone drops it, which is the
// conservative failure. The escape is a stash.drop from the driver, and for a
// region still holding the zeros it should be a repair module that restores the
// bytes first, because dropping alone leaves the region collapsed and unclaimed
// for the next install to record as its originals.

inline QByteArray regionKey(const int offsetFloats, const int lengthFloats)
{
    return QStringLiteral("cube.vertexBuffer/%1+%2")
        .arg(offsetFloats).arg(lengthFloats).toUtf8();
}

inline QByteArray claimKey(const int offsetFloats, const int lengthFloats)
{
    return regionKey(offsetFloats, lengthFloats) + "/claimed";
}

struct Claim {
    int offsetFloats = 0;
    int lengthFloats = 0;
    QString owner;
};

// Every claim currently in the host's stash, as records rather than as a
// pattern read out of the buffer.
inline std::vector<Claim> currentClaims(const RuntimeAgentHostV1* host)
{
    std::vector<Claim> claims;
    if (host->stash_list == nullptr) {
        return claims;
    }
    std::int64_t size = host->stash_list(host->agent_context, nullptr, 0);
    if (size <= 0) {
        return claims;
    }
    QByteArray json(static_cast<int>(size), '\0');
    // The stash can be written from another thread between the size query and
    // the fill. A short read would parse to an empty array and silently turn
    // the overlap guard off for this install, so it is retried instead.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::int64_t written = host->stash_list(host->agent_context, json.data(), size);
        if (written == size) {
            break;
        }
        if (written <= 0) {
            return claims;
        }
        json.resize(static_cast<int>(written));
        size = written;
    }

    static const QRegularExpression pattern(
        QStringLiteral("^cube\\.vertexBuffer/(\\d+)\\+(\\d+)/claimed$"));
    for (const QJsonValue& value : QJsonDocument::fromJson(json).array()) {
        const QJsonObject entry = value.toObject();
        const auto match = pattern.match(entry.value(QStringLiteral("key")).toString());
        if (!match.hasMatch()) {
            continue;
        }
        claims.push_back(Claim{match.captured(1).toInt(),
                               match.captured(2).toInt(),
                               entry.value(QStringLiteral("moduleName")).toString()});
    }
    return claims;
}

// The claim overlapping this region, if any. Overlap is arithmetic over
// records, so a six-vertex displacement and a thirty-six-vertex one compare
// directly instead of one being invisible to the other's guard.
inline bool overlappingClaim(const RuntimeAgentHostV1* host,
                             const int offsetFloats,
                             const int lengthFloats,
                             Claim& found)
{
    for (const Claim& claim : currentClaims(host)) {
        if (offsetFloats < claim.offsetFloats + claim.lengthFloats
            && claim.offsetFloats < offsetFloats + lengthFloats) {
            found = claim;
            return true;
        }
    }
    return false;
}

} // namespace cube_mesh
