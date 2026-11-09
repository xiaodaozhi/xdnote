#pragma once
#include <QString>

namespace xdnote {
struct WorkspacePathResult {
    bool success = false;
    QString path;
    bool hasFragment = false;
    QString fragment;
    QString error;
};

// Both paths are relative to the workspace root. Results are lexical only:
// callers must authorize and check the actual filesystem object separately.
WorkspacePathResult resolveWorkspaceTarget(const QString &document, const QString &target);
WorkspacePathResult relativeWorkspaceUrl(const QString &document, const QString &destination);
}
