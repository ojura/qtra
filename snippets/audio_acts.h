#pragma once

#include "audio_analysis.h"
#include <QVector2D>
#include <QVector3D>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace audio_acts {

struct Vertex {
    float x, y, z;
    float r, g, b, a;
};

struct History {
    static constexpr int capacity = 96;
    static constexpr double interval = .06;
    struct Row {
        std::array<float, audio_analysis::bandCount> bands{};
        float onset = 0;
    };
    std::array<Row, capacity> rows{};
    int head = -1;
    int count = 0;
    double sampledAt = -1;
    std::uint64_t seenBeats = 0;

    void sample(const audio_analysis::Frame& frame)
    {
        if (sampledAt >= 0 && frame.captureSeconds - sampledAt < interval) return;
        head = (head + 1) % capacity;
        rows[head] = Row{frame.bands, frame.beatCount != seenBeats
            ? std::clamp(frame.beatStrength, 0.0F, 1.0F) : 0.0F};
        count = std::min(count + 1, capacity);
        sampledAt = frame.captureSeconds;
        seenBeats = frame.beatCount;
    }

    const Row& ago(const int age) const
    {
        return rows[(head - age + capacity) % capacity];
    }
};

// The acts use normalized screen coordinates, then emit the same view-space
// vertex layout as the existing OpenGL renderer.
class Canvas {
public:
    Canvas(std::vector<Vertex>& vertices, const float aspect, const float height)
        : out(vertices), aspect(aspect), pixel(2.0F / std::max(height, 1.0F)) {}

    void quad(const QVector2D& a, const QVector2D& b, const QVector2D& c,
              const QVector2D& d, const QVector3D& colour, const float alpha)
    {
        vertex(a, colour, alpha); vertex(b, colour, alpha); vertex(c, colour, alpha);
        vertex(a, colour, alpha); vertex(c, colour, alpha); vertex(d, colour, alpha);
    }

