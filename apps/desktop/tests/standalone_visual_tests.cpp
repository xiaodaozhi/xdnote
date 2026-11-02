#include "Appearance.h"
#include "DocumentSession.h"
#include "FileManager.h"
#include "GuiRuntime.h"
#include "InputModality.h"
#include "MarkdownPreview.h"
#include "SharedWidgets.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "VisualTokens.h"
#include "file_fixtures.h"
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <memory>

using namespace xdnote;
namespace {
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QByteArray sample() { return QStringLiteral("# 写作，从这里开始\n\n把想法写下来，让思考慢慢清晰。\n\n> 少一点干扰，多一点专注。\n\n## 一个安静的写作空间\n\n- [x] 记录一个新的想法\n- [ ] 留一点时间，自由写作\n\n```css\n.editor { color: #2c3532; }\n```\n").toUtf8(); }
struct Window { QPointer<SingleFileWindow> value; ~Window() { if (value) delete value; } };
FileDocument *open(FileManager &manager, const QString &path) {
    QSignalSpy opened(&manager, &FileManager::opened); manager.open(path);
    QElapsedTimer timer; timer.start(); while (opened.isEmpty() && timer.elapsed() < 10000) QTest::qWait(10);
    return opened.isEmpty() ? nullptr : qvariant_cast<FileDocument *>(opened.first().at(1));
}
bool painted(MarkdownPreview *preview, Appearance *appearance) {
    const auto image = preview->view()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    if (image.width() < 20 || image.height() < 20) return false;
    const auto background = appearance->color("background"); const auto corner = image.pixelColor(5, 5);
    if (qAbs(corner.red() - background.red()) + qAbs(corner.green() - background.green()) + qAbs(corner.blue() - background.blue()) > 30) return false;
    int ink = 0;
    for (int y = 10; y < image.height() - 10; y += 2) for (int x = 10; x < image.width() - 10; x += 2) {
        const auto color = image.pixelColor(x, y);
        const int difference = (color.red() + color.green() + color.blue()) - (background.red() + background.green() + background.blue());
        if ((appearance->dark() ? difference > 150 : difference < -150) && ++ink > 100) return true;
    }
    return false;
}
void capture(QWidget *widget, const QString &name) {
    const auto root = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE"); if (root.isEmpty()) return;
    QCoreApplication::processEvents(); QDir().mkpath(root + "/standalone"); widget->grab().save(root + "/standalone/" + name + ".png");
}
QString html(MarkdownPreview *preview) {
    struct Result { QString text; bool finished = false; };
    const auto result = std::make_shared<Result>();
    preview->view()->page()->toHtml([result](const QString &value) { result->text = value; result->finished = true; });
    QElapsedTimer timer; timer.start(); while (!result->finished && timer.elapsed() < 10000) QTest::qWait(10);
    return result->text;
}
bool contained(SingleFileWindow *window, QWidget *child) { return child && window->rect().contains(QRect(child->mapTo(window, QPoint()), child->size())); }
double inkCenter(const QImage &image, qreal dpr, int firstX, int lastX, QColor background) {
    int first = image.height(), last = -1;
    for (int y = 4 * dpr; y < image.height() - 4 * dpr; ++y) for (int x = firstX * dpr; x < qMin(image.width(), qRound(lastX * dpr)); ++x) {
        const auto c = image.pixelColor(x, y);
        if (qAbs(c.red() - background.red()) + qAbs(c.green() - background.green()) + qAbs(c.blue() - background.blue()) > 120) { first = qMin(first, y); last = qMax(last, y); }
    }
    return last < first ? -100 : (first + last) / (2.0 * dpr);
}
}

