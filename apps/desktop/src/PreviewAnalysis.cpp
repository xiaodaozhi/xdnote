#include "PreviewAnalysis.h"
#include "MarkdownBridge.h"
#include <QCache>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QScopeGuard>
#include <QWaitCondition>

namespace xdnote {
namespace {
constexpr int CacheBytes = 16 * 1024 * 1024, CacheEntries = 32;
using Value = std::shared_ptr<const PreviewAnalysis>;
struct Pending { QWaitCondition ready; bool done = false; Value value; };
struct Cache {
    QMutex mutex;
    QCache<QByteArray, Value> entries{CacheBytes};
    QHash<QByteArray, std::shared_ptr<Pending>> pending;
};
bool isCancelled(const void *context) { return static_cast<const std::atomic_bool *>(context)->load(std::memory_order_relaxed); }
}
struct PreviewAnalysisStore { Cache value; };
namespace {
std::shared_ptr<PreviewAnalysisStore> globalStore() { static const auto value = std::make_shared<PreviewAnalysisStore>(); return value; }
Cache &cache() { return globalStore()->value; }
}
std::shared_ptr<PreviewAnalysisStore> PreviewAnalysis::store() { return globalStore(); }

Value PreviewAnalysis::get(const DocumentSnapshot &snapshot, const PreviewCancellation &cancel, QJsonObject &metrics, QString *error, std::shared_ptr<PreviewAnalysisStore> ownedStore) {
    if (snapshot.result != EditResult::Applied || previewCancelled(cancel)) return {};
    const QByteArray prefix = snapshot.documentId.toRfc4122();
    const QByteArray key = prefix + QByteArray::number(snapshot.revision) + ':' + QCryptographicHash::hash(snapshot.utf8, QCryptographicHash::Sha256);
    const auto owner = ownedStore ? std::move(ownedStore) : globalStore();
    auto &state = owner->value; std::shared_ptr<Pending> pending;
    {
        QMutexLocker lock(&state.mutex);
        for (;;) {
            if (previewCancelled(cancel)) return {};
            if (auto hit = state.entries.object(key)) { metrics["analysisCacheHit"] = true; return *hit; }
            auto found = state.pending.constFind(key);
            if (found == state.pending.cend()) { pending = std::make_shared<Pending>(); state.pending.insert(key, pending); break; }
            const auto running = *found;
            while (!running->done && !previewCancelled(cancel)) running->ready.wait(&state.mutex, 25);
            if (previewCancelled(cancel)) return {};
            if (running->value) { metrics["analysisCacheHit"] = true; return running->value; }
        }
    }
    Value value;
    // Every exit wakes waiters, including cancellation, decode failure and throw.
    const auto finish = qScopeGuard([&] {
        QMutexLocker lock(&state.mutex);
        if (!previewCancelled(cancel) && value) {
            if (value->estimatedBytes <= CacheBytes) {
                while (state.entries.size() >= CacheEntries) state.entries.remove(state.entries.keys().last());
                state.entries.insert(key, new Value(value), int(value->estimatedBytes));
            }
            pending->value = value;
        }
        pending->done = true; state.pending.remove(key); pending->ready.wakeAll();
    });
    metrics["analysisCacheHit"] = false;
    auto output = xdnote_markdown_analyze(reinterpret_cast<const unsigned char *>(snapshot.utf8.constData()), size_t(snapshot.utf8.size()), cancel.get(), cancel ? isCancelled : nullptr);
    const auto release = qScopeGuard([&] { xdnote_markdown_free(&output.model); xdnote_markdown_free(&output.references); });
    metrics["semanticMs"] = double(output.semanticNs) / 1e6;
    metrics["referencesMs"] = double(output.referencesNs) / 1e6;
    if (previewCancelled(cancel) || output.model.status == 4) return {};
    if (output.model.status || output.references.status || !output.model.data || !output.references.data
        || output.model.length > NativePreviewModel::MaximumBytes || output.references.length > 8 * 1024 * 1024) {
        if (error) *error = QStringLiteral("无法分析 Markdown：源文或派生结果无效／超限"); return {};
    }
    QElapsedTimer timer; timer.start();
    auto owned = new XdnoteRenderBuffer(output.model); output.model = {};
    std::shared_ptr<void> storage(owned, [](void *pointer) { auto buffer = static_cast<XdnoteRenderBuffer *>(pointer); xdnote_markdown_free(buffer); delete buffer; });
    auto model = NativePreviewModel::adopt(QByteArray::fromRawData(reinterpret_cast<const char *>(owned->data), qsizetype(owned->length)), std::move(storage), snapshot.utf8, error);
    auto refs = decodeMarkdownReferences(snapshot, QByteArray::fromRawData(reinterpret_cast<const char *>(output.references.data), qsizetype(output.references.length)));
    metrics["decodeMs"] = timer.nsecsElapsed() / 1e6;
    if (!model || !refs.success || previewCancelled(cancel)) {
        if (!refs.success && error) *error = refs.reason.isEmpty() ? QStringLiteral("引用分析结果无效") : refs.reason;
        return {};
    }
    auto result = std::make_shared<PreviewAnalysis>(); result->model = std::move(*model); result->references = std::move(refs);
    result->estimatedBytes = result->model.encodedBytes() + 512 + qsizetype(result->references.references.capacity()) * qsizetype(sizeof(MarkdownReference));
    for (const auto &ref : result->references.references) result->estimatedBytes += 2 * (ref.target.capacity() + ref.title.capacity() + ref.referenceLabel.capacity());
    // Return a copy: an implicit move would empty the value before the scope
    // guard publishes it to the cache and waiting requests.
    value = std::move(result); return Value(value);
}
void PreviewAnalysis::forget(const QUuid &document) {
    auto &state = cache(); QMutexLocker lock(&state.mutex);
    const auto prefix = document.toRfc4122();
    for (const auto &key : state.entries.keys()) if (key.startsWith(prefix)) state.entries.remove(key);
}
QJsonObject PreviewAnalysis::cacheStatistics() {
    auto &state = cache(); QMutexLocker lock(&state.mutex);
    return {{"entries", state.entries.size()}, {"estimatedBytes", state.entries.totalCost()}, {"limitBytes", CacheBytes}, {"inFlight", state.pending.size()}};
}
Value PreviewAnalysis::find(const DocumentSnapshot &snapshot) {
    if (snapshot.result != EditResult::Applied) return {};
    const auto key = snapshot.documentId.toRfc4122() + QByteArray::number(snapshot.revision) + ':' + QCryptographicHash::hash(snapshot.utf8, QCryptographicHash::Sha256);
    auto &state = cache(); QMutexLocker lock(&state.mutex);
    if (auto hit = state.entries.object(key)) return *hit;
    return {};
}
}
