// Charts of the jack's head as a second-order system, drawn over the cube.
//
// Three panels: the head and its target as they move, the poles in the
// s-plane, and the response from rest on each pass of the open side, one
// curve per tuning, so tunings tried one after another end up side by side.
// The jack feeds this from its draw hook, once per frame.

#pragma once

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <vector>

namespace response_chart {

struct Tuning {
    double stiffness = 0.0;
    double damping = 0.0;
    double smoothingSeconds = 0.0;

    bool operator==(const Tuning&) const = default;

    [[nodiscard]] double naturalRate() const { return std::sqrt(stiffness); }
    [[nodiscard]] double zeta() const { return damping / (2.0 * naturalRate()); }

    // The oscillator's two poles, as (real, imaginary) with the imaginary
    // part of the first one not negative.
    [[nodiscard]] std::pair<QPointF, QPointF> poles() const
    {
        const double rate = naturalRate();
        const double z = zeta();
        if (z < 1.0) {
            const double damped = rate * std::sqrt(1.0 - z * z);
            return {QPointF(-z * rate, damped), QPointF(-z * rate, -damped)};
        }
        const double spread = rate * std::sqrt(z * z - 1.0);
        return {QPointF(-z * rate + spread, 0.0), QPointF(-z * rate - spread, 0.0)};
    }

