#include "Preferences.h"
#include "Appearance.h"
#include "FileApplication.h"
#include "FileManager.h"
#include "AutoSaveController.h"
#include "SettingsWindow.h"
#include "WindowGeometry.h"
#include "SingleFileWindow.h"
#include "WorkspaceWindow.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "NativePreviewView.h"
#include "SharedWidgets.h"
#include "GuiRuntime.h"
#include "VisualTokens.h"
#include "PreviewAnalysis.h"
#include "file_fixtures.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <QScrollBar>
#include <QScrollArea>
#include <QScreen>
#include <cmath>
#include <atomic>
#include <future>

using namespace xdnote;
namespace {
QByteArray bytes(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
SingleFileWindow *standalone(const QString &path) {
    for (auto *widget : QApplication::topLevelWidgets()) if (auto *pane = qobject_cast<SingleFileWindow *>(widget)) if (pane->document() && pane->document()->path() == path) return pane;
    return nullptr;
}
bool select(SettingsWindow *window, const char *name, const QVariant &value) {
    auto *button = window->findChild<SharedButton *>(QString::fromLatin1(name)); if (!button || !button->menu()) return false;
    for (auto *action : button->menu()->actions()) if (action->data() == value) { action->trigger(); return true; } return false;
}
}
class SettingsTests final : public QObject {
    Q_OBJECT
private slots:
    void missingLegacyFieldsUseDefaultsWithoutWarning_data() {
        QTest::addColumn<QByteArray>("original");
        QTest::addColumn<int>("fontSize");
        QTest::addColumn<bool>("automatic");
        QTest::newRow("appearance-only") << QByteArray(R"({"schemaVersion":1,"appearance":{"theme":"dark","accent":"olive"},"future":{"keep":42}})") << 14 << false;
        QTest::newRow("partial-editor-and-autosave") << QByteArray(R"({"schemaVersion":1,"appearance":{"theme":"dark","accent":"olive"},"future":{"keep":42},"editor":{"fontSize":20,"autoSave":{"enabled":true,"future":7}}})") << 20 << true;
        QTest::newRow("empty-optional-objects") << QByteArray(R"({"schemaVersion":1,"appearance":{"theme":"dark","accent":"olive"},"future":{"keep":42},"editor":{"autoSave":{}},"reader":{},"desktop":{},"files":{}})") << 14 << false;
    }
    void missingLegacyFieldsUseDefaultsWithoutWarning() {
        QFETCH(QByteArray, original); QFETCH(int, fontSize); QFETCH(bool, automatic);
        QTemporaryDir dir; const auto path = dir.filePath("settings.json"); QVERIFY(fileFixtures::write(path, original));
        Appearance appearance(nullptr, path); auto *prefs = appearance.preferences();
        QVERIFY2(prefs->error().isEmpty(), qPrintable(prefs->error())); QVERIFY(appearance.error().isEmpty());
        SettingsWindow settings(&appearance, nullptr); auto *status = settings.findChild<QWidget *>("settingsStatus"); QVERIFY(status);
        QCOMPARE(status->toolTip(), QStringLiteral("设置更改即时生效"));
        QCOMPARE(appearance.theme(), QString("dark")); QCOMPARE(appearance.accent(), QString("olive"));
        QCOMPARE(prefs->value("editor", "fontSize").toInt(), fontSize);
        QCOMPARE(prefs->value("editor", "autoSaveEnabled").toBool(), automatic);
        QCOMPARE(prefs->value("editor", "autoSaveDelayMs").toInt(), 1500);
        QCOMPARE(bytes(path), original);
        QVERIFY(prefs->setValues("desktop", {{"standaloneDefaultMode", "source"}}));
        const auto saved = QJsonDocument::fromJson(bytes(path)).object();
        QCOMPARE(saved["future"].toObject()["keep"].toInt(), 42);
        QCOMPARE(saved["appearance"].toObject()["theme"].toString(), QString("dark"));
        QCOMPARE(saved["appearance"].toObject()["accent"].toString(), QString("olive"));
        if (automatic) QCOMPARE(saved["editor"].toObject()["autoSave"].toObject()["future"].toInt(), 7);
        Appearance reopened(nullptr, path); QVERIFY(reopened.preferences()->error().isEmpty());
        QCOMPARE(reopened.preferences()->value("editor", "fontSize").toInt(), fontSize);
        QCOMPARE(reopened.preferences()->value("editor", "autoSaveEnabled").toBool(), automatic);
    }
    void invalidStoredValuesPreserveOtherValidSettings_data() {
        QTest::addColumn<QByteArray>("original");
        const auto base = QJsonDocument::fromJson(R"({"schemaVersion":1,"appearance":{"theme":"dark","accent":"olive"},"editor":{"fontSize":21,"wordWrap":false},"reader":{"fontSize":23},"future":{"keep":42}})").object();
        auto add = [&](const char *name, const QString &section, const QJsonValue &value) {
            auto input = base; input[section] = value; QTest::newRow(name) << QJsonDocument(input).toJson();
        };
        add("null-field", "editor", QJsonObject{{"fontSize", QJsonValue::Null}, {"wordWrap", false}});
        add("wrong-editor-container", "editor", false);
        add("wrong-reader-container", "reader", 7);
        add("null-autosave-container", "editor", QJsonObject{{"fontSize", 21}, {"autoSave", QJsonValue::Null}});
        add("invalid-autosave-value", "editor", QJsonObject{{"fontSize", 21}, {"autoSave", QJsonObject{{"enabled", "yes"}, {"delayMs", 2400}, {"future", 7}}}});
    }
    void invalidStoredValuesPreserveOtherValidSettings() {
        QFETCH(QByteArray, original); QTemporaryDir dir; const auto path = dir.filePath("settings.json");
        QVERIFY(fileFixtures::write(path, original)); Appearance appearance(nullptr, path); auto *prefs = appearance.preferences();
        QVERIFY(!prefs->error().isEmpty()); QCOMPARE(bytes(path), original);
        QCOMPARE(appearance.theme(), QString("dark")); QCOMPARE(appearance.accent(), QString("olive"));
        const auto input = QJsonDocument::fromJson(original).object();
        for (const auto &section : {QString("editor"), QString("reader")}) {
            const auto value = input[section].toObject().value("fontSize");
            if (value.isDouble()) QCOMPARE(prefs->value(section, "fontSize"), value);
        }
        if (input["editor"].toObject()["autoSave"].toObject().contains("delayMs")) {
            QCOMPARE(prefs->value("editor", "autoSaveDelayMs").toInt(), 2400);
            QVERIFY(!prefs->value("editor", "autoSaveEnabled").toBool());
        }
        QVERIFY(prefs->setValues("desktop", {{"standaloneDefaultMode", "source"}}));
        const auto saved = QJsonDocument::fromJson(bytes(path)).object();
        QCOMPARE(saved["future"].toObject()["keep"].toInt(), 42);
        QCOMPARE(saved["appearance"], input["appearance"]);
        const auto inputAuto = input["editor"].toObject()["autoSave"].toObject();
        if (inputAuto.contains("future")) QCOMPARE(saved["editor"].toObject()["autoSave"].toObject()["future"], inputAuto["future"]);
        Appearance reopened(nullptr, path); QVERIFY(reopened.preferences()->error().isEmpty());
    }
    void splitPreferencesReuseAnalysisAndKeepReaderAnchor() {
        QTemporaryDir workspace, configuration;
        const auto source = QByteArray("# heading\n\n") + QStringLiteral("长段落 中文 **强调** ").repeated(160).toUtf8()
            + QByteArray("\n\n") + QByteArray("tail paragraph\n\n").repeated(80);
        const auto path = workspace.filePath("layout.md"); QVERIFY(fileFixtures::write(path, source));
        FileApplication app(nullptr, configuration.filePath("settings.json"));
        auto *window = app.workspaceWindow(workspace.path()); app.openInWorkspace(path, window);
        QTRY_VERIFY(window->activePane()); auto *pane = window->activePane();
        pane->setViewMode(SingleFileWindow::ViewMode::Split);
        auto *session = pane->document()->session();
        QCOMPARE(session->replace(0, 0, "draft ", session->revision()), EditResult::Applied);
        QTRY_VERIFY(pane->preview()->mappingCurrent());
        const auto snapshot = session->snapshot(); const auto analysis = PreviewAnalysis::find(snapshot); QVERIFY(analysis);
        auto *view = pane->preview()->view(); auto *bar = view->verticalScrollBar();
        const auto byte = snapshot.utf8.indexOf(QStringLiteral("长段落").toUtf8()); QVERIFY(byte > 0);
        const auto span = view->scrollSpanForSource(quint32(byte), view->displayedStamp()); QVERIFY(span);
        const auto anchor = pane->editor()->send(SCI_GETANCHOR), caret = pane->editor()->send(SCI_GETCURRENTPOS);
        bar->setSliderPosition(qRound(span->top + .42 * (span->bottom - span->top)));
        bar->triggerAction(QAbstractSlider::SliderMove);
        const auto fraction = [&] {
            if (!pane->preview()->mappingCurrent()) return -1.0;
            const auto current = view->scrollSpanForSource(quint32(byte), view->displayedStamp());
            return current ? (bar->value() - current->top) / (current->bottom - current->top) : -1.0;
        };
        QTRY_VERIFY(std::abs(fraction() - .42) < .015);
        auto *preferences = pane->appearance()->preferences();
        QVERIFY(preferences->setValues("reader", {{"fontSize", 22}, {"maxWidth", 600}, {"lineHeight", 2.0}}));
        QVERIFY(preferences->setValues("editor", {{"fontSize", 18}, {"wordWrap", false}}));
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(fraction() - .42) < .015, 10000);
        QCOMPARE(PreviewAnalysis::find(snapshot), analysis);
        QCOMPARE(session->revision(), snapshot.revision); QCOMPARE(session->snapshot().utf8, snapshot.utf8);
        QCOMPARE(pane->editor()->send(SCI_GETANCHOR), anchor); QCOMPARE(pane->editor()->send(SCI_GETCURRENTPOS), caret);
        QCOMPARE(bytes(path), source); QCOMPARE(session->undo(), EditResult::Applied);
        QCOMPARE(session->snapshot().utf8, source);
    }
    void storagePreservesLegacyAndUnknownFields() {
        QTemporaryDir dir; const auto path = dir.filePath("settings.json");
        const auto original = QByteArray(R"({"schemaVersion":1,"future":{"keep":42},"appearance":{"theme":"dark","accent":"olive","future":"kept"},"editor":{"future":true,"fontSize":-2,"autoSave":{"enabled":true,"delayMs":3000,"future":7}}})");
        QVERIFY(fileFixtures::write(path, original)); Appearance appearance(nullptr, path); auto *prefs = appearance.preferences(); QCOMPARE(appearance.theme(), QString("dark")); QCOMPARE(appearance.accent(), QString("olive")); QCOMPARE(prefs->value("editor", "fontSize").toInt(), 14); QCOMPARE(prefs->value("editor", "autoSaveDelayMs").toInt(), 3000); QCOMPARE(bytes(path), original);
        QVERIFY(prefs->setValues("reader", {{"fontSize", 20}, {"maxWidth", 880}})); QVERIFY(prefs->setValues("editor", {{"autoSaveEnabled", false}})); QVERIFY(prefs->setValues("editor", {{"autoSave", QJsonObject{{"enabled", false}, {"delayMs", 3000}}}}));
        const auto saved = QJsonDocument::fromJson(bytes(path)).object(); QCOMPARE(saved["future"].toObject()["keep"].toInt(), 42); QCOMPARE(saved["appearance"].toObject()["future"].toString(), QString("kept")); QVERIFY(saved["editor"].toObject()["future"].toBool()); QCOMPARE(saved["editor"].toObject()["autoSave"].toObject()["future"].toInt(), 7); QVERIFY(!saved["editor"].toObject()["autoSave"].toObject()["enabled"].toBool());
        Appearance reopened(nullptr, path); QCOMPARE(reopened.preferences()->value("reader", "fontSize").toInt(), 20); QCOMPARE(reopened.preferences()->value("reader", "maxWidth").toInt(), 880); QCOMPARE(reopened.preferences()->value("editor", "autoSaveDelayMs").toInt(), 3000);
    }
    void invalidIntentAndWriteFailureNeverPublish() {
        QTemporaryDir dir; const auto path = dir.filePath("settings.json"); Preferences prefs(nullptr, path); QVERIFY(prefs.setValues("reader", {{"fontSize", 18}})); const auto saved = bytes(path); QSignalSpy changed(&prefs, &Preferences::changed);
        for (const auto &patch : {QJsonObject{{"fontSize", 1}}, QJsonObject{{"fontSize", 18.5}}, QJsonObject{{"maxWidth", 0}}, QJsonObject{{"lineHeight", "big"}}, QJsonObject{{"fontFamily", "bad\nfont"}}}) { QVERIFY(!prefs.setValues("reader", patch)); QCOMPARE(bytes(path), saved); QCOMPARE(changed.size(), 0); }
        QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path)); QVERIFY(!prefs.setValues("reader", {{"fontSize", 24}})); QCOMPARE(prefs.value("reader", "fontSize").toInt(), 18); QCOMPARE(changed.size(), 0); QVERIFY(QDir().rmdir(path)); QVERIFY(prefs.setValues("reader", {{"fontSize", 24}})); QCOMPARE(changed.size(), 1); QVERIFY(prefs.error().isEmpty());
        for (const auto &bad : {QByteArray("{broken"), QByteArray("{\"schemaVersion\":1.5}"), QByteArray("{\"schemaVersion\":2}")}) { QVERIFY(fileFixtures::write(path, bad)); Preferences invalid(nullptr, path); QVERIFY(!invalid.setValues("appearance", {{"theme", "dark"}})); QCOMPARE(bytes(path), bad); }
    }
    void actualControlsShareOneInstanceAndKeepTheBuffer() {
        QTemporaryDir dir; const auto path = dir.filePath("doc.md"); QVERIFY(fileFixtures::write(path, "# heading\n\nretained\n"));
        FileApplication app(nullptr, dir.filePath("settings.json")); app.open(path); QTRY_VERIFY(standalone(path)); auto *pane = standalone(path); auto *session = pane->document()->session(); pane->setEditing(true); QCOMPARE(session->replace(0, 0, "draft ", session->revision()), EditResult::Applied); const auto before = session->snapshot();
        pane->findChild<QAction *>("settingsAction")->trigger(); auto *settings = app.settingsWindow(); QVERIFY(settings); auto *workspace = app.workspaceWindow(dir.path()); QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceSettings"), Qt::LeftButton); QCOMPARE(app.settingsWindow(), settings);
        QVERIFY(select(settings, "settings-editor-fontSize", 20)); QVERIFY(select(settings, "settings-editor-indentWidth", 4));
        settings->selectPage(2); auto *wrap = settings->findChild<QCheckBox *>("settings-editor-wordWrap"); QVERIFY(wrap && wrap->isChecked()); QTest::mouseClick(wrap, Qt::LeftButton); QCOMPARE(pane->editor()->send(SCI_GETWRAPMODE), sptr_t(SC_WRAP_NONE)); QCOMPARE(pane->editor()->send(SCI_GETTABWIDTH), sptr_t(4));
        QVERIFY(select(settings, "settings-reader-fontSize", 22)); QCOMPARE(session->snapshot().utf8, before.utf8); QCOMPARE(session->revision(), before.revision); QCOMPARE(bytes(path), QByteArray("# heading\n\nretained\n")); QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, QByteArray("# heading\n\nretained\n"));
        auto *done = settings->findChild<SharedButton *>("settingsDone"); QPointer<SettingsWindow> closed(settings); QTest::mouseClick(done, Qt::LeftButton); QTRY_VERIFY(closed.isNull()); auto *again = app.settingsWindow(); QCOMPARE(again->windowAppearance()->preferences()->value("editor", "fontSize").toInt(), 20);
    }
    void themeSearchAndFailureRollbackUseRealControls() {
        QTemporaryDir dir; const auto path = dir.filePath("settings.json"); Appearance appearance(nullptr, path); SettingsWindow window(&appearance, nullptr); window.show(); window.selectPage(1);
        auto *dark = window.findChild<QAbstractButton *>("settings-theme-dark"); auto *olive = window.findChild<QAbstractButton *>("settings-accent-olive"); QVERIFY(dark && olive); QTest::mouseClick(dark, Qt::LeftButton); QTest::mouseClick(olive, Qt::LeftButton); QVERIFY(appearance.dark()); QCOMPARE(appearance.accent(), QString("olive"));
        QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path)); auto *light = window.findChild<QAbstractButton *>("settings-theme-light"); QTest::mouseClick(light, Qt::LeftButton); QVERIFY(appearance.dark()); QVERIFY(dark->isChecked()); QVERIFY(!light->isChecked()); QVERIFY(!appearance.preferences()->error().isEmpty()); QVERIFY(QDir().rmdir(path)); QTest::mouseClick(light, Qt::LeftButton); QVERIFY(!appearance.dark()); QVERIFY(appearance.preferences()->error().isEmpty());
        window.selectPage(0); auto *search = window.findChild<QLineEdit *>("settingsSearch"); search->setText(QStringLiteral("软换行"));
        auto *results = window.findChild<QWidget *>("settingsSearchResults");
        QCOMPARE(window.findChild<QStackedWidget *>("settingsContent")->currentWidget(), results);
        auto *go = results->findChild<SharedButton *>("settingsSearchResult"); QVERIFY(go); QTest::mouseClick(go, Qt::LeftButton);
        QCOMPARE(window.findChild<QStackedWidget *>("settingsPages")->currentIndex(), 2);
        window.selectPage(0); search->setText(QStringLiteral("主题")); go = results->findChild<SharedButton *>("settingsSearchResult"); QVERIFY(go); QTest::mouseClick(go, Qt::LeftButton);
        QCOMPARE(window.findChild<QStackedWidget *>("settingsPages")->currentIndex(), 1);
    }
    void externalPreferencesOnlyAffectNewDocuments() {
        QTemporaryDir dir; const auto a = dir.filePath("a.md"), b = dir.filePath("b.md"); QVERIFY(fileFixtures::write(a, "a")); QVERIFY(fileFixtures::write(b, "b")); FileApplication app(nullptr, dir.filePath("settings.json")); auto *settings = app.settingsWindow(); auto *prefs = settings->windowAppearance()->preferences();
        QVERIFY(prefs->setValues("desktop", {{"externalOpenTarget", "workspace-tab"}})); auto *workspace = app.workspaceWindow(dir.path()); app.openExternal(a); QTRY_COMPARE(workspace->panes().size(), 1); auto *existing = workspace->panes().first(); QVERIFY(existing->document()); QCOMPARE(existing->viewMode(), SingleFileWindow::ViewMode::Reading);
        QVERIFY(prefs->setValues("desktop", {{"externalOpenTarget", "standalone"}, {"standaloneDefaultMode", "source"}})); app.openExternal(b); QTRY_VERIFY(standalone(b)); QCOMPARE(standalone(b)->viewMode(), SingleFileWindow::ViewMode::Source); QCOMPARE(existing->viewMode(), SingleFileWindow::ViewMode::Reading); QCOMPARE(workspace->panes().size(), 1); app.openExternal(a); QTest::qWait(100); QCOMPARE(workspace->panes().size(), 1); QVERIFY(!standalone(a));
    }
    void autoSaveDebouncesAndPausesOnConflict() {
        QTemporaryDir dir; const auto path = dir.filePath("doc.md"); QVERIFY(fileFixtures::write(path, "initial")); Preferences prefs; FileManager manager(nullptr, {}, false); AutoSaveController autoSave(&manager, &prefs, this); QSignalSpy opened(&manager, &FileManager::opened); manager.open(path); QTRY_COMPARE(opened.size(), 1); auto *document = qvariant_cast<FileDocument *>(opened.first()[1]); QVERIFY(document); auto *session = document->session(); QCOMPARE(session->replace(0, 0, "draft ", session->revision()), EditResult::Applied); QTest::qWait(600); QCOMPARE(bytes(path), QByteArray("initial"));
        QVERIFY(prefs.setValues("editor", {{"autoSaveEnabled", true}, {"autoSaveDelayMs", 500}})); QCOMPARE(session->replace(0, 0, "next ", session->revision()), EditResult::Applied); QTest::qWait(200); QCOMPARE(bytes(path), QByteArray("initial")); QTRY_VERIFY(!document->dirty()); QCOMPARE(bytes(path), QByteArray("next draft initial"));
        QVERIFY(fileFixtures::write(path, "outside")); QSignalSpy finished(document, &FileDocument::saveFinished); QCOMPARE(session->replace(0, 0, "retain ", session->revision()), EditResult::Applied); QTRY_VERIFY(document->conflict()); QTRY_COMPARE(finished.size(), 1); const auto before = session->snapshot().utf8; QTest::qWait(1200); QCOMPARE(finished.size(), 1); QCOMPARE(bytes(path), QByteArray("outside")); QCOMPARE(session->snapshot().utf8, before); QVERIFY(document->dirty());
    }
    void hardLinkedAutoSavePreservesBothPathsWithoutDialogLoop() {
        QTemporaryDir dir; const auto path = dir.filePath("doc.md"), alias = dir.filePath("alias.md"); QVERIFY(fileFixtures::write(path, "linked")); QVERIFY(fileFixtures::hardLink(path, alias)); FileApplication app(nullptr, dir.filePath("settings.json")); app.open(path); QTRY_VERIFY(standalone(path)); auto *pane = standalone(path); auto *document = pane->document(); auto *prefs = pane->appearance()->preferences(); QVERIFY(prefs->setValues("editor", {{"autoSaveEnabled", true}, {"autoSaveDelayMs", 500}})); auto *session = document->session(); QSignalSpy finished(document, &FileDocument::saveFinished); QCOMPARE(session->replace(0, 0, "retained ", session->revision()), EditResult::Applied); QTRY_VERIFY(document->hasError()); QTRY_COMPARE(finished.size(), 1); QCOMPARE(document->lastSave().problem.code, FileError::HardLinks); QTest::qWait(1200); QCOMPARE(finished.size(), 1); QCOMPARE(bytes(path), QByteArray("linked")); QCOMPARE(bytes(alias), QByteArray("linked")); QVERIFY(document->dirty()); QVERIFY(!pane->findChild<QDialog *>("saveErrorDialog"));
    }
    void editingDuringAutomaticSaveUsesSeparateFrozenVersions() {
        QTemporaryDir dir; const auto path = dir.filePath("doc.md"); QVERIFY(fileFixtures::write(path, "initial"));
        std::promise<void> release; const auto ready = release.get_future().share(); std::atomic_bool first{true}, staged{false}; bool released = false;
        Preferences prefs; FileManager manager(nullptr, [&](SaveStage stage) { if (stage == SaveStage::StageWrite && first.exchange(false)) { staged.store(true); ready.wait(); } }, false);
        const auto finish = qScopeGuard([&] { if (!released) release.set_value(); }); AutoSaveController automatic(&manager, &prefs, this); QSignalSpy opened(&manager, &FileManager::opened); manager.open(path); QTRY_COMPARE(opened.size(), 1); auto *document = qvariant_cast<FileDocument *>(opened.first()[1]); QVERIFY(document); auto *session = document->session();
        QVERIFY(prefs.setValues("editor", {{"autoSaveEnabled", true}, {"autoSaveDelayMs", 500}})); QSignalSpy saved(document, &FileDocument::saveFinished); QCOMPARE(session->replace(0, 0, "first ", session->revision()), EditResult::Applied); QTRY_VERIFY(staged.load()); QCOMPARE(session->replace(0, 0, "later ", session->revision()), EditResult::Applied);
        released = true; release.set_value(); QTRY_COMPARE(saved.size(), 1); QVERIFY(document->dirty()); QCOMPARE(bytes(path), QByteArray("first initial")); QTRY_VERIFY(!document->dirty()); QCOMPARE(saved.size(), 2); QCOMPARE(bytes(path), QByteArray("later first initial"));
        QVERIFY(prefs.setValues("editor", {{"autoSaveEnabled", false}})); QCOMPARE(session->replace(0, 0, "pending ", session->revision()), EditResult::Applied); QTest::qWait(700); QCOMPARE(saved.size(), 2); QCOMPARE(bytes(path), QByteArray("later first initial"));
    }
    void keyboardCardsAndTogglesActivateAndClearNavigation() {
        QTemporaryDir dir; FileApplication application(nullptr, dir.filePath("settings.json")); auto *window = application.settingsWindow(); window->selectPage(1); auto *dark = window->findChild<QAbstractButton *>("settings-theme-dark"); QVERIFY(dark); dark->setFocus(Qt::TabFocusReason); QTest::keyClick(dark, Qt::Key_Return); QCOMPARE(window->windowAppearance()->theme(), QString("dark"));
        window->selectPage(2); auto *toggle = window->findChild<QCheckBox *>("settings-editor-wordWrap"); QVERIFY(toggle); toggle->setFocus(Qt::TabFocusReason); QTest::keyClick(toggle, Qt::Key_Return); QVERIFY(!window->windowAppearance()->preferences()->value("editor", "wordWrap").toBool());
        auto *preview = window->findChild<SourceEditor *>("settingsEditorPreview"); preview->setFocus(Qt::OtherFocusReason); QTRY_VERIFY(preview->hasFocus()); QTest::keyClick(preview, Qt::Key_Tab); QVERIFY(!preview->hasFocus());
    }
    void pageGeometryThemesAndCompactSwatches() {
        QTemporaryDir dir; Appearance appearance(nullptr, dir.filePath("settings.json")); SettingsWindow window(&appearance, nullptr); window.show(); const auto evidence = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE"); if (!evidence.isEmpty()) QVERIFY(QDir().mkpath(evidence + "/settings"));
        for (const auto &theme : {QString("light"), QString("dark")}) {
            QVERIFY(appearance.setPreferences(theme, "graphite")); window.resize(1080, 820);
            for (int page = 0; page < 6; ++page) { window.selectPage(page); QTest::qWait(70); auto *done = window.findChild<SharedButton *>("settingsDone"); QVERIFY(done->isVisible()); QVERIFY(window.rect().contains(QRect(done->mapTo(&window, QPoint()), done->size()))); if (!evidence.isEmpty()) QVERIFY(window.grab().save(evidence + "/settings/" + theme + "-" + QString::number(page) + ".png")); }
            window.resize(860, 640); window.selectPage(1); QTest::qWait(70); auto *first = window.findChild<QAbstractButton *>("settings-accent-graphite"), *fourth = window.findChild<QAbstractButton *>("settings-accent-mauve"); QVERIFY(first && fourth); QVERIFY(fourth->y() > first->y()); if (!evidence.isEmpty()) QVERIFY(window.grab().save(evidence + "/settings/" + theme + "-compact.png"));
        }
    }
    void shortcutCapsKeepNativeEditingAndEmptyBinding() {
        QTemporaryDir dir;Appearance appearance(nullptr,dir.filePath("settings.json"));SettingsWindow w(&appearance,nullptr);w.show();w.selectPage(4);w.activateWindow();
        auto*field=w.findChild<SharedLineEdit*>("shortcut-findAction");QVERIFY(field&&field->shortcutDisplay());
        const auto original=field->text();field->setFocus(Qt::TabFocusReason);QTRY_VERIFY(field->hasFocus());
        field->selectAll();QTest::keyClicks(field,"Ctrl+Alt+K, Ctrl+Alt+C");QCOMPARE(field->text(),QString("Ctrl+Alt+K, Ctrl+Alt+C"));
        w.findChild<SharedButton*>("apply-findAction")->click();QCOMPARE(appearance.preferences()->value("shortcuts","findAction").toString(),field->text());
        field->setFocus();field->selectAll();QTest::keyClick(field,Qt::Key_Backspace);w.findChild<SharedButton*>("apply-findAction")->click();QCOMPARE(field->text(),QString());QCOMPARE(field->placeholderText(),QStringLiteral("未分配"));QCOMPARE(appearance.preferences()->value("shortcuts","findAction").toString(),QString());
        w.findChild<SharedButton*>("reset-findAction")->click();QCOMPARE(field->text(),original);
        // Painting the idle keycaps must not mutate the editable text or undo.
        field->setFocus();field->selectAll();QTest::keyClicks(field,"Ctrl+Alt+J");const auto entered=field->text();field->clearFocus();field->grab();QCOMPARE(field->text(),entered);field->setFocus();QVERIFY(field->isUndoAvailable());field->undo();QVERIFY(field->text()!=entered);
    }
    void constrainedSettingsKeepsNavigationAndActionsReachable() {
        QTemporaryDir dir;Appearance appearance(nullptr,dir.filePath("settings.json"));SettingsWindow w(&appearance,nullptr);w.show();QTest::qWait(120);
        const auto area=w.screen()->availableGeometry();QVERIFY(w.width()<=area.width());QVERIFY(w.height()<=area.height());
        auto*geometry=w.findChild<WindowGeometry*>();QVERIFY(geometry);QCOMPARE(geometry->preferredClientSize(),QSize(1080,820));
        auto*done=w.findChild<SharedButton*>("settingsDone");QVERIFY(w.rect().contains(QRect(done->mapTo(&w,QPoint()),done->size())));
        auto*nav=w.findChild<QScrollArea*>("settingsNavigationScroll");QVERIFY(nav);
        for(int page=0;page<6;++page){
            auto*button=w.findChild<SharedButton*>("settingsPage-"+QString::number(page));nav->ensureWidgetVisible(button);QTest::qWait(10);QVERIFY(nav->viewport()->rect().contains(button->mapTo(nav->viewport(),button->rect().center())));button->click();
            auto*pages=w.findChild<QStackedWidget*>("settingsPages");auto*scroll=qobject_cast<QScrollArea*>(pages->currentWidget());QVERIFY(scroll);QTest::qWait(30);
            for(auto*action:scroll->widget()->findChildren<SharedButton*>())if(action->isVisible()){scroll->ensureWidgetVisible(action);QApplication::processEvents();QVERIFY2(scroll->viewport()->rect().contains(action->mapTo(scroll->viewport(),action->rect().center())),qPrintable(action->objectName()));}
            scroll->verticalScrollBar()->setValue(0);scroll->horizontalScrollBar()->setValue(0);QApplication::processEvents();
            const auto path=qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");if(!path.isEmpty()){QVERIFY(QDir().mkpath(path+"/small-settings"));QVERIFY(w.grab().save(path+"/small-settings/"+QString::number(page)+".png"));}
            QVERIFY(w.rect().contains(QRect(done->mapTo(&w,QPoint()),done->size())));
        }
    }
};
int main(int argc, char **argv) {
    initializeGuiRuntime(); QApplication app(argc, argv); configureGuiApplication(); app.setQuitOnLastWindowClosed(false); SourceEditor::installAccessibility(); SettingsTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "settings_tests.moc"
