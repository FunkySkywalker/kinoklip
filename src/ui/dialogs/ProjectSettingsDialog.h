// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectSettingsDialog — Resolve-style project settings: a section list (Master Settings,
// Image Scaling, Audio, General Options, Optimized Media and Render Cache) and the section's
// settings.
#pragma once

#include "core/Project.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QSpinBox;
class QStackedWidget;

namespace kk {

/// Edits ProjectSettings. Save applies them with Project::setSettings(), which makes
/// resolution and frame-rate changes one undoable step (the name change marks the project
/// modified).
///
/// Sections (left list, like Resolve):
/// - Presets: the saved presets (name, timeline resolution, frame rate) and "Current Project";
///   Save As, Load, Update, Delete, Import and Export; Save As User Default Config (what new
///   projects start from) and Reset to factory default (core/ProjectPresets.h, spec 127).
/// - Master Settings: timeline resolution presets (UHD, QHD, HD, 720p, vertical, square) or
///   Custom, pixel aspect ratio (square only), timeline frame rate (23.976 ... 60; locked
///   once a timeline that follows the project settings has clips, as in Resolve), playback
///   frame rate (= timeline), frame interpolation (the default retime process of retimed
///   clips: Nearest, Frame Blend, Optical Flow), video monitoring (no output devices yet).
/// - Image Scaling: mismatched resolution handling ("scale entire image to fit", the
///   compositor's only mode for now), output scaling (= timeline).
/// - Audio: sample rate 48 kHz and 32-bit float mixing (the mixer's fixed format, PLAN 5.1).
/// - General Options: standard video / audio transition and their duration (frames; what
///   Ctrl+T adds; Project::standardTransitionId()), project name, starting timecode of new
///   timelines (spec 25.12), autosave interval (the user preference `project/autosaveMinutes`,
///   shared with Preferences > Project Save and Load).
/// - Render Cache: render cache / optimized media placeholders.
/// Settings the engine cannot change yet are shown disabled with a tooltip saying why.
class ProjectSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    /// Creates the dialog filled from @p project's current settings.
    explicit ProjectSettingsDialog(Project* project, QWidget* parent = nullptr);

    /// Settings as currently entered.
    [[nodiscard]] ProjectSettings settings() const;
    /// Optimized media / proxy / render cache settings currently shown.
    [[nodiscard]] MediaCacheSettings cacheSettings() const;
    /// Fills the controls from @p settings (selects a matching preset or Custom).
    void setSettings(const ProjectSettings& settings);
    /// Applies the entered settings to the project (one undo step if the timeline changes).
    void apply();

    /// Frame rates offered, in UI order.
    [[nodiscard]] static QVector<Rational> frameRates();
    /// Resolve's timeline resolution presets ("1920 x 1080 HD", ...; Project Settings and
    /// Timeline Settings): fills @p presets with them and "Custom", selects the one matching
    /// @p width x @p height and keeps them in step: a preset sets both sizes and locks them,
    /// Custom unlocks them. Call after the spin boxes have their values.
    static void bindResolutionPresets(QComboBox* presets, QSpinBox* width, QSpinBox* height);
    /// Selects the preset of @p presets (bound above) that matches the sizes, else Custom.
    static void selectResolutionPreset(QComboBox* presets, int width, int height);

    void accept() override;
    // ---- Presets page (the buttons call these) ----
    /// Saves the settings entered here under @p name, replacing a preset of that name.
    bool saveAsPreset(const QString& name);
    /// Applies the saved preset @p name to the project at once (one undo step) and shows it
    /// here. The frame rate (and starting timecode) stay as they are while a timeline that
    /// follows the project has clips; presetStatus() says so. False if there is no such preset.
    bool loadPreset(const QString& name);
    /// Overwrites the saved preset @p name with the settings entered here.
    bool updatePreset(const QString& name);
    /// Removes the saved preset @p name (the button asks first).
    bool deletePreset(const QString& name);
    /// Reads a preset file and adds it to the saved presets (a preset of that name is replaced).
    bool importPreset(const QString& path);
    /// Writes the saved preset @p name (empty = the settings entered here) to @p path.
    bool exportPreset(const QString& name, const QString& path);
    /// Stores the settings entered here as the user default config: new projects start from it.
    bool saveAsUserDefault();
    /// Removes the user default config: new projects start from the built-in defaults again.
    bool resetFactoryDefault();
    /// The sentence the Presets page shows after the last action (also for failures).
    [[nodiscard]] QString presetStatus() const { return m_presetStatusText; }
    /// Fills the cache controls from @p settings.
    void setCacheSettings(const MediaCacheSettings& settings);

    /// Shows the section @p sectionId ("presets", "master", "scaling", "audio", "general", "cache").
    bool showSection(const QString& sectionId);

signals:
    /// The autosave interval (a user preference) was stored.
    void preferencesChanged();

private:
    QWidget* buildPresets();
    void refreshPresets();
    void updatePresetButtons();
    void setPresetStatus(const QString& text, bool problem = false);
    [[nodiscard]] QString selectedPresetName() const;
    QWidget* buildMaster();
    QWidget* buildScaling();
    QWidget* buildAudio();
    QWidget* buildColor();
    QWidget* buildGeneral();
    QWidget* buildCache();
    void addSection(const QString& id, const QString& title, QWidget* page);

    void updateWarning();
    [[nodiscard]] bool followersHaveClips() const;
    [[nodiscard]] bool confirmFrameRateChange();

    Project* m_project = nullptr;
    QLineEdit* m_name = nullptr;
    QComboBox* m_preset = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QComboBox* m_fps = nullptr;
    QComboBox* m_sampleRate = nullptr;
    QComboBox* m_playbackFps = nullptr;
    QComboBox* m_retimeProcess = nullptr; ///< Frame Interpolation > Retime process
    QComboBox* m_toneMap = nullptr; ///< Color Management > Output tone mapping
    QComboBox* m_agxLook = nullptr; ///< Color Management > AgX look
    QSpinBox* m_autosave = nullptr;
    QLineEdit* m_timelineStart = nullptr; ///< General Options > starting timecode
    QComboBox* m_stdVideo = nullptr; ///< General Options > standard video transition
    QComboBox* m_stdAudio = nullptr; ///< General Options > standard audio transition
    QSpinBox* m_stdFrames = nullptr; ///< General Options > standard transition duration
    QLabel* m_warning = nullptr;
    QComboBox* m_proxyResolution = nullptr;
    QComboBox* m_proxyFormat = nullptr;
    QComboBox* m_optimizedResolution = nullptr;
    QComboBox* m_optimizedFormat = nullptr;
    QComboBox* m_cacheFormat = nullptr;
    QCheckBox* m_backgroundCache = nullptr;
    QSpinBox* m_backgroundDelay = nullptr;
    QCheckBox* m_userTransitions = nullptr;
    QCheckBox* m_userComposites = nullptr;
    QTreeWidget* m_presetList = nullptr;
    QLabel* m_presetStatus = nullptr;
    QString m_presetStatusText;
    QPushButton* m_presetLoad = nullptr;
    QPushButton* m_presetUpdate = nullptr;
    QPushButton* m_presetDelete = nullptr;
    QPushButton* m_presetExport = nullptr;
    QListWidget* m_sections = nullptr;
    QStackedWidget* m_stack = nullptr;
};

} // namespace kk
