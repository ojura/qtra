#pragma once

// Shared by the snippets that displace part of the widget's vertex buffer.
//
// Each of them zeroes some region of that buffer so the triangles it feeds
// rasterize nothing, and each has to refuse when another module has already
// done so. Otherwise it saves that module's zeros as the originals and the
// face is lost when either one restores.
//
// That refusal is the safety mechanism rather than a nicety: the menu-based
// exclusion between these snippets is advisory, since two generations can be
// installed at once over the socket while a menu entry tracks only one. So the
// check lives here in one implementation. It was previously three, copied
// verbatim into two files and written differently in a third, which is the
// arrangement that produced the original asymmetry, where one snippet had a
// replay guard and its twin did not.
//
// Reading ownership out of the vertex values is in-band signalling and this
// header does not fix that; it only stops the reading being done three ways.
// The out-of-band answer is a record of the displacement in the host's byte
// stash, at which point this check becomes unnecessary rather than shared.

#include "cube_widget.h"

#include <algorithm>
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

} // namespace cube_mesh