    [[nodiscard]] QString label() const
    {
        QString text = QStringLiteral("ζ %1").arg(zeta(), 0, 'f', 2);
        if (std::abs(stiffness - 62.0) > 1e-3) {
            text += QStringLiteral(", k %1").arg(stiffness, 0, 'g', 4);
        }
        if (smoothingSeconds <= 0.0) {
            text += QStringLiteral(", no filter");
        } else if (std::abs(smoothingSeconds - 0.18) > 1e-3) {
            text += QStringLiteral(", τ %1 s").arg(smoothingSeconds, 0, 'g', 3);
        }
        return text;
    }
};

class Charts final : public QWidget {
public:
    explicit Charts(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("jackResponseCharts"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setGeometry(parent->rect());
        show();
        raise();
    }

    // One frame of the head: the target the open side sets, the target after
    // the input filter, and where the head is.
    void sample(const double time, const double target, const double filtered, const double extension,
                const Tuning& tuning)
    {
        if (parentWidget() != nullptr && geometry() != parentWidget()->rect()) {
            setGeometry(parentWidget()->rect());
        }
        const double step = m_lastTime >= 0.0 ? std::clamp(time - m_lastTime, 0.0, 0.1) : 0.0;
        m_lastTime = time;
        if (!(tuning == m_tuning)) {
            // A curve that started under the old tuning would mix the two.
            m_pending.reset();
            m_tuning = tuning;
        }
        // The poles glide to where the tuning puts them.
        const auto [upper, lower] = tuning.poles();
        const double follow = m_shownUpper ? 1.0 - std::exp(-step / 0.35) : 1.0;
        m_shownUpper = m_shownUpper ? *m_shownUpper + (upper - *m_shownUpper) * follow : upper;
        m_shownLower = m_shownLower ? *m_shownLower + (lower - *m_shownLower) * follow : lower;
        const double filterPole = tuning.smoothingSeconds > 0.0 ? -1.0 / tuning.smoothingSeconds : -100.0;
        m_shownFilter = m_shownFilter ? *m_shownFilter + (filterPole - *m_shownFilter) * follow : filterPole;

        m_recent.push_back({time, target, filtered, extension});
        while (!m_recent.empty() && m_recent.front().time < time - liveSeconds - 0.5) {
            m_recent.pop_front();
        }
        while (!m_peaks.empty() && m_peaks.front().time < time - liveSeconds) {
            m_peaks.erase(m_peaks.begin());
        }

        // A pass starts as the side comes into view and the target leaves 0.
        if (target > 1e-3 && m_lastTarget <= 1e-3) {
            m_pending = Curve{tuning, tuning.label(), {}, std::nullopt};
            m_pendingStart = time;
        }
        m_lastTarget = target;
        if (m_pending) {
            const double since = time - m_pendingStart;
            if (since <= passSeconds) {
                m_pending->points.emplace_back(since, extension);
            } else {
                keep(*m_pending);
                m_pending.reset();
            }
        }
        update();
    }

    // The first swing of the current pass reached `peak`, `overshoot` percent
    // past fully out.
    void peak(const double time, const double peak, const double overshoot)
    {
        m_peaks.push_back({time, peak, overshoot});
        if (m_pending) {
            m_pending->overshoot = overshoot;
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double w = width();
        const double h = height();
        const double panelWidth = w * 0.27;
        const double left = w * 0.02;
        const double right = w - w * 0.02 - panelWidth;
        m_unit = std::max(10.0, h / 56.0);
        paintLive(p, QRectF(left, h * 0.05, panelWidth, h * 0.37));
        paintPlane(p, QRectF(left, h * 0.45, panelWidth, h * 0.25));
        paintPasses(p, QRectF(right, h * 0.05, panelWidth, h * 0.65));
    }

private:
    static constexpr double liveSeconds = 6.0;
    static constexpr double passSeconds = 2.6;
    static constexpr double lowest = -0.1;
    static constexpr double highest = 1.85;
    static constexpr double endStop = 1.75;

    struct Sample {
        double time;
        double target;
        double filtered;
        double extension;
    };
    struct Peak {
        double time;
        double value;
        double overshoot;
    };
    struct Curve {
        Tuning tuning;
        QString label;
        std::vector<QPointF> points;
        std::optional<double> overshoot;
    };

    static QColor colorFor(const int index)
    {
        static const QColor palette[] = {QColor(242, 177, 52), QColor(60, 198, 184), QColor(224, 86, 155),
                                         QColor(155, 211, 90), QColor(95, 168, 245), QColor(167, 139, 250)};
        return palette[index % static_cast<int>(std::size(palette))];
    }

    // One curve per tuning: a later pass under the same tuning replaces it.
    void keep(const Curve& curve)
    {
        for (Curve& kept : m_curves) {
            if (kept.label == curve.label) {
                kept = curve;
                return;
            }
        }
        m_curves.push_back(curve);
    }

    [[nodiscard]] int colorIndexOf(const QString& label) const
    {
        for (std::size_t i = 0; i < m_curves.size(); ++i) {
            if (m_curves[i].label == label) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(m_curves.size());
    }

    QFont font(const double scale, const bool bold = false) const
    {
        QFont f(QStringLiteral("DejaVu Sans"));
        f.setPixelSize(static_cast<int>(std::round(m_unit * scale)));
        f.setBold(bold);
        return f;
    }

    void panel(QPainter& p, const QRectF& r, const QString& title, const QString& note) const
    {
        p.setPen(QPen(QColor(255, 255, 255, 45), 1.0));
        p.setBrush(QColor(10, 12, 18, 205));
        p.drawRoundedRect(r, m_unit * 0.9, m_unit * 0.9);
        p.setPen(QColor(243, 234, 216));
        p.setFont(font(1.1, true));
        const QRectF heading = r.adjusted(m_unit, m_unit * 0.55, -m_unit, 0);
        p.drawText(heading, Qt::AlignLeft | Qt::AlignTop, title);
        p.setPen(QColor(190, 196, 205));
        p.setFont(font(0.85));
        p.drawText(heading.adjusted(0, m_unit * 1.45, 0, 0), Qt::AlignLeft | Qt::AlignTop, note);
    }

    // The plot area of a panel, below its title and note.
    [[nodiscard]] QRectF plotArea(const QRectF& r, const double bottomExtra = 0.0) const
    {
        return r.adjusted(m_unit * 3.0, m_unit * 3.6, -m_unit * 0.9, -m_unit * (1.9 + bottomExtra));
    }

    void valueGrid(QPainter& p, const QRectF& plot, const auto& toY) const
    {
        p.setFont(font(0.8));
        for (const double v : {0.0, 0.5, 1.0, 1.5}) {
            const double y = toY(v);
            p.setPen(QPen(QColor(255, 255, 255, v == 1.0 ? 70 : 28), 1.0, v == 1.0 ? Qt::DashLine : Qt::SolidLine));
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(QColor(170, 176, 186));
            p.drawText(QRectF(plot.left() - m_unit * 2.8, y - m_unit, m_unit * 2.5, m_unit * 2),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'f', 1));
        }
        const double stop = toY(endStop);
        p.setPen(QPen(QColor(230, 80, 80, 160), 1.0, Qt::DashLine));
        p.drawLine(QPointF(plot.left(), stop), QPointF(plot.right(), stop));
        p.setPen(QColor(230, 110, 110));
        p.drawText(QRectF(plot.right() - m_unit * 6, stop - m_unit * 1.3, m_unit * 6, m_unit * 1.2),
                   Qt::AlignRight | Qt::AlignBottom, QStringLiteral("end stop"));
    }

    void paintLive(QPainter& p, const QRectF& r) const
    {
        const QString note = QStringLiteral("%1 · ωn %2 rad/s")
                                 .arg(m_tuning.label())
                                 .arg(m_tuning.naturalRate(), 0, 'f', 2);
        panel(p, r, QStringLiteral("The head, live"), note);
        const QRectF plot = plotArea(r, 0.9);
        if (m_recent.empty()) {
            return;
        }
        const double now = m_recent.back().time;
        const auto toX = [&](const double t) { return plot.right() - (now - t) / liveSeconds * plot.width(); };
        const auto toY = [&](const double v) {
            return plot.bottom() - (v - lowest) / (highest - lowest) * plot.height();
        };
        valueGrid(p, plot, toY);
        p.setFont(font(0.8));
        p.setPen(QColor(170, 176, 186));
        for (int s = 0; s <= 6; s += 2) {
            const double x = plot.right() - s / liveSeconds * plot.width();
            p.drawText(QRectF(x - m_unit * 2, plot.bottom() + m_unit * 0.2, m_unit * 4, m_unit * 1.2),
                       Qt::AlignHCenter | Qt::AlignTop, s == 0 ? QStringLiteral("now") : QStringLiteral("−%1 s").arg(s));
        }

        p.save();
        p.setClipRect(plot.adjusted(-1, -m_unit, 1, 1));
        const auto trace = [&](auto value, const QPen& pen) {
            QPainterPath path;
            bool first = true;
            for (const Sample& s : m_recent) {
                const QPointF point(toX(s.time), toY(value(s)));
                if (first) {
                    path.moveTo(point);
                    first = false;
                } else {
                    path.lineTo(point);
                }
            }
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        };
        trace([](const Sample& s) { return s.target; }, QPen(QColor(200, 200, 200, 150), 1.2, Qt::DashLine));
        trace([](const Sample& s) { return s.filtered; }, QPen(QColor(95, 168, 245), 1.4));
        trace([](const Sample& s) { return s.extension; }, QPen(QColor(242, 177, 52), 2.6));
        p.setFont(font(0.85, true));
        for (const Peak& peak : m_peaks) {
            const QPointF at(toX(peak.time), toY(peak.value));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 236, 180));
            p.drawEllipse(at, m_unit * 0.28, m_unit * 0.28);
            p.setPen(QColor(255, 236, 180));
            p.drawText(QRectF(at.x() - m_unit * 3, at.y() - m_unit * 1.6, m_unit * 6, m_unit * 1.3),
                       Qt::AlignHCenter | Qt::AlignBottom, QStringLiteral("%1%2%")
                           .arg(peak.overshoot >= 0 ? QStringLiteral("+") : QString())
                           .arg(peak.overshoot, 0, 'f', 1));
        }
        p.restore();

        // The legend, under the time axis.
        const double y = plot.bottom() + m_unit * 1.55;
        double x = plot.left();
        p.setFont(font(0.8));
        for (const auto& [label, pen] : {std::pair{QStringLiteral("r target"), QPen(QColor(200, 200, 200, 170), 1.2, Qt::DashLine)},
                                         std::pair{QStringLiteral("filtered"), QPen(QColor(95, 168, 245), 1.6)},
                                         std::pair{QStringLiteral("x head"), QPen(QColor(242, 177, 52), 2.6)}}) {
            p.setPen(pen);
            p.drawLine(QPointF(x, y + m_unit * 0.6), QPointF(x + m_unit * 1.6, y + m_unit * 0.6));
            p.setPen(QColor(200, 204, 212));
            p.drawText(QRectF(x + m_unit * 2.0, y, m_unit * 7, m_unit * 1.2), Qt::AlignLeft | Qt::AlignVCenter, label);
            x += m_unit * 7.2;
        }
    }

