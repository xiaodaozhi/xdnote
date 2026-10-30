#include "spike_document.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QStringConverter>

namespace {
const QByteArray utf8Bom = QByteArray::fromHex("efbbbf");

bool readBytes(const QString &path, QByteArray *bytes, QString *error)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        *error = input.errorString();
        return false;
    }
    *bytes = input.readAll();
    if (input.error() != QFileDevice::NoError) {
        *error = input.errorString();
        return false;
    }
    return true;
}

bool sourceText(const QTextDocument &document, QString *text, QString *error)
{
    QString raw = document.toRawText();
    if (raw.contains(QChar(0x2028))) {
        *error = QStringLiteral("Unsupported Unicode line separator in editor");
        return false;
    }
    // QTextDocument represents ordinary paragraph breaks with U+2029.
    // Existing U+2029 input is rejected on open; insertion during editing is
    // indistinguishable and remains an explicit experiment limitation.
    raw.replace(QChar(0x2029), QChar('\n'));
    *text = raw;
    return true;
}
} // namespace

bool SpikeFile::open(const QString &path, QString *text, QString *error)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        *error = QStringLiteral("Expected an existing regular, non-symlink file");
        return false;
    }

    QByteArray bytes;
    if (!readBytes(info.absoluteFilePath(), &bytes, error))
        return false;

    QByteArray body = bytes;
    const bool bom = body.startsWith(utf8Bom);
    if (bom)
        body.remove(0, utf8Bom.size());

    const bool hasCrlf = body.contains("\r\n");
    QByteArray withoutPairs = body;
    withoutPairs.replace("\r\n", "");
    if (withoutPairs.contains('\r') || (hasCrlf && withoutPairs.contains('\n'))) {
        *error = QStringLiteral("Mixed or unsupported line endings");
        return false;
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    QString decoded = decoder(body);
    if (decoder.hasError()) {
        *error = QStringLiteral("Invalid UTF-8");
        return false;
    }
    decoded.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (decoded.contains(QChar(0x2028)) || decoded.contains(QChar(0x2029))) {
        *error = QStringLiteral("Unsupported Unicode line or paragraph separator");
        return false;
    }

    m_path = info.absoluteFilePath();
    m_baseline = bytes;
    m_bom = bom;
    m_crlf = hasCrlf;
    *text = decoded;
    error->clear();
    return true;
}

SpikeFile::SaveOutcome SpikeFile::checkBaseline(QString *error) const
{
    const QFileInfo info(m_path);
    if (m_path.isEmpty() || !info.exists() || !info.isFile() || info.isSymLink()) {
        *error = QStringLiteral("Target removed, replaced or no longer a regular file");
        return SaveOutcome::Conflict;
    }

    QByteArray current;
    if (!readBytes(m_path, &current, error))
        return SaveOutcome::FailedBeforeCommit;
    if (current != m_baseline) {
        *error = QStringLiteral("Target bytes changed since opening");
        return SaveOutcome::Conflict;
    }
    return SaveOutcome::Saved; // Baseline matches; no commit has occurred yet.
}

SpikeFile::SaveOutcome SpikeFile::save(const QString &text,
                                       const std::function<void()> &beforeFinalCheck,
                                       QString *error)
{
    if (text.contains(QChar('\r')) || text.contains(QChar(0x2028))
        || text.contains(QChar(0x2029))) {
        *error = QStringLiteral("Unsupported line separator in editor text");
        return SaveOutcome::FailedBeforeCommit;
    }

    const SaveOutcome initial = checkBaseline(error);
    if (initial != SaveOutcome::Saved)
        return initial;

    QByteArray encoded = text.toUtf8();
    if (m_crlf)
        encoded.replace("\n", "\r\n");
    if (m_bom)
        encoded.prepend(utf8Bom);

    QSaveFile output(m_path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        *error = output.errorString();
        return SaveOutcome::FailedBeforeCommit;
    }
    if (output.write(encoded) != encoded.size()) {
        *error = output.errorString();
        output.cancelWriting();
        return SaveOutcome::FailedBeforeCommit;
    }

    // Tests use this hook to make a real external change before the last check.
    if (beforeFinalCheck)
        beforeFinalCheck();
    const SaveOutcome checked = checkBaseline(error);
    if (checked != SaveOutcome::Saved) {
        output.cancelWriting();
        return checked;
    }
    if (!output.commit()) {
        *error = output.errorString();
        return SaveOutcome::CommitUncertain;
    }

    m_baseline = encoded;
    error->clear();
    return SaveOutcome::Saved;
}

bool SpikeSession::open(const QString &path, QString *error)
{
    QString text;
    if (!m_file.open(path, &text, error))
        return false;
    m_document.setPlainText(text);
    m_document.setModified(false);
    m_savedText = text;
    return true;
}

SpikeFile::SaveOutcome SpikeSession::save(const std::function<void()> &beforeFinalCheck,
                                         QString *error)
{
    QString snapshot;
    if (!sourceText(m_document, &snapshot, error))
        return SpikeFile::SaveOutcome::FailedBeforeCommit;
    const auto outcome = m_file.save(snapshot, beforeFinalCheck, error);
    if (outcome == SpikeFile::SaveOutcome::Saved)
        m_savedText = snapshot;
    return outcome;
}

bool SpikeSession::isDirty() const
{
    QString current;
    QString error;
    return !sourceText(m_document, &current, &error) || current != m_savedText;
}

QTextDocument *SpikeSession::document()
{
    return &m_document;
}

void RestrictedPreview::render(const QString &markdown)
{
    m_requestedResources.clear();
    const auto features = QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
        | QTextDocument::MarkdownNoHTML;
    setMarkdown(markdown, features);
}

QStringList RestrictedPreview::requestedResources() const
{
    return m_requestedResources;
}

QVariant RestrictedPreview::loadResource(int type, const QUrl &name)
{
    m_requestedResources.append(name.toString());
    if (type == QTextDocument::ImageResource) {
        QImage placeholder(1, 1, QImage::Format_ARGB32_Premultiplied);
        placeholder.fill(Qt::transparent);
        return placeholder;
    }
    // Return a valid placeholder instead of delegating to Qt's default loader.
    return QByteArray("blocked");
}
