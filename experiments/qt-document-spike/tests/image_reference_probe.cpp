#include "image_reference_probe.h"

#include <QChar>
#include <QUrl>

namespace {
bool isWindowsDeviceName(const QString &component)
{
    const QString base = component.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    if (base == QStringLiteral("CON") || base == QStringLiteral("PRN")
        || base == QStringLiteral("AUX") || base == QStringLiteral("NUL"))
        return true;
    if (base.size() != 4 || (!base.startsWith(QStringLiteral("COM"))
                             && !base.startsWith(QStringLiteral("LPT"))))
        return false;
    const QChar suffix = base.at(3);
    return (suffix >= QLatin1Char('1') && suffix <= QLatin1Char('9'))
        || suffix == QChar(0x00b9) || suffix == QChar(0x00b2) || suffix == QChar(0x00b3);
}
} // namespace

std::optional<QStringList> candidateImageReference(const QByteArray &encodedReference)
{
    const QUrl url = QUrl::fromEncoded(encodedReference, QUrl::StrictMode);
    if (!url.isValid() || !url.isRelative() || !url.scheme().isEmpty()
        || !url.authority().isEmpty() || url.hasQuery() || url.hasFragment())
        return std::nullopt;

    // FullyDecoded can silently replace invalid UTF-8. Decode the encoded path
    // exactly once and reject any byte sequence that does not round-trip.
    const QByteArray pathBytes = QByteArray::fromPercentEncoding(url.path(QUrl::FullyEncoded).toLatin1());
    const QString path = QString::fromUtf8(pathBytes.constData(), pathBytes.size());
    if (path.isEmpty() || path.toUtf8() != pathBytes || path.contains(QLatin1Char('\\')))
        return std::nullopt;

    QStringList components;
    for (const QString &part : path.split(QLatin1Char('/'), Qt::KeepEmptyParts)) {
        if (part.isEmpty() || part == QStringLiteral(".."))
            return std::nullopt;
        if (part == QStringLiteral("."))
            continue;
        if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))
            || isWindowsDeviceName(part))
            return std::nullopt;
        for (QChar character : part) {
            if (character.category() == QChar::Other_Control
                || QStringLiteral("<>:\"|?*").contains(character))
                return std::nullopt;
        }
        components.append(part);
    }
    if (components.isEmpty())
        return std::nullopt;
    return components;
}