    void rect(const float x0, const float y0, const float x1, const float y1,
              const QVector3D& colour, const float alpha)
    {
        quad({x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, colour, alpha);
    }

    void fadeRect(const float x0, const float y0, const float x1, const float y1,
                  const QVector3D& colour, const float alpha0, const float alpha1)
    {
        vertex({x0,y0},colour,alpha0); vertex({x1,y0},colour,alpha0); vertex({x1,y1},colour,alpha1);
        vertex({x0,y0},colour,alpha0); vertex({x1,y1},colour,alpha1); vertex({x0,y1},colour,alpha1);
    }

    void line(const QVector2D& a, const QVector2D& b, const QVector3D& colour,
              const float alpha, const float width = 1.5F)
    {
        const QVector2D along((b.x()-a.x())*aspect, b.y()-a.y());
        if (along.lengthSquared() < 1.0e-10F) return;
        const QVector2D unit = along.normalized();
        const QVector2D normal(-unit.y()/aspect, unit.x());
        for (int glow = 1; glow >= 0; --glow) {
            const QVector2D offset = normal * pixel * width * (glow ? 2.1F : .5F);
            quad(a-offset,b-offset,b+offset,a+offset,colour,alpha*(glow ? .15F : 1.0F));
        }
    }

private:
    void vertex(const QVector2D& p, const QVector3D& c, const float alpha)
    {
        constexpr float depth = 5.4F;
        constexpr float halfHeight = depth * .41421356237F;
        out.push_back({p.x()*aspect*halfHeight,p.y()*halfHeight,-depth,
                       c.x(),c.y(),c.z(),alpha});
    }
    std::vector<Vertex>& out;
    float aspect, pixel;
};

inline float band(const std::array<float,audio_analysis::bandCount>& bands, const float position)
{
    const float at = std::clamp(position,0.0F,1.0F)*(audio_analysis::bandCount-1);
    const int lo = static_cast<int>(at), hi = std::min(lo+1,audio_analysis::bandCount-1);
    return std::clamp(bands[lo]+(bands[hi]-bands[lo])*(at-lo),0.0F,1.0F);
}

inline QVector3D cityColour(const float position)
{
    const QVector3D orange(1.0F,.29F,.055F), violet(.50F,.22F,1.0F);
    const float t = std::clamp((position-.35F)/.65F,0.0F,1.0F);
    return orange*(1-t)+violet*t;
}

inline void city(std::vector<Vertex>& out, const audio_analysis::Frame& frame,
                 const float clock, const float flash, const float aspect, const float height)
{
    Canvas c(out,aspect,height);
    constexpr float horizon = -.30F;
    const QVector3D orange(1.0F,.38F,.09F), violet(.45F,.20F,1.0F);
    c.fadeRect(-1,-1,1,.72F,QVector3D(.12F,.055F,.25F),.18F,.015F);
    c.fadeRect(-1,horizon-.14F,1,horizon+.65F,orange,.11F,0);

    // A distant, fine-grained skyline makes the larger foreground towers read
    // as a place with depth. Every height still comes from a measured band.
    for (int i=0;i<64;++i) {
        const float p = i/63.0F;
        const float x = -.98F+1.96F*p;
        const float h = .06F+.49F*std::pow(frame.bands[i],1.8F);
        const QVector3D colour = cityColour(p);
        c.rect(x-.011F,horizon+.08F,x+.011F,horizon+.08F+h,colour,.10F);
        c.line({x-.011F,horizon+.08F+h},{x+.011F,horizon+.08F+h},colour,.22F,1.0F);
    }

    // The street recedes toward the skyline. Its moving transverse lines give
    // the scene direction; bass and individual beats brighten the pavement.
    for (int i=-10;i<=10;++i) {
        const float nearX = i*.15F;
        c.line({nearX*.035F,horizon},{nearX,-1.05F},violet,.11F+.10F*frame.bass,1.1F);
    }
    for (int i=0;i<14;++i) {
        const float t = std::fmod(i/14.0F+clock*.075F,1.0F);
        const float y = horizon-.77F*t*t;
        c.line({-1.2F,y},{1.2F,y},i%3==0 ? orange : violet,
               (.035F+.16F*t)*(.8F+.2F*frame.bass),1.0F+1.4F*t);
    }
    c.line({-.98F,horizon},{.98F,horizon},orange,.55F+.20F*std::min(flash,1.0F),2.6F);

    constexpr int towers = 32;
    for (int i=0;i<towers;++i) {
        const float p = i/static_cast<float>(towers-1);
        const float level = std::pow(band(frame.bands,p),1.65F);
        const float x = -.94F+1.88F*p;
        const float w = .044F;
        const float h = .045F+.93F*level;
        const float roof = horizon+h;
        const QVector3D colour = cityColour(p);
        const float strength = .18F+.43F*level;
        c.fadeRect(x-w*.5F,horizon,x+w*.5F,roof,colour,.06F,strength*.42F);
        c.quad({x+w*.5F,horizon},{x+w*.5F+.011F,horizon+.014F},
               {x+w*.5F+.011F,roof+.014F},{x+w*.5F,roof},colour,strength*.18F);
        c.quad({x-w*.5F,roof},{x+w*.5F,roof},{x+w*.5F+.011F,roof+.014F},
               {x-w*.5F+.011F,roof+.014F},colour,strength*.55F);
        c.line({x-w*.5F,horizon},{x-w*.5F,roof},colour,strength,1.2F);
        c.line({x+w*.5F,horizon},{x+w*.5F,roof},colour,strength*.6F,1.0F);
        c.line({x-w*.5F,roof},{x+w*.5F+.011F,roof+.014F},colour,.62F+.25F*level,2.4F);
        for (int row=0;row<13;++row) {
            const float y = horizon+.026F+row*.057F;
            if (y+.016F >= roof) break;
            for (int col=0;col<2;++col) {
                const float wx = x-w*.30F+col*w*.34F;
                const float lit = ((i*7+row*3+col)%5==0) ? .12F : .56F;
                c.rect(wx,y,wx+w*.14F,y+.010F,colour,lit*(.35F+.65F*level));
            }
        }
        c.fadeRect(x-w*.5F,horizon-.018F,x+w*.5F,horizon-.04F-h*.45F,
                   colour,.20F*level,0);
    }

    // A beat sends a horizontal sweep down the street, not another circle.
    const float sweep = std::fmod(clock*.55F,1.0F);
    const float lineY = horizon-.65F*sweep*sweep;
    c.line({-1,lineY},{1,lineY},orange,.16F*std::min(flash,1.0F),3.0F);
    for (int i=0;i<42;++i) {
        const float x = std::fmod(i*.6180339F,1.0F)*1.90F-.95F;
        const float y = .08F+std::fmod(i*.381966F,1.0F)*.66F;
        const float glow = .08F+.16F*frame.treble*(.5F+.5F*std::sin(clock*.6F+i));
        c.rect(x,y,x+.0015F,y+.002F,QVector3D(.7F,.55F,1),glow);
    }
}

inline QVector3D waterColour(const float level, const float frequency)
{
    const QVector3D violet(.30F,.10F,.72F), rose(1.0F,.18F,.43F), gold(1.0F,.68F,.20F);
    const float warm = std::clamp((level-.38F)*2.4F,0.0F,1.0F);
    const QVector3D peak = rose*(.35F+.65F*frequency)+gold*(.65F-.65F*frequency);
    return violet*(1-warm)+peak*warm;
}

inline void waterfall(std::vector<Vertex>& out, const audio_analysis::Frame& frame,
                      const History& history, const float aspect, const float height)
{
    Canvas c(out,aspect,height);
    constexpr float top=.37F, bottom=-.88F;
    constexpr int columns=48;
    const float rowHeight=(top-bottom)/History::capacity;
    const float drift=history.sampledAt>=0
        ? std::clamp(static_cast<float>((frame.captureSeconds-history.sampledAt)/History::interval),0.0F,1.0F) : 0;
    c.fadeRect(-.97F,bottom,.97F,top,QVector3D(.12F,.055F,.28F),.10F,.20F);
    for (int age=history.count-1;age>=0;--age) {
        const History::Row& row=history.ago(age);
        const float y=top-(age+drift)*rowHeight;
        const float fade=std::pow(1.0F-age/static_cast<float>(History::capacity),.65F);
        for (int col=0;col<columns;++col) {
            const float p=col/static_cast<float>(columns-1);
            const float level=std::pow(band(row.bands,p),1.65F);
            const float x=-.94F+col*(1.88F/columns);
            c.rect(x,y-rowHeight*.92F,x+(1.88F/columns)*.92F,y,
                   waterColour(level,p),(.025F+.75F*level)*fade);
        }
        if (row.onset>.05F)
            c.line({-.95F,y},{.95F,y},QVector3D(1,.49F,.18F),.20F*row.onset*fade,1.3F);
    }
    for (int i=0;i<9;++i) {
        const float x=-.94F+i*(1.88F/8);
        c.line({x,bottom},{x,top},QVector3D(.55F,.3F,.95F),.10F,.8F);
    }
    c.line({-.95F,top},{.95F,top},QVector3D(1,.36F,.08F),.58F,1.8F);
    for (int bandIndex=0;bandIndex<audio_analysis::bandCount-1;++bandIndex) {
        const float p=bandIndex/static_cast<float>(audio_analysis::bandCount-1);
        const float next=(bandIndex+1)/static_cast<float>(audio_analysis::bandCount-1);
        const float a=std::pow(frame.bands[bandIndex],1.65F);
        const float b=std::pow(frame.bands[bandIndex+1],1.65F);
        const float x=-.94F+1.88F*p, nx=-.94F+1.88F*next;
        c.line({x,top+.04F+a*.28F},{nx,top+.04F+b*.28F},waterColour(.9F,p),.82F,2.6F);
        c.fadeRect(x,top+.012F,nx,top+.04F+a*.28F,waterColour(a,p),.02F,.14F*a);
    }
    c.line({-.95F,bottom},{.95F,bottom},QVector3D(.6F,.25F,1),.18F,1.2F);
}

} // namespace audio_acts
