// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/color/ColorSizing.h"

#include "core/ParamKeys.h"
#include "core/Sizing.h"
#include "ui/common/VimRowOps.h"
#include "ui/inspector/ValueField.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

QToolButton* sizingButton(const QString& text, QWidget* parent, bool checkable)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

ValueField* sizingField(QWidget* parent, double lo, double hi, double step, int decimals)
{
    auto* f = new ValueField(parent);
    f->setRange(lo, hi);
    f->setStep(step);
    f->setDecimals(decimals);
    f->setFixedWidth(std::max(72, f->sizeHint().width()));
    return f;
}

} // namespace

SizingPanel::SizingPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    markVimForm(this);
    setObjectName(QStringLiteral("SizingPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 4);
    layout->setSpacing(3);

    auto* tabs = new QHBoxLayout;
    auto* group = new QButtonGroup(this);
    const QString names[2] = {tr("Input Sizing"), tr("Output Sizing")};
    for (int i = 0; i < 2; ++i) {
        m_scopeButtons[i] = sizingButton(names[i], this, true);
        m_scopeButtons[i]->setProperty("kkTab", true); // Kinoklip style: red underline when checked
        m_scopeButtons[i]->setObjectName(QStringLiteral("SizingScope_") + QString::number(i));
        group->addButton(m_scopeButtons[i], i);
        tabs->addWidget(m_scopeButtons[i]);
    }
    tabs->addStretch(1);
    m_scopeButtons[0]->setChecked(true);
    layout->addLayout(tabs);
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        setScope(id == 0 ? ColorSession::SizingScope::Input : ColorSession::SizingScope::Output);
    });

    struct Spec
    {
        QString name;
        QString label;
        double lo, hi, step;
        int decimals;
    };
    const Spec specs[] = {
        {QStringLiteral("zoom"), tr("Zoom"), 0.01, 20.0, 0.005, 3},
        {QStringLiteral("pan"), tr("Pan"), -8000.0, 8000.0, 1.0, 1},
        {QStringLiteral("tilt"), tr("Tilt"), -8000.0, 8000.0, 1.0, 1},
        {QStringLiteral("rotate"), tr("Rotate"), -3600.0, 3600.0, 0.1, 2},
        {QStringLiteral("width"), tr("Width"), 0.01, 20.0, 0.005, 3},
        {QStringLiteral("height"), tr("Height"), 0.01, 20.0, 0.005, 3},
    };
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 2, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(3);
    int i = 0;
    for (const Spec& s : specs) {
        const int col = (i / 3) * 3; // two columns of three rows: Zoom Pan Tilt | Rotate Width Height
        const int row = i % 3;
        grid->addWidget(new QLabel(s.label, this), row, col);
        ValueField* f = sizingField(this, s.lo, s.hi, s.step, s.decimals);
        f->setObjectName(QStringLiteral("Sizing_") + s.name);
        grid->addWidget(f, row, col + 1);
        bind(f, s.name);
        ++i;
    }
    grid->setColumnMinimumWidth(2, 24);
    grid->setColumnStretch(6, 1);
    layout->addLayout(grid);

    auto* flips = new QHBoxLayout;
    m_flips[0] = sizingButton(tr("Flip H"), this, true);
    m_flips[0]->setObjectName(QStringLiteral("Sizing_flipH"));
    m_flips[1] = sizingButton(tr("Flip V"), this, true);
    m_flips[1]->setObjectName(QStringLiteral("Sizing_flipV"));
    for (int v = 0; v < 2; ++v) {
        flips->addWidget(m_flips[v]);
        connect(m_flips[v], &QToolButton::clicked, this, [this, v](bool on) {
            if (m_refreshing || !m_session)
                return;
            m_session->setSizingFlip(m_scope, v ? keys::FlipV : keys::FlipH, on,
                v ? tr("Flip Vertical") : tr("Flip Horizontal"));
        });
    }
    flips->addStretch(1);
    layout->addLayout(flips);
    layout->addStretch(1);

    if (m_session) {
        connect(m_session, &ColorSession::sizingChanged, this, &SizingPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &SizingPanel::refresh);
    }
    refresh();
}

