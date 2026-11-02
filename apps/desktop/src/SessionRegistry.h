#pragma once

#include "DocumentSession.h"
#include "FileService.h"
#include <QHash>

namespace xdnote {
enum class RegistryResult { Created, Reused, Pending, NotRegistered, Closing, WrongThread, InvalidBaseline, StaleRequest, Cancelled, TargetInUse };
struct RegisteredSession {
    RegistryResult result = RegistryResult::NotRegistered;
    QPointer<DocumentSession> session;
};
struct OpenRegistration {
    RegistryResult result = RegistryResult::NotRegistered;
    QPointer<DocumentSession> session;
    FileReadRequest read;
    bool startRead = false;
};

// One application registry, owned and accessed by the GUI thread. File I/O is
// deliberately separate so duplicate opens need not read disk text at all.
class SessionRegistry final : public QObject {
public:
    explicit SessionRegistry(QObject *parent = nullptr) : QObject(parent) {}
    RegisteredSession lookup(const std::shared_ptr<const FileProbe> &file) const;
    OpenRegistration beginOpen(const std::shared_ptr<const FileProbe> &file);
    RegisteredSession complete(const FileReadRequest &request, const std::shared_ptr<const FileBaseline> &baseline);
    RegistryResult cancel(const FileReadRequest &request);
    RegistryResult rebind(DocumentSession *session, const std::shared_ptr<const FileBaseline> &baseline);
private:
    bool onGuiThread() const;
    struct Entry {
        std::shared_ptr<const FileBaseline> baseline;
        QPointer<DocumentSession> session;
        FileReadRequest request;
        QUuid documentId;
    };
    QHash<QString, Entry> m_sessions;
};
}
