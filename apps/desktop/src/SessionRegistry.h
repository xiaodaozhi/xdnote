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

// File identity is unique within a window scope. Each registered session owns
// its original text and undo history; other windows may edit the same file.
class SessionRegistry final : public QObject {
public:
    explicit SessionRegistry(QObject *parent = nullptr) : QObject(parent) {}
    RegisteredSession lookup(const std::shared_ptr<const FileProbe> &file, const QUuid &scope = {}) const;
    OpenRegistration beginOpen(const std::shared_ptr<const FileProbe> &file, const QUuid &scope = {});
    RegisteredSession complete(const FileReadRequest &request, const std::shared_ptr<const FileBaseline> &baseline);
    RegistryResult cancel(const FileReadRequest &request);
    RegistryResult rebind(DocumentSession *session, const std::shared_ptr<const FileBaseline> &baseline);
    RegistryResult setScope(DocumentSession *session, const QUuid &scope);
private:
    bool onGuiThread() const;
    struct Entry {
        std::shared_ptr<const FileBaseline> baseline;
        QPointer<DocumentSession> session;
        FileReadRequest request;
        QUuid documentId;
        QUuid scope;
    };
    QHash<QString, Entry> m_sessions;
};
}
