#include "AdmissionWindow.h"
#include "DocumentSession.h"
#include "MarkdownPreview.h"
#include "SourceEditor.h"
#include "VisualTokens.h"
#include "InputModality.h"
#include "QuickAccessibleRoot.h"

#include <QLabel>
#include <QQmlContext>
#include <QQuickWidget>
#include <QSplitter>
#include <QVBoxLayout>

namespace xdnote {
AdmissionWindow::AdmissionWindow(DocumentSession *session, QWidget *parent) : QWidget(parent), m_session(session) {
    setObjectName(QStringLiteral("desktopAdmission"));
    setWindowTitle(QStringLiteral("xdnote · 桌面组件准入"));
    const auto geometry = visualTokens().value(QStringLiteral("windows")).toObject().value(QStringLiteral("main")).toObject();
    resize(geometry.value(QStringLiteral("defaultWidth")).toInt(), geometry.value(QStringLiteral("defaultHeight")).toInt());
    setMinimumSize(geometry.value(QStringLiteral("minWidth")).toInt(), geometry.value(QStringLiteral("minHeight")).toInt());
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_chrome = new QQuickWidget(this);
    registerQuickAccessibleRoot();
    m_chrome->setObjectName(QStringLiteral("sharedChrome"));
    m_chrome->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_chrome->rootContext()->setContextProperty(QStringLiteral("admission"), this);
    m_chrome->rootContext()->setContextProperty(QStringLiteral("visual"), visualTokens().toVariantMap());
    m_chrome->rootContext()->setContextProperty(QStringLiteral("inputMode"), InputModality::forApplication());
    m_chrome->rootContext()->setContextProperty(QStringLiteral("chromeHost"), m_chrome);
    m_chrome->setFixedHeight(geometry.value(QStringLiteral("toolbarHeight")).toInt());
    m_chrome->setSource(QUrl(QStringLiteral("qrc:/xdnote/apps/desktop/ui/Chrome.qml")));
    layout->addWidget(m_chrome);
    auto *split = new QSplitter(Qt::Horizontal, this);
    m_editor = new SourceEditor(session, split);
    m_preview = new MarkdownPreview(session, split);
    connect(m_preview, &MarkdownPreview::previewError, this, [this](const QString &message) {
        m_previewError = message;
        emit sessionStateChanged();
    });
    connect(m_preview, &MarkdownPreview::previewReady, this, [this] {
        if (!m_previewError.isEmpty()) { m_previewError.clear(); emit sessionStateChanged(); }
    });
    split->addWidget(m_editor);
    split->addWidget(m_preview);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setSizes({width() / 2, width() / 2});
    layout->addWidget(split, 1);
    auto *notice = new QLabel(QStringLiteral("组件准入：内容仅在内存中，不读取或保存文件。链接与图片暂作惰性展示。"), this);
    notice->setWordWrap(true);
    notice->setContentsMargins(12, 4, 12, 4);
    layout->addWidget(notice);
    connect(session, &DocumentSession::stateChanged, this, &AdmissionWindow::sessionStateChanged);
    connect(session, &DocumentSession::changed, this, [this] { emit sessionStateChanged(); });
    connect(session, &QObject::destroyed, this, &AdmissionWindow::sessionStateChanged);
}
AdmissionWindow::~AdmissionWindow() {
    // Destroy bindings while their context object still has this class's metaobject.
    delete m_chrome;
    m_chrome = nullptr;
}
bool AdmissionWindow::readOnly() const { return !m_session || m_session->isReadOnly(); }
QString AdmissionWindow::feedback() const {
    if (!m_session) return QStringLiteral("文档会话已关闭");
    if (!m_session->rejection().isEmpty()) return m_session->rejection();
    return m_previewError.isEmpty() ? QStringLiteral("Markdown 源文与受限预览") : m_previewError;
}
void AdmissionWindow::undo() { if (m_session) m_session->undo(); }
void AdmissionWindow::redo() { if (m_session) m_session->redo(); }
void AdmissionWindow::toggleReadOnly() { if (m_session) m_session->setReadOnly(!m_session->isReadOnly()); }
}
