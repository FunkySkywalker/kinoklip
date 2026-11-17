// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/WelcomeAnimations.h"

#include "ui/common/UiAnimation.h"

#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStackedWidget>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace kk::welcomeanim {
namespace {

// Calm and a little slow, like KDE's Welcome Center: small moves, long soft fades, no bounce.
constexpr int kDot = 6; // dot diameter
constexpr int kPitch = 14; // distance between dot centres
constexpr int kLeaveDistance = 12; // how far the old page drifts while it fades out
constexpr int kLeaveMs = 220;
constexpr int kStepMs = 480;
constexpr int kRise = 10;
constexpr int kEnterMs = 520;
constexpr int kStaggerMs = 70;

/// @p ms at the user's animation speed.
int scaled(int ms)
{
    return std::max(1, qRound(ms / uianim::speed()));
}

/// The old page's picture, fading out over the new page (which is still invisible).
class LeaveOverlay : public QWidget
{
public:
    LeaveOverlay(QWidget* parent, QPixmap from, bool forward)
        : QWidget(parent)
        , m_from(std::move(from))
        , m_dir(forward ? 1 : -1)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setGeometry(parent->rect());
    }
    void setProgress(double t)
    {
        m_t = t;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const double e = QEasingCurve(QEasingCurve::InOutSine).valueForProgress(m_t);
        p.setOpacity(1.0 - e);
        p.drawPixmap(QPointF(-m_dir * kLeaveDistance * e, 0), m_from);
    }

private:
    QPixmap m_from;
    int m_dir;
    double m_t = 0;
};

/// A running cascade. Lives under the stacked widget; deleting it puts every element back.
class Entrance : public QObject
{
public:
    Entrance(QObject* owner, const QList<Group>& groups, int leadMs)
        : QObject(owner)
        , m_lead(leadMs)
    {
        for (const Group& group : groups) {
            Item item;
            for (QWidget* w : group) {
                if (auto* icon = dynamic_cast<IconBadge*>(w)) {
                    icon->setProgress(0);
                    item.icons << icon;
                } else {
                    auto* effect = new FadeRise(kRise, w);
                    effect->setProgress(0);
                    w->setGraphicsEffect(effect);
                    item.widgets << QPointer<QWidget>(w);
                }
            }
            m_items << item;
        }
        const int total = m_lead + int(m_items.size() - 1) * kStaggerMs + kEnterMs;
        auto* anim = new QVariantAnimation(this);
        anim->setDuration(scaled(total));
        anim->setStartValue(0.0);
        anim->setEndValue(double(total));
        anim->setEasingCurve(QEasingCurve::Linear);
        connect(anim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { step(v.toDouble()); });
        connect(anim, &QVariantAnimation::finished, this, [this] {
            step(1e9);
            deleteLater();
        });
        anim->start();
    }
    ~Entrance() override
    {
        for (Item& item : m_items)
            finishItem(item);
    }

private:
    struct Item
    {
        QList<QPointer<QWidget>> widgets;
        QList<IconBadge*> icons;
        bool done = false;
    };
    void finishItem(Item& item)
    {
        if (item.done)
            return;
        item.done = true;
        for (const QPointer<QWidget>& w : item.widgets)
            if (w)
                w->setGraphicsEffect(nullptr); // an effect renders through a pixmap: dropped when done
        for (IconBadge* icon : item.icons)
            icon->setProgress(1);
    }
    /// @p ms in animation time (unscaled).
    void step(double ms)
    {
        const QEasingCurve curve(QEasingCurve::OutCubic);
        for (qsizetype i = 0; i < m_items.size(); ++i) {
            Item& item = m_items[i];
            if (item.done)
                continue;
            const double t = std::clamp((ms - m_lead - i * kStaggerMs) / kEnterMs, 0.0, 1.0);
            if (t >= 1.0) {
                finishItem(item);
                continue;
            }
            const double e = curve.valueForProgress(t);
            for (const QPointer<QWidget>& w : item.widgets)
                if (w)
                    if (auto* effect = dynamic_cast<FadeRise*>(w->graphicsEffect()))
                        effect->setProgress(e);
            for (IconBadge* icon : item.icons)
                icon->setProgress(e);
        }
    }

    QList<Item> m_items;
    int m_lead;
};

} // namespace

// --- StepIndicator ---------------------------------------------------------------------------

StepIndicator::StepIndicator(int count, QWidget* parent)
    : QWidget(parent)
    , m_count(std::max(1, count))
    , m_anim(new QVariantAnimation(this))
{
    setObjectName(QStringLiteral("WelcomeStep"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_anim->setEasingCurve(QEasingCurve::InOutSine);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_pos = value.toDouble();
        update();
    });
}

QSize StepIndicator::sizeHint() const
{
    return {(m_count - 1) * kPitch + kDot + 4, kDot + 4};
}

