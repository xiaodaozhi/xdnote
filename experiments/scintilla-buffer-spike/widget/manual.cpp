#include "handoff.h"
#include "ScintillaEditBase.h"

#include <QApplication>
#include <QByteArray>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

struct FixtureWindow
{
    explicit FixtureWindow(const QString &title)
    {
        window.setWindowTitle(title);
        window.resize(620, 330);
        auto *layout = new QVBoxLayout(&window);
        auto *description = new QLabel(QStringLiteral("仅使用内存测试文字；F8 或按钮接管到另一窗口。"), &window);
        layout->addWidget(description);
        editor = new ScintillaEditBase(&window);
        layout->addWidget(editor, 1);
        state = new QLabel(&window);
        state->setWordWrap(true);
        layout->addWidget(state);
        button = new QPushButton(QStringLiteral("接管到另一窗口（F8）"), &window);
        button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(button);
        status = new QLabel(&window);
        status->setWordWrap(true);
        layout->addWidget(status);
    }

    QWidget window;
    ScintillaEditBase *editor = nullptr;
    QLabel *state = nullptr;
    QPushButton *button = nullptr;
    QLabel *status = nullptr;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    FixtureWindow first(QStringLiteral("Scintilla 文档接管试验 — 独立窗口"));
    FixtureWindow second(QStringLiteral("Scintilla 文档接管试验 — 工作区窗口"));
    FixtureWindow *windows[] = {&first, &second};
    first.editor->setObjectName(QStringLiteral("scintillaStandaloneEditor"));
    second.editor->setObjectName(QStringLiteral("scintillaWorkspaceEditor"));
    first.editor->send(SCI_SETCODEPAGE, SC_CP_UTF8);
    const QByteArray initial = QStringLiteral("甲乙丙\n请在这里输入测试文字。").toUtf8();
    first.editor->send(SCI_ADDTEXT, initial.size(), reinterpret_cast<sptr_t>(initial.constData()));
    first.editor->send(SCI_EMPTYUNDOBUFFER);
    second.editor->send(SCI_SETDOCPOINTER, 0, first.editor->send(SCI_GETDOCPOINTER));
    second.editor->setEnabled(false);

    int active = 0;
    QString lastStatus = QStringLiteral("试验已就绪；不读取或保存文件。");
    const auto refresh = [&] {
        for (int i = 0; i < 2; ++i) {
            const bool composing = windows[i]->editor->send(SCI_GETUNDOTENTATIVE) >= 0;
            windows[i]->state->setText(QStringLiteral("%1；输入法临时组合：%2；文本字节：%3")
                .arg(i == active ? QStringLiteral("当前编辑窗口") : QStringLiteral("非活动窗口（控件禁用）"))
                .arg(composing ? QStringLiteral("是") : QStringLiteral("否"))
                .arg(windows[i]->editor->send(SCI_GETTEXTLENGTH)));
            windows[i]->status->setText(lastStatus);
        }
    };
    const auto request = [&] {
        ScintillaEditBase &from = *windows[active]->editor;
        ScintillaEditBase &to = *windows[1 - active]->editor;
        const FixtureHandoff result = transferFixture(from, to);
        if (result == FixtureHandoff::Composing) {
            lastStatus = QStringLiteral("已拒绝接管：请求时输入法仍在组合。请提交或取消后重试。");
        } else if (result == FixtureHandoff::Failed) {
            lastStatus = QStringLiteral("接管失败；原窗口继续保留测试内容。");
        } else {
            active = 1 - active;
            windows[active]->window.raise();
            windows[active]->window.activateWindow();
            to.setFocus(Qt::OtherFocusReason);
            lastStatus = QStringLiteral("接管成功；选区锚点 %1，光标 %2（UTF-8 字节位置）。")
                .arg(to.send(SCI_GETANCHOR)).arg(to.send(SCI_GETCURRENTPOS));
        }
        refresh();
    };
    for (auto *window : windows) {
        QObject::connect(window->button, &QPushButton::clicked, &window->window, request);
        auto *shortcut = new QShortcut(QKeySequence(Qt::Key_F8), &window->window);
        QObject::connect(shortcut, &QShortcut::activated, &window->window, request);
    }
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &first.window, refresh);
    timer.start(100);
    first.window.show();
    second.window.show();
    first.window.activateWindow();
    first.editor->setFocus();
    refresh();
    return app.exec();
}
