// SPDX-License-Identifier: GPL-3.0-or-later
// VoiceIsolation implementation.
#include "core/VoiceIsolation.h"

#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"

#include <algorithm>
#include <cmath>

namespace kk::voiceisolation {

namespace {

EffectInstance* findMutable(QVector<EffectInstance>& effects)
{
    for (EffectInstance& fx : effects) {
        if (fx.type == QLatin1String(afxid::VoiceIsolation))
            return &fx;
    }
    return nullptr;
}

} // namespace

const EffectInstance* find(const QVector<EffectInstance>& effects)
{
    return findMutable(const_cast<QVector<EffectInstance>&>(effects));
}

bool isOn(const QVector<EffectInstance>& effects)
{
    const EffectInstance* fx = find(effects);
    return fx && fx->enabled;
}

double amount(const QVector<EffectInstance>& effects)
{
    const EffectInstance* fx = find(effects);
    return fx ? std::clamp(fx->params.value(QLatin1String(afxkeys::DeAmount), 0, kDefaultAmount), 0.0, 100.0)
              : kDefaultAmount;
}

bool setOn(QVector<EffectInstance>& effects, bool on, quint64 newId)
{
    if (EffectInstance* fx = findMutable(effects)) {
        if (fx->enabled == on)
            return false;
        fx->enabled = on;
        return true;
    }
    if (!on)
        return false;
    effects.prepend(
        EffectRegistry::instance().createEffect(QLatin1String(afxid::VoiceIsolation), newId));
    return true;
}

bool setAmount(QVector<EffectInstance>& effects, double percent, quint64 newId)
{
    percent = std::clamp(std::round(percent), 0.0, 100.0);
    EffectInstance* fx = findMutable(effects);
    if (!fx) {
        EffectInstance created
            = EffectRegistry::instance().createEffect(QLatin1String(afxid::VoiceIsolation), newId);
        created.enabled = false;
        created.params.setBase(QLatin1String(afxkeys::DeAmount), percent);
        effects.prepend(created);
        return true;
    }
    if (amount(effects) == percent && fx->params.values.contains(QLatin1String(afxkeys::DeAmount)))
        return false;
    fx->params.setBase(QLatin1String(afxkeys::DeAmount), percent);
    return true;
}

} // namespace kk::voiceisolation
