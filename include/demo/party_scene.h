#pragma once

// Shared by the separately loaded party modules. The registry lives once in the
// cube's QObject tree, not in a DSO-local static. The host retains loaded DSOs,
// so the first owner's inline vtable remains valid across independent reloads.
#include "cube_widget.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLPaintDevice>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPicture>
#include <QPixmap>
#include <QRegion>
#include <QPointer>
#include <QTimer>
#include <QVector2D>
#include <QVector4D>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <vector>

namespace party_scene {

inline float groundDepth(double y)
{
    // One tilted ground plane under the host's perspective camera. Its
    // reciprocal view depth is affine in screen y; 65% is the cube's depth.
    return static_cast<float>(1.0 / std::clamp(1.0 / cubeViewDistance + (y - .65) * .40, 1.0 / 18.0, 1.0 / 2.5));
}


struct DepthSlice {
    enum Part { Whole = 0, FarOfGround = 1, NearOfGround = 2 };
    float farDepth = 100.F, nearDepth = .1F;
    Part part = Whole;
    QMatrix4x4 projection;
    QVector4D viewport;
    QVector2D depthRange{0.F, 1.F};
    void apply(QOpenGLShaderProgram& program) const
    {
        program.setUniformValue("partyClipEnabled", 1);
        program.setUniformValue("partyClipPart", int(part));
        program.setUniformValue("partyClipBounds", QVector2D(nearDepth, farDepth));
        program.setUniformValue("partyProjection", QVector2D(projection(2, 2), projection(2, 3)));
        program.setUniformValue("partyViewport", viewport);
        program.setUniformValue("partyDepthRange", depthRange);
        program.setUniformValue("partyCameraDistance", cubeViewDistance);
    }
    static void disable(QOpenGLShaderProgram& program) { program.setUniformValue("partyClipEnabled", 0); }
};

inline QByteArray withDepthSlice(const char* fragmentSource)
{
    static constexpr auto declarations = R"glsl(
uniform int partyClipEnabled;
uniform int partyClipPart;
uniform vec2 partyClipBounds;
uniform vec2 partyProjection;
uniform vec4 partyViewport;
uniform vec2 partyDepthRange;
uniform float partyCameraDistance;
float partyGroundDepth() {
    float y = 1.0 - (gl_FragCoord.y - partyViewport.y) / partyViewport.w;
    return 1.0 / clamp(1.0 / partyCameraDistance + (y - 0.65) * 0.40, 1.0 / 18.0, 1.0 / 2.5);
}
bool partyOutsideGroundInterval() {
    if (partyClipEnabled == 0) return false;
    float g = partyGroundDepth();
    return !(g > partyClipBounds.x && g <= partyClipBounds.y);
}
bool partyOutsideDepthSlice() {
    if (partyClipEnabled == 0) return false;
    float z = (gl_FragCoord.z - partyDepthRange.x) / (partyDepthRange.y - partyDepthRange.x);
    float d = partyProjection.y / (2.0 * z - 1.0 + partyProjection.x);
    float n = partyClipBounds.x, f = partyClipBounds.y;
    float b = clamp(partyGroundDepth(), n, f);
    if (partyClipPart == 1) n = b;
    if (partyClipPart == 2) f = b;
    return !(d > n && d <= f);
}
)glsl";
    QByteArray result(fragmentSource);
    const auto version = result.indexOf("#version");
    const auto end = version < 0 ? -1 : result.indexOf('\n', version);
    result.insert(end < 0 ? 0 : end + 1, declarations);
    return result;
}

struct GeometryPasses {
    // Preparation advances the provider once per real frame. Draw callbacks
    // reuse that prepared state, and may not advance clocks or emit events.
    std::function<void()> prepare, background, opaque;
    std::function<bool()> translucentActive;
    std::function<void(const DepthSlice&)> translucent;
};

