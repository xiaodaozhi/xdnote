#pragma once

#include <QByteArray>
#include <QString>
#include <memory>
#include <functional>

namespace xdnote {
enum class WorkspaceReadError {
    None, InvalidPath, Missing, Denied, Unsupported, OutsideOrLink,
    RootChanged, Revoked, TooLarge, Changed, Io
};

struct WorkspaceReadResult {
    WorkspaceReadError error = WorkspaceReadError::None;
    QByteArray bytes;
    qint64 nativeCode = 0;
    QString identity;
    bool ok() const { return error == WorkspaceReadError::None; }
};

enum class WorkspaceWriteStatus { Committed, Reused, Failed, PublishedButUnverified };
enum class WorkspaceWriteStage { BeforeCommit, AfterCommit };
struct WorkspaceWriteResult {
    WorkspaceWriteStatus status = WorkspaceWriteStatus::Failed;
    WorkspaceReadError error = WorkspaceReadError::None;
    qint64 nativeCode = 0;
    // Root-relative published path, if a file was committed or reused.
    QString path;
    // Only an uncommitted, owned temporary entry whose cleanup failed.
    QString recoveryPath;
    // Keeps the verified target, document and their directory handles stable.
    // Retain until the caller has rechecked the session and inserted the link.
    std::shared_ptr<void> lease;
};

class WorkspaceResourceReader;
struct WorkspaceRootResult {
    std::shared_ptr<WorkspaceResourceReader> reader;
    WorkspaceReadError error = WorkspaceReadError::None;
    qint64 nativeCode = 0;
};

// A root is bound to the actual directory selected by the user. A lexical
// path is never itself authorization. Call revoke when the workspace closes
// or its identity changes; every consumer must also check its own generation.
// This lower-level reader does not decode images or grant WebEngine access.
class WorkspaceResourceReader final {
public:
    static WorkspaceRootResult open(const QString &absoluteRoot);
    ~WorkspaceResourceReader();
    WorkspaceResourceReader(const WorkspaceResourceReader &) = delete;
    WorkspaceResourceReader &operator=(const WorkspaceResourceReader &) = delete;
    WorkspaceReadResult read(const QString &rootRelativePath, qsizetype maxBytes) const;
    // Publishes an already validated, bounded byte snapshot under assets/images.
    // This method does not decode images or edit a Markdown document. A caller
    // must recheck the document session/revision before inserting a reference.
    WorkspaceWriteResult publishImage(const QString &documentPath, const QString &documentIdentity,
                                      const QByteArray &bytes, const QString &suggestedName, const QString &extension,
                                      const std::function<void(WorkspaceWriteStage)> &observer = {}) const;
    void revoke();
private:
    struct State;
    explicit WorkspaceResourceReader(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
};
}
