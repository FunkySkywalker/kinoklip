// SPDX-License-Identifier: GPL-3.0-or-later
// ParamAccess implementation.
#include "ui/inspector/ParamAccess.h"

#include "core/EffectRegistry.h"
#include "core/KeyframeOps.h"
#include "core/ParamKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::paramaccess {

namespace {

/// Value an Int parameter can take (descriptor ranges are inclusive).
double normalise(const ParamDescriptor& desc, double value)
{
    if (desc.type == ParamType::Int)
        value = std::round(value);
    return std::clamp(value, desc.min, desc.max);
}

/// For linked zoom: the partner key of zoomX/zoomY, or nullptr if @p ref is not linked.
const QString* linkedZoomPartner(const ClipData& clip, const ParamRef& ref)
{
    if (ref.scope != ParamScope::Transform)
        return nullptr;
    if (ref.key != keys::ZoomX && ref.key != keys::ZoomY)
        return nullptr;
    if (!clip.transform.staticValue(keys::ZoomLinked, true).toBool())
        return nullptr;
    return ref.key == keys::ZoomX ? &keys::ZoomY : &keys::ZoomX;
}

template<typename Clip, typename Fx>
Fx* findEffect(Clip& clip, quint64 effectId)
{
    for (auto& fx : clip.effects) {
        if (fx.id == effectId)
            return &fx;
    }
    return nullptr;
}

} // namespace

std::optional<ParamScope> scopeForStorage(const QString& storage)
{
    if (storage == QLatin1String("transform"))
        return ParamScope::Transform;
    if (storage == QLatin1String("audio"))
        return ParamScope::Audio;
    if (storage == QLatin1String("title"))
        return ParamScope::Title;
    if (storage == QLatin1String("solidColor"))
        return ParamScope::SolidColor;
    return std::nullopt;
}

const EffectInstance* effect(const ClipData& clip, quint64 effectId)
{
    return findEffect<const ClipData, const EffectInstance>(clip, effectId);
}

EffectInstance* effect(ClipData& clip, quint64 effectId)
{
    return findEffect<ClipData, EffectInstance>(clip, effectId);
}

const MaskData* mask(const ClipData& clip, quint64 maskId)
{
    return keyframeops::findMask(clip, maskId);
}

MaskData* mask(ClipData& clip, quint64 maskId)
{
    return keyframeops::findMask(clip, maskId);
}

const TransitionData* transition(const ClipData& clip, ParamScope scope)
{
    const auto& t = scope == ParamScope::TransitionIn ? clip.transitionIn : clip.transitionOut;
    return t ? &*t : nullptr;
}

TransitionData* transition(ClipData& clip, ParamScope scope)
{
    auto& t = scope == ParamScope::TransitionIn ? clip.transitionIn : clip.transitionOut;
    return t ? &*t : nullptr;
}

const ParamSet* paramSet(const ClipData& clip, const ParamRef& ref)
{
    switch (ref.scope) {
    case ParamScope::Transform:
        return &clip.transform;
    case ParamScope::Audio:
        return &clip.audio;
    case ParamScope::Title:
        return &clip.title;
    case ParamScope::TitleLayer: {
        for (const TitleLayer& layer : clip.titleLayers) {
            if (layer.id == ref.effectId)
                return &layer.params;
        }
        return nullptr;
    }
    case ParamScope::SolidColor:
        return nullptr;
    case ParamScope::Effect: {
        const EffectInstance* fx = effect(clip, ref.effectId);
        return fx ? &fx->params : nullptr;
    }
    case ParamScope::TransitionIn:
    case ParamScope::TransitionOut: {
        const TransitionData* t = transition(clip, ref.scope);
        return t ? &t->params : nullptr;
    }
    case ParamScope::Mask: {
        const MaskData* m = mask(clip, ref.effectId);
        return m ? &m->params : nullptr;
    }
    case ParamScope::CompNode: {
        const CompNode* n = clip.composition ? clip.composition->node(ref.effectId) : nullptr;
        return n ? &n->params : nullptr;
    }
    }
    return nullptr;
}

ParamSet* paramSet(ClipData& clip, const ParamRef& ref)
{
    // The same lookup as the const overload, but through the non-const containers: the
    // effect, mask and transition lists are implicitly shared with the undo snapshot taken at
    // beginEdit(), and only a non-const access detaches them (a const_cast of the const
    // lookup would write into the snapshot too, and the edit would leave no undo step).
    switch (ref.scope) {
    case ParamScope::Transform:
        return &clip.transform;
    case ParamScope::Audio:
        return &clip.audio;
    case ParamScope::Title:
        return &clip.title;
    case ParamScope::TitleLayer: {
        for (TitleLayer& layer : clip.titleLayers) {
            if (layer.id == ref.effectId)
                return &layer.params;
        }
        return nullptr;
    }
    case ParamScope::SolidColor:
        return nullptr;
    case ParamScope::Effect: {
        EffectInstance* fx = effect(clip, ref.effectId);
        return fx ? &fx->params : nullptr;
    }
    case ParamScope::TransitionIn:
    case ParamScope::TransitionOut: {
        TransitionData* t = transition(clip, ref.scope);
        return t ? &t->params : nullptr;
    }
    case ParamScope::Mask: {
        MaskData* m = mask(clip, ref.effectId);
        return m ? &m->params : nullptr;
    }
    case ParamScope::CompNode: {
        CompNode* n = clip.composition ? clip.composition->node(ref.effectId) : nullptr;
        return n ? &n->params : nullptr;
    }
    }
    return nullptr;
}

