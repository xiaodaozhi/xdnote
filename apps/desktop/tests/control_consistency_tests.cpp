#include "Appearance.h"
#include "DocumentExport.h"
#include "DocumentOutline.h"
#include "FileApplication.h"
#include "FileService.h"
#include "GuiRuntime.h"
#include "InputModality.h"
#include "Preferences.h"
#include "ResourceMaintenanceController.h"
#include "SettingsWindow.h"
#include "SharedWidgets.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "WorkspaceWindow.h"
#include "file_fixtures.h"
#include <QAbstractItemModel>
#include <QAccessible>
#include <QAction>
#include <QBuffer>
#include <QJsonDocument>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QStyleOptionFrame>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QtTest>

using namespace xdnote;
namespace {
QByteArray disk(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
void evidence(QWidget *w, const QString &name) {
    const auto dir=qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
    if (!dir.isEmpty()) { QDir().mkpath(dir+"/controls"); w->grab().save(dir+"/controls/"+name+".png"); }
}
SingleFileWindow *paneFor(WorkspaceWindow *w, const QString &path) {
    for (auto *p:w->panes()) if (p->document()&&p->document()->path()==path) return p;
    return nullptr;
}
void dragMove(QWidget *w, QPoint point) {
    QMouseEvent move(QEvent::MouseMove,point,w->mapToGlobal(point),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(w,&move);
}
}
class ControlConsistencyTests final: public QObject {
    Q_OBJECT
private slots:
    void settingsFramesGroupsAndFailureRecovery_data() { QTest::addColumn<QString>("theme"); QTest::newRow("light")<<QString("light"); QTest::newRow("dark")<<QString("dark"); }
    void settingsFramesGroupsAndFailureRecovery() {
        QFETCH(QString,theme); QTemporaryDir dir; const auto path=dir.filePath("settings.json");
        QVERIFY(fileFixtures::write(path,R"({"schemaVersion":1,"keep":{"value":42}})"));
        Appearance a(nullptr,path); QVERIFY(a.setPreferences(theme,"graphite")); SettingsWindow w(&a,nullptr); w.show(); w.resize(1080,820); QTest::qWait(40);
        auto *field=w.findChild<SharedButton *>("settings-desktop-externalOpenTarget"); QVERIFY(field); QCOMPARE(field->height(),32); QVERIFY(field->property("fieldDropdown").toBool());
        auto *segment=w.findChild<QWidget *>("settingsDefaultMode"); QVERIFY(segment); QCOMPARE(segment->height(),32); QCOMPARE(segment->layout()->contentsMargins(),QMargins(2,2,2,2));
        for(auto *button:segment->findChildren<SharedButton *>()) { QCOMPARE(button->height(),28); QCOMPARE(button->width(),72); }
        auto firstGroupY=[&](int page) { w.selectPage(page); QApplication::processEvents(); for(auto *label:w.findChildren<QLabel *>()) if(label->property("settingsGroupTitle").toBool()&&label->isVisible()) return label->mapTo(&w,QPoint()).y(); return -1; };
        const auto general=firstGroupY(0); QVERIFY(general>0); QCOMPARE(firstGroupY(2),general); QCOMPARE(firstGroupY(3),general);
        auto *search=w.findChild<SharedLineEdit *>("settingsSearch"); QVERIFY(search); QCOMPARE(search->height(),32); QVERIFY(search->isClearButtonEnabled()); QVERIFY(!search->actions().isEmpty());
        search->setText("NO_MATCH_FIXTURE_126"); QVERIFY(w.findChild<SharedButton *>("settingsClearSearch")->isVisible()); evidence(&w,theme+"-search-empty");
        QTest::mouseClick(w.findChild<SharedButton *>("settingsClearSearch"),Qt::LeftButton); QVERIFY(search->text().isEmpty());
        QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path)); const auto before=a.preferences()->value("editor","fontSize");
        QVERIFY(!a.preferences()->setValues("editor",{{"fontSize",20}})); QCOMPARE(a.preferences()->value("editor","fontSize"),before);
        QVERIFY(w.findChild<QWidget *>("settingsFeedback")->isVisible()); QVERIFY(w.findChild<SharedButton *>("settingsRetry")->isVisible()); evidence(&w,theme+"-write-error");
        QVERIFY(QDir().rmdir(path)); QVERIFY(fileFixtures::write(path,R"({"schemaVersion":1,"keep":{"value":42},"future":true})"));
        QTest::mouseClick(w.findChild<SharedButton *>("settingsRetry"),Qt::LeftButton); QCOMPARE(a.preferences()->value("editor","fontSize").toInt(),20);
        QVERIFY(a.preferences()->error().isEmpty()); QVERIFY(!w.findChild<QWidget *>("settingsFeedback")->isVisible());
        const auto saved=QJsonDocument::fromJson(disk(path)).object(); QCOMPARE(saved["keep"].toObject()["value"].toInt(),42); QVERIFY(saved["future"].toBool());
        for(int page=0;page<6;++page) { w.selectPage(page); QApplication::processEvents(); evidence(&w,theme+"-settings-"+QString::number(page)); }
    }
    void fieldArrowsStayRightAndMenusRetainChecks() {
        Appearance a; QVERIFY(a.setPreferences("light","graphite")); QWidget owner; owner.setFixedSize(400,180); auto*l=new QVBoxLayout(&owner);
        SharedButton field("A","field",&a,&owner); field.setProperty("fieldDropdown",true); field.setProperty("showMenuIndicator",true); field.setOutlined(true); field.setFixedSize(240,32); l->addWidget(&field);
        SharedMenu menu(&a,&field); field.setMenu(&menu); auto *action=menu.addAction(themedIcon("document",a.color("text")),QStringLiteral("文档")); action->setCheckable(true); action->setChecked(true); menu.addAction(QStringLiteral("其他选项"));
        owner.show(); QApplication::processEvents(); const auto shortValue=field.grab().toImage(); field.setText(QStringLiteral("很长的字体名称 Long Font Family 123456789")); const auto longValue=field.grab().toImage();
        const QRect caret(216,8,12,16); QCOMPARE(shortValue.copy(caret),longValue.copy(caret));
        menu.popup(field.mapToGlobal(QPoint(0,32))); QTRY_VERIFY(menu.isVisible()); const auto checked=menu.grab().toImage(); action->setChecked(false); menu.update(); QApplication::processEvents(); const auto unchecked=menu.grab().toImage();
        const auto r=menu.actionGeometry(action); QVERIFY(checked.copy(QRect(r.right()-26,r.top(),24,r.height()))!=unchecked.copy(QRect(r.right()-26,r.top(),24,r.height())));
        QCOMPARE(checked.pixelColor(0,0).alpha(),0); evidence(&menu,"menu-check-slot"); menu.hide();
    }
    void longMenusScrollToKeyboardSelection() {
        Appearance a; QWidget owner; owner.show(); SharedMenu menu(&a,&owner);
        for(int i=0;i<240;++i)menu.addAction(QStringLiteral("Font Family %1 — a sufficiently long family name").arg(i));
        menu.popup(QPoint(100,100)); QTRY_VERIFY(menu.isVisible()); QTest::keyClick(&menu,Qt::Key_End); QTRY_COMPARE(menu.activeAction(),menu.actions().last());
        QVERIFY(menu.surfaceRect().intersects(menu.actionGeometry(menu.actions().last()))); QVERIFY(menu.width()<1000); evidence(&menu,"long-menu-bottom");
        QTest::keyClick(&menu,Qt::Key_Home); QTRY_COMPARE(menu.activeAction(),menu.actions().first()); QVERIFY(menu.surfaceRect().intersects(menu.actionGeometry(menu.actions().first()))); menu.hide();
    }
    void resourceDialogUsesSharedDarkAndLightControls_data() { settingsFramesGroupsAndFailureRecovery_data(); }
    void resourceDialogUsesSharedDarkAndLightControls() {
        QFETCH(QString,theme); QTemporaryDir dir; const auto path=dir.filePath("a.md"); QVERIFY(fileFixtures::write(path,"# keep\n")); FileApplication app(nullptr,dir.filePath("settings.json")); auto*w=app.workspaceWindow(dir.path()); QVERIFY(w->windowAppearance()->setPreferences(theme,"graphite"));
        bool correct=false; QTimer::singleShot(30,w,[&] {auto*d=w->findChild<QDialog *>("resourceMaintenanceDialog");if(!d)return;auto*body=d->findChild<QWidget *>("resourceMaintenanceBody");auto*source=d->findChild<SharedLineEdit *>("resourceMoveSource");auto*target=d->findChild<SharedLineEdit *>("resourceMoveTarget");const auto checks=d->findChildren<SharedCheckBox *>();correct=body&&source&&target&&source->height()==32&&target->height()==32&&checks.size()==3&&d->palette().color(QPalette::WindowText)==w->windowAppearance()->color("text");evidence(d,theme+"-resource-dialog");d->reject();});
        w->resourceMaintenance()->show(path); QVERIFY(correct); QCOMPARE(disk(path),QByteArray("# keep\n"));
    }
    void checkboxActivationKeepsLogicalFocusWithoutOutline() {
        Appearance a; QWidget owner; owner.setFixedSize(260,100); auto*l=new QVBoxLayout(&owner); SharedCheckBox check(QStringLiteral("保留原文"),&a,&owner); l->addWidget(&check); owner.show(); owner.activateWindow(); QApplication::processEvents();
        QTest::keyClick(&owner,Qt::Key_Tab); check.setFocus(Qt::TabFocusReason); QTRY_VERIFY(check.hasFocus());
        QTest::keyClick(&check,Qt::Key_Space); QVERIFY(check.isChecked()); QVERIFY(check.hasFocus()); QVERIFY(!InputModality::forApplication()->keyboardNavigation());
        const auto activated=check.grab().toImage(); check.clearFocus(); QCOMPARE(check.grab().toImage(),activated);
        check.setEnabled(false); const auto iface=QAccessible::queryAccessibleInterface(&check); QVERIFY(iface); QVERIFY(iface->state().disabled); QVERIFY(iface->state().checked);
    }
    void fencedSyntaxDoesNotChangeSourceOrUndo() {
        const QByteArray source="```cpp\nconst int number = 42; // comment\nconst char *s = \"text\";\n```\n";
        DocumentSession session(source); Appearance a; SourceEditor editor(&session,nullptr,&a); editor.resize(640,300); editor.show(); QApplication::processEvents();
        QCOMPARE(editor.send(SCI_GETSTYLEAT,source.indexOf("const")),sptr_t(4)); QCOMPARE(editor.send(SCI_GETSTYLEAT,source.indexOf("42")),sptr_t(6)); QCOMPARE(editor.send(SCI_GETSTYLEAT,source.indexOf("//")),sptr_t(7)); QCOMPARE(editor.send(SCI_GETSTYLEAT,source.indexOf("\"text")),sptr_t(5));
        QCOMPARE(session.snapshot().utf8,source); QVERIFY(!session.isDirty()); QCOMPARE(session.replace(0,0,"draft\n",session.revision()),EditResult::Applied); QCOMPARE(session.undo(),EditResult::Applied); QCOMPARE(session.snapshot().utf8,source); evidence(&editor,"source-fenced-syntax");
    }
    void tabDragHasOneFollowerAndCancellationRestoresOrder() {
        QTemporaryDir dir; FileApplication app(nullptr,dir.filePath("settings.json")); auto*w=app.workspaceWindow(dir.path());
        for(const auto *name:{"a.md","b.md","c.md"}) {const auto p=dir.filePath(name); QVERIFY(fileFixtures::write(p,"# heading\n")); app.openInWorkspace(p,w); QTRY_VERIFY(paneFor(w,p));}
        auto*tabs=w->findChild<QTabBar *>(); QVERIFY(tabs); QTRY_COMPARE(tabs->count(),3); QApplication::processEvents(); const auto id=tabs->tabData(0); auto*ghost=tabs->findChild<QWidget *>("workspaceTabDragSurface"); QVERIFY(ghost); QVERIFY(!ghost->isVisible());
        const auto start=tabs->tabRect(0).center(); QTest::mousePress(tabs,Qt::LeftButton,Qt::NoModifier,start); const auto end=tabs->tabRect(2).center()+QPoint(8,0); dragMove(tabs,end); QTRY_VERIFY(ghost->isVisible());
        QCOMPARE(tabs->tabData(2),id); QVERIFY(!tabs->tabButton(2,QTabBar::RightSide)->isVisible()); const auto x=ghost->x(); dragMove(tabs,end-QPoint(18,0)); QVERIFY(ghost->x()<x); QCOMPARE(ghost->y(),0); evidence(w,"tab-drag-moving"); QTest::qWait(180); evidence(w,"tab-drag-follow");
        QTest::keyClick(tabs,Qt::Key_Escape); QTRY_VERIFY(!ghost->isVisible()); QCOMPARE(tabs->tabData(0),id); QTest::mouseRelease(tabs,Qt::LeftButton,Qt::NoModifier,start);
        QTest::mousePress(tabs,Qt::LeftButton,Qt::NoModifier,start); dragMove(tabs,end); QTest::mouseRelease(tabs,Qt::LeftButton,Qt::NoModifier,end); QTRY_VERIFY(!ghost->isVisible()); QCOMPARE(tabs->tabData(2),id); QCOMPARE(w->panes().size(),3);
        for(int i=0;i<tabs->count();++i) QVERIFY(tabs->tabButton(i,QTabBar::RightSide)->isVisible());
    }
    void outlineAndFocusKeepDocumentAndViewState() {
        QTemporaryDir dir; const auto path=dir.filePath("doc.md"); const QByteArray source="# First\n\n## Second\n\n# First\n"; QVERIFY(fileFixtures::write(path,source)); FileApplication app(nullptr,dir.filePath("settings.json")); auto*w=app.workspaceWindow(dir.path()); app.openInWorkspace(path,w); QTRY_VERIFY(paneFor(w,path)); auto*p=paneFor(w,path); p->setEditing(true); const auto mode=p->viewMode(); const auto geometry=w->geometry();
        p->findChild<QAction *>("outlineAction")->trigger(); auto *outline=w->findChild<DocumentOutline *>(); QVERIFY(outline); QTRY_VERIFY(outline->isVisible()); auto*list=outline->findChild<QListView *>("outlineHeadings"); QTRY_COMPARE(list->model()->rowCount(),3);
        outline->findChild<QLineEdit *>("outlineSearch")->setText("Second"); QCOMPARE(list->model()->rowCount(),1); QSignalSpy activated(outline,&DocumentOutline::headingActivated); QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,list->visualRect(list->model()->index(0,0)).center()); QCOMPARE(activated.size(),1); QCOMPARE(activated.first()[1].toUuid(),p->document()->session()->documentId());
        w->setFocusMode(true); QVERIFY(p->focusMode()); QVERIFY(p->findChild<QWidget *>("documentToolbar")->isHidden()); QVERIFY(p->findChild<SharedButton *>("exitFocus")->isVisible()); QCOMPARE(w->geometry(),geometry);
        QTest::mouseClick(p->findChild<SharedButton *>("exitFocus"),Qt::LeftButton); QVERIFY(!p->focusMode()); QVERIFY(p->findChild<QWidget *>("documentToolbar")->isVisible()); QCOMPARE(p->viewMode(),mode); QCOMPARE(p->document()->session()->snapshot().utf8,source); QCOMPARE(disk(path),source); evidence(w,"outline-workspace");
    }
    void exportUsesSemanticModelAndProtectsOriginal() {
        QVERIFY(QDir().mkpath("build")); QTemporaryDir dir(QDir::current().absoluteFilePath("build/export-test-XXXXXX")); QVERIFY(dir.isValid()); const auto sourcePath=dir.filePath("doc.md"); const QByteArray source=QStringLiteral("# 中文\n\n<script>alert(1)</script>\n\n[unsafe](javascript:alert) [safe](https://example.com)\n\n- item\n\n|A|B|\n|--|--|\n|甲|乙|\n").toUtf8(); QVERIFY(fileFixtures::write(sourcePath,source));
        const auto alias=dir.filePath("alias.html"); const bool linked=fileFixtures::hardLink(sourcePath,alias);
#ifdef Q_OS_WIN
        QVERIFY2(linked,qPrintable(QString("CreateHardLink failed: %1").arg(GetLastError())));
#else
        QVERIFY(linked);
#endif
        DocumentSession session(source); const auto snapshot=session.snapshot(); const auto probe=FileService::probe(sourcePath); QVERIFY(probe.file);
        const auto data=prepareDocumentExport(snapshot,"标题",{},sourcePath,probe.file->identity(),{},{}); QVERIFY2(data.error.isEmpty(),qPrintable(data.error));
        QVERIFY(data.html.contains("<h1 id=")); QVERIFY(data.html.contains(QStringLiteral("中文").toUtf8())); QVERIFY(data.html.contains("&lt;script&gt;")); QVERIFY(!data.html.contains("<script>")); QVERIFY(!data.html.contains("href=\"javascript:")); QVERIFY(data.html.contains("href=\"https://example.com\"")); QVERIFY(data.html.contains("<table>"));
        QString error; const auto html=dir.filePath("result.html"),pdf=dir.filePath("result.pdf"); QVERIFY2(writeDocumentExport(data,html,false,sourcePath,probe.file->identity(),&error),qPrintable(error)); QCOMPARE(disk(html),data.html); QVERIFY2(writeDocumentExport(data,pdf,true,sourcePath,probe.file->identity(),&error),qPrintable(error)); QVERIFY(disk(pdf).startsWith("%PDF-"));
        QVERIFY(!writeDocumentExport(data,alias,false,sourcePath,probe.file->identity(),&error)); QCOMPARE(disk(sourcePath),source); QCOMPARE(session.snapshot().utf8,source); QVERIFY(!session.isDirty());
        QVERIFY(!writeDocumentExport(data,dir.filePath("missing/out.html"),false,sourcePath,probe.file->identity(),&error)); QVERIFY(!error.isEmpty());
    }
    void standaloneOutlineDoesNotSwitchEditingMode() {
        QTemporaryDir dir; const auto path=dir.filePath("outline.md"); const QByteArray source="# First\n\n## Second\n"; QVERIFY(fileFixtures::write(path,source)); FileApplication app(nullptr,dir.filePath("settings.json")); auto*p=app.emptyWindow(); app.open(path,p); QTRY_VERIFY(p->document()); p->setEditing(true); const auto geometry=p->geometry();
        p->findChild<QAction *>("outlineAction")->trigger(); auto*outline=p->findChild<DocumentOutline *>(); QVERIFY(outline&&outline->isVisible()); QCOMPARE(p->viewMode(),SingleFileWindow::ViewMode::Source); QTRY_COMPARE(outline->findChild<QListView *>("outlineHeadings")->model()->rowCount(),2);
        p->setFocusMode(true); QVERIFY(!outline->isVisible()); p->setFocusMode(false); QVERIFY(outline->isVisible()); QCOMPARE(p->viewMode(),SingleFileWindow::ViewMode::Source); QCOMPARE(p->geometry(),geometry); QCOMPARE(p->document()->session()->snapshot().utf8,source); evidence(p,"standalone-source-outline");
    }
    void thirdPartyViewerUsesBundledNotices() {
        QTemporaryDir dir; Appearance a(nullptr,dir.filePath("settings.json")); SettingsWindow w(&a,nullptr); w.show(); w.selectPage(5); auto*button=w.findChild<SharedButton *>("settingsThirdPartyLicenses"); QVERIFY(button);
        bool seen=false; QTimer::singleShot(30,&w,[&] { auto*dialog=w.findChild<QDialog *>("thirdPartyLicenses"); if(!dialog)return; auto*text=dialog->findChild<QTextBrowser *>("thirdPartyLicenseText"); seen=text&&text->toPlainText().contains("Qt")&&text->toPlainText().contains("Scintilla"); evidence(dialog,"third-party-notices");if(text){text->anchorClicked(QUrl("LICENSES/third-party/scintilla.txt"));seen&=text->toPlainText().contains("Copyright");dialog->findChild<SharedButton *>("licenseIndex")->click();text->anchorClicked(QUrl("docs/engineering/licensing.md"));text->anchorClicked(QUrl("../../LICENSES/CC-BY-4.0.txt"));seen&=text->toPlainText().contains("Attribution 4.0");} dialog->reject(); });
        QTest::mouseClick(button,Qt::LeftButton); QVERIFY(seen);
    }
};
int main(int argc,char**argv) { initializeGuiRuntime(); QApplication app(argc,argv); configureGuiApplication(); app.setQuitOnLastWindowClosed(false); SourceEditor::installAccessibility(); ControlConsistencyTests tests; return QTest::qExec(&tests,argc,argv); }
#include "control_consistency_tests.moc"
