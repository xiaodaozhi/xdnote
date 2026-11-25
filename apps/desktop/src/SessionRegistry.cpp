#include "SessionRegistry.h"
#include <QCoreApplication>
#include <QThread>
#include <QDir>

namespace xdnote {
namespace {
QString registrationKey(const QUuid &scope, const QString &identity) {
    return scope.toString(QUuid::WithoutBraces) + '/' + identity;
}
bool samePath(const QString &a, const QString &b) {
#ifdef Q_OS_WIN
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(a) == QDir::cleanPath(b);
#endif
}
}
static_assert(FileService::SourceLimit == DocumentSession::AdmissionSourceLimit);
bool SessionRegistry::onGuiThread() const {
    return QCoreApplication::instance() && QThread::currentThread() == thread()
        && thread() == QCoreApplication::instance()->thread();
}

RegisteredSession SessionRegistry::lookup(const std::shared_ptr<const FileProbe> &file, const QUuid &scope) const {
    if (!onGuiThread()) return {RegistryResult::WrongThread, {}};
    if (!file) return {RegistryResult::InvalidBaseline, {}};
    auto found = m_sessions.constFind(registrationKey(scope, file->identity()));
    // A save in another window can replace the on-disk identity. The existing
    // tab still owns this path and its unsaved text must not be read over.
    if (found == m_sessions.cend()) for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
        if (it->scope == scope && samePath(it->request.file->path(), file->path())) { found = it; break; }
    if (found == m_sessions.cend()) return {};
    if (!found->session && !found->baseline) return {RegistryResult::Pending, {}};
    if (!found->session || found->session->isClosing()) return {RegistryResult::Closing, {}};
    return {RegistryResult::Reused, found->session};
}

OpenRegistration SessionRegistry::beginOpen(const std::shared_ptr<const FileProbe> &file, const QUuid &scope) {
    const auto existing = lookup(file, scope);
    if (existing.result == RegistryResult::Pending) {
        for (const auto &entry : m_sessions) if (entry.scope == scope
            && (entry.request.file->identity() == file->identity() || samePath(entry.request.file->path(), file->path())))
            return {RegistryResult::Pending, {}, entry.request};
    }
    if (existing.result != RegistryResult::NotRegistered) return {existing.result, existing.session, {}};
    FileReadRequest request{file};
    m_sessions.insert(registrationKey(scope, file->identity()), {{}, {}, request, {}, scope});
    return {RegistryResult::Pending, {}, request, true};
}

RegistryResult SessionRegistry::cancel(const FileReadRequest &request) {
    if (!onGuiThread()) return RegistryResult::WrongThread;
    if (!request.file) return RegistryResult::InvalidBaseline;
    auto found = m_sessions.begin();
    while (found != m_sessions.end() && found->request.requestId != request.requestId) ++found;
    if (found == m_sessions.end() || found->request.file->identity() != request.file->identity()) return RegistryResult::StaleRequest;
    if (found->session || found->baseline) return found->session && !found->session->isClosing()
        ? RegistryResult::Reused : RegistryResult::Closing;
    m_sessions.erase(found);
    return RegistryResult::Cancelled;
}

RegisteredSession SessionRegistry::complete(const FileReadRequest &request, const std::shared_ptr<const FileBaseline> &baseline) {
    if (!onGuiThread()) return {RegistryResult::WrongThread, {}};
    if (!request.file || !baseline || baseline->requestId() != request.requestId
        || baseline->file()->identity() != request.file->identity()) return {RegistryResult::InvalidBaseline, {}};
    auto found = m_sessions.begin();
    while (found != m_sessions.end() && found->request.requestId != request.requestId) ++found;
    if (found == m_sessions.end() || found->request.file->identity() != request.file->identity()) return {RegistryResult::StaleRequest, {}};
    if (found->session || found->baseline) return lookup(request.file, found->scope);
    auto *session = new DocumentSession(baseline->utf8(), this);
    found->baseline = baseline;
    found->session = session;
    found->documentId = session->documentId();
    const QPointer<DocumentSession> guard(session);
    const auto documentId = session->documentId();
    // A new request may already occupy this key when deferred destruction runs.
    connect(session, &DocumentSession::closed, this, [this, guard, documentId] {
        for (auto entry = m_sessions.begin(); entry != m_sessions.end(); ++entry)
            if (entry->documentId == documentId) { m_sessions.erase(entry); break; }
        if (guard) guard->deleteLater();
    }, Qt::QueuedConnection);
    connect(session, &QObject::destroyed, this, [this, documentId] {
        for (auto entry = m_sessions.begin(); entry != m_sessions.end(); ++entry)
            if (entry->documentId == documentId) { m_sessions.erase(entry); break; }
    });
    return {RegistryResult::Created, session};
}

RegistryResult SessionRegistry::rebind(DocumentSession *session, const std::shared_ptr<const FileBaseline> &baseline) {
    if (!onGuiThread()) return RegistryResult::WrongThread;
    if (!session || session->isClosing()) return RegistryResult::Closing;
    if (!baseline) return RegistryResult::InvalidBaseline;
    for (auto old = m_sessions.begin(); old != m_sessions.end(); ++old) {
        if (old->session != session) continue;
        const auto target = lookup(baseline->file(), old->scope);
        if (target.result != RegistryResult::NotRegistered && target.session != session) return RegistryResult::TargetInUse;
        auto entry = old.value();
        m_sessions.erase(old);
        entry.baseline = baseline;
        entry.request = {baseline->file(), baseline->requestId()};
        m_sessions.insert(registrationKey(entry.scope, baseline->file()->identity()), entry);
        return RegistryResult::Reused;
    }
    return RegistryResult::NotRegistered;
}
RegistryResult SessionRegistry::setScope(DocumentSession *session, const QUuid &scope) {
    if (!onGuiThread()) return RegistryResult::WrongThread;
    if (!session || session->isClosing()) return RegistryResult::Closing;
    for (auto old = m_sessions.begin(); old != m_sessions.end(); ++old) {
        if (old->session != session) continue;
        if (old->scope == scope) return RegistryResult::Reused;
        if (lookup(old->baseline->file(), scope).result != RegistryResult::NotRegistered) return RegistryResult::TargetInUse;
        auto entry = old.value(); m_sessions.erase(old); entry.scope = scope;
        m_sessions.insert(registrationKey(scope, entry.baseline->file()->identity()), entry);
        return RegistryResult::Reused;
    }
    return RegistryResult::NotRegistered;
}
}
