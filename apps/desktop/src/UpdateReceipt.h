#pragma once
#include <QDir>
#include <QFileInfo>
#include <QString>
namespace xdnote {
// Debian packages move the running image to a new immutable version directory.
// An authenticated helper result and the new compiled identity are still required.
inline bool updateReceiptPaths(const QString &platform, const QString &oldRoot, const QString &oldExecutable,
                               const QString &runningRoot, const QString &runningExecutable) {
    if (platform != "linux") return oldRoot == runningRoot && oldExecutable == runningExecutable;
    const auto versioned = [](const QString &root, const QString &executable) {
        return QDir::isAbsolutePath(root) && QDir::cleanPath(root) == root &&
            QFileInfo(root).absolutePath() == "/opt/xdnote/versions" && executable == root + "/bin/xdnote";
    };
    return versioned(oldRoot, oldExecutable) && versioned(runningRoot, runningExecutable);
}
}
