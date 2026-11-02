#include "SessionRegistry.h"
#include <QCoreApplication>
#include <QThread>

namespace xdnote {
static_assert(FileService::SourceLimit == DocumentSession::AdmissionSourceLimit);
bool SessionRegistry::onGuiThread() const {
    return QCoreApplication::instance() && QThread::currentThread() == thread()
        && thread() == QCoreApplication::instance()->thread();
}

RegisteredSession SessionRegistry::lookup(const std::shared_ptr<const FileProbe> &file) const {
    if (!onGuiThread()) return {RegistryResult::WrongThread, {}};
    if (!file) return {RegistryResult::InvalidBaseline, {}};
    const auto found = m_sessions.constFind(file->identity());
    if (found == m_sessions.cend()) return {};
    if (!found->session && !found->baseline) return {RegistryResult::Pending, {}};
    if (!found->session || found->session->isClosing()) return {RegistryResult::Closing, {}};
    return {RegistryResult::Reused, found->session};
}

OpenRegistration SessionRegistry::beginOpen(const std::shared_ptr<const FileProbe> &file) {
    const auto existing = lookup(file);
    if (existing.result == RegistryResult::Pending)
        return {RegistryResult::Pending, {}, m_sessions.value(file->identity()).request};
    if (existing.result != RegistryResult::NotRegistered) return {existing.result, existing.session, {}};
    FileReadRequest request{file};
    m_sessions.insert(file->identity(), {{}, {}, request});
    return {RegistryResult::Pending, {}, request, true};
}

RegistryResult SessionRegistry::cancel(const FileReadRequest &request) {
    if (!onGuiThread()) return RegistryResult::WrongThread;
    if (!request.file) return RegistryResult::InvalidBaseline;
    const auto found = m_sessions.find(request.file->identity());
    if (found == m_sessions.end() || found->request.requestId != request.requestId) return RegistryResult::StaleRequest;
    if (found->session || found->baseline) return found->session && !found->session->isClosing()
        ? RegistryResult::Reused : RegistryResult::Closing;
    m_sessions.erase(found);
    return RegistryResult::Cancelled;
}

RegisteredSession SessionRegistry::complete(const FileReadRequest &request, const std::shared_ptr<const FileBaseline> &baseline) {
    if (!onGuiThread()) return {RegistryResult::WrongThread, {}};
    if (!request.file || !baseline || baseline->requestId() != request.requestId
        || baseline->file()->identity() != request.file->identity()) return {RegistryResult::InvalidBaseline, {}};
    const auto key = request.file->identity();
    const auto found = m_sessions.find(key);
    if (found == m_sessions.end() || found->request.requestId != request.requestId) return {RegistryResult::StaleRequest, {}};
    if (found->session || found->baseline) return lookup(request.file);
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
    auto target = m_sessions.find(baseline->file()->identity());
    if (target != m_sessions.end() && target->session != session) return RegistryResult::TargetInUse;
    for (auto old = m_sessions.begin(); old != m_sessions.end(); ++old) {
        if (old->session != session) continue;
        auto entry = old.value();
        m_sessions.erase(old);
        entry.baseline = baseline;
        entry.request = {baseline->file(), baseline->requestId()};
        m_sessions.insert(baseline->file()->identity(), entry);
        return RegistryResult::Reused;
    }
    return RegistryResult::NotRegistered;
}
}
