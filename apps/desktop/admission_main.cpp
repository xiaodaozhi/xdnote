#include "AdmissionWindow.h"
#include "DocumentSession.h"
#include "SourceEditor.h"

#include <QApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("xdnote-admission"));
    xdnote::SourceEditor::installAccessibility();
    xdnote::DocumentSession session(QStringLiteral(
        "# 写作，从这里开始\n\n这是 xdnote 的内存组件准入文档。中文、😀 和源文撤销由同一个会话保留。\n\n"
        "## 正文与预览\n\n- [x] 标题、列表和任务\n- [ ] 图片与文档链接在后续任务实现\n\n"
        "| 项目 | 状态 |\n| --- | --- |\n| 源文与预览 | 同一版本快照 |\n\n"
        "```cpp\n// 围栏代码只展示文字\n```\n\n[示例链接](https://example.com)\n").toUtf8());
    xdnote::AdmissionWindow window(&session);
    window.show();
    return application.exec();
}
