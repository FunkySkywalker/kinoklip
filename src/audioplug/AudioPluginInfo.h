// SPDX-License-Identifier: GPL-3.0-or-later
// AudioPluginInfo — what the host learns of an LV2 or CLAP audio effect plugin when it loads it
// (the probe program writes it as JSON, the registry keeps it in its cache): identity, audio
// channels, parameters with ranges, latency. Plain data, Qt Core only (docs/plugins/audio-plugins.md).
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace kk::audioplug {

inline constexpr char kFormatLv2[] = "lv2";
inline constexpr char kFormatClap[] = "clap";

/// One parameter of a plugin: an LV2 control input port or a CLAP parameter.
struct ParamInfo
{
    QString key; ///< the ParamSet key: a plain word, unique inside the plugin
    QString label;
    QString group; ///< CLAP module / LV2 port group, may be empty
    QString unit; ///< "dB", "Hz", ... or empty
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    bool integer = false;
    bool toggle = false;
    bool enumeration = false; ///< a list of choices (enumLabels / enumValues)
    bool logarithmic = false;
    bool hidden = false;
    bool readOnly = false;
    QStringList enumLabels;
    QVector<double> enumValues;
    quint32 id = 0; ///< LV2: the port index; CLAP: the parameter id

    [[nodiscard]] QJsonObject toJson() const;
    static ParamInfo fromJson(const QJsonObject& o);
};

/// One plugin (an LV2 URI, a CLAP plugin id).
struct PluginInfo
{
    QString format; ///< kFormatLv2 / kFormatClap
    QString id; ///< the LV2 URI / the CLAP plugin id
    QString name;
    QString vendor;
    QString category; ///< LV2 class label / CLAP features, for the library tooltip
    QString description;
    QString version;
    int inChannels = 0; ///< the main audio input (LV2: the first non-side-chain audio inputs)
    int outChannels = 0;
    QVector<quint32> audioInPorts; ///< LV2: port indices of the main inputs, in order
    QVector<quint32> audioOutPorts;
    int latencyPort = -1; ///< LV2: control output port that reports the latency (frames)
    quint32 portCount = 0; ///< LV2: the number of ports
    double tailSeconds = 0.0; ///< how long the sound goes on after the input stops (0 = none known)
    QVector<ParamInfo> params;
    QString error; ///< empty = usable; otherwise why Kinoklip does not offer it

    [[nodiscard]] QJsonObject toJson() const;
    static PluginInfo fromJson(const QJsonObject& o);
};

/// The plugins of one bundle (an .lv2 folder, a .clap file).
struct BundleInfo
{
    QString path;
    QString format;
    QVector<PluginInfo> plugins;
    QString error; ///< the bundle as a whole could not be read

    [[nodiscard]] QJsonObject toJson() const;
    static BundleInfo fromJson(const QJsonObject& o);
};

/// A key made of letters, numbers and '_' (keys are plain words; see the OpenFX registry).
QString plainKey(const QString& text);
/// Makes the keys of @p plugin plain and unique (two ports may share a name).
void uniqueKeys(PluginInfo* plugin);

} // namespace kk::audioplug