namespace detail {

// Records one actor's QPainter commands in memory, with shared copies of their
// paths, pens and fonts, and measures their device bounds as it goes, text
// included. Replaying needs no decoding, and the bounds need no second pass.
class Recording {
public:
    void replay(QPainter& painter, const QTransform& base, const QRect& slot) const;
    QRectF bounds;
private:
    friend class RecordingEngine;
    struct State {
        QPaintEngine::DirtyFlags dirty;
        QPen pen; QBrush brush, background; QPointF brushOrigin; QFont font;
        Qt::BGMode backgroundMode = Qt::TransparentMode; QTransform transform;
        Qt::ClipOperation clipOperation = Qt::NoClip; QRegion clipRegion; QPainterPath clipPath; bool clipEnabled = false;
        QPainter::RenderHints hints; QPainter::CompositionMode composition = QPainter::CompositionMode_SourceOver;
        qreal opacity = 1;
    };
    struct Command {
        enum Kind : quint8 { Apply, Path, Polygon, Polyline, ConvexPolygon, Rects, Lines, Ellipse, Points, Text, Image, Pixmap, TiledPixmap };
        Kind kind;
        int index = 0;   // into the matching store below
        int extra = 0;   // winding fill for polygons; image or pixmap index for images
    };
    std::vector<Command> commands;
    std::vector<State> states;
    std::vector<QPainterPath> paths;
    std::vector<QPolygonF> polygons;
    std::vector<std::pair<QRectF, QRectF>> rects;   // target and source, or one rectangle
    struct TextRun { QPointF origin; QString text; QFont font; };
    std::vector<TextRun> texts;
    std::vector<QImage> images;
    std::vector<QPixmap> pixmaps;
    std::vector<Qt::ImageConversionFlags> imageFlags;
};

class RecordingEngine final : public QPaintEngine {
public:
    RecordingEngine() : QPaintEngine(QPaintEngine::AllFeatures) {}
    bool begin(QPaintDevice*) override { transform_ = QTransform(); pen_ = QPen(); return true; }
    bool end() override { return true; }
    Type type() const override { return QPaintEngine::User; }
    void updateState(const QPaintEngineState& state) override
    {
        Recording::State copy; copy.dirty = state.state();
        if (copy.dirty & DirtyPen) pen_ = copy.pen = state.pen();
        if (copy.dirty & DirtyBrush) copy.brush = state.brush();
        if (copy.dirty & DirtyBrushOrigin) copy.brushOrigin = state.brushOrigin();
        if (copy.dirty & DirtyFont) copy.font = state.font();
        if (copy.dirty & DirtyBackground) copy.background = state.backgroundBrush();
        if (copy.dirty & DirtyBackgroundMode) copy.backgroundMode = state.backgroundMode();
        if (copy.dirty & DirtyTransform) transform_ = copy.transform = state.transform();
        if (copy.dirty & (DirtyClipRegion | DirtyClipPath)) copy.clipOperation = state.clipOperation();
        if (copy.dirty & DirtyClipRegion) copy.clipRegion = state.clipRegion();
        if (copy.dirty & DirtyClipPath) copy.clipPath = state.clipPath();
        if (copy.dirty & DirtyClipEnabled) copy.clipEnabled = state.isClipEnabled();
        if (copy.dirty & DirtyHints) copy.hints = state.renderHints();
        if (copy.dirty & DirtyCompositionMode) copy.composition = state.compositionMode();
        if (copy.dirty & DirtyOpacity) copy.opacity = state.opacity();
        push(Recording::Command::Apply, int(target->states.size())); target->states.push_back(std::move(copy));
    }
    void drawPath(const QPainterPath& path) override
    {
        stroke(path.controlPointRect()); push(Recording::Command::Path, int(target->paths.size())); target->paths.push_back(path);
    }
    void drawPolygon(const QPointF* points, int count, PolygonDrawMode mode) override
    {
        QPolygonF polygon(count); std::copy(points, points + count, polygon.begin());
        addPolygon(std::move(polygon), mode);
    }
    void drawPolygon(const QPoint* points, int count, PolygonDrawMode mode) override
    {
        QPolygonF polygon(count); for (int i = 0; i < count; ++i) polygon[i] = points[i];
        addPolygon(std::move(polygon), mode);
    }
    void drawRects(const QRectF* rects, int count) override
    {
        for (int i = 0; i < count; ++i) { stroke(rects[i]); push(Recording::Command::Rects, int(target->rects.size())); target->rects.push_back({rects[i], {}}); }
    }
    void drawRects(const QRect* rects, int count) override
    {
        for (int i = 0; i < count; ++i) { const QRectF rect(rects[i]); drawRects(&rect, 1); }
    }
    void drawLines(const QLineF* lines, int count) override
    {
        QPolygonF points; points.reserve(count * 2);
        for (int i = 0; i < count; ++i) points << lines[i].p1() << lines[i].p2();
        stroke(points.boundingRect()); push(Recording::Command::Lines, int(target->polygons.size())); target->polygons.push_back(std::move(points));
    }
    void drawLines(const QLine* lines, int count) override
    {
        for (int i = 0; i < count; ++i) { const QLineF line(lines[i]); drawLines(&line, 1); }
    }
    void drawEllipse(const QRectF& rect) override
    {
        stroke(rect); push(Recording::Command::Ellipse, int(target->rects.size())); target->rects.push_back({rect, {}});
    }
    void drawEllipse(const QRect& rect) override { drawEllipse(QRectF(rect)); }
    void drawPoints(const QPointF* points, int count) override
    {
        QPolygonF polygon(count); std::copy(points, points + count, polygon.begin());
        stroke(polygon.boundingRect()); push(Recording::Command::Points, int(target->polygons.size())); target->polygons.push_back(std::move(polygon));
    }
    void drawPoints(const QPoint* points, int count) override
    {
        for (int i = 0; i < count; ++i) { const QPointF point(points[i]); drawPoints(&point, 1); }
    }
    void drawTextItem(const QPointF& origin, const QTextItem& item) override
    {
        measure(transform_.mapRect(QRectF(origin.x(), origin.y() - item.ascent(), item.width(), item.ascent() + item.descent())), 2);
        // QPainter draws underline, overline and strike-out as separate lines,
        // so the replayed font must not draw them again.
        QFont font = item.font(); font.setUnderline(false); font.setOverline(false); font.setStrikeOut(false);
        push(Recording::Command::Text, int(target->texts.size())); target->texts.push_back({origin, item.text(), font});
    }
    void drawPixmap(const QRectF& rect, const QPixmap& pixmap, const QRectF& source) override
    {
        measure(transform_.mapRect(rect), 1);
        push(Recording::Command::Pixmap, int(target->rects.size()), int(target->pixmaps.size()));
        target->rects.push_back({rect, source}); target->pixmaps.push_back(pixmap);
    }
    void drawTiledPixmap(const QRectF& rect, const QPixmap& pixmap, const QPointF& offset) override
    {
        measure(transform_.mapRect(rect), 1);
        push(Recording::Command::TiledPixmap, int(target->rects.size()), int(target->pixmaps.size()));
        target->rects.push_back({rect, QRectF(offset, QSizeF())}); target->pixmaps.push_back(pixmap);
    }
    void drawImage(const QRectF& rect, const QImage& image, const QRectF& source, Qt::ImageConversionFlags flags) override
    {
        measure(transform_.mapRect(rect), 1);
        push(Recording::Command::Image, int(target->rects.size()), int(target->images.size()));
        target->rects.push_back({rect, source}); target->images.push_back(image); target->imageFlags.push_back(flags);
    }
    Recording* target = nullptr;
private:
    void push(Recording::Command::Kind kind, int index, int extra = 0) { target->commands.push_back({kind, index, extra}); }
    void addPolygon(QPolygonF polygon, PolygonDrawMode mode)
    {
        stroke(polygon.boundingRect());
        push(mode == PolylineMode ? Recording::Command::Polyline : mode == ConvexMode ? Recording::Command::ConvexPolygon
             : Recording::Command::Polygon, int(target->polygons.size()), mode == WindingMode);
        target->polygons.push_back(std::move(polygon));
    }
    void measure(const QRectF& device, double margin) { target->bounds |= device.adjusted(-margin, -margin, margin, margin); }
    // Strokes extend by their width: logically for normal pens, in device
    // pixels for cosmetic ones. A full width also covers miter joins and caps.
    void stroke(const QRectF& logical)
    {
        if (pen_.style() == Qt::NoPen) { measure(transform_.mapRect(logical), 1); return; }
        const double width = pen_.widthF();
        if (pen_.isCosmetic() || width <= 0) { measure(transform_.mapRect(logical), std::max(1.0, width) + 1); return; }
        measure(transform_.mapRect(logical.adjusted(-width, -width, width, width)), 1);
    }
    QTransform transform_;
    QPen pen_;
};

// A paint device for recording. Its metrics match QPicture's, whose logical
// DPI is the application default that QOpenGLPaintDevice also uses, so text
// keeps its size when replayed.
class RecordingDevice final : public QPaintDevice {
public:
    explicit RecordingDevice(QSize size) : size_(size) {}
    QPaintEngine* paintEngine() const override { return &engine; }
    mutable RecordingEngine engine;
protected:
    int metric(PaintDeviceMetric metric) const override
    {
        static const QPicture reference;
        switch (metric) {
        case PdmWidth: return size_.width();
        case PdmHeight: return size_.height();
        case PdmWidthMM: return qRound(size_.width() * 25.4 / reference.logicalDpiX());
        case PdmHeightMM: return qRound(size_.height() * 25.4 / reference.logicalDpiY());
        case PdmDpiX: case PdmPhysicalDpiX: return reference.logicalDpiX();
        case PdmDpiY: case PdmPhysicalDpiY: return reference.logicalDpiY();
        case PdmNumColors: return INT_MAX;
        case PdmDepth: return 32;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
        default: return QPaintDevice::metric(metric);
        }
    }
private:
    QSize size_;
};

// Replays into a painter whose base transform places the actor in its atlas
// slot. The slot stays clipped: an actor's own clip intersects it, and an actor
// that removes its clip returns to the slot.
inline void Recording::replay(QPainter& painter, const QTransform& base, const QRect& slot) const
{
    QTransform transform;
    const auto slotClip = [&] {
        painter.setWorldTransform(QTransform()); painter.setClipRect(slot, Qt::ReplaceClip); painter.setWorldTransform(transform * base);
    };
    painter.save();
    painter.setWorldTransform(base); slotClip();
    for (const auto& command : commands) {
        const int index = command.index;
        switch (command.kind) {
        case Command::Apply: {
            const auto& state = states[index];
            if (state.dirty & QPaintEngine::DirtyTransform) { transform = state.transform; painter.setWorldTransform(transform * base); }
            if (state.dirty & (QPaintEngine::DirtyClipRegion | QPaintEngine::DirtyClipPath)) {
                if (state.clipOperation == Qt::NoClip || state.clipOperation == Qt::ReplaceClip) slotClip();
                if (state.clipOperation != Qt::NoClip) {
                    if (state.dirty & QPaintEngine::DirtyClipPath) painter.setClipPath(state.clipPath, Qt::IntersectClip);
                    else painter.setClipRegion(state.clipRegion, Qt::IntersectClip);
                }
            } else if ((state.dirty & QPaintEngine::DirtyClipEnabled) && !state.clipEnabled) slotClip();
            if (state.dirty & QPaintEngine::DirtyPen) painter.setPen(state.pen);
            if (state.dirty & QPaintEngine::DirtyBrush) painter.setBrush(state.brush);
            if (state.dirty & QPaintEngine::DirtyBrushOrigin) painter.setBrushOrigin(state.brushOrigin);
            if (state.dirty & QPaintEngine::DirtyFont) painter.setFont(state.font);
            if (state.dirty & QPaintEngine::DirtyBackground) painter.setBackground(state.background);
            if (state.dirty & QPaintEngine::DirtyBackgroundMode) painter.setBackgroundMode(state.backgroundMode);
            if (state.dirty & QPaintEngine::DirtyHints) {
                painter.setRenderHints(painter.renderHints(), false); painter.setRenderHints(state.hints, true);
            }
            if (state.dirty & QPaintEngine::DirtyCompositionMode) painter.setCompositionMode(state.composition);
            if (state.dirty & QPaintEngine::DirtyOpacity) painter.setOpacity(state.opacity);
            break;
        }
        case Command::Path: painter.drawPath(paths[index]); break;
        case Command::Polygon: painter.drawPolygon(polygons[index], command.extra ? Qt::WindingFill : Qt::OddEvenFill); break;
        case Command::ConvexPolygon: painter.drawConvexPolygon(polygons[index]); break;
        case Command::Polyline: painter.drawPolyline(polygons[index]); break;
        case Command::Rects: painter.drawRect(rects[index].first); break;
        case Command::Lines: {
            const auto& points = polygons[index];
            for (qsizetype i = 0; i + 1 < points.size(); i += 2) painter.drawLine(points[i], points[i + 1]);
            break;
        }
        case Command::Ellipse: painter.drawEllipse(rects[index].first); break;
        case Command::Points: painter.drawPoints(polygons[index]); break;
        case Command::Text: {
            const auto& run = texts[index];
            if (painter.font() != run.font) painter.setFont(run.font);
            painter.drawText(run.origin, run.text);
            break;
        }
        case Command::Image:
            painter.drawImage(rects[index].first, images[command.extra], rects[index].second, imageFlags[command.extra]); break;
        case Command::Pixmap: painter.drawPixmap(rects[index].first, pixmaps[command.extra], rects[index].second); break;
        case Command::TiledPixmap: painter.drawTiledPixmap(rects[index].first, pixmaps[command.extra], rects[index].second.topLeft()); break;
        }
    }
    painter.restore();
}

// Paints into the atlas page's multisampled framebuffer. The page is bound
// through QOpenGLFramebufferObject, which also records it as the context's
// current framebuffer: Qt's glyph cache copies through a framebuffer of its own
// and then rebinds that recorded one, so a raw glBindFramebuffer would send all
// following text and shapes to the cube's framebuffer.
class PageDevice final : public QOpenGLPaintDevice {
public:
    explicit PageDevice(QOpenGLFramebufferObject* page) : QOpenGLPaintDevice(page->size()), page_(page) { setPaintFlipped(true); }
    void ensureActiveTarget() override { page_->bind(); }
private:
    QOpenGLFramebufferObject* page_;
};

} // namespace detail

