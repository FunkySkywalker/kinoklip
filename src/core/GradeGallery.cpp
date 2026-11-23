// SPDX-License-Identifier: GPL-3.0-or-later
// GradeGallery — stills, albums and memories, and their JSON forms.
#include "core/GradeGallery.h"

#include "core/Serialization.h"

#include <QBuffer>
#include <QDateTime>
#include <QJsonObject>

#include <algorithm>

namespace kk {

namespace {
/// JPEG quality of stored stills (keeps project files small).
constexpr int kJpegQuality = 85;
/// Memories keep a small picture only (their tile in the gallery).
constexpr int kMemoryImageWidth = 160;
} // namespace

QImage reducedGalleryImage(const QImage& image)
{
    return image.width() > kGalleryImageWidth
        ? image.scaledToWidth(kGalleryImageWidth, Qt::SmoothTransformation)
        : image;
}

QString galleryImageToBase64(const QImage& image)
{
    if (image.isNull())
        return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPG", kJpegQuality);
    return QString::fromLatin1(bytes.toBase64());
}

QImage galleryImageFromBase64(const QString& text)
{
    QImage image;
    const QByteArray bytes = QByteArray::fromBase64(text.toLatin1());
    if (!bytes.isEmpty())
        image.loadFromData(bytes, "JPG");
    return image;
}

GradeGallery::GradeGallery(QObject* parent, const QString& defaultAlbumName)
    : QObject(parent)
    , m_defaultName(defaultAlbumName.isEmpty() ? tr("Stills") : defaultAlbumName)
    , m_memories(kGalleryMemoryCount)
{
    m_albums.append({kDefaultAlbum, m_defaultName});
}

QString GradeGallery::memoryLetter(int index)
{
    return index >= 0 && index < kGalleryMemoryCount ? QString(QChar(u'A' + index)) : QString();
}

const GalleryStill* GradeGallery::still(quint64 id) const
{
    for (const GalleryStill& s : m_stills) {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

quint64 GradeGallery::addStill(const QImage& image, const ClipGrade& grade, quint64 album)
{
    GalleryStill s;
    s.album = album;
    s.createdMs = QDateTime::currentMSecsSinceEpoch();
    s.grade = grade;
    s.image = image;
    return addStillCopy(s, album);
}

quint64 GradeGallery::addStillCopy(const GalleryStill& still, quint64 album)
{
    GalleryStill s = still;
    s.id = 0;
    for (const GalleryStill& other : std::as_const(m_stills))
        s.id = std::max(s.id, other.id);
    ++s.id;
    s.album = this->album(album) ? album : kDefaultAlbum;
    if (s.name.isEmpty())
        s.name = QStringLiteral("1.%1").arg(s.id);
    if (s.createdMs == 0)
        s.createdMs = QDateTime::currentMSecsSinceEpoch();
    s.image = reducedGalleryImage(s.image);
    m_stills.append(s);
    emit changed();
    return s.id;
}

QVector<GalleryStill> GradeGallery::stillsIn(quint64 album) const
{
    QVector<GalleryStill> list;
    for (const GalleryStill& s : m_stills) {
        if (s.album == album)
            list.append(s);
    }
    return list;
}

bool GradeGallery::moveStill(quint64 id, quint64 album)
{
    if (!this->album(album))
        return false;
    for (GalleryStill& s : m_stills) {
        if (s.id == id) {
            if (s.album == album)
                return false;
            s.album = album;
            emit changed();
            return true;
        }
    }
    return false;
}

quint64 GradeGallery::copyStill(quint64 id, quint64 album)
{
    return copyStillTo(*this, id, album);
}

quint64 GradeGallery::copyStillTo(GradeGallery& other, quint64 id, quint64 album) const
{
    const GalleryStill* s = still(id);
    if (!s || !other.album(album))
        return 0;
    GalleryStill copy = *s;
    // The name is kept (a copy of "1.3" is "1.3" again), numbering only fills empty names.
    return other.addStillCopy(copy, album);
}

bool GradeGallery::removeStill(quint64 id)
{
    const qsizetype n = m_stills.removeIf([id](const GalleryStill& s) { return s.id == id; });
    if (n > 0)
        emit changed();
    return n > 0;
}

bool GradeGallery::renameStill(quint64 id, const QString& name)
{
    for (GalleryStill& s : m_stills) {
        if (s.id == id && s.name != name) {
            s.name = name;
            emit changed();
            return true;
        }
    }
    return false;
}

// ---- Albums ----

const GalleryAlbum* GradeGallery::album(quint64 id) const
{
    for (const GalleryAlbum& a : m_albums) {
        if (a.id == id)
            return &a;
    }
    return nullptr;
}

QString GradeGallery::albumName(quint64 id) const
{
    const GalleryAlbum* a = album(id);
    return a ? a->name : QString();
}

quint64 GradeGallery::addAlbum(const QString& name)
{
    QString base = name.trimmed();
    if (base.isEmpty())
        base = tr("Album");
    auto taken = [this](const QString& n) {
        return std::any_of(m_albums.cbegin(), m_albums.cend(),
            [&n](const GalleryAlbum& a) { return a.name.compare(n, Qt::CaseInsensitive) == 0; });
    };
    QString unique = base;
    for (int n = 2; taken(unique); ++n)
        unique = QStringLiteral("%1 %2").arg(base).arg(n);
    quint64 id = 0;
    for (const GalleryAlbum& a : std::as_const(m_albums))
        id = std::max(id, a.id);
    m_albums.append({id + 1, unique});
    emit changed();
    return id + 1;
}

bool GradeGallery::renameAlbum(quint64 id, const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return false;
    for (GalleryAlbum& a : m_albums) {
        if (a.id == id) {
            if (a.name == trimmed)
                return false;
            for (const GalleryAlbum& other : std::as_const(m_albums)) {
                if (other.id != id && other.name.compare(trimmed, Qt::CaseInsensitive) == 0)
                    return false;
            }
            a.name = trimmed;
            emit changed();
            return true;
        }
    }
    return false;
}

bool GradeGallery::removeAlbum(quint64 id)
{
    if (id == kDefaultAlbum || !album(id))
        return false;
    m_stills.removeIf([id](const GalleryStill& s) { return s.album == id; });
    m_albums.removeIf([id](const GalleryAlbum& a) { return a.id == id; });
    emit changed();
    return true;
}

// ---- Memories ----

const GalleryMemory& GradeGallery::memory(int index) const
{
    static const GalleryMemory none;
    return index >= 0 && index < m_memories.size() ? m_memories[index] : none;
}

bool GradeGallery::setMemory(int index, const ClipGrade& grade, const QImage& image)
{
    if (index < 0 || index >= m_memories.size())
        return false;
    GalleryMemory& m = m_memories[index];
    m.used = true;
    m.grade = grade;
    m.image = image.width() > kMemoryImageWidth
        ? image.scaledToWidth(kMemoryImageWidth, Qt::SmoothTransformation)
        : image;
    emit changed();
    return true;
}

bool GradeGallery::clearMemory(int index)
{
    if (index < 0 || index >= m_memories.size() || !m_memories[index].used)
        return false;
    m_memories[index] = {};
    emit changed();
    return true;
}

int GradeGallery::usedMemories() const
{
    return int(std::count_if(
        m_memories.cbegin(), m_memories.cend(), [](const GalleryMemory& m) { return m.used; }));
}

void GradeGallery::clear()
{
    const bool wasEmpty = m_stills.isEmpty() && !hasExtra();
    m_stills.clear();
    m_albums = {{kDefaultAlbum, m_defaultName}};
    m_memories = QVector<GalleryMemory>(kGalleryMemoryCount);
    if (!wasEmpty)
        emit changed();
}

// ---- JSON ----

QJsonArray GradeGallery::toJson() const
{
    QJsonArray list;
    for (const GalleryStill& s : m_stills) {
        QJsonObject o {{QLatin1String("id"), qint64(s.id)}, {QLatin1String("name"), s.name},
            {QLatin1String("created"), s.createdMs},
            {QLatin1String("grade"), serialization::gradeToJson(s.grade)}};
        if (s.album != kDefaultAlbum)
            o[QLatin1String("album")] = qint64(s.album);
        if (!s.image.isNull())
            o[QLatin1String("image")] = galleryImageToBase64(s.image);
        list.append(o);
    }
    return list;
}

void GradeGallery::fromJson(const QJsonArray& json)
{
    m_stills.clear();
    for (const QJsonValue& v : json) {
        const QJsonObject o = v.toObject();
        GalleryStill s;
        s.id = quint64(o.value(QLatin1String("id")).toInteger());
        if (s.id == 0 || still(s.id))
            continue;
        s.name = o.value(QLatin1String("name")).toString();
        s.createdMs = o.value(QLatin1String("created")).toInteger();
        s.grade = serialization::gradeFromJson(o.value(QLatin1String("grade")).toObject());
        s.image = galleryImageFromBase64(o.value(QLatin1String("image")).toString());
        const quint64 album = quint64(o.value(QLatin1String("album")).toInteger(kDefaultAlbum));
        s.album = this->album(album) ? album : kDefaultAlbum;
        m_stills.append(s);
    }
    emit changed();
}

bool GradeGallery::hasExtra() const
{
    return m_albums.size() > 1 || m_albums.first().name != m_defaultName || usedMemories() > 0;
}

QJsonObject GradeGallery::extraToJson() const
{
    if (!hasExtra())
        return {};
    QJsonArray albums;
    for (const GalleryAlbum& a : m_albums) {
        if (a.id == kDefaultAlbum && a.name == m_defaultName)
            continue;
        albums.append(QJsonObject {{QLatin1String("id"), qint64(a.id)}, {QLatin1String("name"), a.name}});
    }
    QJsonArray memories;
    for (int i = 0; i < m_memories.size(); ++i) {
        const GalleryMemory& m = m_memories[i];
        if (!m.used)
            continue;
        QJsonObject o {{QLatin1String("i"), i}, {QLatin1String("grade"), serialization::gradeToJson(m.grade)}};
        if (!m.image.isNull())
            o[QLatin1String("image")] = galleryImageToBase64(m.image);
        memories.append(o);
    }
    QJsonObject root;
    if (!albums.isEmpty())
        root[QLatin1String("albums")] = albums;
    if (!memories.isEmpty())
        root[QLatin1String("memories")] = memories;
    return root;
}

void GradeGallery::extraFromJson(const QJsonObject& json)
{
    m_albums = {{kDefaultAlbum, m_defaultName}};
    for (const QJsonValue& v : json.value(QLatin1String("albums")).toArray()) {
        const QJsonObject o = v.toObject();
        const quint64 id = quint64(o.value(QLatin1String("id")).toInteger());
        const QString name = o.value(QLatin1String("name")).toString().trimmed();
        if (id == 0 || name.isEmpty())
            continue;
        if (id == kDefaultAlbum) {
            m_albums[0].name = name;
        } else if (!album(id)) {
            m_albums.append({id, name});
        }
    }
    m_memories = QVector<GalleryMemory>(kGalleryMemoryCount);
    for (const QJsonValue& v : json.value(QLatin1String("memories")).toArray()) {
        const QJsonObject o = v.toObject();
        const int i = o.value(QLatin1String("i")).toInt(-1);
        if (i < 0 || i >= kGalleryMemoryCount)
            continue;
        GalleryMemory& m = m_memories[i];
        m.used = true;
        m.grade = serialization::gradeFromJson(o.value(QLatin1String("grade")).toObject());
        m.image = galleryImageFromBase64(o.value(QLatin1String("image")).toString());
    }
    emit changed();
}

} // namespace kk