ValueField* SizingPanel::field(const QString& name) const
{
    for (const auto& [n, f] : m_fields) {
        if (n == name)
            return f;
    }
    return nullptr;
}

void SizingPanel::setScope(ColorSession::SizingScope scope)
{
    m_scope = scope;
    m_scopeButtons[scope == ColorSession::SizingScope::Input ? 0 : 1]->setChecked(true);
    refresh();
}

void SizingPanel::bind(ValueField* f, const QString& name)
{
    connect(f, &ValueField::scrubStarted, this, [this] {
        if (m_session)
            m_session->beginGesture(tr("Sizing"));
    });
    connect(f, &ValueField::valueScrubbed, this, [this, name](double v) { apply(name, v); });
    connect(f, &ValueField::scrubFinished, this, [this] {
        if (m_session)
            m_session->endGesture();
    });
    connect(f, &ValueField::valueCommitted, this, [this, name](double v) { apply(name, v); });
    m_fields.append({name, f});
}

void SizingPanel::apply(const QString& name, double value)
{
    if (m_refreshing || !m_session)
        return;
    if (name == QLatin1String("zoom")) {
        m_session->setSizingZoom(m_scope, value, tr("Zoom"));
    } else if (name == QLatin1String("width")) {
        m_session->setSizing(m_scope, {{keys::ZoomX, value}}, tr("Width"));
    } else if (name == QLatin1String("height")) {
        m_session->setSizing(m_scope, {{keys::ZoomY, value}}, tr("Height"));
    } else if (name == QLatin1String("pan")) {
        m_session->setSizing(m_scope, {{keys::PosX, value}}, tr("Pan"));
    } else if (name == QLatin1String("tilt")) {
        m_session->setSizing(m_scope, {{keys::PosY, -value}}, tr("Tilt")); // Tilt is up
    } else if (name == QLatin1String("rotate")) {
        m_session->setSizing(m_scope, {{keys::Rotation, value}}, tr("Rotate"));
    }
}

void SizingPanel::resetAll()
{
    if (m_session)
        m_session->resetSizing(m_scope);
}

void SizingPanel::refresh()
{
    m_refreshing = true;
    const bool usable = m_session && m_session->hasSizing(m_scope);
    setEnabled(true);
    for (const auto& [n, f] : std::as_const(m_fields))
        f->setEnabled(usable);
    for (QToolButton* b : m_flips)
        b->setEnabled(usable);
    m_scopeButtons[0]->setEnabled(m_session && m_session->hasSizing(ColorSession::SizingScope::Input));
    if (usable) {
        const ParamSet p = m_session->sizingOf(m_scope);
        const int64_t frame = m_session->sizingFrame(m_scope);
        const struct
        {
            const char* name;
            double value;
        } values[] = {
            {"zoom", sizing::zoomOf(p, frame)},
            {"width", p.value(keys::ZoomX, frame)},
            {"height", p.value(keys::ZoomY, frame)},
            {"pan", p.value(keys::PosX, frame)},
            {"tilt", -p.value(keys::PosY, frame)},
            {"rotate", p.value(keys::Rotation, frame)},
        };
        for (const auto& v : values) {
            if (ValueField* f = field(QLatin1String(v.name)); f && !f->isScrubbing())
                f->setValue(v.value);
        }
        m_flips[0]->setChecked(p.staticValue(keys::FlipH, false).toBool());
        m_flips[1]->setChecked(p.staticValue(keys::FlipV, false).toBool());
    } else {
        for (const auto& [n, f] : std::as_const(m_fields))
            f->setValue(n == QLatin1String("zoom") || n == QLatin1String("width")
                    || n == QLatin1String("height") ? 1.0 : 0.0);
        m_flips[0]->setChecked(false);
        m_flips[1]->setChecked(false);
    }
    m_refreshing = false;
}

} // namespace kk