struct Sprite {
    QString name;
    QRect bounds;           // logical window pixels
    std::shared_ptr<const detail::Recording> drawing;
    float depth = cubeViewDistance;
    bool ground = false;
    int page = -1;          // atlas page and its device-pixel rectangle
    QRect slot;
};

class Frame {
public:
    static constexpr int maxSprites = 96;
    explicit Frame(QSize size) : size_(size), device_(size) {}
    // Records the drawing immediately: callers may capture loop locals.
    void add(const QString& name, float depth, const std::function<void(QPainter&)>& draw, bool ground = false)
    {
        if (int(sprites.size()) >= maxSprites || !std::isfinite(depth) || depth <= .1F || depth >= 100.F) {
            error = QStringLiteral("Invalid or excessive scene submission"); return;
        }
        auto drawing = std::make_shared<detail::Recording>();
        device_.engine.target = drawing.get();
        { QPainter painter(&device_); painter.setRenderHint(QPainter::Antialiasing);
          painter.setRenderHint(QPainter::TextAntialiasing); draw(painter); }
        device_.engine.target = nullptr;
        const QRect bounds = drawing->bounds.toAlignedRect().adjusted(-2, -2, 2, 2).intersected(QRect(QPoint(), size_));
        if (bounds.isEmpty()) return;
        sprites.push_back({name, bounds, std::move(drawing), depth, ground});
    }
    void ground(const QString& name, const std::function<void(QPainter&)>& draw)
    { add(name, cubeViewDistance, draw, true); }
    QSize size() const { return size_; }
    std::vector<Sprite> sprites;
    QString error;
private:
    QSize size_;
    detail::RecordingDevice device_;
};

