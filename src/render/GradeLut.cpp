// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/GradeLut.h"

#include "render/GradeEval.h"

#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <vector>

namespace kk::render {

QByteArray gradeToCube(const ClipGrade& grade, int64_t frame, int size, const QString& title)
{
    size = std::clamp(size, 2, kMaxGradeLutSize);
    // A LUT is a colour mapping: drop what depends on where the pixel is or on its neighbours.
    ClipGrade flat = grade;
    for (GradeNode& n : flat.nodes) {
        n.windows.clear();
        n.qualifier.enabled = false;
    }
    const PreparedGrade prepared = prepareGrade(flat, frame, grademath::PictureGeometry {});
    QByteArray out;
    out.reserve(size * size * size * 30 + 128);
    QString safeTitle = title;
    safeTitle.remove(QLatin1Char('"')).remove(QLatin1Char('\n'));
    out += "TITLE \"" + safeTitle.toUtf8() + "\"\n";
    out += "LUT_3D_SIZE " + QByteArray::number(size) + "\n";
    out += "DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n";
    std::vector<float> scratch;
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                float c[3] = {float(r) / float(size - 1), float(g) / float(size - 1),
                    float(b) / float(size - 1)};
                gradePixel(prepared, c, 0.5f, 0.5f, scratch);
                for (float& v : c)
                    v = std::clamp(v, 0.0f, 1.0f);
                out += QByteArray::number(double(c[0]), 'f', 6) + ' '
                    + QByteArray::number(double(c[1]), 'f', 6) + ' '
                    + QByteArray::number(double(c[2]), 'f', 6) + '\n';
            }
        }
    }
    return out;
}

bool writeGradeCube(const ClipGrade& grade, int64_t frame, int size, const QString& title,
    const QString& path, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(gradeToCube(grade, frame, size, title));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace kk::render
