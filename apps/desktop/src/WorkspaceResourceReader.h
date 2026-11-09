#pragma once

#include <QByteArray>
#include <QString>
#include <memory>

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
    void revoke();
private:
    struct State;
    explicit WorkspaceResourceReader(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
};
}