// A name tag: text on its own dark rounded backing, so it stays readable over
// any scene content. The caller chooses the depth: a tag can stand on the floor
// in front of a walking actor or hang below a flying one.
inline QSizeF tagSize(const QFont& font, const QString& text)
{
    const QFontMetricsF metrics(font);
    return {metrics.horizontalAdvance(text) + metrics.height(), metrics.height() * 1.32};
}
inline void addTag(Frame& frame, const QString& name, float depth, QPointF topCenter, const QString& text,
                   const QFont& font, const QColor& ink, const QColor& edge)
{
    frame.add(name, depth, [&](QPainter& p) {
        const QSizeF size = tagSize(font, text);
        const QRectF box(topCenter.x() - size.width() / 2, topCenter.y(), size.width(), size.height());
        p.setPen(QPen(edge, std::max(1.0, size.height() * .05))); p.setBrush(QColor(7, 12, 24, 216));
        p.drawRoundedRect(box, size.height() / 2, size.height() / 2);
        p.setFont(font); p.setPen(ink); p.drawText(box, Qt::AlignCenter, text);
    });
}

class Scene final : public QObject {
public:
    // Modules built against a different Scene layout refuse the shared object.
    static constexpr int abi = 3;
    using Provider = std::function<void(Frame&)>;
    static Scene* acquire(CubeWidget* cube)
    {
        if (auto* existing = cube->findChild<QObject*>(QStringLiteral("partySceneDepth"), Qt::FindDirectChildrenOnly))
            return existing->property("partySceneAbi").toInt() == abi ? static_cast<Scene*>(existing) : nullptr;
        return new Scene(cube);
    }
    void addOwner(QObject* owner, Provider provider) { add(owner, std::move(provider), {}); }
    void addGeometryOwner(QObject* owner, GeometryPasses passes) { add(owner, {}, std::move(passes)); }
    void removeOwner(QObject* owner, bool destroyEmpty = true)
    {
        for (auto i = owners_.begin(); i != owners_.end();) {
            if (!i->object || i->object == owner) { disconnect(i->destroyed); i = owners_.erase(i); }
            else ++i;
        }
        forceRecord_ = true;
        if (destroyEmpty && owners_.empty()) { delete this; return; }
        if (cube_) cube_->update();
    }
    void invalidate() { forceRecord_ = true; if (cube_) cube_->update(); }
    QJsonObject status() const
    {
        QJsonArray actors;
        for (const auto& sprite : sprites_) actors.append(QJsonObject{
            {QStringLiteral("name"), sprite.name}, {QStringLiteral("depth"), sprite.depth},
            {QStringLiteral("ground"), sprite.ground}, {QStringLiteral("bounds"), QJsonArray{
                sprite.bounds.x(), sprite.bounds.y(), sprite.bounds.width(), sprite.bounds.height()}}});
        return {{QStringLiteral("owners"), int(owners_.size())}, {QStringLiteral("frames"), qint64(frames_)},
                {QStringLiteral("rasterFrames"), qint64(rasterFrames_)}, {QStringLiteral("rasterPixels"), rasterPixels_},
                {QStringLiteral("rasterScale"), rasterScale_}, {QStringLiteral("transparentDraws"), transparentDraws_},
                {QStringLiteral("atlas"), QJsonObject{{QStringLiteral("pages"), pagesUsed_}, {QStringLiteral("capacity"), atlasPages_},
                    {QStringLiteral("width"), pageSize_.width()}, {QStringLiteral("pageHeight"), pageSize_.height()},
                    {QStringLiteral("samples"), samples_}}},
                {QStringLiteral("timingMs"), QJsonObject{{QStringLiteral("record"), recordMs_},
                    {QStringLiteral("raster"), rasterMs_}, {QStringLiteral("composite"), compositeMs_}}},
                {QStringLiteral("error"), error_}, {QStringLiteral("actors"), actors}};
    }
    ~Scene() override
    {
        disconnect(drawConnection_); disconnect(contextConnection_); timer_.stop();
        for (const auto& owner : owners_) disconnect(owner.destroyed);
        owners_.clear();
        cleanupWithContext();
    }
private:
    struct Owner { QPointer<QObject> object; Provider paint; GeometryPasses geometry; QMetaObject::Connection destroyed; };
    // At most four window-sized atlas pages; the scale shrinks before that is exceeded.
    static constexpr int pageLimit = 4;
    // Two cube updates closer than this reuse the recorded actors.
    static constexpr qint64 minimumRecordNs = 6000000;
    void add(QObject* owner, Provider provider, GeometryPasses geometry)
    {
        removeOwner(owner, false);
        Owner entry; entry.object = owner; entry.paint = std::move(provider); entry.geometry = std::move(geometry);
        entry.destroyed = connect(owner, &QObject::destroyed, this, [this, owner] { removeOwner(owner); });
        owners_.push_back(std::move(entry)); forceRecord_ = true; cube_->update();
    }
    explicit Scene(CubeWidget* cube) : QObject(cube), cube_(cube)
    {
        setObjectName(QStringLiteral("partySceneDepth")); setProperty("partySceneAbi", abi);
        recordClock_.start();
        // The cube's own animation timer drives frames. This timer keeps actors
        // moving only while that timer is stopped.
        timer_.setInterval(16); timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, [this] { if (cube_ && !cube_->m_timer.isActive()) cube_->update(); });
        timer_.start();
        drawConnection_ = connect(cube, &CubeWidget::frameRendered, this, [this](qulonglong, float) { render(); }, Qt::DirectConnection);
    }
    void cleanup()
    {
        if (gl_ && owner_ == QOpenGLContext::currentContext()) {
            device_.reset(); page_.reset();
            if (atlasTexture_) gl_->glDeleteTextures(1, &atlasTexture_);
            if (atlasFramebuffer_) gl_->glDeleteFramebuffers(1, &atlasFramebuffer_);
            if (vbo_) gl_->glDeleteBuffers(1, &vbo_);
            if (vao_) gl_->glDeleteVertexArrays(1, &vao_);
        } else {
            // Qt's paint engine and framebuffer object free GL objects in their
            // destructors. Without their context current that would call into
            // GL, so they are left behind.
            (void)device_.release(); (void)page_.release();
        }
        atlasTexture_ = atlasFramebuffer_ = vbo_ = vao_ = 0;
        pageSize_ = QSize(); atlasPages_ = pagesUsed_ = 0;
        program_.reset(); gl_ = nullptr; owner_.clear();
    }
    void cleanupWithContext()
    {
        auto* previous = QOpenGLContext::currentContext();
        auto* surface = previous ? previous->surface() : nullptr;
        if (cube_ && owner_ && cube_->context() == owner_) {
            cube_->makeCurrent(); cleanup(); cube_->doneCurrent();
            if (previous && surface) previous->makeCurrent(surface);
        } else cleanup();
    }
    bool initialize()
    {
        auto* context = QOpenGLContext::currentContext();
        if (!context || !cube_ || context != cube_->context()) return false;
        if (owner_ == context && program_) return true;
        cleanup();
        owner_ = context; gl_ = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context);
        if (!gl_ || !gl_->initializeOpenGLFunctions()) { error_ = QStringLiteral("Scene needs OpenGL 3.3"); return false; }
        program_ = std::make_unique<QOpenGLShaderProgram>();
        static constexpr auto vertex = R"glsl(
