// SPDX-License-Identifier: GPL-3.0-or-later
// GradeGallery — the Color page's gallery of stills: a grabbed picture plus the grade of the
// clip it was grabbed from, saved with the project ("gallery", format version 16). Stills sit in
// albums (Resolve's Stills albums; the optional project key "galleryExtra", no format bump) and
// the gallery keeps the memories A to Z. The same class holds the PowerGrade albums shared by
// all projects (core/PowerGradeStore.h).
#pragma once

#include "core/Grade.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

namespace kk {

/// One gallery still.
struct GalleryStill
{
    quint64 id = 0; ///< unique within the gallery
    quint64 album = 1; ///< the album it is in (GradeGallery::kDefaultAlbum)
    QString name; ///< "1.1", "1.2", ... (Resolve-like numbering) unless renamed
    qint64 createdMs = 0; ///< ms since the epoch
    ClipGrade grade; ///< the grade of the clip at grab time
    QImage image; ///< reduced picture (at most kGalleryImageWidth wide), ungraded alpha
};

/// An album of stills (the first one, GradeGallery::kDefaultAlbum, always exists and cannot be
/// deleted).
struct GalleryAlbum
{
    quint64 id = 0;
    QString name;
};

/// A gallery memory (A to Z): a grade, with the picture it was saved from when there was one.
struct GalleryMemory
{
    bool used = false;
    ClipGrade grade;
    QImage image;
};

/// Number of memories (A to Z).
inline constexpr int kGalleryMemoryCount = 26;
/// Number of memories with default keys (A to H: Alt+1..8 save, Ctrl+1..8 apply).
inline constexpr int kGalleryKeyedMemories = 8;

/// Width of the stored still pictures (reduced pictures, enough for the gallery, the wipe and an
/// exported image; not reference frames).
inline constexpr int kGalleryImageWidth = 640;

/// The stills, albums and memories of a project, owned by Project (Project::gallery()). Not part
/// of the undo stack (like Resolve's gallery); every change marks the project modified. GUI
/// thread only.
class GradeGallery : public QObject
{
    Q_OBJECT
public:
    /// @p defaultAlbumName is the display name of the first album while it is not renamed
    /// (project gallery: "Stills"; PowerGrade store: "PowerGrade").
    explicit GradeGallery(QObject* parent = nullptr, const QString& defaultAlbumName = {});

    /// Id of the album that always exists.
    static constexpr quint64 kDefaultAlbum = 1;
    /// "A" to "Z" for memory index 0 to 25.
    [[nodiscard]] static QString memoryLetter(int index);

    [[nodiscard]] const QVector<GalleryStill>& stills() const { return m_stills; }
    /// The still with @p id, or nullptr.
    [[nodiscard]] const GalleryStill* still(quint64 id) const;
    /// Adds a still of @p image (scaled down to kGalleryImageWidth) with @p grade to album
    /// @p album (an unknown one: the default album); returns its id.
    quint64 addStill(const QImage& image, const ClipGrade& grade, quint64 album = kDefaultAlbum);
    /// Adds a copy of @p still (its name, grade, picture and time; a new id) to album @p album.
    quint64 addStillCopy(const GalleryStill& still, quint64 album);
    /// The stills of album @p album, in order.
    [[nodiscard]] QVector<GalleryStill> stillsIn(quint64 album) const;
    /// Moves still @p id to album @p album (false: unknown still or album, or already there).
    bool moveStill(quint64 id, quint64 album);
    /// Copies still @p id into album @p album of this gallery; returns the new id (0: failed).
    quint64 copyStill(quint64 id, quint64 album);
    /// Copies still @p id into album @p album of @p other (the project gallery to the PowerGrade
    /// store and back); returns the new id there (0: failed).
    quint64 copyStillTo(GradeGallery& other, quint64 id, quint64 album) const;
    /// Removes still @p id (false if there is none).
    bool removeStill(quint64 id);
    /// Renames still @p id.
    bool renameStill(quint64 id, const QString& name);

    // ---- Albums ----
    [[nodiscard]] const QVector<GalleryAlbum>& albums() const { return m_albums; }
    [[nodiscard]] const GalleryAlbum* album(quint64 id) const;
    /// Display name of album @p id ("" for an unknown one).
    [[nodiscard]] QString albumName(quint64 id) const;
    /// Adds an empty album called @p name (made unique: "Name 2"); returns its id.
    quint64 addAlbum(const QString& name);
    /// Renames album @p id (an empty name or a name another album has is refused).
    bool renameAlbum(quint64 id, const QString& name);
    /// Deletes album @p id with its stills (never the default album); false if there is none.
    bool removeAlbum(quint64 id);

    // ---- Memories A to Z ----
    [[nodiscard]] const GalleryMemory& memory(int index) const;
    /// Saves @p grade (and @p image, scaled down) into memory @p index (0 = A).
    bool setMemory(int index, const ClipGrade& grade, const QImage& image = {});
    bool clearMemory(int index);
    [[nodiscard]] int usedMemories() const;

    /// Removes every still, album (but the default one, back to its name) and memory.
    void clear();

    /// JSON array of the stills (images as base64 JPEG; "album" omitted for the default one).
    [[nodiscard]] QJsonArray toJson() const;
    /// Replaces the stills with @p json (toJson() form); malformed entries are skipped, stills
    /// of an unknown album go to the default one. Albums and memories stay as they are.
    void fromJson(const QJsonArray& json);
    /// The albums (besides a default one that is not renamed) and the used memories as
    /// {"albums": [{"id", "name"}], "memories": [{"i", "grade", "image"}]}; empty when there is
    /// none.
    [[nodiscard]] QJsonObject extraToJson() const;
    /// Replaces the albums and memories with @p json (extraToJson() form; empty = none); call it
    /// before fromJson() so the stills find their albums.
    void extraFromJson(const QJsonObject& json);
    /// True if there are albums besides the default one, a renamed default one or memories.
    [[nodiscard]] bool hasExtra() const;

Q_SIGNALS:
    /// The list, an album or a memory changed (also emitted by clear(), fromJson() and
    /// extraFromJson()).
    void changed();

private:
    QString m_defaultName;
    QVector<GalleryAlbum> m_albums;
    QVector<GalleryStill> m_stills;
    QVector<GalleryMemory> m_memories;
};

/// Scales @p image to at most kGalleryImageWidth wide (smooth).
[[nodiscard]] QImage reducedGalleryImage(const QImage& image);
/// A picture as base64 JPEG text and back (stills, memories, PowerGrade files).
[[nodiscard]] QString galleryImageToBase64(const QImage& image);
[[nodiscard]] QImage galleryImageFromBase64(const QString& text);

} // namespace kk
