#pragma once

// Shared by the separately loaded party modules. The registry lives once in the
// cube's QObject tree, not in a DSO-local static. The host retains loaded DSOs,
// so the first owner's inline vtable remains valid across independent reloads.
#include "cube_widget.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QPainter>
#include <QPicture>
#include <QPointer>
#include <QTimer>
#include <QVector2D>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
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

struct Sprite {
    QString name;
    QRect bounds;
    QImage image;
    QPicture picture;
    float depth = cubeViewDistance;
    bool ground = false;
};

class Frame {
public:
    explicit Frame(QSize size) : size_(size) {}
    void add(const QString& name, float depth, const std::function<void(QPainter&)>& draw, bool ground = false)
    {
        if (sprites.size() >= 96 || !std::isfinite(depth) || depth <= .1F || depth >= 100.F) {
            error = QStringLiteral("Invalid or excessive scene submission"); return;
        }
        QPicture picture;
        { QPainter painter(&picture); painter.setRenderHint(QPainter::Antialiasing);
          painter.setRenderHint(QPainter::TextAntialiasing); draw(painter); }
        const QRect bounds = picture.boundingRect().adjusted(-3, -3, 3, 3).intersected(QRect(QPoint(), size_));
        if (bounds.isEmpty()) return;
        requestedPixels_ += qint64(bounds.width()) * bounds.height();
        largestSide_ = std::max({largestSide_, bounds.width(), bounds.height()});
        sprites.push_back({name, bounds, {}, std::move(picture), depth, ground});
    }
    void ground(const QString& name, const std::function<void(QPainter&)>& draw)
    { add(name, cubeViewDistance, draw, true); }
    void rasterize()
    {
        // Measure the whole scene before allocating: a large window or a wide
        // streamer reduces sampling uniformly rather than dropping later actors.
        // The margin covers integer image-size rounding for all 96 submissions.
        rasterScale = std::min({1.0, requestedPixels_ > 0 ? std::sqrt(11500000.0 / requestedPixels_) : 1.0,
                                largestSide_ > 0 ? 4096.0 / largestSide_ : 1.0});
        for (auto& sprite : sprites) {
            const QSize pixels(std::max(1, int(std::ceil(sprite.bounds.width() * rasterScale))),
                               std::max(1, int(std::ceil(sprite.bounds.height() * rasterScale))));
            QImage image(pixels, QImage::Format_RGBA8888_Premultiplied);
            if (image.isNull()) { error = QStringLiteral("Scene image allocation failed"); continue; }
            image.fill(Qt::transparent);
            { QPainter painter(&image);
              painter.scale(double(pixels.width()) / sprite.bounds.width(), double(pixels.height()) / sprite.bounds.height());
              painter.translate(-sprite.bounds.topLeft()); painter.drawPicture(QPoint(), sprite.picture); }
            rasterPixels += qint64(pixels.width()) * pixels.height();
            sprite.image = std::move(image); sprite.picture = QPicture();
        }
    }
    std::vector<Sprite> sprites;
    qint64 rasterPixels = 0;
    double rasterScale = 1;
    QString error;
private:
    QSize size_;
    qint64 requestedPixels_ = 0;
    int largestSide_ = 0;
};

