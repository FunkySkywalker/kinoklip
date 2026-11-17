// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/WelcomeAnimations.h"

#include "ui/common/UiAnimation.h"

#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include <QStackedWidget>
#include <QTimer>
#include <QVariantAnimation>

#include <cmath>

namespace kk::welcomeanim {
namespace {

constexpr int kDot = 7; // inactive dot diameter
constexpr int kPill = 20; // active pill width
constexpr int kPitch = 20; // distance between dot centres
constexpr int kSlideDistance = 48; // how far a page travels while it fades

/// @p ms at the user's animation speed.
int scaled(int ms)
{
    return std::max(1, qRound(ms / uianim::speed()));
}

/// The two page pictures of a running page change, drawn over the stacked widget.
class SlideOverlay : public QWidget
{
public:
    SlideOverlay(QWidget* parent, QPixmap from, QPixmap to, bool forward)
        : QWidget(parent)
        , m_from(std::move(from))
        , m_to(std::move(to))
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
        p.fillRect(rect(), Card::fill(palette()));
        const double out = kSlideDistance * m_t;
        const double in = kSlideDistance * (1.0 - m_t);
        p.setOpacity(1.0 - m_t);
        p.drawPixmap(QPointF(-m_dir * out, 0), m_from);
        p.setOpacity(m_t);
        p.drawPixmap(QPointF(m_dir * in, 0), m_to);
    }

private:
    QPixmap m_from;
    QPixmap m_to;
    int m_dir;
    double m_t = 0;
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
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_pos = value.toDouble();
        update();
    });
}

QSize StepIndicator::sizeHint() const
{
    return {(m_count - 1) * kPitch + kPill + 2, kDot + 4};
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
    m_anim->setDuration(scaled(320));
    m_anim->setStartValue(m_pos);
    m_anim->setEndValue(double(index));
    m_anim->start();
}

void StepIndicator::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    const double cy = height() / 2.0;
    const double x0 = (width() - ((m_count - 1) * kPitch + kPill)) / 2.0 + kPill / 2.0;
    QColor dot = palette().color(QPalette::PlaceholderText);
    dot.setAlphaF(0.55);
    p.setBrush(dot);
    for (int i = 0; i < m_count; ++i)
        p.drawEllipse(QPointF(x0 + i * kPitch, cy), kDot / 2.0, kDot / 2.0);
    // The pill stretches a little mid-glide, like something pulled between two places.
    const double frac = m_pos - std::floor(m_pos);
    const double width = kPill + 10.0 * std::sin(M_PI * frac);
    const QRectF pill(x0 + m_pos * kPitch - width / 2.0, cy - kDot / 2.0, width, kDot);
    p.setBrush(palette().color(QPalette::Highlight));
    p.drawRoundedRect(pill, kDot / 2.0, kDot / 2.0);
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
    // Grows from 70 % with a light overshoot (the easing does it); pixel art stays sharp.
    const double s = m_size * (0.7 + 0.3 * m_t);
    const QRectF target((width() - s) / 2.0, (height() - s) / 2.0, s, s);
    p.drawPixmap(target, m_art, QRectF(m_art.rect()));
}

// --- page slide and reveal -------------------------------------------------------------------

void slideTo(QStackedWidget* pages, int index, bool forward)
{
    if (index == pages->currentIndex())
        return;
    if (!uianim::enabled() || !pages->isVisible()) {
        pages->setCurrentIndex(index);
        return;
    }
    // A change while one runs: the running one ends at once, this one starts from there.
    for (QObject* child : pages->children())
        if (auto* old = dynamic_cast<SlideOverlay*>(child))
            delete old;
    // Pictures on the card colour (the pages themselves are transparent over the card).
    const auto picture = [pages](QWidget* page) {
        const qreal dpr = pages->devicePixelRatioF();
        QPixmap pixmap(page->size() * dpr);
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Card::fill(pages->palette()));
        page->render(&pixmap, QPoint(), QRegion(), QWidget::DrawChildren);
        return pixmap;
    };
    QPixmap from = picture(pages->currentWidget());
    pages->setCurrentIndex(index);
    QPixmap to = picture(pages->currentWidget());
    auto* overlay = new SlideOverlay(pages, std::move(from), std::move(to), forward);
    overlay->show();
    overlay->raise();
    auto* anim = new QVariantAnimation(overlay);
    anim->setDuration(scaled(340));
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    QObject::connect(anim, &QVariantAnimation::valueChanged, overlay,
        [overlay](const QVariant& v) { overlay->setProgress(v.toDouble()); });
    QObject::connect(anim, &QVariantAnimation::finished, overlay, &QObject::deleteLater);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void reveal(IconBadge* icon, const QList<QWidget*>& widgets)
{
    if (!uianim::enabled()) {
        icon->setProgress(1);
        return;
    }
    icon->setProgress(0);
    auto* pop = new QVariantAnimation(icon);
    pop->setDuration(scaled(520));
    pop->setEasingCurve(QEasingCurve(QEasingCurve::OutBack));
    pop->setStartValue(0.0);
    pop->setEndValue(1.0);
    QObject::connect(pop, &QVariantAnimation::valueChanged, icon,
        [icon](const QVariant& v) { icon->setProgress(v.toDouble()); });
    pop->start(QAbstractAnimation::DeleteWhenStopped);

    int delay = 140;
    for (QWidget* widget : widgets) {
        auto* effect = new QGraphicsOpacityEffect(widget);
        effect->setOpacity(0);
        widget->setGraphicsEffect(effect);
        QPointer<QWidget> guard(widget);
        QTimer::singleShot(scaled(delay), widget, [guard, effect] {
            if (!guard)
                return;
            auto* fade = new QPropertyAnimation(effect, "opacity", guard);
            fade->setDuration(scaled(420));
            fade->setEasingCurve(QEasingCurve::OutCubic);
            fade->setStartValue(0.0);
            fade->setEndValue(1.0);
            // Dropped when done: an opacity effect renders the widget through a pixmap.
            QObject::connect(fade, &QPropertyAnimation::finished, guard,
                [guard] { if (guard) guard->setGraphicsEffect(nullptr); });
            fade->start(QAbstractAnimation::DeleteWhenStopped);
        });
        delay += 90;
    }
}

} // namespace kk::welcomeanim