#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
uniform mat4 projection;
uniform vec4 uvRect;
noperspective out vec2 texCoord;
void main() { gl_Position = projection * vec4(position, 1.0); texCoord = uvRect.xy + uv * uvRect.zw; }
)glsl";
        static constexpr auto fragment = R"glsl(
#version 330 core
uniform sampler2D image;
uniform int translucent;
uniform int groundSurface;
noperspective in vec2 texCoord;
out vec4 color;
void main() {
    if (groundSurface != 0 ? partyOutsideGroundInterval() : partyOutsideDepthSlice()) discard;
    vec4 sampled = texture(image, texCoord);
    if (sampled.a < 0.01 || (translucent == 0 && sampled.a < 0.98) || (translucent != 0 && sampled.a >= 0.98)) discard;
    color = sampled;
}
)glsl";
        if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
            || !program_->addShaderFromSourceCode(QOpenGLShader::Fragment, withDepthSlice(fragment)) || !program_->link()) {
            error_ = program_->log(); cleanup(); return false;
        }
        gl_->glGenVertexArrays(1, &vao_); gl_->glGenBuffers(1, &vbo_);
        gl_->glBindVertexArray(vao_); gl_->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl_->glBufferData(GL_ARRAY_BUFFER, 18 * 5 * sizeof(float), nullptr, GL_STREAM_DRAW);
        gl_->glEnableVertexAttribArray(0); gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        gl_->glEnableVertexAttribArray(1); gl_->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
        GLint samples = 0, textureSize = 0;
        gl_->glGetIntegerv(GL_MAX_SAMPLES, &samples); gl_->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &textureSize);
        samples_ = std::clamp(samples, 1, 4); maxTextureSize_ = std::max(textureSize, 1024);
        disconnect(contextConnection_);
        contextConnection_ = connect(context, &QOpenGLContext::aboutToBeDestroyed, this, [this] { cleanupWithContext(); }, Qt::DirectConnection);
        forceRecord_ = true;
        return true;
    }
    struct GlState {
        QOpenGLFunctions_3_3_Core* gl;
        GLint program, vao, buffer, unpackBuffer, activeTexture, texture, depthFunction, srcRgb, dstRgb, srcAlpha, dstAlpha, eqRgb, eqAlpha;
        GLint unpackAlignment, unpackRowLength, unpackSkipPixels, unpackSkipRows, sampler, polygon[2];
        GLboolean depthMask, depthTest, blend, cull, scissor, pointSize, colorMask[4];
        GLdouble depthRange[2];
        explicit GlState(QOpenGLFunctions_3_3_Core* functions) : gl(functions)
        {
            gl->glGetIntegerv(GL_CURRENT_PROGRAM, &program); gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            gl->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer); gl->glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
            gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture); gl->glActiveTexture(GL_TEXTURE0);
            gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture); gl->glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
            gl->glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb); gl->glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
            gl->glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha); gl->glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
            gl->glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb); gl->glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqAlpha);
            gl->glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment); gl->glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
            gl->glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpackSkipPixels); gl->glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpackSkipRows);
            gl->glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &sampler);
            gl->glGetIntegerv(GL_POLYGON_MODE, polygon); gl->glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
            depthTest = gl->glIsEnabled(GL_DEPTH_TEST); blend = gl->glIsEnabled(GL_BLEND);
            cull = gl->glIsEnabled(GL_CULL_FACE); scissor = gl->glIsEnabled(GL_SCISSOR_TEST);
            gl->glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
            pointSize = gl->glIsEnabled(GL_PROGRAM_POINT_SIZE); gl->glGetDoublev(GL_DEPTH_RANGE, depthRange);
        }
        ~GlState()
        {
            gl->glUseProgram(program); gl->glBindVertexArray(vao); gl->glBindBuffer(GL_ARRAY_BUFFER, buffer);
            gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer); gl->glActiveTexture(GL_TEXTURE0);
            gl->glBindTexture(GL_TEXTURE_2D, texture); gl->glActiveTexture(activeTexture);
            gl->glDepthFunc(depthFunction); gl->glDepthMask(depthMask);
            gl->glBlendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha); gl->glBlendEquationSeparate(eqRgb, eqAlpha);
            gl->glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment); gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
            gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpackSkipPixels); gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, unpackSkipRows);
            gl->glBindSampler(0, sampler);
            gl->glPolygonMode(GL_FRONT_AND_BACK, polygon[0]); gl->glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
            set(GL_DEPTH_TEST, depthTest); set(GL_BLEND, blend); set(GL_CULL_FACE, cull); set(GL_SCISSOR_TEST, scissor);
            set(GL_PROGRAM_POINT_SIZE, pointSize); gl->glDepthRange(depthRange[0], depthRange[1]);
        }
        void set(GLenum flag, bool enabled) { enabled ? gl->glEnable(flag) : gl->glDisable(flag); }
    };
    // Offscreen rasterization changes the target, viewport, stencil, clear values
    // and the paint engine's second texture unit as well.
    struct TargetState : GlState {
        GLint drawFramebuffer, readFramebuffer, renderbuffer, viewport[4], scissorBox[4], secondTexture, secondSampler;
        GLint stencilFunction, stencilReference, stencilValueMask, stencilFail, stencilDepthFail, stencilDepthPass, stencilWriteMask, clearStencil;
        GLfloat clearColor[4]; GLdouble clearDepth; GLboolean stencil, multisample;
        explicit TargetState(QOpenGLFunctions_3_3_Core* functions) : GlState(functions)
        {
            gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer); gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
            gl->glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
            gl->glGetIntegerv(GL_VIEWPORT, viewport); gl->glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
            gl->glActiveTexture(GL_TEXTURE1); gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &secondTexture);
            gl->glGetIntegeri_v(GL_SAMPLER_BINDING, 1, &secondSampler); gl->glActiveTexture(GL_TEXTURE0);
            gl->glGetIntegerv(GL_STENCIL_FUNC, &stencilFunction); gl->glGetIntegerv(GL_STENCIL_REF, &stencilReference);
            gl->glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencilValueMask); gl->glGetIntegerv(GL_STENCIL_FAIL, &stencilFail);
            gl->glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &stencilDepthFail); gl->glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &stencilDepthPass);
            gl->glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilWriteMask); gl->glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &clearStencil);
            gl->glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor); gl->glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clearDepth);
            stencil = gl->glIsEnabled(GL_STENCIL_TEST); multisample = gl->glIsEnabled(GL_MULTISAMPLE);
        }
        ~TargetState()
        {
            gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer); gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
            gl->glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            gl->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            gl->glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
            gl->glActiveTexture(GL_TEXTURE1); gl->glBindTexture(GL_TEXTURE_2D, secondTexture); gl->glBindSampler(1, secondSampler);
            gl->glActiveTexture(GL_TEXTURE0);
            gl->glStencilFunc(stencilFunction, stencilReference, stencilValueMask);
            gl->glStencilOp(stencilFail, stencilDepthFail, stencilDepthPass); gl->glStencilMask(stencilWriteMask);
            gl->glClearStencil(clearStencil); gl->glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
            gl->glClearDepth(clearDepth);
            set(GL_STENCIL_TEST, stencil); set(GL_MULTISAMPLE, multisample);
        }
    };
    // Shelf packing in device pixels. Returns the number of pages used, or 0
    // when the actors do not fit at this scale.
    int pack(QSize page, double scale, int pages)
    {
        std::vector<std::size_t> order(sprites_.size());
        std::iota(order.begin(), order.end(), std::size_t(0));
        std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
            return sprites_[a].bounds.height() > sprites_[b].bounds.height();
        });
        int index = 0, x = 0, y = 0, shelf = 0;
        for (const auto item : order) {
            auto& sprite = sprites_[item];
            const QSize slot(std::max(1, int(std::ceil(sprite.bounds.width() * scale))),
                             std::max(1, int(std::ceil(sprite.bounds.height() * scale))));
            if (slot.width() > page.width() || slot.height() > page.height()) return 0;
            if (x + slot.width() > page.width()) { x = 0; y += shelf; shelf = 0; }
            if (y + slot.height() > page.height()) { if (++index >= pages) return 0; x = y = shelf = 0; }
            sprite.page = index; sprite.slot = QRect(QPoint(x, y), slot);
            x += slot.width() + 1; shelf = std::max(shelf, slot.height() + 1);
        }
        return index + 1;
    }
    bool ensureTargets(QSize page, int pages)
    {
        if (page != pageSize_ || !page_) {
            device_.reset(); page_.reset(); atlasPages_ = 0; pageSize_ = QSize();
            QOpenGLFramebufferObjectFormat format;
            format.setSamples(samples_); format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
            format.setInternalTextureFormat(GL_RGBA8);
            page_ = std::make_unique<QOpenGLFramebufferObject>(page, format);
            if (!page_->isValid()) { page_.reset(); error_ = QStringLiteral("Scene page framebuffer is incomplete"); return false; }
            page_->release();
            pageSize_ = page;
            device_ = std::make_unique<detail::PageDevice>(page_.get());
        }
        if (pages > atlasPages_) {
            if (!atlasTexture_) { gl_->glGenTextures(1, &atlasTexture_); gl_->glGenFramebuffers(1, &atlasFramebuffer_); }
            gl_->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            gl_->glBindTexture(GL_TEXTURE_2D, atlasTexture_);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl_->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, page.width(), page.height() * pages, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl_->glBindFramebuffer(GL_FRAMEBUFFER, atlasFramebuffer_);
            gl_->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, atlasTexture_, 0);
            if (gl_->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                error_ = QStringLiteral("Scene atlas framebuffer is incomplete"); atlasPages_ = 0; return false;
            }
            atlasPages_ = pages;
        }
        return true;
    }
    // Replays every actor through Qt's OpenGL paint engine into a multisampled
    // page and resolves the page into the atlas. Nothing is rasterized on the
    // CPU and nothing is uploaded.
    bool rasterize(QSize size)
    {
        const double ratio = std::max(1.0, cube_->devicePixelRatioF());
        const QSize page(std::max(1, int(std::ceil(size.width() * ratio))), std::max(1, int(std::ceil(size.height() * ratio))));
        if (page.width() > maxTextureSize_) { error_ = QStringLiteral("Scene page exceeds the texture limit"); return false; }
        const int pages = std::clamp(maxTextureSize_ / page.height(), 1, pageLimit);
        double scale = ratio; int used = 0;
        for (int attempt = 0; attempt < 16 && !(used = pack(page, scale, pages)); ++attempt) scale *= .85;
        if (!used) { error_ = QStringLiteral("Scene actors exceed the atlas"); return false; }
        TargetState state(gl_);
        if (!ensureTargets(page, used)) return false;
        // Qt's paint engine sets the state it uses and assumes defaults for the
        // rest. The cube enables back-face culling, and painting flipped
        // reverses the winding of every glyph quad, so culling would drop all text.
        gl_->glDisable(GL_CULL_FACE); gl_->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        gl_->glBindSampler(0, 0); gl_->glBindSampler(1, 0); gl_->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        qint64 pixels = 0;
        for (int index = 0; index < used; ++index) {
            page_->bind();
            gl_->glViewport(0, 0, page.width(), page.height());
            gl_->glDisable(GL_SCISSOR_TEST); gl_->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            gl_->glDepthMask(GL_TRUE); gl_->glStencilMask(0xff);
            gl_->glClearColor(0, 0, 0, 0); gl_->glClearDepth(1); gl_->glClearStencil(0);
            gl_->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            int rows = 0;
            {
                QPainter painter(device_.get());
                painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
                for (const auto& sprite : sprites_) {
                    if (sprite.page != index) continue;
                    QTransform base;
                    base.translate(sprite.slot.x(), sprite.slot.y());
                    base.scale(double(sprite.slot.width()) / sprite.bounds.width(), double(sprite.slot.height()) / sprite.bounds.height());
                    base.translate(-sprite.bounds.x(), -sprite.bounds.y());
                    sprite.drawing->replay(painter, base, sprite.slot);
                    rows = std::max(rows, sprite.slot.y() + sprite.slot.height());
                    pixels += qint64(sprite.slot.width()) * sprite.slot.height();
                }
            }
            page_->release();
            if (rows <= 0) continue;
            // The page is painted flipped, so its rows are already in top-down order.
            gl_->glBindFramebuffer(GL_READ_FRAMEBUFFER, page_->handle()); gl_->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, atlasFramebuffer_);
            gl_->glDisable(GL_SCISSOR_TEST);
            gl_->glBlitFramebuffer(0, 0, page.width(), rows, 0, index * page.height(), page.width(), index * page.height() + rows,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        pagesUsed_ = used; rasterPixels_ = pixels; rasterScale_ = scale / ratio; ++rasterFrames_;
        return true;
    }
    void render()
    {
        auto* context = QOpenGLContext::currentContext();
        if (!context || !cube_ || context != cube_->context() || owners_.empty()) return;
        auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context);
        if (!functions || !functions->initializeOpenGLFunctions()) return;
        GlState saved(functions);
        if (!initialize()) return;
        const QSize size = cube_->size();
        if (size.isEmpty()) return;
        const auto isolated = [this](const std::function<void()>& callback) {
            if (callback) { GlState state(gl_); callback(); }
        };
        QElapsedTimer phase; phase.start();
        const auto lap = [&phase] { const double ms = phase.nsecsElapsed() / 1000000.0; phase.restart(); return ms; };
        const auto average = [](double& value, double sample) { value = value * .9 + sample * .1; };
        // All providers participate in the same ordered passes, independently
        // of which retained module connected/was reloaded first.
        for (const auto& owner : owners_) if (owner.object) isolated(owner.geometry.prepare);
        // Actors are recorded and drawn for every rendered frame, so their motion
        // follows the cube's frame rate.
        if (forceRecord_ || size != size_ || recordClock_.nsecsElapsed() >= minimumRecordNs) {
            recordClock_.restart(); phase.restart();
            Frame frame(size);
            for (const auto& owner : owners_) if (owner.object && owner.paint) owner.paint(frame);
            std::stable_sort(frame.sprites.begin(), frame.sprites.end(), [](const Sprite& a, const Sprite& b) {
                return a.depth > b.depth;
            });
            sprites_ = std::move(frame.sprites); error_ = frame.error; size_ = size; forceRecord_ = false;
            average(recordMs_, lap());
            if (!sprites_.empty() && !rasterize(size)) sprites_.clear();
            average(rasterMs_, lap());
        }
        phase.restart();
        for (const auto& owner : owners_) if (owner.object) isolated(owner.geometry.background);
        gl_->glEnable(GL_DEPTH_TEST); gl_->glDepthFunc(GL_LEQUAL); gl_->glDepthMask(GL_TRUE);
        for (const auto& owner : owners_) if (owner.object) isolated(owner.geometry.opaque);

        GLint viewport[4]; GLdouble range[2];
        gl_->glGetIntegerv(GL_VIEWPORT, viewport); gl_->glGetDoublev(GL_DEPTH_RANGE, range);
        DepthSlice slice;
        slice.projection = cube_->m_projection;
        slice.viewport = QVector4D(viewport[0], viewport[1], viewport[2], viewport[3]);
        slice.depthRange = QVector2D(range[0], range[1]);
        const float cameraNear = cube_->m_projection(2, 3) / (-1.F + cube_->m_projection(2, 2));
        const float cameraFar = cube_->m_projection(2, 3) / (1.F + cube_->m_projection(2, 2));
        const double width = size.width(), height = size.height();
        const double atlasWidth = pageSize_.width(), atlasHeight = double(pageSize_.height()) * std::max(1, atlasPages_);
        const auto drawSprite = [&](std::size_t index, bool translucent, const DepthSlice* clip) {
            const auto& sprite = sprites_[index]; if (sprite.page < 0) return;
            // A ground sprite drawn for one depth interval only covers the rows
            // whose floor depth lies in that interval (plus a row of margin; the
            // shader decides exactly). Without this, a large floor would be
            // drawn in full once per interval.
            double first = sprite.bounds.top(), last = sprite.bounds.y() + sprite.bounds.height();
            const auto floorRow = [height](double depth) { return (.65 + (1.0 / depth - 1.0 / cubeViewDistance) / .40) * height; };
            if (sprite.ground && clip) {
                if (clip->farDepth < 18.F) first = std::max(first, floorRow(clip->farDepth) - 1);
                if (clip->nearDepth > 2.5F) last = std::min(last, floorRow(clip->nearDepth) + 1);
                if (first >= last) return;
            }
            // A legacy material may bind a different program/VAO or blend mode.
            // Establish actor state explicitly as well as isolating each call.
            gl_->glActiveTexture(GL_TEXTURE0); gl_->glBindSampler(0, 0);
            gl_->glEnable(GL_DEPTH_TEST); gl_->glDepthFunc(GL_LEQUAL); gl_->glDepthMask(translucent ? GL_FALSE : GL_TRUE);
            gl_->glDisable(GL_CULL_FACE); gl_->glDisable(GL_SCISSOR_TEST);
            gl_->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); gl_->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            gl_->glEnable(GL_BLEND); gl_->glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            gl_->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            program_->bind(); program_->setUniformValue("projection", cube_->m_projection); program_->setUniformValue("image", 0);
            program_->setUniformValue("translucent", translucent ? 1 : 0); program_->setUniformValue("groundSurface", sprite.ground ? 1 : 0);
            program_->setUniformValue("uvRect", QVector4D(sprite.slot.x() / atlasWidth,
                (double(sprite.page) * pageSize_.height() + sprite.slot.y()) / atlasHeight,
                sprite.slot.width() / atlasWidth, sprite.slot.height() / atlasHeight));
            if (clip) clip->apply(*program_); else DepthSlice::disable(*program_);
            gl_->glBindVertexArray(vao_); gl_->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            gl_->glBindTexture(GL_TEXTURE_2D, atlasTexture_);
            // Reciprocal depth is affine between the ground clamp breakpoints.
            // Subdivision keeps the rasterized plane equal to shader G(y).
            std::vector<double> ys{first, last};
            if (sprite.ground) for (double reciprocal : {1.0 / 18.0, 1.0 / 2.5}) {
                const double y = floorRow(1.0 / reciprocal);
                if (y > first && y < last) ys.push_back(y);
            }
            std::sort(ys.begin(), ys.end());
            float vertices[90]; int offset = 0;
            for (std::size_t band = 1; band < ys.size(); ++band) {
                const double top = ys[band - 1], bottom = ys[band];
                for (const auto& point : {QPointF(0, top), QPointF(0, bottom), QPointF(1, bottom),
                                          QPointF(0, top), QPointF(1, bottom), QPointF(1, top)}) {
                    const double x = sprite.bounds.x() + point.x() * sprite.bounds.width(), y = point.y();
                    const float depth = sprite.ground ? groundDepth(y / height) : sprite.depth;
                    vertices[offset++] = (2 * x / width - 1) * depth / cube_->m_projection(0, 0);
                    vertices[offset++] = (1 - 2 * y / height) * depth / cube_->m_projection(1, 1);
                    vertices[offset++] = -depth; vertices[offset++] = point.x();
                    vertices[offset++] = (y - sprite.bounds.y()) / sprite.bounds.height();
                }
            }
            gl_->glBufferSubData(GL_ARRAY_BUFFER, 0, offset * sizeof(float), vertices); gl_->glDrawArrays(GL_TRIANGLES, 0, offset / 5);
        };
        // Finish opaque depth first. Transparent padding never writes it.
        for (std::size_t index = 0; index < sprites_.size(); ++index) drawSprite(index, false, nullptr);
        std::vector<const GeometryPasses*> transparent;
        for (const auto& owner : owners_) if (owner.object && owner.geometry.translucent
            && (!owner.geometry.translucentActive || owner.geometry.translucentActive())) transparent.push_back(&owner.geometry);
        transparentDraws_ = 0;
        const auto legacy = [&](float far, float near, DepthSlice::Part part) {
            if (far <= near) return;
            slice.farDepth = far; slice.nearDepth = near; slice.part = part;
            for (const auto* passes : transparent) {
                GlState isolatedState(gl_);
                gl_->glEnable(GL_DEPTH_TEST); gl_->glDepthFunc(GL_LEQUAL); gl_->glDepthMask(GL_FALSE);
                passes->translucent(slice); ++transparentDraws_;
            }
        };
        const auto interval = [&](float far, float near) {
            if (far <= near) return;
            std::vector<std::size_t> grounds;
            for (std::size_t index = 0; index < sprites_.size(); ++index) if (sprites_[index].ground) {
                const auto& bounds = sprites_[index].bounds;
                if (groundDepth(bounds.top() / height) > near
                    && groundDepth((bounds.y() + bounds.height()) / height) <= far) grounds.push_back(index);
            }
            if (grounds.empty()) { legacy(far, near, DepthSlice::Whole); return; }
            legacy(far, near, DepthSlice::FarOfGround);
            slice.farDepth = far; slice.nearDepth = near; slice.part = DepthSlice::Whole;
            for (const auto index : grounds) drawSprite(index, true, &slice);
            legacy(far, near, DepthSlice::NearOfGround);
        };
        float far = cameraFar;
        for (std::size_t index = 0; index < sprites_.size();) {
            if (sprites_[index].ground) { ++index; continue; }
            const float depth = sprites_[index].depth;
            interval(far, depth);
            do {
                if (!sprites_[index].ground) drawSprite(index, true, nullptr);
                ++index;
            } while (index < sprites_.size() && sprites_[index].depth == depth);
            far = depth;
        }
        interval(far, cameraNear);
        average(compositeMs_, lap());
        ++frames_;
    }
    QPointer<CubeWidget> cube_;
    QPointer<QOpenGLContext> owner_;
    QOpenGLFunctions_3_3_Core* gl_ = nullptr;
    std::unique_ptr<QOpenGLShaderProgram> program_;
    std::unique_ptr<QOpenGLFramebufferObject> page_;
    std::unique_ptr<detail::PageDevice> device_;
    GLuint vao_ = 0, vbo_ = 0, atlasFramebuffer_ = 0, atlasTexture_ = 0;
    QSize pageSize_; int atlasPages_ = 0, pagesUsed_ = 0, samples_ = 4, maxTextureSize_ = 4096;
    std::vector<Owner> owners_;
    std::vector<Sprite> sprites_;
    QMetaObject::Connection drawConnection_, contextConnection_;
    QTimer timer_; QElapsedTimer recordClock_; QSize size_;
    quint64 frames_ = 0, rasterFrames_ = 0; qint64 rasterPixels_ = 0; double rasterScale_ = 1;
    double recordMs_ = 0, rasterMs_ = 0, compositeMs_ = 0;
    int transparentDraws_ = 0;
    QString error_; bool forceRecord_ = true;
};

} // namespace party_scene
