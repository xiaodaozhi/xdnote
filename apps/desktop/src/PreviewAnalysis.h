#pragma once
#include "NativePreviewModel.h"
#include "MarkdownReferences.h"
#include <QJsonObject>
#include <atomic>

namespace xdnote {
struct PreviewAnalysisStore;
using PreviewCancellation = std::shared_ptr<std::atomic_bool>;
inline bool previewCancelled(const PreviewCancellation &token) { return token && token->load(std::memory_order_relaxed); }

// Only immutable syntax is cached. This object never grants filesystem access.
struct PreviewAnalysis {
    NativePreviewModel model;
    MarkdownReferenceAnalysis references;
    qsizetype estimatedBytes = 0;
    static std::shared_ptr<const PreviewAnalysis> get(const DocumentSnapshot &snapshot,
        const PreviewCancellation &cancel, QJsonObject &metrics, QString *error,
        std::shared_ptr<PreviewAnalysisStore> store = {});
    static std::shared_ptr<PreviewAnalysisStore> store();
    static void forget(const QUuid &document);
    // A nonblocking lookup lets heading/reference-only callers reuse a ready
    // result without constructing a full model or waiting on the UI thread.
    static std::shared_ptr<const PreviewAnalysis> find(const DocumentSnapshot &snapshot);
    static QJsonObject cacheStatistics();
};
}
