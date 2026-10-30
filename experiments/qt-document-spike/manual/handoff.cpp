#include "spike_handoff.h"

#include <QGuiApplication>
#include <QDebug>
#include <QInputMethodEvent>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QPointer>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTextDocument>

#include <memory>

class HandoffHarness final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int activeWindow READ activeWindow NOTIFY activeWindowChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
public:
    bool initialize(QObject *root)
    {
        m_editors[0] = root->findChild<QQuickItem *>(QStringLiteral("standaloneEditor"));
        m_editors[1] = root->findChild<QQuickItem *>(QStringLiteral("workspaceEditor"));
        if (!m_editors[0] || !m_editors[1])
            return false;
        auto *quick = m_editors[0]->property("textDocument").value<QQuickTextDocument *>();
        if (!quick)
            return false;
        m_document.setPlainText(QStringLiteral("甲乙丙\n请在这里输入测试文字。"));
        m_document.clearUndoRedoStacks();
        quick->setTextDocument(&m_document);
        m_editors[0]->forceActiveFocus();
        return true;
    }

    int activeWindow() const { return m_activeWindow; }
    QString status() const { return m_status; }
    QQuickItem *editor(int index) const { return m_editors[index].data(); }
    QTextDocument *document() { return &m_document; }

    Q_INVOKABLE void requestHandoff()
    {
        QQuickItem *from = m_editors[m_activeWindow].data();
        QQuickItem *to = m_editors[1 - m_activeWindow].data();
        if (!from || !to) {
            setStatus(QStringLiteral("接管失败：试验窗口尚未准备好。"));
            return;
        }
        if (from->property("inputMethodComposing").toBool()) {
            setStatus(QStringLiteral("已拒绝接管：请求时输入法仍在组合。请提交或取消组合后重试。"));
            return;
        }
        const int start = from->property("selectionStart").toInt();
        const int end = from->property("selectionEnd").toInt();
        const int cursor = from->property("cursorPosition").toInt();
        if (!transferDocument(from, to)) {
            setStatus(QStringLiteral("接管失败：原窗口继续保留内容。"));
            return;
        }
        m_activeWindow = 1 - m_activeWindow;
        emit activeWindowChanged();
        if (auto *window = to->window())
            window->requestActivate();
        to->forceActiveFocus();
        // Changing a TextEdit from read-only back to editable can reset its
        // cursor. Restore after changing the active window and focusing it.
        if (!restoreSelection(to, start, end, cursor)) {
            m_activeWindow = 1 - m_activeWindow;
            emit activeWindowChanged();
            if (auto *window = from->window())
                window->requestActivate();
            from->forceActiveFocus();
            restoreSelection(from, start, end, cursor);
            setStatus(QStringLiteral("接管失败：无法恢复选区，原窗口继续保留内容。"));
            return;
        }
        setStatus(QStringLiteral("接管成功；目标选区 %1–%2，光标 %3。")
                      .arg(to->property("selectionStart").toInt())
                      .arg(to->property("selectionEnd").toInt())
                      .arg(to->property("cursorPosition").toInt()));
    }

signals:
    void activeWindowChanged();
    void statusChanged();

private:
    void setStatus(const QString &status)
    {
        m_status = status;
        emit statusChanged();
    }

    QTextDocument m_document;
    QPointer<QQuickItem> m_editors[2];
    int m_activeWindow = 0;
    QString m_status = QStringLiteral("试验已就绪；仅使用内存中的测试文本，不打开或保存文件。");
};

static bool smokeTest(HandoffHarness &harness)
{
    auto *first = harness.editor(0);
    auto *second = harness.editor(1);
    if (!QMetaObject::invokeMethod(first, "insert", Qt::DirectConnection,
                                   Q_ARG(int, 3), Q_ARG(QString, QStringLiteral("丁")))
        || !QMetaObject::invokeMethod(first, "select", Qt::DirectConnection,
                                     Q_ARG(int, 3), Q_ARG(int, 1))) {
        qCritical("Smoke test could not insert or select in the first editor");
        return false;
    }
    harness.requestHandoff();
    auto *quick = second->property("textDocument").value<QQuickTextDocument *>();
    if (harness.activeWindow() != 1 || !quick || quick->textDocument() != harness.document()
        || second->property("selectionStart").toInt() != 1
        || second->property("selectionEnd").toInt() != 3
        || second->property("cursorPosition").toInt() != 1) {
        qCritical() << "Smoke handoff state:" << harness.status() << harness.activeWindow()
                    << (quick && quick->textDocument() == harness.document())
                    << second->property("selectionStart") << second->property("selectionEnd")
                    << second->property("cursorPosition");
        return false;
    }
    if (!QMetaObject::invokeMethod(second, "undo", Qt::DirectConnection)
        || !harness.document()->toPlainText().startsWith(QStringLiteral("甲乙丙\n"))) {
        qCritical("Smoke test undo did not restore the initial text");
        return false;
    }
    QInputMethodEvent preedit(QStringLiteral("pin"), {});
    QCoreApplication::sendEvent(second, &preedit);
    if (!second->property("inputMethodComposing").toBool()) {
        qCritical("Smoke test could not start synthetic input-method composition");
        return false;
    }
    harness.requestHandoff();
    if (harness.activeWindow() != 1) {
        qCritical("Smoke test transferred while synthetic composition was active");
        return false;
    }
    QInputMethodEvent cancel;
    QCoreApplication::sendEvent(second, &cancel);
    if (second->property("inputMethodComposing").toBool()) {
        qCritical("Smoke test could not cancel synthetic composition");
        return false;
    }
    harness.requestHandoff();
    if (harness.activeWindow() != 0) {
        qCritical("Smoke test did not transfer after synthetic composition was cancelled");
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    HandoffHarness harness;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("harness"), &harness);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/manual/handoff.qml")));
    std::unique_ptr<QObject> root(component.create());
    if (!root) {
        qCritical() << component.errors();
        return 1;
    }
    if (!harness.initialize(root.get())) {
        qCritical("Could not initialize manual handoff editors");
        return 1;
    }
    if (app.arguments().contains(QStringLiteral("--smoke-test"))) {
        if (!smokeTest(harness)) {
            qCritical("Manual handoff smoke test failed");
            return 1;
        }
        return 0;
    }
    return app.exec();
}

#include "handoff.moc"
