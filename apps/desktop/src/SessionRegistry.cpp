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
    const QPointer<DocumentSession> guard(session);
    const auto requestId = request.requestId;
    connect(session, &DocumentSession::closed, this, [this, key, guard, requestId] {
        const auto found = m_sessions.find(key);
        if (found != m_sessions.end() && found->request.requestId == requestId && found->session == guard) {
            m_sessions.erase(found);
            if (guard) guard->deleteLater();
        }
    }, Qt::QueuedConnection);
    connect(session, &QObject::destroyed, this, [this, key, requestId] {
        const auto found = m_sessions.find(key);
        if (found != m_sessions.end() && found->request.requestId == requestId && !found->session) m_sessions.erase(found);
    });
    return {RegistryResult::Created, session};
}
}