class StandaloneVisualTests final : public QObject {
    Q_OBJECT
private slots:
    void appearanceStoragePreservesUnknownFieldsAndFailure() {
        QTemporaryDir temporary; const auto path = temporary.filePath("settings.json");
        QVERIFY(fileFixtures::write(path, R"({"schemaVersion":1,"other":{"keep":42},"appearance":{"theme":"light","accent":"cobalt","future":true}})"));
        Appearance appearance(nullptr, path); QCOMPARE(appearance.theme(), QString("light")); QCOMPARE(appearance.accent(), QString("cobalt"));
        QVERIFY(appearance.setPreferences("dark", "olive")); Appearance reopened(nullptr, path); QCOMPARE(reopened.theme(), QString("dark")); QCOMPARE(reopened.accent(), QString("olive"));
        const auto saved = read(path); const auto object = QJsonDocument::fromJson(saved).object();
        QCOMPARE(object["other"].toObject()["keep"].toInt(), 42); QVERIFY(object["appearance"].toObject()["future"].toBool());
        QVERIFY(!appearance.setPreferences("invalid", "olive")); QCOMPARE(read(path), saved); QCOMPARE(appearance.theme(), QString("dark"));
        const auto invalid = temporary.filePath("invalid.json"); QVERIFY(fileFixtures::write(invalid, "{broken"));
        Appearance broken(nullptr, invalid); QVERIFY(!broken.error().isEmpty()); QVERIFY(!broken.setPreferences("dark", "cobalt")); QCOMPARE(read(invalid), QByteArray("{broken"));
        const auto parentFile = temporary.filePath("not-a-directory"); QVERIFY(fileFixtures::write(parentFile, "keep"));
        Appearance unwritable(nullptr, parentFile + "/settings.json"); QSignalSpy error(&unwritable, &Appearance::preferenceFailed);
        QVERIFY(!unwritable.setPreferences("dark", "mauve")); QCOMPARE(unwritable.theme(), QString("system")); QCOMPARE(error.count(), 1); QCOMPARE(read(parentFile), QByteArray("keep"));
        const auto unknown = temporary.filePath("unknown.json"); QVERIFY(fileFixtures::write(unknown, R"({"schemaVersion":1,"appearance":{"theme":"future","accent":"unknown"}})"));
        Appearance fallback(nullptr, unknown); QCOMPARE(fallback.theme(), QString("system")); QCOMPARE(fallback.accent(), QString("graphite"));
        QCOMPARE(fallback.dark(), QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    }
    void themeGeometryAndEditing_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<QString>("accent");
        for (const auto &theme : {"light", "dark"}) for (const auto &accent : {"graphite", "mist-blue", "cobalt", "mauve", "terracotta", "olive"})
            QTest::newRow((QByteArray(theme) + '-' + accent).constData()) << QString(theme) << QString(accent);
    }
    void themeGeometryAndEditing() {
        QFETCH(QString, theme); QFETCH(QString, accent);
        QTemporaryDir temporary; const auto path = temporary.filePath(QStringLiteral("写作，从这里开始.md")); QVERIFY(fileFixtures::write(path, sample()));
        Appearance appearance; QVERIFY(appearance.setPreferences(theme, accent)); FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        Window owned{new SingleFileWindow(&manager, &appearance)}; auto *window = owned.value.data(); window->setDocument(document); window->show();
        QTRY_VERIFY_WITH_TIMEOUT(painted(window->preview(), &appearance), 20000);
        auto *header = window->findChild<QWidget *>("documentHeader"); const auto pixels = header->grab().toImage();
        const auto logoCenter = inkCenter(pixels, header->devicePixelRatioF(), 43, 128, appearance.color("sidebar"));
        const auto titleCenter = inkCenter(pixels, header->devicePixelRatioF(), 160, 780, appearance.color("sidebar"));
        QVERIFY(logoCenter > 0 && titleCenter > 0); QVERIFY(qAbs(logoCenter - titleCenter) <= 1.25);
        QCOMPARE(window->findChild<QWidget *>("documentHeader")->height(), 48); QCOMPARE(window->findChild<QWidget *>("documentToolbar")->height(), 48);
        QCOMPARE(window->findChild<QWidget *>("documentFooter")->height(), 26); QCOMPARE(window->findChild<QWidget *>("modeSwitch")->height(), 32);
        auto *modes = window->findChild<QWidget *>("modeSwitch"); auto *toolbar = window->findChild<QWidget *>("documentToolbar");
        QVERIFY(qAbs(modes->mapTo(toolbar, modes->rect().center()).x() - toolbar->rect().center().x()) <= 1);
        const auto rendered = html(window->preview()); QVERIFY(rendered.contains(appearance.color("background").name(), Qt::CaseInsensitive)); QVERIFY(rendered.contains(QStringLiteral("写作")));
        QVERIFY(!window->preview()->view()->settings()->testAttribute(QWebEngineSettings::JavascriptEnabled));
        capture(window, "reading-" + theme + '-' + accent);
        const auto initial = document->session()->snapshot(); window->setEditing(true); QCOMPARE(window->findChild<QWidget *>("formatToolbar")->height(), 36);
        QCOMPARE(document->session()->replace(0, 0, QStringLiteral("新内容\n"), document->session()->revision()), EditResult::Applied);
        QTRY_VERIFY(window->isWindowModified()); const auto edited = document->session()->snapshot();
        QCOMPARE(window->findChild<ElidedLabel *>("documentStatus")->fullText(), QStringLiteral("未保存"));
        const auto ref = [](QColor color) { return color.red() | color.green() << 8 | color.blue() << 16; };
        QCOMPARE(window->editor()->send(SCI_STYLEGETBACK, STYLE_DEFAULT), sptr_t(ref(appearance.color("background"))));
        const auto caret = window->editor()->send(SCI_GETCURRENTPOS); window->setContentZoom(150);
        QCOMPARE(window->contentZoom(), 150); QCOMPARE(window->preview()->view()->zoomFactor(), 1.5); QCOMPARE(window->editor()->send(SCI_GETCURRENTPOS), caret);
        QCOMPARE(document->session()->snapshot().utf8, edited.utf8); QCOMPARE(document->session()->revision(), edited.revision); window->setContentZoom(100);
        capture(window, "editing-" + theme + '-' + accent);
        window->resize(560, 480); QTest::qWait(30);
        for (const auto &name : {"readMode", "editMode", "saveButton", "moreActions", "contentZoom", "fileLocation"}) QVERIFY2(contained(window, window->findChild<QWidget *>(name)), name);
        QCOMPARE(window->findChild<SharedButton *>("saveButton")->font().pixelSize(), 14);
        QCOMPARE(document->session()->undo(), EditResult::Applied); QCOMPARE(document->session()->snapshot().utf8, initial.utf8); QVERIFY(!document->dirty());
        QCOMPARE(document->session()->redo(), EditResult::Applied); QCOMPARE(document->session()->snapshot().utf8, edited.utf8);
    }
    void sharedAppearanceAndReloadPreserveTextUndoAndPosition() {
        QTemporaryDir temporary; QByteArray text = "# Long document\n\n"; for (int i = 0; i < 180; ++i) text += "Paragraph " + QByteArray::number(i) + "\n\n";
        const auto path = temporary.filePath("long.md"), other = temporary.filePath("other.md"); QVERIFY(fileFixtures::write(path, text)); QVERIFY(fileFixtures::write(other, sample()));
        Appearance appearance; QVERIFY(appearance.setPreferences("light", "graphite")); FileManager manager(nullptr, {}, false);
        auto *document = open(manager, path), *second = open(manager, other); QVERIFY(document && second);
        Window first{new SingleFileWindow(&manager, &appearance)}, next{new SingleFileWindow(&manager, &appearance)};
        first.value->setDocument(document); first.value->show(); next.value->setDocument(second); next.value->show();
        QTRY_VERIFY_WITH_TIMEOUT(painted(first.value->preview(), &appearance), 20000);
        first.value->preview()->restoreReadingPosition({0, 1000}); QTRY_VERIFY_WITH_TIMEOUT(first.value->preview()->view()->page()->scrollPosition().y() > 900, 10000);
        const auto original = document->session()->snapshot(); const auto position = first.value->preview()->view()->page()->scrollPosition().y();
        QCOMPARE(second->session()->replace(0, 0, "keep\n", second->session()->revision()), EditResult::Applied); const auto otherEdited = second->session()->snapshot();
        QVERIFY(appearance.setPreferences("dark", "cobalt")); QTRY_VERIFY_WITH_TIMEOUT(painted(first.value->preview(), &appearance), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(first.value->preview()->view()->page()->scrollPosition().y() - position) < 5, 10000);
        QCOMPARE(next.value->appearance()->accent(), QString("cobalt")); QTRY_VERIFY_WITH_TIMEOUT(painted(next.value->preview(), &appearance), 20000); QVERIFY(html(next.value->preview()).contains(appearance.color("background").name(), Qt::CaseInsensitive));
        QCOMPARE(document->session()->snapshot().utf8, original.utf8); QCOMPARE(document->session()->revision(), original.revision); QVERIFY(!document->dirty());
        QCOMPARE(second->session()->snapshot().utf8, otherEdited.utf8); QVERIFY(second->dirty()); QCOMPARE(second->session()->undo(), EditResult::Applied); QCOMPARE(second->session()->snapshot().utf8, sample());
        QVERIFY(fileFixtures::write(path, text + "external append\n")); document->checkExternal(); QTRY_VERIFY(document->conflict());
        auto *reload = first.value->findChild<QPushButton *>("resolveConflict"); QCOMPARE(reload->text(), QStringLiteral("重新载入")); reload->click();
        QTRY_VERIFY(!document->loading()); QTRY_VERIFY(!document->conflict()); QTRY_COMPARE_WITH_TIMEOUT(first.value->preview()->displayedRevision(), document->session()->revision(), 20000); QTRY_VERIFY_WITH_TIMEOUT(qAbs(first.value->preview()->view()->page()->scrollPosition().y() - position) < 5, 10000);
        QCOMPARE(document->session()->snapshot().utf8, text + "external append\n"); QCOMPARE(document->session()->undo(), EditResult::Applied); QCOMPARE(document->session()->snapshot().utf8, original.utf8); QVERIFY(document->dirty());
    }
    void tabOutlineActivationCancelAndWindowDeactivation() {
        Appearance appearance; QVERIFY(appearance.setPreferences("light", "cobalt")); QWidget panel; auto *layout = new QHBoxLayout(&panel);
        auto *first = new SharedButton("First", "first", &appearance, &panel), *second = new SharedButton("Second", "second", &appearance, &panel); layout->addWidget(first); layout->addWidget(second);
        panel.show(); QApplication::setActiveWindow(&panel); first->setFocus(Qt::OtherFocusReason); InputModality::forApplication()->clear();
        QVERIFY(!first->keyboardOutline()); QTest::keyClick(first, Qt::Key_Tab); QTRY_VERIFY(second->hasFocus()); QVERIFY(second->keyboardOutline());
        QSignalSpy clicked(second, &QPushButton::clicked); QTest::keyClick(second, Qt::Key_Space); QCOMPARE(clicked.count(), 1); QVERIFY(second->hasFocus()); QVERIFY(!second->keyboardOutline());
        QTest::keyClick(second, Qt::Key_Backtab); QTRY_VERIFY(first->hasFocus()); QVERIFY(first->keyboardOutline()); QTest::keyClick(first, Qt::Key_Escape); QVERIFY(!first->keyboardOutline()); QVERIFY(first->hasFocus());
        QTest::keyClick(first, Qt::Key_Tab); QVERIFY(second->keyboardOutline()); QTest::mouseClick(first, Qt::LeftButton); QVERIFY(!first->keyboardOutline()); QVERIFY(!second->keyboardOutline());
        QTest::keyClick(first, Qt::Key_Tab); QVERIFY(second->keyboardOutline()); QEvent inactive(QEvent::WindowDeactivate); QApplication::sendEvent(&panel, &inactive); QVERIFY(!second->keyboardOutline());
        QEvent active(QEvent::WindowActivate); QApplication::sendEvent(&panel, &active); QVERIFY(!second->keyboardOutline());
    }
    void appearanceMenuFailureRetainsSelectionAndDocument() {
        QTemporaryDir temporary; const auto path = temporary.filePath("document.md"), parent = temporary.filePath("blocked");
        QVERIFY(fileFixtures::write(path, sample())); QVERIFY(fileFixtures::write(parent, "keep")); Appearance appearance(nullptr, parent + "/settings.json");
        FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        Window owned{new SingleFileWindow(&manager, &appearance)}; owned.value->setDocument(document); owned.value->show();
        owned.value->findChild<QAction *>("theme-dark")->trigger(); QCOMPARE(appearance.theme(), QString("system"));
        QVERIFY(owned.value->findChild<QAction *>("theme-system")->isChecked()); QVERIFY(!owned.value->findChild<QAction *>("theme-dark")->isChecked());
        QVERIFY(owned.value->findChild<QLabel *>("documentNotice")->text().contains(QStringLiteral("无法"))); QCOMPARE(document->session()->snapshot().utf8, sample()); QVERIFY(!document->dirty());
    }
    void readOnlyFeedback_data() { QTest::addColumn<QString>("theme"); QTest::newRow("light") << QString("light"); QTest::newRow("dark") << QString("dark"); }
    void readOnlyFeedback() {
        QFETCH(QString, theme); QTemporaryDir temporary; const auto path = temporary.filePath("read-only.md"); QVERIFY(fileFixtures::write(path, sample()));
        struct Writable { QString path; ~Writable() {
#ifdef Q_OS_WIN
            SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_ATTRIBUTE_NORMAL);
#else
            ::chmod(path.toUtf8().constData(), 0600);
#endif
        }} restore{path};
#ifdef Q_OS_WIN
        QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_ATTRIBUTE_READONLY));
