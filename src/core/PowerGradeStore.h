// SPDX-License-Identifier: GPL-3.0-or-later
// PowerGradeStore — the PowerGrade albums of the Color page's gallery: stills with grades that
// belong to the user, not to a project, so they are there in every project. Kept in
// `<AppDataLocation>/powergrades.json` and saved on every change (like core/PowerBins.h).
#pragma once

#include "core/GradeGallery.h"

#include <QObject>
#include <QString>

namespace kk {

class PowerGradeStore : public QObject
{
    Q_OBJECT
public:
    /// Loads @p filePath (empty: defaultFilePath()). A missing file is an empty store; a damaged
    /// one is copied to `<file>.bak` and replaced by the next change.
    explicit PowerGradeStore(const QString& filePath = {}, QObject* parent = nullptr);

    /// The albums and stills; changes made through it are saved.
    [[nodiscard]] GradeGallery* gallery() const { return m_gallery; }
    [[nodiscard]] QString filePath() const { return m_filePath; }
    [[nodiscard]] static QString defaultFilePath();

    /// Reads the file again (false with @p error when it is damaged).
    bool load(QString* error = nullptr);
    /// Writes the file (done after every change).
    bool save(QString* error = nullptr) const;

private:
    GradeGallery* m_gallery = nullptr;
    QString m_filePath;
    bool m_loading = false;
};

} // namespace kk
