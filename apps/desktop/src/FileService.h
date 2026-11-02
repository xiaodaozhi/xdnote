#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>
#include <memory>

namespace xdnote {
struct NativeFile;

enum class FileError { None, InvalidPath, Missing, Denied, InUse, UnsupportedFile, TooLarge, InvalidUtf8, UnsupportedContent, Changed, Io };
struct FileProblem {
    FileError code = FileError::None;
    qint64 nativeCode = 0;
    QString description() const;
};

// Explicitly selected files only. This is not a workspace/resource capability.
// The lease keeps the actual object alive while its identity is registered.
class FileProbe final {
public:
    const QString &path() const { return m_path; }
    const QString &identity() const { return m_identity; }
private:
    friend class FileService;
    FileProbe(QString path, QString identity, std::shared_ptr<NativeFile> lease);
    QString m_path;
    QString m_identity;
    std::shared_ptr<NativeFile> m_lease;
};

class FileBaseline final {
public:
    const std::shared_ptr<const FileProbe> &file() const { return m_file; }
    const QByteArray &utf8() const { return m_utf8; }
    const QByteArray &diskBytes() const { return m_diskBytes; }
    const QByteArray &sha256() const { return m_sha256; }
    bool hasUtf8Bom() const { return m_bom; }
    const QUuid &requestId() const { return m_requestId; }
private:
    friend class FileService;
    FileBaseline(std::shared_ptr<const FileProbe> file, QUuid requestId, QByteArray disk, QByteArray utf8, bool bom);
    std::shared_ptr<const FileProbe> m_file;
    QByteArray m_utf8;
    QByteArray m_diskBytes;
    QByteArray m_sha256;
    bool m_bom;
    QUuid m_requestId;
};

struct FileReadRequest {
    std::shared_ptr<const FileProbe> file;
    QUuid requestId = QUuid::createUuid();
};

struct ProbeResult {
    std::shared_ptr<const FileProbe> file;
    FileProblem problem;
};
struct ReadResult {
    std::shared_ptr<const FileBaseline> baseline;
    FileProblem problem;
};

// Blocking, bounded worker operations; no QObject or active editor access.
// Probe -> GUI registry beginOpen -> read only if new -> GUI complete.
class FileService final {
public:
    static constexpr qsizetype SourceLimit = 2 * 1024 * 1024;
    static ProbeResult probe(const QString &absolutePath);
    static ReadResult read(const FileReadRequest &request);
};
}