const AnimatedValue* animated(const ClipData& clip, const ParamRef& ref)
{
    if (ref.scope == ParamScope::Mask && ref.key == maskkeys::Shape) {
        // The animated shape: its key timing (values are shape indices, see core/Mask.h).
        const MaskData* m = mask(clip, ref.effectId);
        return m ? &m->path.timing : nullptr;
    }
    const ParamSet* set = paramSet(clip, ref);
    if (!set)
        return nullptr;
    const auto it = set->values.constFind(ref.key);
    return it == set->values.cend() ? nullptr : &*it;
}

double scalar(
    const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame)
{
    if (ref.key == kTransitionDuration) {
        const TransitionData* t = transition(clip, ref.scope);
        return t ? double(t->duration) : desc.def;
    }
    const AnimatedValue* value = animated(clip, ref);
    if (!value)
        return desc.def;
    if (!value->modifiers.isEmpty()) {
        // Modified (Compositing page): the value the renderer sees.
        if (const ParamSet* set = paramSet(clip, ref))
            return set->valueAtTime(ref.key, double(localFrame), desc.def);
    }
    return value->valueAt(localFrame);
}

namespace {

/// The descriptor of the effect addressed by @p ref (Effect scope), or nullptr.
const EffectDescriptor* effectDescriptor(const ClipData& clip, const ParamRef& ref)
{
    if (ref.scope != ParamScope::Effect)
        return nullptr;
    const EffectInstance* fx = effect(clip, ref.effectId);
    return fx ? EffectRegistry::instance().find(fx->type) : nullptr;
}

/// A hand edit of @p ref: every Choice of the effect whose choiceValues set that parameter
/// falls back to its customChoice (Reverb: moving a slider makes the preset Custom).
void noteHandEdit(ClipData& clip, const ParamRef& ref)
{
    const EffectDescriptor* d = effectDescriptor(clip, ref);
    ParamSet* set = paramSet(clip, ref);
    if (!d || !set)
        return;
    for (const ParamDescriptor& p : d->params) {
        if (p.type != ParamType::Choice || p.customChoice.isEmpty() || p.key == ref.key)
            continue;
        bool sets = false;
        for (const auto& list : p.choiceValues) {
            for (const auto& kv : list)
                sets = sets || kv.first == ref.key;
        }
        if (sets && set->staticValue(p.key, p.defStatic).toString() != p.customChoice)
            set->setStatic(p.key, p.customChoice);
    }
}

/// Choosing @p value of choice @p ref: writes the parameters it sets (same edit).
void applyChoiceValues(ClipData& clip, const ParamRef& ref, const QVariant& value)
{
    const EffectDescriptor* d = effectDescriptor(clip, ref);
    const ParamDescriptor* p = d ? d->param(ref.key) : nullptr;
    ParamSet* set = paramSet(clip, ref);
    if (!p || !set || p->choiceValues.isEmpty())
        return;
    const auto it = p->choiceValues.constFind(value.toString());
    if (it == p->choiceValues.constEnd())
        return;
    for (const auto& kv : *it) {
        if (const ParamDescriptor* target = d->param(kv.first))
            set->animated(kv.first, target->def)
                .setValue(0, std::clamp(kv.second, target->min, target->max));
    }
}

} // namespace

void setScalar(ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame,
    double value)
{
    value = normalise(desc, value);
    if (ref.key == kTransitionDuration) {
        if (TransitionData* t = transition(clip, ref.scope))
            t->duration
                = std::clamp<int64_t>(std::llround(value), 1, std::max<int64_t>(1, clip.duration));
        return;
    }
    if (ref.scope == ParamScope::Mask && ref.key == maskkeys::Shape)
        return; // shapes are edited in the viewer, not as a number
    ParamSet* set = paramSet(clip, ref);
    if (!set)
        return;
    AnimatedValue& own = set->animated(ref.key, desc.def);
    const double before = own.valueAt(localFrame);
    own.setValue(localFrame, value);
    noteHandEdit(clip, ref);
    if (const QString* partner = linkedZoomPartner(clip, ref)) {
        // Linked zoom changes both axes by the same factor: equal axes stay equal, and a
        // different X:Y ratio (left by a free Shift scale in the viewer) is kept instead of
        // snapping one axis to the other (spec 49.2).
        AnimatedValue& other = set->animated(*partner, desc.def);
        const double otherBefore = other.valueAt(localFrame);
        const bool sameRatio = before <= 0.0 || std::abs(otherBefore - before) < 1e-9;
        other.setValue(
            localFrame, sameRatio ? value : normalise(desc, otherBefore * value / before));
    }
}

