// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/UiFonts.h"

#include "core/Preferences.h"

#include <QApplication>
#include <QFontDatabase>
#include <QSettings>

#include <algorithm>

namespace kk::uifonts {

namespace {

// Sizes in points, measured from Resolve 21 on a 1920x1080 screen (readme.md): body text has a
// cap height of about 7 px (9 px font), the timeline timecode 13 px cap height.
constexpr double kBodyPt = 7.0;
constexpr double kHeadingPt = 8.0;
constexpr double kTitlePt = 12.0;
constexpr double kSmallPt = 6.5;
constexpr double kTinyPt = 6.0;
constexpr double kTimecodePt = 12.0;
constexpr double kViewerTimecodePt = 9.0;

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
    f.setStyleStrategy(QFont::PreferDefault);
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
        QFont::Weight w = QFont::Normal;
        switch (role) {
        case Role::Body:
        case Role::SectionHeader: pt = kBodyPt; break;
        case Role::Emphasis: pt = kBodyPt; w = QFont::Bold; break;
        case Role::PanelTitle: pt = kBodyPt; w = QFont::DemiBold; break;
        case Role::Heading: pt = kHeadingPt; w = QFont::DemiBold; break;
        case Role::Title: pt = kTitlePt; w = QFont::DemiBold; break;
        case Role::Small: pt = kSmallPt; break;
        case Role::SmallEmphasis: pt = kSmallPt; w = QFont::Bold; break;
        case Role::Tiny: pt = kTinyPt; break;
        case Role::Timecode: pt = kTimecodePt; w = QFont::Bold; break;
        case Role::ViewerTimecode: pt = kViewerTimecodePt; w = QFont::DemiBold; break;
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
    for (const char* file : {"Regular", "Italic", "SemiBold", "Bold"})
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
        case Role::Tiny:
        case Role::Mono:
            return {};
        default:
            return QStringLiteral("font-weight: 600;");
        }
    }
    QString out = QStringLiteral("font-weight: %1;").arg(f.weight() >= QFont::Bold ? 700 : f.weight() >= QFont::DemiBold ? 600 : 400);
    if (role == Role::ViewerTimecode || role == Role::Timecode)
        out += QStringLiteral(" font-size: %1pt;").arg(f.pointSizeF());
    return out;
}

void apply(QApplication& app)
{
    loadBundledFonts();
    const QFont system = systemFont(); // before the first change
    app.setFont(font(Role::Body, choice(), system));
}

QString styleSheetRules()
{
    return QStringLiteral("QLabel#PanelTitle { %1 }\nQLabel#TimecodeLabel { %2 }\n")
        .arg(css(Role::PanelTitle), css(Role::ViewerTimecode));
}

} // namespace kk::uifonts
