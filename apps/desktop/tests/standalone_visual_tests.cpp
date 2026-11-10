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
#include "UiFonts.h"
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
#include <QScrollBar>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineScript>
#include <QScreen>
#include <QTextLayout>
#include <QGlyphRun>
#include <QRawFont>
#include <QFontInfo>
#include <QPainter>
#ifdef Q_OS_WIN
#include <dwmapi.h>
#endif
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
QVariant evaluate(QWebEnginePage *page, const QString &script) {
    struct Result { QVariant value; bool ready = false; }; auto result = std::make_shared<Result>();
    page->runJavaScript(script, QWebEngineScript::ApplicationWorld, [result](const QVariant &value) { result->value = value; result->ready = true; });
    QElapsedTimer timer; timer.start(); while (!result->ready && timer.elapsed() < 10000) QTest::qWait(10); return result->value;
}
bool contained(SingleFileWindow *window, QWidget *child) { return child && window->rect().contains(QRect(child->mapTo(window, QPoint()), child->size())); }
bool noOuterRing(QWidget *owner, QWidget *button, QColor background) {
    const auto pixels = owner->grab().toImage(); const auto dpr = owner->devicePixelRatioF();
    const QRect area(button->mapTo(owner, QPoint()), button->size());
    for (int y = area.top() - 3; y <= area.bottom() + 3; ++y) for (int x = area.left() - 3; x <= area.right() + 3; ++x) {
        if (area.contains(x, y) || !owner->rect().contains(x, y)) continue;
        const auto color = pixels.pixelColor(qRound(x * dpr), qRound(y * dpr));
        if (qAbs(color.red() - background.red()) + qAbs(color.green() - background.green()) + qAbs(color.blue() - background.blue()) > 40) return false;
    }
    return true;
}
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
    void popupShadowIsSoftSymmetricAndUnclipped() {
        for (const qreal dpr : {1.0, 1.5, 2.0, 3.0}) {
            QImage image(QSize(248 * dpr, 188 * dpr), QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(dpr); image.fill(Qt::transparent);
            { QPainter painter(&image); paintPopupShell(painter, QRectF(24, 24, 200, 140), Qt::white, QColor("#DDE2E8"), 12, 18); }
            for (int x = 0; x < image.width(); ++x) { QVERIFY(image.pixelColor(x, 0).alpha() <= 1); QVERIFY(image.pixelColor(x, image.height() - 1).alpha() <= 1); }
            for (int y = 0; y < image.height(); ++y) { QVERIFY(image.pixelColor(0, y).alpha() <= 1); QVERIFY(image.pixelColor(image.width() - 1, y).alpha() <= 1); }
            int previous = 255;
            for (int distance = 2; distance < 20; ++distance) {
                const int left = qRound(24 * dpr) - qRound(distance * dpr), right = qRound(224 * dpr) + qRound(distance * dpr) - 1, y = qRound(96 * dpr);
                const int alpha = image.pixelColor(left, y).alpha(); QVERIFY(alpha <= 60); QVERIFY(alpha <= previous); previous = alpha; QVERIFY(qAbs(alpha - image.pixelColor(right, y).alpha()) <= 2);
            }
        }
    }
    void platformFontsResolveChineseSans() {
        FileManager manager(nullptr, {}, false); Window owned{new SingleFileWindow(&manager)};
        const QList<QFont> fonts{uiFont(), sourceFont(), owned.value->findChild<SharedButton *>("saveButton")->font(), owned.value->findChild<QLabel *>("documentNotice")->font()};
        for (const auto &font : fonts) {
            QTextLayout layout(QStringLiteral("中文界面"), font); layout.beginLayout(); auto line = layout.createLine(); line.setLineWidth(300); layout.endLayout();
            QVERIFY(!layout.glyphRuns().isEmpty());
            for (const auto &run : layout.glyphRuns()) {
                qInfo() << "font-request=" << font.families() << "resolved=" << QFontInfo(font).family() << "han=" << run.rawFont().familyName();
                QVERIFY(!run.rawFont().familyName().contains("SimSun", Qt::CaseInsensitive));
#ifdef Q_OS_WIN
                QVERIFY(run.rawFont().familyName().contains("YaHei", Qt::CaseInsensitive) || run.rawFont().familyName().contains(QStringLiteral("微软雅黑")));
#endif
            }
        }
    }
    void vectorIconPixmapTransparency() {
        for (const auto &name : {"document", "clock", "warning", "check-circle", "lock", "status-dirty"}) for (const qreal dpr : {1.0, 1.5, 2.0}) {
            const auto pixmap = themedIcon(name, QColor("#374151")).pixmap(QSize(16, 16), dpr);
            QVERIFY2(!pixmap.isNull(), name); QCOMPARE(pixmap.devicePixelRatioF(), dpr);
            const auto pixels = pixmap.toImage(); QCOMPARE(pixels.size(), QSize(qRound(16 * dpr), qRound(16 * dpr)));
            for (const auto &corner : {QPoint(0, 0), QPoint(pixels.width() - 1, 0), QPoint(0, pixels.height() - 1), QPoint(pixels.width() - 1, pixels.height() - 1)})
                QCOMPARE(pixels.pixelColor(corner).alpha(), 0);
            int opaque = 0; for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x) if (pixels.pixelColor(x, y).alpha() > 200) ++opaque;
            QVERIFY2(opaque > 10, name);
        }
    }
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
        const auto logoCenter = inkCenter(pixels, header->devicePixelRatioF(), 40, 103, appearance.color("sidebar"));
        const auto titleCenter = inkCenter(pixels, header->devicePixelRatioF(), 136, 440, appearance.color("sidebar"));
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
        QCOMPARE(window->size(), QSize(560, 480));
        for (const auto &name : {"readMode", "editMode", "saveButton", "moreActions", "contentZoom", "fileLocation"}) QVERIFY2(contained(window, window->findChild<QWidget *>(name)), name);
        QCOMPARE(window->findChild<SharedButton *>("saveButton")->font().pixelSize(), 14);
        auto *scrollbar = window->editor()->verticalScrollBar(); QTRY_VERIFY(scrollbar->isVisible()); QCOMPARE(scrollbar->width(), 12);
        QCOMPARE(scrollbar->grab().toImage().pixelColor(0, 3).rgb(), appearance.color("background").rgb());
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
        layout->setContentsMargins(12, 12, 12, 12); layout->setSpacing(12); auto palette = panel.palette(); palette.setColor(QPalette::Window, appearance.color("background")); panel.setPalette(palette); panel.setAutoFillBackground(true);
        auto *first = new SharedButton("First", "first", &appearance, &panel), *second = new SharedButton("Second", "second", &appearance, &panel); layout->addWidget(first); layout->addWidget(second);
        panel.show(); QApplication::setActiveWindow(&panel); first->setFocus(Qt::OtherFocusReason); InputModality::forApplication()->clear();
        QVERIFY(!first->keyboardOutline()); QTRY_VERIFY(noOuterRing(&panel, first, appearance.color("background"))); QTest::keyClick(first, Qt::Key_Tab); QTRY_VERIFY(second->hasFocus()); QVERIFY(second->keyboardOutline());
        QSignalSpy clicked(second, &QPushButton::clicked); QTest::keyClick(second, Qt::Key_Space); QCOMPARE(clicked.count(), 1); QVERIFY(second->hasFocus()); QVERIFY(!second->keyboardOutline()); QTRY_VERIFY(noOuterRing(&panel, second, appearance.color("background")));
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
        QVERIFY(owned.value->findChild<QWidget *>("previewFailure")->isVisible());
        QVERIFY(owned.value->findChild<SharedButton *>("retryPreview")->isVisible());
        QCOMPARE(owned.value->findChild<QLabel *>("documentStatusIcon")->property("semanticColor").toString(), appearance.color("textSecondary").name());
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
            QCOMPARE(dialog->objectName(), QString("unsavedConfirmation")); QCOMPARE(dialog->findChild<QWidget *>("dialogSurface")->width(), 480);
            QVERIFY(dialog->windowFlags().testFlag(Qt::FramelessWindowHint)); QVERIFY(dialog->findChild<SharedButton *>("dialogClose"));
            QVERIFY(owned.value->findChild<QWidget *>("dialogDimOverlay")->isVisible()); QVERIFY(dialog->findChild<QLabel *>("dialogDirtyIndicator"));
            QVERIFY(dialog->findChild<QWidget *>("dialogFile")); QVERIFY(dialog->findChildren<QScrollArea *>().isEmpty());
            auto *cancel = dialog->findChild<SharedButton *>("choice0"); auto *save = dialog->findChild<SharedButton *>("choice1"); auto *discard = dialog->findChild<SharedButton *>("choice2");
            QVERIFY(cancel && save && discard); QVERIFY(discard->x() < cancel->x() && cancel->x() < save->x());
            QVERIFY(!cancel->keyboardOutline()); QVERIFY(noOuterRing(dialog, cancel, appearance.color("background"))); capture(dialog, "unsaved-close-" + theme); QTest::keyClick(dialog, Qt::Key_Escape);
        });
        timer.start(); QTimer::singleShot(10000, &timer, [] { if (auto *dialog = qobject_cast<QDialog *>(qApp->activeModalWidget())) dialog->reject(); });
        QVERIFY(!owned.value->close()); QVERIFY(inspected); QVERIFY(owned.value->isVisible());
        QCOMPARE(document->session()->snapshot().utf8, edited.utf8); QVERIFY(document->dirty());
        QTRY_COMPARE(owned.value->focusWidget(), editor);
        qInfo() << "dialog-return ownerActive=" << owned.value->isActiveWindow() << "logicalFocus=" << owned.value->focusWidget()->objectName();
        // Xvfb has no window manager. Verify the stored target first, then
        // synthesize Qt activation as in the other focus tests; no OS claim.
        QApplication::setActiveWindow(owned.value); QTRY_VERIFY(editor->hasFocus());
        QCOMPARE(read(path), sample());
    }
    void menusModesLongPathsAndOpenFailure() {
        QTemporaryDir temporary; const auto folder = temporary.filePath(QString(65, 'a')); QVERIFY(QDir().mkpath(folder)); const auto path = folder + "/document.md"; QVERIFY(fileFixtures::write(path, sample()));
        FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document); Window owned{new SingleFileWindow(&manager)}; auto *window = owned.value.data(); window->setDocument(document); window->resize(560, 480); window->show(); QApplication::setActiveWindow(window);
        auto *label = window->findChild<ElidedLabel *>("documentPath"); QCOMPARE(label->fullText(), folder); QCOMPARE(label->accessibleName(), folder); QTRY_VERIFY(label->text().contains(QChar(0x2026)));
        QSignalSpy copied(window, &SingleFileWindow::copyPathRequested); window->findChild<QAction *>("copyPath")->trigger(); QCOMPARE(copied.count(), 1); QCOMPARE(copied.first().first().toString(), path);
        auto *readMode = window->findChild<SharedButton *>("readMode"), *editMode = window->findChild<SharedButton *>("editMode"); readMode->setFocus(Qt::OtherFocusReason); QTest::keyClick(readMode, Qt::Key_Right); QVERIFY(window->editing()); QVERIFY(editMode->isChecked()); QVERIFY(!editMode->keyboardOutline());
        QTest::mouseClick(readMode, Qt::LeftButton); QVERIFY(!window->editing());
        window->findChild<SharedButton *>("fileLocation")->setFocus(Qt::OtherFocusReason); QTest::keyClick(window->findChild<SharedButton *>("fileLocation"), Qt::Key_Tab);
        QTRY_VERIFY(readMode->hasFocus()); QVERIFY(readMode->keyboardOutline());
        QVERIFY(!window->findChild<QAction *>("saveAction")->isEnabled()); readMode->click(); QVERIFY(!readMode->keyboardOutline()); QVERIFY(readMode->hasFocus()); QVERIFY(!document->saving());
        auto *more = window->findChild<SharedButton *>("moreActions"); auto *menu = more->menu();
        QTimer::singleShot(100, menu, [menu] { QTest::keyClick(menu, Qt::Key_Escape); }); more->setFocus(Qt::OtherFocusReason); more->showMenu();
        QTRY_VERIFY(!menu->isVisible()); QTRY_COMPARE(window->focusWidget(), more);
        qInfo() << "menu-return ownerActive=" << window->isActiveWindow() << "logicalFocus=" << window->focusWidget()->objectName();
        QApplication::setActiveWindow(window); QTRY_VERIFY(more->hasFocus()); QVERIFY(!more->keyboardOutline());
        QTimer::singleShot(100, qApp, [] { if (auto *dialog = qobject_cast<QFileDialog *>(qApp->activeModalWidget())) dialog->reject(); });
        QSignalSpy opening(window, &SingleFileWindow::openRequested); window->findChild<QAction *>("openAction")->trigger(); QCOMPARE(opening.count(), 0); QCOMPARE(document->session()->snapshot().utf8, sample());
        Window empty{new SingleFileWindow(&manager)}; empty.value->showOpenError(path, QStringLiteral("没有读取权限")); empty.value->show(); empty.value->resize(600, 500); QTest::qWait(20);
        QCOMPARE(empty.value->findChild<ElidedLabel *>("documentStatus")->fullText(), QStringLiteral("打开失败")); QVERIFY(!empty.value->findChild<QAction *>("reloadAction")->isEnabled()); capture(empty.value, "open-error");
    }
    void menuShellsAndNestedNavigation_data() { readOnlyFeedback_data(); }
    void menuShellsAndNestedNavigation() {
        QFETCH(QString, theme); Appearance appearance; QVERIFY(appearance.setPreferences(theme, "cobalt")); FileManager manager(nullptr, {}, false);
        Window owned{new SingleFileWindow(&manager, &appearance)}; owned.value->show();
        auto inspect = [&](SharedMenu *menu, const QString &name) {
            QVERIFY(menu); QVERIFY(menu->isVisible()); QVERIFY(menu->testAttribute(Qt::WA_TranslucentBackground));
            capture(menu, "menu-" + theme + '-' + name);
            QVERIFY(!menu->graphicsEffect());
            const auto pixels = menu->grab().toImage(); const auto dpr = menu->devicePixelRatioF(); const auto surface = menu->surfaceRect();
            QVERIFY(pixels.hasAlphaChannel()); QCOMPARE(pixels.pixelColor(0, 0).alpha(), 0);
            QVERIFY(pixels.pixelColor(qRound(surface.left() * dpr), qRound(surface.top() * dpr)).alpha() < 255);
            const auto shadow = pixels.pixelColor(qRound((surface.left() - 3) * dpr), qRound(surface.center().y() * dpr));
            QVERIFY2(shadow.alpha() > 0 && shadow.alpha() < 255, qPrintable(QString("shadow alpha=%1").arg(shadow.alpha())));
            const int leftEdge = qRound(surface.x() * dpr), rightEdge = qRound((surface.x() + surface.width()) * dpr);
            for (int distance = qRound(2 * dpr); distance < qRound(12 * dpr); ++distance) { const auto left = pixels.pixelColor(leftEdge - distance, qRound(surface.center().y() * dpr)).alpha(), right = pixels.pixelColor(rightEdge + distance - 1, qRound(surface.center().y() * dpr)).alpha(); QVERIFY2(right < 65, qPrintable(QString("right shadow alpha=%1").arg(right))); QVERIFY(qAbs(left - right) <= 4); }
            QCOMPARE(pixels.pixelColor(qRound((surface.right() - 2) * dpr), qRound(surface.center().y() * dpr)).rgb(), appearance.color("surface").rgb());
            QVERIFY(menu->screen()->availableGeometry().contains(menu->geometry())); capture(menu, "menu-" + theme + '-' + name);
        };
        auto *root = static_cast<SharedMenu *>(owned.value->findChild<SharedButton *>("moreActions")->menu());
        root->popup(owned.value->mapToGlobal(QPoint(400, 90))); QTRY_VERIFY(root->isVisible()); inspect(root, "document");
        auto *appearanceMenu = static_cast<SharedMenu *>(owned.value->findChild<QMenu *>("appearanceMenu"));
        root->setActiveAction(appearanceMenu->menuAction()); QTest::keyClick(root, Qt::Key_Right); QTRY_VERIFY(appearanceMenu->isVisible()); inspect(appearanceMenu, "appearance");
        auto *accents = static_cast<SharedMenu *>(owned.value->findChild<QMenu *>("accentMenu")); appearanceMenu->setActiveAction(accents->menuAction()); QTest::keyClick(appearanceMenu, Qt::Key_Right); QTRY_VERIFY(accents->isVisible()); inspect(accents, "accent");
        QCOMPARE(accents->actions().size(), 6); int checked = 0; for (auto *item : accents->actions()) { QVERIFY(!item->icon().isNull()); checked += item->isChecked(); } QCOMPARE(checked, 1);
        QTest::keyClick(accents, Qt::Key_Escape); QTRY_VERIFY(!accents->isVisible()); if (appearanceMenu->isVisible()) QTest::keyClick(appearanceMenu, Qt::Key_Escape); QTRY_VERIFY(!appearanceMenu->isVisible()); if (root->isVisible()) QTest::keyClick(root, Qt::Key_Escape); QTRY_VERIFY(!root->isVisible());
        for (const auto &name : {"pathMenu", "contentZoom"}) { auto *menu = QString::fromLatin1(name) == "pathMenu" ? owned.value->findChild<SharedMenu *>(name) : static_cast<SharedMenu *>(owned.value->findChild<SharedButton *>(name)->menu()); QVERIFY(menu); menu->popup(owned.value->screen()->availableGeometry().bottomRight()); QTRY_VERIFY(menu->isVisible()); inspect(menu, name); QTest::keyClick(menu, Qt::Key_Escape); }
    }
    void missingFragmentShowsNoticeWithoutReplacingThePreview() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const auto path = temporary.filePath("fragments.md");
        const QByteArray source("[missing](#missing) [ok](#ok)\n\n# ok\n"); QVERIFY(fileFixtures::write(path, source));
        Appearance appearance; FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        Window owned{new SingleFileWindow(&manager, &appearance)}; owned.value->setDocument(document); owned.value->resize(800, 600); owned.value->show();
        auto *preview = owned.value->preview(); QSignalSpy ready(preview, &MarkdownPreview::previewReady), moved(preview, &MarkdownPreview::fragmentNavigated);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 20000); auto *page = preview->view()->page();
        owned.value->reportHostMessage(QStringLiteral("之前的提示"));
        const auto click = [&](int index) {
            const auto point = evaluate(page, QStringLiteral("(()=>{const a=document.querySelectorAll('a.link-placeholder')[%1];if(!a)return null;const r=a.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()").arg(index)).toMap();
            if (point.isEmpty()) return false;
            QTest::mouseClick(preview->view()->focusProxy() ? preview->view()->focusProxy() : preview->view(), Qt::LeftButton, Qt::NoModifier,
                QPoint(qRound(point.value("x").toDouble()), qRound(point.value("y").toDouble())));
            return true;
        };
        QVERIFY(click(0)); auto *notice = owned.value->findChild<QLabel *>("documentNotice"); QVERIFY(notice);
        QTRY_VERIFY_WITH_TIMEOUT(notice->isVisible() && notice->text().contains(QStringLiteral("未找到标题")), 10000);
        QVERIFY(preview->isVisible()); auto *close = owned.value->findChild<SharedButton *>("closeNotice"); QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton); QTRY_COMPARE(notice->text(), QStringLiteral("之前的提示"));
        QVERIFY(click(0)); QTRY_VERIFY_WITH_TIMEOUT(notice->text().contains(QStringLiteral("未找到标题")), 10000);
        QVERIFY(click(1)); QTRY_COMPARE_WITH_TIMEOUT(moved.size(), 1, 10000);
        QTRY_COMPARE(notice->text(), QStringLiteral("之前的提示"));
        owned.value->reportHostMessage({}); QTRY_VERIFY(!notice->isVisible());
        QCOMPARE(document->session()->snapshot().utf8, source); QVERIFY(!document->dirty());
    }
    void readerComponentsCopyAndHorizontalScroll() {
        QTemporaryDir temporary; const auto path = temporary.filePath("components.md");
        const auto code = QStringLiteral("const text = '<script>中文😀</script>';\n") + QString(240, 'x') + '\n';
        const auto text = "# Components\n\n- [x] 完成\n- [ ] 待办\n\n```js\n" + code.toUtf8() + "```\n\n|列|" + QByteArray(180, 'y') + "|\n|--|--|\n|一|二|\n";
        QVERIFY(fileFixtures::write(path, text)); Appearance appearance; FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        Window owned{new SingleFileWindow(&manager, &appearance)}; owned.value->setDocument(document); owned.value->show(); auto *preview = owned.value->preview(); QSignalSpy ready(preview, &MarkdownPreview::previewReady); QSignalSpy copied(preview, &MarkdownPreview::copyTextRequested);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 20000); auto *page = preview->view()->page();
        qInfo() << "reader-font=" << evaluate(page, "getComputedStyle(document.body).fontFamily").toString();
        QVERIFY(evaluate(page, "(()=>{let e=document.querySelector('pre');return e.scrollWidth>e.clientWidth&&getComputedStyle(e).whiteSpace==='pre'})()").toBool());
        QVERIFY(evaluate(page, "(()=>{let e=document.querySelector('.table-scroll');return e.scrollWidth>e.clientWidth})()").toBool());
        QVERIFY(evaluate(page, "(()=>{let e=document.querySelector('input[type=checkbox]');return getComputedStyle(e.parentElement).listStyleType==='none'&&getComputedStyle(e).width==='16px'})()").toBool());
        QCOMPARE(evaluate(page, "document.querySelector('pre code').textContent").toString(), code);
        QVERIFY(evaluate(page, "document.querySelectorAll('.syntax-keyword').length>0").toBool());
        const auto point = evaluate(page, "(()=>{const r=document.querySelector('button[data-xdnote-code]').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()").toMap();
        QVERIFY(!point.isEmpty()); QTest::mouseClick(preview->view()->focusProxy() ? preview->view()->focusProxy() : preview->view(), Qt::LeftButton, Qt::NoModifier, QPoint(qRound(point["x"].toDouble()), qRound(point["y"].toDouble())));
        QTRY_COMPARE_WITH_TIMEOUT(copied.count(), 1, 10000); QCOMPARE(copied.first().first().toString(), code); QCOMPARE(document->session()->snapshot().utf8, text); QVERIFY(!document->dirty());
        QVERIFY(!page->settings()->testAttribute(QWebEngineSettings::JavascriptEnabled)); QVERIFY(!page->settings()->testAttribute(QWebEngineSettings::JavascriptCanAccessClipboard));
        capture(owned.value, "reader-code-tasks-table");
        QTimer timer; timer.setInterval(50); bool inspected = false; connect(&timer, &QTimer::timeout, this, [&] { auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!menu) return; timer.stop(); inspected = true; QCOMPARE(menu->objectName(), QString("readingContextMenu")); QCOMPARE(menu->actions().size(), 2); QCOMPARE(menu->actions()[0]->text(), QStringLiteral("复制")); QCOMPARE(menu->actions()[1]->text(), QStringLiteral("全选")); capture(menu, "reading-context-menu"); menu->close(); }); timer.start();
        emit preview->view()->customContextMenuRequested(QPoint(80, 80)); QVERIFY(inspected);
    }
    void headingsLoadingErrorsAndSaveState() {
        FileManager manager(nullptr, {}, false); Appearance appearance; Window empty{new SingleFileWindow(&manager, &appearance)}; empty.value->show();
        const auto intent = QUuid::createUuid(); QSignalSpy cancelled(empty.value, &SingleFileWindow::cancelOpenRequested); empty.value->setLoading("/sample.md", intent);
        QTRY_VERIFY(empty.value->findChild<SharedSpinner *>("busySpinner")->running());
        QVERIFY(empty.value->findChild<SharedButton *>("cancelOpen")->isVisible()); capture(empty.value, "opening"); empty.value->findChild<SharedButton *>("cancelOpen")->click(); QCOMPARE(cancelled.count(), 1); QCOMPARE(cancelled.first().first().toUuid(), intent);
        empty.value->showOpenError("/sample.md", QStringLiteral("没有读取权限")); QCOMPARE(empty.value->findChild<QLabel *>("openStateTitle")->text(), QStringLiteral("无法打开文件")); QVERIFY(empty.value->findChild<SharedButton *>("welcomeOpen")->isVisible()); QVERIFY(!empty.value->findChild<SharedButton *>("cancelOpen")->isVisible());
        QTemporaryDir temporary; const auto path = temporary.filePath("headings.md"); QVERIFY(fileFixtures::write(path, "## original\n")); auto *document = open(manager, path); QVERIFY(document);
        Window owned{new SingleFileWindow(&manager, &appearance)}; owned.value->setDocument(document); owned.value->show(); QVERIFY(!owned.value->findChild<SharedButton *>("saveButton")->isEnabled()); owned.value->setEditing(true);
        auto *headings = owned.value->findChild<QMenu *>("headingMenu"); QVERIFY(headings); QCOMPARE(headings->actions().size(), 6); headings->actions()[3]->trigger(); QCOMPARE(document->session()->snapshot().utf8, QByteArray("#### original\n")); QVERIFY(owned.value->findChild<SharedButton *>("saveButton")->isEnabled()); QCOMPARE(document->session()->undo(), EditResult::Applied); QVERIFY(!owned.value->findChild<SharedButton *>("saveButton")->isEnabled());
        emit owned.value->preview()->previewError(QStringLiteral("预览进程已停止")); owned.value->setEditing(false); QVERIFY(owned.value->findChild<QWidget *>("previewFailure")->isVisible()); capture(owned.value, "preview-error"); owned.value->findChild<SharedButton *>("viewSource")->click(); QVERIFY(owned.value->editing()); QVERIFY(owned.value->editor()->isVisible()); QCOMPARE(document->session()->snapshot().utf8, QByteArray("## original\n"));
    }
    void windowsSingleCaptionAndNativeHitRegions() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires the Windows platform plugin");
        FileManager manager(nullptr, {}, false); Window owned{new SingleFileWindow(&manager)}; auto *window = owned.value.data(); QCOMPARE(window->size(), QSize(800, 820)); window->show(); QTest::qWait(100);
        auto hwnd = reinterpret_cast<HWND>(window->winId()); RECT outer{}, client{}; QVERIFY(GetWindowRect(hwnd, &outer)); QVERIFY(GetClientRect(hwnd, &client)); POINT origin{}; QVERIFY(ClientToScreen(hwnd, &origin));
        QVERIFY2(origin.y - outer.top <= 2, "Native caption must not be added above the 48 logical pixel application titlebar");
        QCOMPARE(window->findChild<QWidget *>("documentHeader")->height(), 48); QVERIFY(window->width() <= 800 && window->width() >= 560); QVERIFY(window->height() <= 820 && window->height() >= 480);
        const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE); QVERIFY((style & WS_CAPTION) == WS_CAPTION); QVERIFY(style & WS_THICKFRAME); QVERIFY(!(style & WS_POPUP));
        BOOL composition = FALSE; if (SUCCEEDED(DwmIsCompositionEnabled(&composition)) && composition) { BOOL enabled = FALSE; QVERIFY(SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_ENABLED, &enabled, sizeof(enabled)))); QVERIFY(enabled); }
        DWORD corner = 0; if (SUCCEEDED(DwmGetWindowAttribute(hwnd, 33, &corner, sizeof(corner)))) QCOMPARE(corner, DWORD(2));
        auto hit = [&](QPoint point) { const qreal dpr = window->devicePixelRatioF(); POINT native{qRound(point.x() * dpr), qRound(point.y() * dpr)}; ClientToScreen(hwnd, &native); return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(native.x, native.y)); };
        QCOMPARE(hit(QPoint(180, 24)), LRESULT(HTCAPTION)); auto *maximize = window->findChild<SharedButton *>("captionMaximize"); QVERIFY(maximize); QCOMPARE(hit(maximize->mapTo(window, maximize->rect().center())), LRESULT(HTMAXBUTTON));
        auto *close = window->findChild<SharedButton *>("captionClose"); QVERIFY(close); QCOMPARE(hit(close->mapTo(window, close->rect().center())), LRESULT(HTCLIENT));
        auto *header = window->findChild<QWidget *>("documentHeader"); auto *minimize = window->findChild<SharedButton *>("captionMinimize"); QVERIFY(minimize);
        for (auto *button : {minimize, maximize, close}) { QCOMPARE(button->size(), QSize(48, 48)); QCOMPARE(button->mapTo(header, QPoint()).y(), 0); }
        QCOMPARE(close->mapTo(header, QPoint(close->width(), 0)).x(), header->width()); QCOMPARE(minimize->x() + 48, maximize->x()); QCOMPARE(maximize->x() + 48, close->x());
        for (auto *button : {minimize, maximize, close}) {
            button->setNativeHover(false); const auto normal = button->grab().toImage(); button->setNativeHover(true); const auto hover = button->grab().toImage(); QVERIFY(normal != hover); QCOMPARE(hover.pixelColor(2, 2).rgb(), hover.pixelColor(2, hover.height() / 2).rgb()); button->setNativeHover(false);
        }
        POINT hoverPoint{0, 0}; ClientToScreen(hwnd, &hoverPoint); SendMessageW(hwnd, WM_NCMOUSEMOVE, HTMAXBUTTON, MAKELPARAM(hoverPoint.x, hoverPoint.y)); const auto hover = maximize->grab().toImage(); SendMessageW(hwnd, WM_NCMOUSELEAVE, 0, 0); QVERIFY(hover != maximize->grab().toImage());
        const auto initial = window->size(); const qreal dpr = window->devicePixelRatioF(); QCOMPARE(client.right, qRound(initial.width() * dpr)); QCOMPARE(client.bottom, qRound(initial.height() * dpr));
        window->showMaximized(); QTRY_VERIFY(window->isMaximized()); MONITORINFO monitor{sizeof(MONITORINFO)}; QVERIFY(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)); POINT maximizedOrigin{}; QVERIFY(ClientToScreen(hwnd, &maximizedOrigin)); RECT maximizedClient{}; QVERIFY(GetClientRect(hwnd, &maximizedClient));
        QVERIFY(qAbs(maximizedOrigin.x - monitor.rcWork.left) <= 2); QVERIFY(qAbs(maximizedOrigin.y - monitor.rcWork.top) <= 2); QVERIFY(qAbs(maximizedClient.right - (monitor.rcWork.right - monitor.rcWork.left)) <= 2); QVERIFY(qAbs(maximizedClient.bottom - (monitor.rcWork.bottom - monitor.rcWork.top)) <= 2);
        window->showNormal(); QTRY_VERIFY(!window->isMaximized()); QCOMPARE(window->size(), initial);
        const auto evidence = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE"); if (!evidence.isEmpty()) {
            QVERIFY(QDir().mkpath(evidence + "/standalone")); window->screen()->grabWindow(window->winId()).save(evidence + "/standalone/native-full-window.png");
            // Capture the external frame only over an owned solid backdrop.
            // The widget grab above cannot establish DWM border/shadow output.
            window->resize(700, 540); const auto available = window->screen()->availableGeometry(); window->move(available.center() - window->rect().center());
            QWidget backdrop(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint); auto palette = backdrop.palette(); palette.setColor(QPalette::Window, QColor("#CFD8E5")); backdrop.setPalette(palette); backdrop.setAutoFillBackground(true); backdrop.setGeometry(available); backdrop.show();
            struct RestoreOrder { HWND hwnd; ~RestoreOrder() { SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); } } restoreOrder{hwnd};
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); window->raise(); QTest::qWait(200);
            auto *screen = window->screen(); const auto crop = window->frameGeometry().adjusted(-24, -24, 24, 24).translated(-screen->geometry().topLeft());
            RECT native{}; QVERIFY(GetWindowRect(hwnd, &native)); const int margin = qRound(22 * window->devicePixelRatioF()); bool owned = true;
            for (const POINT point : {POINT{native.left - margin, native.top - margin}, POINT{native.right + margin, native.top - margin}, POINT{native.left - margin, native.bottom + margin}, POINT{native.right + margin, native.bottom + margin}}) owned = owned && WindowFromPoint(point) == reinterpret_cast<HWND>(backdrop.winId());
            if (!owned) { qInfo() << "External DWM capture unavailable: owned backdrop is occluded"; return; }
            const auto frame = screen->grabWindow(0, crop.x(), crop.y(), crop.width(), crop.height()); QVERIFY(!frame.isNull()); QVERIFY(frame.save(evidence + "/standalone/native-dwm-frame.png"));
        }
#else
        QSKIP("Windows custom caption is platform specific");
#endif
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
