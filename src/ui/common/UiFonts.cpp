// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/UiFonts.h"

#include "core/Preferences.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPointer>
#include <QWidget>
#include <QSettings>

#include <algorithm>

namespace kk::uifonts {

namespace {

// Sizes in points at 96 DPI (1 pt = 4/3 px), fitted to the native 1920x1017 screenshot of Resolve 17
// (100 % scaling) by ink width and height; all of Resolve's text is regular weight (readme.md).
constexpr double kBodyPt = 9.0; ///< 12 px: menus, toolbar labels, inspector
constexpr double kPanelPt = 9.75; ///< 13 px: panel titles, tree items, section headers, viewer numbers
constexpr double kHeadingPt = 10.5; ///< 14 px
constexpr double kTitlePt = 12.0; ///< 16 px: the project name
constexpr double kSmallPt = 8.25; ///< 11 px: item labels, mixer labels, captions, ruler (10 px in Resolve)
constexpr double kTinyPt = 7.5; ///< 10 px: ruler numbers (Role::Ruler), scales
constexpr double kTimecodePt = 15.75; ///< 21 px: the big timeline timecode

QString& sessionOverride()
{
    static QString id;
    return id;
}

QFont resolveFont(double pt, QFont::Weight weight)
{
    QFont f{QString::fromLatin1(kFamily)};
    f.setPointSizeF(pt);
    f.setWeight(weight);
    // Hinted Medium has the ink density of Resolve's text (within 3 %); Regular is 14 % lighter.
    return f;
}


QFont fontScaled(Role role, Choice which, const QFont& base, double scale)
{
    QFont f = base;
    double pt = 0;
    bool bold = false;
    if (which == Choice::System) {
        const double b = base.pointSizeF() > 0 ? base.pointSizeF() : 9.0;
        switch (role) {
        case Role::Body:
        case Role::SectionHeader:
            pt = b;
            break;
        case Role::Emphasis:
            pt = b;
            bold = true;
            break;
        case Role::PanelTitle:
            pt = b;
            f.setWeight(QFont::DemiBold);
            break;
        case Role::Heading:
            pt = b + 2;
            f.setWeight(QFont::DemiBold);
            break;
        case Role::Title:
            pt = b * 1.8;
            f.setWeight(QFont::DemiBold);
            break;
        case Role::Ruler:
        case Role::Small:
            pt = std::max(6.0, b - 1.5);
            break;
        case Role::SmallEmphasis:
            pt = std::max(6.0, b - 1.5);
            bold = true;
            break;
        case Role::Tiny:
            pt = std::max(6.0, b * 0.8);
            break;
        case Role::Timecode:
            pt = b + 2;
            bold = true;
            break;
        case Role::ViewerTimecode:
            pt = b;
            f.setFamily(QStringLiteral("monospace"));
            f.setWeight(QFont::DemiBold);
            break;
        case Role::Mono:
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        }
    } else {
        QFont::Weight w = QFont::Medium; // Resolve's text is heavier than Regular (readme)
        switch (role) {
        case Role::Body: pt = kBodyPt; break;
        case Role::SectionHeader: pt = kPanelPt; break;
        case Role::Emphasis: pt = kBodyPt; w = QFont::Bold; break;
        case Role::PanelTitle: pt = kPanelPt; break;
        case Role::Heading: pt = kHeadingPt; w = QFont::DemiBold; break;
        case Role::Title: pt = kTitlePt; break;
        case Role::Small: pt = kSmallPt; break;
        case Role::Ruler: pt = kTinyPt; break;
        case Role::SmallEmphasis: pt = kSmallPt; w = QFont::Bold; break;
        case Role::Tiny: pt = kTinyPt; break;
        case Role::Timecode: pt = kTimecodePt; break;
        case Role::ViewerTimecode: pt = kPanelPt; break;
        case Role::Mono: return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        }
        f = resolveFont(pt, w);
    }
    if (bold)
        f.setBold(true);
    if (scale != 1.0)
        pt *= scale;
    f.setPointSizeF(std::max(4.0, pt));
    return f;
}

} // namespace

QString choiceId(Choice choice)
{
    return QLatin1String(choice == Choice::System ? kSystemId : kResolveId);
}

Choice choiceFromId(const QString& id)
{
    return id == QLatin1String(kSystemId) ? Choice::System : Choice::Resolve;
}

Choice choice()
{
    if (!sessionOverride().isEmpty())
        return choiceFromId(sessionOverride());
    return choiceFromId(
        QSettings().value(QLatin1String(prefs::kInterfaceFont), QLatin1String(kResolveId)).toString());
}

namespace {
std::function<void()>& changedHandler()
{
    static std::function<void()> handler;
    return handler;
}
} // namespace

void setChangedHandler(std::function<void()> handler)
{
    changedHandler() = std::move(handler);
}

void setChoice(Choice wanted)
{
    const Choice before = choice();
    QSettings().setValue(QLatin1String(prefs::kInterfaceFont), choiceId(wanted));
    sessionOverride().clear();
    if (before != wanted && changedHandler())
        changedHandler()();
}

void setSessionOverride(const QString& id)
{
    const QString lower = id.toLower();
    sessionOverride() = (lower == QLatin1String(kResolveId) || lower == QLatin1String(kSystemId))
        ? lower
        : QString();
}

void loadBundledFonts()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    for (const char* file : {"Regular", "Italic", "Medium", "SemiBold", "Bold"})
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/OpenSans-%1.ttf").arg(QLatin1String(file)));
}

