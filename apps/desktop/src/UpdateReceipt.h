#pragma once
#include <QString>
namespace xdnote {
// Debian packages move the running image to a new immutable version directory.
// An authenticated helper result and the new compiled identity are still required.
inline bool updateReceiptPaths(const QString &platform, const QString &oldRoot, const QString &oldExecutable,
                               const QString &runningRoot, const QString &runningExecutable) {
    if (platform != "linux") return oldRoot == runningRoot && oldExecutable == runningExecutable;
    const auto versioned = [](const QString &root, const QString &executable) {
        const QString prefix = "/opt/xdnote/versions/";
        if (!root.startsWith(prefix) || root.size() == prefix.size()) return false;
        for (const auto c : QStringView(root).sliced(prefix.size()))
            if (!((c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') || c == u'-')) return false;
        return executable == root + "/bin/xdnote";
    };
    return versioned(oldRoot, oldExecutable) && versioned(runningRoot, runningExecutable);
}
}