#else
        QVERIFY(::chmod(path.toUtf8().constData(), 0400) == 0);
#endif
        Appearance appearance; QVERIFY(appearance.setPreferences(theme, "graphite")); FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        Window owned{new SingleFileWindow(&manager, &appearance)}; owned.value->setDocument(document); owned.value->show(); QTRY_VERIFY_WITH_TIMEOUT(painted(owned.value->preview(), &appearance), 20000);
        QVERIFY(!owned.value->findChild<SharedButton *>("editMode")->isEnabled()); QVERIFY(owned.value->findChild<QAction *>("saveAsAction")->isEnabled());
        QVERIFY(owned.value->findChild<QLabel *>("documentNotice")->text().contains(QStringLiteral("只读"))); QCOMPARE(owned.value->findChild<ElidedLabel *>("documentStatus")->fullText(), QStringLiteral("只读")); capture(owned.value, "read-only-" + theme);
        emit owned.value->preview()->previewError(QStringLiteral("预览进程已停止"));
        QCOMPARE(owned.value->findChild<QLabel *>("documentNotice")->text(), QStringLiteral("预览进程已停止"));
        QCOMPARE(owned.value->findChild<SharedButton *>("resolveConflict")->text(), QStringLiteral("重试预览"));
    }
    void unsavedDialogAppearanceAndCancel_data() { readOnlyFeedback_data(); }
    void unsavedDialogAppearanceAndCancel() {
        QFETCH(QString, theme); QTemporaryDir temporary; const auto path = temporary.filePath("document.md"); QVERIFY(fileFixtures::write(path, sample()));
        Appearance appearance; QVERIFY(appearance.setPreferences(theme, "graphite")); FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        Window owned{new SingleFileWindow(&manager, &appearance)}; owned.value->setDocument(document); owned.value->setEditing(true); owned.value->show();
        QCOMPARE(document->session()->replace(0, 0, "keep\n", document->session()->revision()), EditResult::Applied); const auto edited = document->session()->snapshot();
        auto *editor = owned.value->editor(); QApplication::setActiveWindow(owned.value); editor->setFocus(Qt::OtherFocusReason); QTRY_VERIFY(editor->hasFocus());
        bool inspected = false; QTimer timer; timer.setInterval(50);
        connect(&timer, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(qApp->activeModalWidget()); if (!dialog) return; timer.stop();
            inspected = true;
            QCOMPARE(dialog->objectName(), QString("unsavedConfirmation")); QCOMPARE(dialog->width(), 480);
            QVERIFY(dialog->findChild<QWidget *>("dialogFile")); QVERIFY(dialog->findChildren<QScrollArea *>().isEmpty());
            auto *cancel = dialog->findChild<SharedButton *>("choice0"); auto *save = dialog->findChild<SharedButton *>("choice1"); auto *discard = dialog->findChild<SharedButton *>("choice2");
            QVERIFY(cancel && save && discard); QVERIFY(discard->x() < cancel->x() && cancel->x() < save->x());
            QVERIFY(!cancel->keyboardOutline()); capture(dialog, "unsaved-close-" + theme); QTest::keyClick(dialog, Qt::Key_Escape);
        });
        timer.start(); QTimer::singleShot(10000, &timer, [] { if (auto *dialog = qobject_cast<QDialog *>(qApp->activeModalWidget())) dialog->reject(); });
        QVERIFY(!owned.value->close()); QVERIFY(inspected); QVERIFY(owned.value->isVisible());
        QCOMPARE(document->session()->snapshot().utf8, edited.utf8); QVERIFY(document->dirty()); QTRY_VERIFY(editor->hasFocus());
        QCOMPARE(read(path), sample());
    }
    void menusModesLongPathsAndOpenFailure() {
        QTemporaryDir temporary; const auto folder = temporary.filePath(QString(65, 'a')); QVERIFY(QDir().mkpath(folder)); const auto path = folder + "/document.md"; QVERIFY(fileFixtures::write(path, sample()));
        FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document); Window owned{new SingleFileWindow(&manager)}; auto *window = owned.value.data(); window->setDocument(document); window->resize(560, 480); window->show(); QApplication::setActiveWindow(window);
        auto *label = window->findChild<ElidedLabel *>("documentPath"); QCOMPARE(label->fullText(), folder); QCOMPARE(label->accessibleName(), folder); QTRY_VERIFY(label->text().contains(QChar(0x2026)));
        QSignalSpy copied(window, &SingleFileWindow::copyPathRequested); window->findChild<QAction *>("copyPath")->trigger(); QCOMPARE(copied.count(), 1); QCOMPARE(copied.first().first().toString(), path);
        auto *readMode = window->findChild<SharedButton *>("readMode"), *editMode = window->findChild<SharedButton *>("editMode"); readMode->setFocus(Qt::OtherFocusReason); QTest::keyClick(readMode, Qt::Key_Right); QVERIFY(window->editing()); QVERIFY(editMode->isChecked()); QVERIFY(!editMode->keyboardOutline());
        QTest::mouseClick(readMode, Qt::LeftButton); QVERIFY(!window->editing());
        auto *more = window->findChild<SharedButton *>("moreActions"); auto *menu = more->menu();
        QTimer::singleShot(100, menu, [menu] { QTest::keyClick(menu, Qt::Key_Escape); }); more->setFocus(Qt::OtherFocusReason); more->showMenu();
        QTRY_VERIFY(!menu->isVisible()); QTRY_VERIFY(more->hasFocus()); QVERIFY(!more->keyboardOutline());
        QTimer::singleShot(100, qApp, [] { if (auto *dialog = qobject_cast<QFileDialog *>(qApp->activeModalWidget())) dialog->reject(); });
        QSignalSpy opening(window, &SingleFileWindow::openRequested); window->findChild<QAction *>("openAction")->trigger(); QCOMPARE(opening.count(), 0); QCOMPARE(document->session()->snapshot().utf8, sample());
        Window empty{new SingleFileWindow(&manager)}; empty.value->showOpenError(path, QStringLiteral("没有读取权限")); empty.value->show(); empty.value->resize(600, 500); QTest::qWait(20);
        QCOMPARE(empty.value->findChild<ElidedLabel *>("documentStatus")->fullText(), QStringLiteral("打开失败")); QVERIFY(!empty.value->findChild<QAction *>("reloadAction")->isEnabled()); capture(empty.value, "open-error");
    }
    void samplingDoesNotMultiplyLogicalGeometry() {
        QTemporaryDir temporary; double baseDpr = 0;
        for (const auto &scale : {"1", "1.5", "2"}) {
            QProcess process; auto environment = QProcessEnvironment::systemEnvironment(); environment.insert("QT_SCALE_FACTOR", scale); environment.insert("QT_MESSAGE_PATTERN", "%{type}: %{message}"); process.setProcessEnvironment(environment);
            const auto report = temporary.filePath(QString(scale) + ".json"); process.start(QCoreApplication::applicationFilePath(), {"--scale-probe", report});
            QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 60000);
            const auto diagnostics = process.readAllStandardOutput() + process.readAllStandardError();
            const auto evidence = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
            if (!evidence.isEmpty()) {
                QVERIFY(QDir().mkpath(evidence + "/standalone"));
                QVERIFY(fileFixtures::write(evidence + "/standalone/geometry-" + scale + ".json", read(report)));
                QVERIFY(fileFixtures::write(evidence + "/standalone/sampling-" + scale + ".log", diagnostics));
            }
            QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, diagnostics.constData());
            const auto lower = diagnostics.toLower(); QVERIFY2(!lower.contains("critical:") && !lower.contains("fatal:") && !diagnostics.contains("ASSERT"), diagnostics.constData());
            const auto object = QJsonDocument::fromJson(read(report)).object(); QVERIFY(!object.isEmpty()); QCOMPARE(object["header"].toInt(), 48); QCOMPARE(object["toolbar"].toInt(), 48); QCOMPARE(object["footer"].toInt(), 26); QCOMPARE(object["button"].toInt(), 32); QCOMPARE(object["font"].toInt(), 14);
            if (baseDpr == 0) baseDpr = object["dpr"].toDouble(); QVERIFY(qAbs(object["dpr"].toDouble() / baseDpr - QString(scale).toDouble()) < 0.1);
        }
    }
};