QFont systemFont()
{
    static const QFont captured = QApplication::font();
    return captured;
}

QFont font(Role role, Choice which, const QFont& base)
{
    return fontScaled(role, which, base, 1.0);
}

QFont font(Role role)
{
    return font(role, choice(), systemFont());
}

QFont font(Role role, const QFont& base, double scale)
{
    return fontScaled(role, choice(), base, scale);
}

QFont italic(QFont f, bool on)
{
    f.setItalic(on);
    return f;
}

QFont withPixelSize(Role role, const QFont& base, int px)
{
    QFont f = font(role, base);
    f.setPixelSize(std::max(1, px));
    return f;
}

QString css(Role role)
{
    const QFont f = font(role, systemFont());
    if (choice() == Choice::System) {
        switch (role) {
        case Role::Emphasis:
        case Role::SmallEmphasis:
            return QStringLiteral("font-weight: bold;");
        case Role::ViewerTimecode:
            return QStringLiteral("font-family: monospace; font-weight: 600;");
        case Role::Body:
        case Role::SectionHeader:
        case Role::Small:
        case Role::Ruler:
        case Role::Tiny:
        case Role::Mono:
            return {};
        default:
            return QStringLiteral("font-weight: 600;");
        }
    }
    QString out = QStringLiteral("font-weight: %1; font-size: %2pt;")
                      .arg(f.weight() >= QFont::Bold ? 700 : f.weight() >= QFont::DemiBold ? 600 : f.weight() >= QFont::Medium ? 500 : 400)
                      .arg(f.pointSizeF());
    return out;
}

namespace {
struct Bound
{
    QPointer<QWidget> widget;
    Role role;
};
QList<Bound>& bound()
{
    static QList<Bound> list;
    return list;
}
void restyle(const Bound& b)
{
    if (!b.widget)
        return;
    const QFont f = font(b.role, systemFont());
    if (b.widget->font() != f)
        b.widget->setFont(f);
}
} // namespace

void bindLabel(QWidget* label, Role role)
{
    bound().removeIf([](const Bound& b) { return b.widget.isNull(); });
    bound().append({label, role});
    restyle(bound().last());
}

void apply(QApplication& app)
{
    loadBundledFonts();
    const QFont system = systemFont(); // before the first change
    const QFont body = font(Role::Body, choice(), system);
    if (app.font() != body)
        app.setFont(body);
    for (const Bound& b : std::as_const(bound()))
        restyle(b);
}

QString styleSheetRules()
{
    // The project name in the top bar is the one place Resolve uses its Title size.
    return QStringLiteral("QLabel#PanelTitle { %1 }\nQLabel#TimecodeLabel { %2 }\n"
                          "QLabel#ProjectTitle { %3 }\n")
               .arg(css(Role::PanelTitle), css(Role::ViewerTimecode), css(Role::Title));
}

} // namespace kk::uifonts