class Scene final : public QObject {
public:
    using Provider = std::function<void(Frame&)>;
    static Scene* acquire(CubeWidget* cube)
    {
        if (auto* existing = cube->findChild<QObject*>(QStringLiteral("partySceneDepth"), Qt::FindDirectChildrenOnly))
            return existing->property("partySceneAbi").toInt() == 2 ? static_cast<Scene*>(existing) : nullptr;
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
        dirty_ = forceRaster_ = true;
        if (destroyEmpty && owners_.empty()) { delete this; return; }
        if (cube_) cube_->update();
    }
    void invalidate() { dirty_ = true; if (cube_) cube_->update(); }
    QJsonObject status() const
    {
        QJsonArray actors;
        for (const auto& sprite : sprites_) actors.append(QJsonObject{
            {QStringLiteral("name"), sprite.name}, {QStringLiteral("depth"), sprite.depth},
            {QStringLiteral("ground"), sprite.ground}, {QStringLiteral("bounds"), QJsonArray{
                sprite.bounds.x(), sprite.bounds.y(), sprite.bounds.width(), sprite.bounds.height()}}});
        return {{QStringLiteral("owners"), int(owners_.size())}, {QStringLiteral("frames"), qint64(frames_)},
                {QStringLiteral("rasterFrames"), qint64(rasterFrames_)}, {QStringLiteral("rasterPixels"), rasterPixels_},
                {QStringLiteral("rasterScale"), rasterScale_},
                {QStringLiteral("transparentDraws"), transparentDraws_},
                {QStringLiteral("rasterMs"), rasterMs_}, {QStringLiteral("error"), error_}, {QStringLiteral("actors"), actors}};
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
    void add(QObject* owner, Provider provider, GeometryPasses geometry)
    {
        removeOwner(owner, false);
        Owner entry; entry.object = owner; entry.paint = std::move(provider); entry.geometry = std::move(geometry);
        entry.destroyed = connect(owner, &QObject::destroyed, this, [this, owner] { removeOwner(owner); });
        owners_.push_back(std::move(entry)); dirty_ = forceRaster_ = true; cube_->update();
    }
    struct Texture { GLuint id = 0; };
    explicit Scene(CubeWidget* cube) : QObject(cube), cube_(cube)
    {
        setObjectName(QStringLiteral("partySceneDepth")); setProperty("partySceneAbi", 2);
        rasterClock_.start();
        timer_.setInterval(34); timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, [this] { invalidate(); });
        timer_.start();
        drawConnection_ = connect(cube, &CubeWidget::frameRendered, this, [this](qulonglong, float) { render(); }, Qt::DirectConnection);
    }
    void cleanup()
    {
        if (gl_ && owner_ == QOpenGLContext::currentContext()) {
            for (const auto& texture : textures_) if (texture.id) gl_->glDeleteTextures(1, &texture.id);
            if (vbo_) gl_->glDeleteBuffers(1, &vbo_);
            if (vao_) gl_->glDeleteVertexArrays(1, &vao_);
        }
        textures_.clear(); vbo_ = vao_ = 0; program_.reset(); gl_ = nullptr; owner_.clear();
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
noperspective out vec2 texCoord;
void main() { gl_Position = projection * vec4(position, 1.0); texCoord = uv; }
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
        disconnect(contextConnection_);
        contextConnection_ = connect(context, &QOpenGLContext::aboutToBeDestroyed, this, [this] { cleanupWithContext(); }, Qt::DirectConnection);
        dirty_ = forceRaster_ = true;
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
        // All providers participate in the same ordered passes, independently
        // of which retained module connected/was reloaded first.
        for (const auto& owner : owners_) if (owner.object) isolated(owner.geometry.prepare);
        const bool rebuild = forceRaster_ || size != size_ || (dirty_ && rasterClock_.elapsed() >= 33);
        if (rebuild) {
            QElapsedTimer work; work.start();
            Frame frame(size);
            for (const auto& owner : owners_) if (owner.object && owner.paint) owner.paint(frame);
            frame.rasterize();
            std::stable_sort(frame.sprites.begin(), frame.sprites.end(), [](const Sprite& a, const Sprite& b) {
                return a.depth > b.depth;
            });
            sprites_ = std::move(frame.sprites); rasterPixels_ = frame.rasterPixels; rasterScale_ = frame.rasterScale; error_ = frame.error;
            size_ = size; dirty_ = forceRaster_ = false; rasterClock_.restart(); ++rasterFrames_;
            rasterMs_ = work.nsecsElapsed() / 1000000.0;
        }
        while (textures_.size() > sprites_.size()) { gl_->glDeleteTextures(1, &textures_.back().id); textures_.pop_back(); }
        while (textures_.size() < sprites_.size()) { GLuint id = 0; gl_->glGenTextures(1, &id); textures_.push_back({id}); }
        gl_->glActiveTexture(GL_TEXTURE0); gl_->glBindSampler(0, 0);
        gl_->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); gl_->glPixelStorei(GL_UNPACK_ALIGNMENT, 4); gl_->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl_->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0); gl_->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        if (rebuild) for (std::size_t index = 0; index < sprites_.size(); ++index) {
            const auto& image = sprites_[index].image; if (image.isNull()) continue;
            gl_->glBindTexture(GL_TEXTURE_2D, textures_[index].id);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl_->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
        }
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
        const auto drawSprite = [&](std::size_t index, bool translucent, const DepthSlice* clip) {
            const auto& sprite = sprites_[index]; if (sprite.image.isNull()) return;
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
            if (clip) clip->apply(*program_); else DepthSlice::disable(*program_);
            gl_->glBindVertexArray(vao_); gl_->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            gl_->glBindTexture(GL_TEXTURE_2D, textures_[index].id);
            // Reciprocal depth is affine between the ground clamp breakpoints.
            // Subdivision keeps the rasterized plane equal to shader G(y).
            std::vector<double> ys{double(sprite.bounds.top()), double(sprite.bounds.y() + sprite.bounds.height())};
            if (sprite.ground) for (double reciprocal : {1.0 / 18.0, 1.0 / 2.5}) {
                const double y = (.65 + (reciprocal - 1.0 / cubeViewDistance) / .40) * height;
                if (y > sprite.bounds.y() && y < sprite.bounds.y() + sprite.bounds.height()) ys.push_back(y);
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
        ++frames_;
    }
    QPointer<CubeWidget> cube_;
    QPointer<QOpenGLContext> owner_;
    QOpenGLFunctions_3_3_Core* gl_ = nullptr;
    std::unique_ptr<QOpenGLShaderProgram> program_;
    GLuint vao_ = 0, vbo_ = 0;
    std::vector<Texture> textures_;
    std::vector<Owner> owners_;
    std::vector<Sprite> sprites_;
    QMetaObject::Connection drawConnection_, contextConnection_;
    QTimer timer_; QElapsedTimer rasterClock_; QSize size_;
    quint64 frames_ = 0, rasterFrames_ = 0; qint64 rasterPixels_ = 0; double rasterMs_ = 0, rasterScale_ = 1;
    int transparentDraws_ = 0;
    QString error_; bool dirty_ = true, forceRaster_ = true;
};

} // namespace party_scene