void StepIndicator::setCurrent(int index)
{
    index = std::clamp(index, 0, m_count - 1);
    if (index == m_current && m_anim->state() != QAbstractAnimation::Running)
        return;
    m_current = index;
    m_anim->stop();
    if (!uianim::enabled() || !isVisible()) {
        m_pos = index;
        update();
        return;
    }
    m_anim->setDuration(scaled(kStepMs));
    m_anim->setStartValue(m_pos);
    m_anim->setEndValue(double(index));
    m_anim->start();
}

void StepIndicator::paintEvent(QPaintEvent*)
{
    // Quiet dots; the current one is brighter. While the step changes the brightness passes
    // from one dot to the next.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    const double cy = height() / 2.0;
    const double x0 = (width() - (m_count - 1) * kPitch) / 2.0;
    const QColor text = palette().color(QPalette::WindowText);
    for (int i = 0; i < m_count; ++i) {
        const double near = std::max(0.0, 1.0 - std::abs(m_pos - i)); // 1 on the current dot
        QColor dot = text;
        dot.setAlphaF(0.22 + 0.63 * near);
        p.setBrush(dot);
        const double r = kDot / 2.0 + 0.5 * near;
        p.drawEllipse(QPointF(x0 + i * kPitch, cy), r, r);
    }
}

// --- Card ------------------------------------------------------------------------------------

QColor Card::fill(const QPalette& palette)
{
    const QColor window = palette.color(QPalette::Window);
    return window.lightness() < 128 ? window.lighter(118) : window.darker(104);
}

void Card::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.08);
    p.setPen(QPen(border, 1));
    p.setBrush(fill(palette()));
    p.drawRoundedRect(r, 10, 10);
}

// --- IconBadge -------------------------------------------------------------------------------

IconBadge::IconBadge(const QPixmap& art, int size, QWidget* parent)
    : QWidget(parent)
    , m_art(art)
    , m_size(size)
{
    setObjectName(QStringLiteral("WelcomeIcon"));
    setFixedSize(size, size);
}

void IconBadge::setProgress(double t)
{
    m_t = t;
    update();
}

void IconBadge::paintEvent(QPaintEvent*)
{
    if (m_t <= 0)
        return;
    QPainter p(this);
    p.setOpacity(std::clamp(m_t, 0.0, 1.0));
    // Settles from 94 % while it fades in; pixel art stays sharp.
    const double s = m_size * (0.94 + 0.06 * m_t);
    const QRectF target((width() - s) / 2.0, (height() - s) / 2.0, s, s);
    p.drawPixmap(target, m_art, QRectF(m_art.rect()));
}

// --- FadeRise, page change and entrance ---------------------------------------------------------

FadeRise::FadeRise(int rise, QObject* parent)
    : QGraphicsEffect(parent)
    , m_rise(rise)
{
}

void FadeRise::setProgress(double t)
{
    m_t = t;
    update();
}

QRectF FadeRise::boundingRectFor(const QRectF& rect) const
{
    return rect.adjusted(0, 0, 0, m_rise);
}

void FadeRise::draw(QPainter* painter)
{
    QPoint offset;
    const QPixmap source = sourcePixmap(Qt::LogicalCoordinates, &offset, PadToEffectiveBoundingRect);
    if (source.isNull())
        return;
    painter->save();
    painter->setOpacity(painter->opacity() * m_t);
    painter->drawPixmap(offset + QPoint(0, qRound(m_rise * (1.0 - m_t))), source);
    painter->restore();
}

void enter(QWidget* owner, const QList<Group>& groups, int leadMs)
{
    if (!uianim::enabled() || groups.isEmpty())
        return;
    for (QObject* child : owner->children())
        if (auto* old = dynamic_cast<Entrance*>(child))
            delete old;
    new Entrance(owner, groups, leadMs);
}

void changePage(QStackedWidget* pages, int index, bool forward, const QList<Group>& groups)
{
    if (index == pages->currentIndex())
        return;
    if (!uianim::enabled() || !pages->isVisible()) {
        pages->setCurrentIndex(index);
        return;
    }
    // A change while one runs: the running one ends at once, this one starts from there.
    for (QObject* child : pages->children())
        if (auto* old = dynamic_cast<LeaveOverlay*>(child))
            delete old;
    const qreal dpr = pages->devicePixelRatioF();
    QPixmap from(pages->currentWidget()->size() * dpr);
    from.setDevicePixelRatio(dpr);
    from.fill(Qt::transparent);
    pages->currentWidget()->render(&from, QPoint(), QRegion(), QWidget::DrawChildren);
    enter(pages, groups, kLeaveMs); // hides the new page's elements at once
    pages->setCurrentIndex(index);
    auto* overlay = new LeaveOverlay(pages, std::move(from), forward);
    overlay->show();
    overlay->raise();
    auto* anim = new QVariantAnimation(overlay);
    anim->setDuration(scaled(kLeaveMs));
    anim->setEasingCurve(QEasingCurve::Linear); // the overlay eases itself
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    QObject::connect(anim, &QVariantAnimation::valueChanged, overlay,
        [overlay](const QVariant& v) { overlay->setProgress(v.toDouble()); });
    QObject::connect(anim, &QVariantAnimation::finished, overlay, &QObject::deleteLater);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace kk::welcomeanim