    void paintPlane(QPainter& p, const QRectF& r) const
    {
        panel(p, r, QStringLiteral("s-plane"), QStringLiteral("× oscillator poles   × input filter pole"));
        // Recolour the note's two crosses so they match the marks below.
        p.setFont(font(0.85));
        const QRectF noteLine = r.adjusted(m_unit, m_unit * 2.0, -m_unit, 0);
        p.setPen(QColor(242, 177, 52));
        p.drawText(noteLine, Qt::AlignLeft | Qt::AlignTop, QStringLiteral("×"));
        const QFontMetricsF metrics(font(0.85));
        p.setPen(QColor(95, 168, 245));
        p.drawText(noteLine.adjusted(metrics.horizontalAdvance(QStringLiteral("× oscillator poles   ")), 0, 0, 0),
                   Qt::AlignLeft | Qt::AlignTop, QStringLiteral("×"));
        if (!m_shownUpper) {
            return;
        }
        const QRectF plot = r.adjusted(m_unit * 1.2, m_unit * 3.6, -m_unit * 1.2, -m_unit * 1.0);
        const double reLow = std::min(-9.0, std::min(m_shownUpper->x(), m_shownLower->x()) * 1.15);
        const double reHigh = 1.5;
        const double imHigh = std::max(9.0, std::abs(m_shownUpper->y()) * 1.15);
        const auto toPoint = [&](const QPointF& s) {
            return QPointF(plot.left() + (s.x() - reLow) / (reHigh - reLow) * plot.width(),
                           plot.center().y() - s.y() / imHigh * plot.height() / 2.0);
        };
        const QPointF origin = toPoint(QPointF(0, 0));
        // The left half-plane, where the poles of a stable system sit.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(90, 200, 120, 22));
        p.drawRect(QRectF(plot.left(), plot.top(), origin.x() - plot.left(), plot.height()));
        p.setFont(font(0.75));
        p.setPen(QColor(120, 210, 150, 170));
        p.drawText(QRectF(plot.left() + m_unit * 0.3, plot.top(), m_unit * 6, m_unit * 1.2), Qt::AlignLeft | Qt::AlignTop,
                   QStringLiteral("stable"));
        p.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
        p.drawLine(QPointF(plot.left(), origin.y()), QPointF(plot.right(), origin.y()));
        p.drawLine(QPointF(origin.x(), plot.top()), QPointF(origin.x(), plot.bottom()));
        p.setPen(QColor(150, 156, 166));
        for (const double re : {-8.0, -4.0}) {
            const QPointF at = toPoint(QPointF(re, 0));
            p.drawText(QRectF(at.x() - m_unit * 1.5, origin.y() + m_unit * 0.15, m_unit * 3, m_unit * 1.1),
                       Qt::AlignHCenter | Qt::AlignTop, QString::number(re, 'f', 0).replace(QLatin1Char('-'), QStringLiteral("−")));
        }
        p.drawText(QRectF(origin.x() + m_unit * 0.3, plot.top(), m_unit * 3, m_unit * 1.1), Qt::AlignLeft | Qt::AlignTop,
                   QStringLiteral("Im"));
        p.drawText(QRectF(plot.right() - m_unit * 2.2, origin.y() - m_unit * 1.2, m_unit * 2, m_unit * 1.1),
                   Qt::AlignRight | Qt::AlignBottom, QStringLiteral("Re"));

        // Lines of constant damping ratio through the poles, kept inside the
        // plot, which they would otherwise cross the whole window to leave.
        p.save();
        p.setClipRect(plot);
        const auto cross = [&](const QPointF& at, const QColor& color) {
            const double a = m_unit * 0.45;
            p.setPen(QPen(color, 2.2));
            p.drawLine(at + QPointF(-a, -a), at + QPointF(a, a));
            p.drawLine(at + QPointF(-a, a), at + QPointF(a, -a));
        };
        p.setPen(QPen(QColor(242, 177, 52, 90), 1.0, Qt::DashLine));
        for (const QPointF& pole : {*m_shownUpper, *m_shownLower}) {
            const QPointF at = toPoint(pole);
            const QPointF direction = at - origin;
            const double length = std::hypot(direction.x(), direction.y());
            if (length > 1.0) {
                p.drawLine(origin, origin + direction * (plot.width() * 1.5 / length));
            }
        }
        if (*m_shownFilter > reLow) {
            cross(toPoint(QPointF(*m_shownFilter, 0)), QColor(95, 168, 245));
        } else {
            p.setFont(font(0.75));
            p.setPen(QColor(95, 168, 245));
            p.drawText(QRectF(plot.left() + m_unit * 0.3, origin.y() + m_unit * 0.2, m_unit * 9, m_unit * 1.1),
                       Qt::AlignLeft | Qt::AlignTop, QStringLiteral("← filter off: −∞"));
        }
        cross(toPoint(*m_shownUpper), QColor(242, 177, 52));
        cross(toPoint(*m_shownLower), QColor(242, 177, 52));
        p.restore();
        p.setFont(font(0.85, true));
        p.setPen(QColor(255, 220, 150));
        const QPointF label = toPoint(*m_shownUpper);
        const double zeta = m_tuning.zeta();
        const QPointF poleValue = m_tuning.poles().first;
        QString text = QStringLiteral("ζ %1").arg(zeta, 0, 'f', 2);
        text += poleValue.y() > 0.0
            ? QStringLiteral("   %1 ± %2j").arg(poleValue.x(), 0, 'f', 2).arg(poleValue.y(), 0, 'f', 2)
            : QStringLiteral("   %1").arg(poleValue.x(), 0, 'f', 2);
        text.replace(QLatin1Char('-'), QStringLiteral("−"));
        const bool roomOnRight = label.x() < plot.center().x();
        p.drawText(QRectF(roomOnRight ? label.x() + m_unit * 0.8 : label.x() - m_unit * 16.8,
                          std::max(plot.top(), label.y() - m_unit * 0.6), m_unit * 16, m_unit * 1.3),
                   (roomOnRight ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter, text);
    }

    void paintPasses(QPainter& p, const QRectF& r) const
    {
        panel(p, r, QStringLiteral("Each pass, from rest"),
              QStringLiteral("x(t) from the moment the side comes into view"));
        const int legendRows = static_cast<int>(m_curves.size()) + (m_pending ? 1 : 0);
        const double legendHeight = std::max(1, legendRows) * m_unit * 1.45;
        QRectF plot = plotArea(r);
        plot.setBottom(plot.bottom() - legendHeight - m_unit * 0.4);
        const auto toX = [&](const double t) { return plot.left() + t / passSeconds * plot.width(); };
        const auto toY = [&](const double v) {
            return plot.bottom() - (v - lowest) / (highest - lowest) * plot.height();
        };
        valueGrid(p, plot, toY);
        p.setFont(font(0.8));
        p.setPen(QColor(170, 176, 186));
        for (const double s : {0.0, 1.0, 2.0}) {
            p.drawText(QRectF(toX(s) - m_unit * 2, plot.bottom() + m_unit * 0.2, m_unit * 4, m_unit * 1.2),
                       Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("%1 s").arg(s, 0, 'f', 0));
        }

        const auto draw = [&](const Curve& curve, const QColor& color, const double thickness) {
            QPainterPath path;
            for (std::size_t i = 0; i < curve.points.size(); ++i) {
                const QPointF point(toX(curve.points[i].x()), toY(curve.points[i].y()));
                if (i == 0) {
                    path.moveTo(point);
                } else {
                    path.lineTo(point);
                }
            }
            p.setPen(QPen(color, thickness, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        };
        p.save();
        p.setClipRect(plot.adjusted(-1, -m_unit, 1, 1));
        for (std::size_t i = 0; i < m_curves.size(); ++i) {
            const bool superseded = m_pending && m_pending->label == m_curves[i].label;
            QColor color = colorFor(static_cast<int>(i));
            color.setAlpha(superseded ? 70 : 230);
            draw(m_curves[i], color, 1.9);
        }
        if (m_pending) {
            draw(*m_pending, colorFor(colorIndexOf(m_pending->label)).lighter(125), 3.0);
        }
        p.restore();

        // The legend: one row per tuning, with what its first swing measured.
        double y = plot.bottom() + m_unit * 1.7;
        p.setFont(font(0.88));
        const auto row = [&](const Curve& curve, const QColor& color, const bool live) {
            p.setPen(QPen(color, live ? 3.0 : 2.2));
            p.drawLine(QPointF(plot.left(), y + m_unit * 0.6), QPointF(plot.left() + m_unit * 1.6, y + m_unit * 0.6));
            p.setPen(QColor(220, 224, 232));
            const QString swing = curve.overshoot
                ? QStringLiteral("%1%2%").arg(*curve.overshoot >= 0 ? QStringLiteral("+") : QString())
                      .arg(*curve.overshoot, 0, 'f', 1)
                : QStringLiteral("…");
            p.drawText(QRectF(plot.left() + m_unit * 2.1, y, plot.width() - m_unit * 2.1, m_unit * 1.25),
                       Qt::AlignLeft | Qt::AlignVCenter, curve.label + (live ? QStringLiteral("  (now)") : QString()));
            p.drawText(QRectF(plot.left() + m_unit * 2.1, y, plot.width() - m_unit * 2.1, m_unit * 1.25),
                       Qt::AlignRight | Qt::AlignVCenter, swing);
            y += m_unit * 1.45;
        };
        for (std::size_t i = 0; i < m_curves.size(); ++i) {
            row(m_curves[i], colorFor(static_cast<int>(i)), false);
        }
        if (m_pending) {
            row(*m_pending, colorFor(colorIndexOf(m_pending->label)).lighter(125), true);
        }
    }

    Tuning m_tuning;
    std::deque<Sample> m_recent;
    std::vector<Peak> m_peaks;
    std::vector<Curve> m_curves;
    std::optional<Curve> m_pending;
    double m_pendingStart = 0.0;
    double m_lastTarget = 0.0;
    double m_lastTime = -1.0;
    std::optional<QPointF> m_shownUpper;
    std::optional<QPointF> m_shownLower;
    std::optional<double> m_shownFilter;
    double m_unit = 11.0;
};

} // namespace response_chart