int main(int argc, char **argv) {
    initializeGuiRuntime(); QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); configureGuiApplication(); SourceEditor::installAccessibility();
    if (app.arguments().size() == 3 && app.arguments()[1] == "--scale-probe") {
        const auto report = app.arguments()[2]; QTemporaryDir temporary; const auto path = temporary.filePath("sample.md"); if (!fileFixtures::write(path, sample())) return 2;
        Appearance appearance; appearance.setPreferences("light", "graphite"); FileManager manager(nullptr, {}, false); auto *document = open(manager, path); if (!document) return 3;
        Window owned{new SingleFileWindow(&manager, &appearance)}; auto *window = owned.value.data(); window->setDocument(document); window->show(); QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, window, [&] {
            if (!painted(window->preview(), &appearance)) return;
            const QJsonObject result{{"header", window->findChild<QWidget *>("documentHeader")->height()}, {"toolbar", window->findChild<QWidget *>("documentToolbar")->height()}, {"footer", window->findChild<QWidget *>("documentFooter")->height()}, {"button", window->findChild<SharedButton *>("saveButton")->height()}, {"font", window->findChild<SharedButton *>("saveButton")->font().pixelSize()}, {"dpr", window->devicePixelRatioF()}};
            const bool written = fileFixtures::write(report, QJsonDocument(result).toJson()); capture(window, "sampling-" + qEnvironmentVariable("QT_SCALE_FACTOR")); timer.stop(); app.exit(written ? 0 : 4);
        });
        timer.start(50); QTimer::singleShot(45000, &app, [&] { app.exit(5); }); return app.exec();
    }
    StandaloneVisualTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "standalone_visual_tests.moc"