bool toggleKey(ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame)
{
    if (ref.scope == ParamScope::Mask && ref.key == maskkeys::Shape) {
        MaskData* m = mask(clip, ref.effectId);
        if (!m)
            return false;
        if (m->path.timing.hasKeyAt(localFrame)) {
            m->path.removeKey(localFrame);
            return false;
        }
        m->path.setKey(localFrame);
        return true;
    }
    ParamSet* set = paramSet(clip, ref);
    if (!set || ref.key == kTransitionDuration)
        return false;
    AnimatedValue& value = set->animated(ref.key, desc.def);
    const bool remove = value.hasKeyAt(localFrame);
    auto apply = [&](AnimatedValue& v) {
        if (remove)
            v.removeKey(localFrame);
        else
            v.setKey(localFrame, v.valueAt(localFrame));
    };
    apply(value);
    if (const QString* partner = linkedZoomPartner(clip, ref)) {
        AnimatedValue& other = set->animated(*partner, desc.def);
        if (other.hasKeyAt(localFrame) == remove)
            apply(other);
    }
    return !remove;
}

std::optional<Interp> keyInterp(const ClipData& clip, const ParamRef& ref, int64_t localFrame)
{
    if (const AnimatedValue* value = animated(clip, ref)) {
        for (const Keyframe& k : value->keys) {
            if (k.frame == localFrame)
                return kk::keyInterp(k);
        }
    }
    return std::nullopt;
}

bool setKeyInterp(ClipData& clip, const ParamRef& ref, int64_t localFrame, Interp interp)
{
    if (ref.scope == ParamScope::Mask && ref.key == maskkeys::Shape) {
        MaskData* m = mask(clip, ref.effectId);
        const qsizetype i = m ? m->path.timing.indexOf(localFrame) : -1;
        if (i < 0)
            return false;
        applyInterp(m->path.timing.keys[i], interp);
        return true;
    }
    ParamSet* set = paramSet(clip, ref);
    if (!set)
        return false;
    auto apply = [&](const QString& key) {
        const auto it = set->values.find(key);
        if (it == set->values.end())
            return false;
        for (Keyframe& k : it->keys) {
            if (k.frame == localFrame) {
                applyInterp(k, interp);
                return true;
            }
        }
        return false;
    };
    const bool changed = apply(ref.key);
    if (changed) {
        if (const QString* partner = linkedZoomPartner(clip, ref))
            apply(*partner);
    }
    return changed;
}

void reset(ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc)
{
    if (ref.scope == ParamScope::Mask && ref.key == maskkeys::Shape)
        return; // a shape has no default
    if (desc.isScalar()) {
        if (ref.key == kTransitionDuration) {
            setScalar(clip, ref, desc, 0, desc.def);
            return;
        }
        if (ParamSet* set = paramSet(clip, ref)) {
            set->values.insert(ref.key, AnimatedValue(desc.def));
            if (const QString* partner = linkedZoomPartner(clip, ref))
                set->values.insert(*partner, AnimatedValue(desc.def));
        }
        return;
    }
    setStatic(clip, ref, desc.defStatic);
}

QVariant staticValue(const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc)
{
    if (ref.scope == ParamScope::SolidColor)
        return clip.solidColor.isValid() ? QVariant(clip.solidColor) : desc.defStatic;
    if (ref.scope == ParamScope::Mask
        && (ref.key == maskkeys::Mode || ref.key == maskkeys::Invert)) {
        const MaskData* m = mask(clip, ref.effectId);
        if (!m)
            return desc.defStatic;
        return ref.key == maskkeys::Mode ? QVariant(maskModeToString(m->mode))
                                         : QVariant(m->invert);
    }
    const ParamSet* set = paramSet(clip, ref);
    return set ? set->staticValue(ref.key, desc.defStatic) : desc.defStatic;
}

void setStatic(ClipData& clip, const ParamRef& ref, const QVariant& value)
{
    if (ref.scope == ParamScope::SolidColor) {
        clip.solidColor = value.value<QColor>();
        return;
    }
    if (ref.scope == ParamScope::Mask
        && (ref.key == maskkeys::Mode || ref.key == maskkeys::Invert)) {
        if (MaskData* m = mask(clip, ref.effectId)) {
            if (ref.key == maskkeys::Mode)
                m->mode = maskModeFromString(value.toString());
            else
                m->invert = value.toBool();
        }
        return;
    }
    if (ParamSet* set = paramSet(clip, ref)) {
        set->setStatic(ref.key, value);
        applyChoiceValues(clip, ref, value);
    }
}

bool isModified(
    const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame)
{
    if (desc.isScalar()) {
        if (ref.key != kTransitionDuration) {
            const AnimatedValue* value = animated(clip, ref);
            if (value && value->isAnimated())
                return true;
        }
        return std::abs(scalar(clip, ref, desc, localFrame) - desc.def) > 1e-9;
    }
    return staticValue(clip, ref, desc) != desc.defStatic;
}

} // namespace kk::paramaccess
