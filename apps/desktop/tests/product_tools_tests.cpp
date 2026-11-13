#include "GuiRuntime.h"
#include "DocumentSearch.h"
#include "WorkspaceSearch.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "RecoveryStore.h"
#include "Shortcuts.h"
#include "Preferences.h"
#include "FileManager.h"
#include "SingleFileWindow.h"
#include "WorkspaceWindow.h"
#include "SourceEditor.h"
#include "SystemIntegration.h"
#include "FileApplication.h"
#include <QAction>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QTimer>
#include <QDialog>
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>
using namespace xdnote;
class ProductToolsTests final : public QObject {
    Q_OBJECT
private slots:
    void explicitDiscardRemovesRecoveryDraftButRetainsLastFile(){
        QTemporaryDir dir;const auto original=dir.filePath("original.md"),settings=dir.filePath("settings.json");QFile file(original);QVERIFY(file.open(QIODevice::WriteOnly));file.write("disk");file.close();
        {FileApplication app(nullptr,settings);auto *workspace=app.workspaceWindow(dir.path());app.openInWorkspace(original,workspace);QTRY_VERIFY(workspace->activePane()&&workspace->activePane()->document());auto *pane=workspace->activePane();auto *session=pane->document()->session();QCOMPARE(session->replace(0,4,"discarded edit",session->revision()),EditResult::Applied);QTimer::singleShot(0,workspace,[]{auto *dialog=QApplication::activeModalWidget();if(dialog)if(auto *discard=dialog->findChild<QPushButton*>("choice2"))discard->click();});pane->close();QTRY_VERIFY(workspace->panes().isEmpty());}
        RecoveryStore store(settings+".recovery");QVERIFY(store.state()["drafts"].toArray().isEmpty());QCOMPARE(store.state()["documents"].toArray().size(),1);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("disk"));
    }
    void remappedUndoDoesNotKeepTheOldEditorBinding(){
        Appearance a;QVERIFY(a.preferences()->setValues("shortcuts",{{"undoAction","Ctrl+Alt+U"}}));DocumentSession session("before");SourceEditor editor(&session,nullptr,&a);QCOMPARE(session.replace(0,6,"after",session.revision()),EditResult::Applied);QKeyEvent old(QEvent::KeyPress,Qt::Key_Z,Qt::ControlModifier);QApplication::sendEvent(&editor,&old);QCOMPARE(session.snapshot().utf8,QByteArray("after"));QKeyEvent current(QEvent::KeyPress,Qt::Key_U,Qt::ControlModifier|Qt::AltModifier);QApplication::sendEvent(&editor,&current);QCOMPARE(session.snapshot().utf8,QByteArray("before"));
    }
    void newDefaultShortcutDoesNotResetAnExistingCustomBinding(){
        QTemporaryDir dir;const auto path=dir.filePath("settings.json");QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly));f.write(R"({"schemaVersion":1,"shortcuts":{"saveAction":"Ctrl+Shift+F","futureCommand":"retained"},"reader":{"fontSize":19}})");f.close();Preferences p(nullptr,path);QCOMPARE(p.value("shortcuts","saveAction").toString(),QString("Ctrl+Shift+F"));QCOMPARE(p.value("shortcuts","fullSearchAction").toString(),QString());QCOMPARE(p.value("reader","fontSize").toInt(),19);QVERIFY(p.setValues("reader",{{"fontSize",20}}));QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(f.readAll()).object()["shortcuts"].toObject()["futureCommand"].toString(),QString("retained"));
    }
    void sessionReopenRestoresDirtyCopyWithoutOverwritingDisk(){
        QTemporaryDir dir;const auto original=dir.filePath("original.md"),settings=dir.filePath("settings.json");QFile file(original);QVERIFY(file.open(QIODevice::WriteOnly));file.write("disk");file.close();
        {FileApplication app(nullptr,settings);auto *workspace=app.workspaceWindow(dir.path());app.openInWorkspace(original,workspace);QTRY_VERIFY(workspace->activePane()&&workspace->activePane()->document());auto *pane=workspace->activePane();auto *session=pane->document()->session();QCOMPARE(session->replace(0,4,QStringLiteral("未保存😀"),session->revision()),EditResult::Applied);pane->setViewMode(SingleFileWindow::ViewMode::Source);pane->setContentZoom(150);}
        RecoveryStore evidence(settings+".recovery");QCOMPARE(evidence.state()["drafts"].toArray().size(),1);
        {FileApplication app(nullptr,settings);auto *welcome=app.workspaceWindow();auto *restore=welcome->findChild<QPushButton*>("restoreLastSession");QVERIFY(restore);restore->click();QTRY_VERIFY(([&]{for(auto *w:QApplication::topLevelWidgets())if(auto *workspace=qobject_cast<WorkspaceWindow*>(w))for(auto *pane:workspace->panes())if(pane->document()&&pane->document()->untitled())return true;return false;})());SingleFileWindow *recovered=nullptr;for(auto *w:QApplication::topLevelWidgets())if(auto *workspace=qobject_cast<WorkspaceWindow*>(w))for(auto *pane:workspace->panes())if(pane->document()&&pane->document()->untitled())recovered=pane;QVERIFY(recovered);QCOMPARE(recovered->document()->session()->snapshot().utf8,QStringLiteral("未保存😀").toUtf8());QCOMPARE(recovered->contentZoom(),150);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("disk"));file.close();}
    }
    void registrationOnlyOffersOpenWithAndDoesNotTakeDefaults(){
        const auto entries=associationValues(QStringLiteral("C:/程序/xdnote.exe"));QCOMPARE(entries.size(),13);for(const auto &entry:entries){QVERIFY(!entry.path.contains("UserChoice"));QVERIFY(!entry.path.endsWith("\\.md"));QVERIFY(!entry.path.endsWith("\\.markdown"));}QVERIFY(entries[2].value.endsWith("\" \"%1\""));QVERIFY(associationValues("bad\"path").isEmpty());
    }
    void liveFindBarAndWorkspaceEntryUseSharedControls(){
        QTemporaryDir dir;QFile f(dir.filePath("a.md"));QVERIFY(f.open(QIODevice::WriteOnly));f.write("one two one");f.close();FileManager manager(nullptr,{},false);QSignalSpy opened(&manager,&FileManager::opened);manager.open(f.fileName());QTRY_COMPARE(opened.size(),1);auto *document=qvariant_cast<FileDocument*>(opened.first()[1]);QVERIFY(document);Appearance a;SingleFileWindow pane(&manager,&a);pane.setAttribute(Qt::WA_DeleteOnClose,false);pane.setDocument(document);pane.show();pane.findChild<QAction*>("findAction")->trigger();auto *bar=pane.findChild<DocumentSearch*>();QVERIFY(bar->isVisible());bar->findChild<QLineEdit*>("findQuery")->setText("two");QCOMPARE(pane.viewMode(),SingleFileWindow::ViewMode::Source);QCOMPARE(pane.editor()->send(SCI_GETSELECTIONSTART),sptr_t(4));QCOMPARE(pane.editor()->send(SCI_GETSELECTIONEND),sptr_t(7));WorkspaceWindow workspace(&a,nullptr,nullptr,&manager);workspace.setAttribute(Qt::WA_DeleteOnClose,false);workspace.setRootPath(dir.path());workspace.show();workspace.findChild<QPushButton*>("workspaceFullSearchButton")->click();QVERIFY(workspace.findChild<WorkspaceSearch*>()->isVisible());
    }
    void shortcutsPersistRejectConflictsAndUpdateActions(){
        QTemporaryDir dir;Preferences p(nullptr,dir.filePath("settings.json"));QAction action(nullptr);bindShortcut(&action,&p,"findAction");QCOMPARE(action.shortcut(),QKeySequence::Find);QVERIFY(!p.setValues("shortcuts",{{"findAction",p.value("shortcuts","saveAction")}}));QCOMPARE(action.shortcut(),QKeySequence::Find);
        QVERIFY(p.setValues("shortcuts",{{"findAction","Ctrl+Alt+F"}}));QCOMPARE(action.shortcut(),QKeySequence("Ctrl+Alt+F"));QVERIFY(!p.setValues("shortcuts",{{"replaceAction","Ctrl+Alt+F, Ctrl+H"}}));QVERIFY(!p.setValues("shortcuts",{{"findAction","A"}}));Preferences reopened(nullptr,dir.filePath("settings.json"));QCOMPARE(reopened.value("shortcuts","findAction").toString(),QString("Ctrl+Alt+F"));QVERIFY(reopened.setValues("shortcuts",{{"findAction",""}}));QVERIFY(validShortcut(""));QVERIFY(!validShortcut("Ctrl+UnknownKey"));
    }
    void recoveryPreservesBytesAndCorruptRecords(){
        QTemporaryDir dir;const auto path=dir.filePath("state.json");const QString id=QUuid::createUuid().toString();const QJsonObject entry{{"id",id},{"path",dir.filePath("original.md")},{"root",QString()},{"content",QString::fromLatin1(QStringLiteral("未保存😀").toUtf8().toBase64())}};
        {RecoveryStore store(path);store.save({{"schemaVersion",1},{"documents",QJsonArray{entry}},{"drafts",QJsonArray{entry}}});QVERIFY(store.flush());}RecoveryStore reopened(path);QCOMPARE(reopened.state()["drafts"].toArray().size(),1);QCOMPARE(QByteArray::fromBase64(reopened.state()["drafts"].toArray()[0].toObject()["content"].toString().toLatin1()),QStringLiteral("未保存😀").toUtf8());QVERIFY(!QFile::exists(dir.filePath("original.md")));
        const auto broken=dir.filePath("broken.json");QFile f(broken);QVERIFY(f.open(QIODevice::WriteOnly));f.write("broken");f.close();RecoveryStore invalid(broken);QVERIFY(!invalid.error().isEmpty());invalid.save(reopened.state());QVERIFY(!invalid.flush());QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),QByteArray("broken"));
    }
    void recoveredDraftRequiresSaveAsAndProtectsOriginal(){
        QTemporaryDir dir;FileManager manager(nullptr,{},false);QSignalSpy opened(&manager,&FileManager::opened);manager.createDraft("recovered",QUuid::createUuid(),true);QTRY_COMPARE(opened.size(),1);auto *doc=qvariant_cast<FileDocument*>(opened.first()[1]);QVERIFY(doc&&doc->untitled());QVERIFY(doc->bom());QVERIFY(doc->dirty());QCOMPARE(doc->session()->snapshot().utf8,QByteArray("recovered"));QVERIFY(!doc->save());const auto path=dir.filePath("chosen.md");QVERIFY(doc->saveAs(path));QTRY_VERIFY(!doc->saving());QVERIFY(!doc->untitled());QVERIFY(!doc->dirty());QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("\xef\xbb\xbf")+"recovered");
    }
    void unicodeLiteralWordCaseAndBounds(){
        QCOMPARE(findText(QStringLiteral("中文 😀 One one alone"),"one").items.size(),3);
        QCOMPARE(findText(QStringLiteral("中文 😀 One one alone"),"one",false,true).items.size(),2);
        QCOMPARE(findText("One one","one",true).items.size(),1);
        QCOMPARE(findText(QStringLiteral("😀word😀"),"word",false,true).items.size(),1);
        const auto emoji=findText(QStringLiteral("甲😀乙😀"),QStringLiteral("😀"));QCOMPARE(emoji.items.size(),2);QCOMPARE(emoji.items[0].start,1);QCOMPARE(emoji.items[0].length,2);
        const auto limited=findText("aaa","a",false,false,2);QCOMPARE(limited.items.size(),2);QVERIFY(!limited.complete);QVERIFY(findText("a","").items.isEmpty());
    }
    void replacementsUseOneUndoAndRespectReadOnly(){
        Appearance a;DocumentSession session("one one");DocumentSearch bar(&a,nullptr);bar.setSession(&session);bar.open(true);bar.findChild<QLineEdit*>("findQuery")->setText("one");bar.findChild<QLineEdit*>("replaceText")->setText(QStringLiteral("中文"));
        bar.findChild<QPushButton*>("replaceAll")->click();QCOMPARE(session.snapshot().utf8,QStringLiteral("中文 中文").toUtf8());QCOMPARE(session.undo(),EditResult::Applied);QCOMPARE(session.snapshot().utf8,QByteArray("one one"));
        session.setReadOnly(true);bar.findChild<QPushButton*>("replaceAll")->click();QCOMPARE(session.snapshot().utf8,QByteArray("one one"));session.setReadOnly(false);QCOMPARE(session.redo(),EditResult::Applied);QCOMPARE(session.snapshot().utf8,QStringLiteral("中文 中文").toUtf8());
    }
    void workspaceUsesDirtyBufferSkipsInvalidAndCancels(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto write=[&](const QString&name,const QByteArray&bytes){QFile f(dir.filePath(name));QVERIFY(f.open(QIODevice::WriteOnly));QCOMPARE(f.write(bytes),bytes.size());};write("a.md","disk only");write("b.md",QStringLiteral("中文 target\nnext target").toUtf8());write("invalid.md",QByteArray(1,char(0xff)));write("other.txt","target");
        auto root=WorkspaceResourceReader::open(dir.path()).reader;QVERIFY(root);const auto metadata=root->inspect("a.md");QVERIFY(metadata.ok());QHash<QString,SearchBuffer> buffers{{"a.md",{"unsaved target",metadata.identity,true}}};auto cancel=std::make_shared<std::atomic_bool>(false);
        const auto result=searchWorkspace(root,"target",buffers,cancel);QCOMPARE(result.hits.size(),3);QVERIFY(!result.warning.isEmpty());bool dirty=false;for(const auto&hit:result.hits){QVERIFY(hit.end>hit.start);if(hit.path=="a.md"){dirty=true;QCOMPARE(hit.start,qsizetype(8));}QVERIFY(hit.path.endsWith(".md"));}QVERIFY(dirty);
        auto stale=buffers;stale["a.md"].identity="stale-identity";const auto rejected=searchWorkspace(root,"unsaved",stale,cancel);QVERIFY(rejected.hits.isEmpty());QVERIFY(!rejected.warning.isEmpty());
#ifdef Q_OS_WIN
        QHash<QString,SearchBuffer> alternate{{"A.MD",buffers["a.md"]}};QCOMPARE(searchWorkspace(root,"unsaved",alternate,cancel).hits.size(),1);
#endif
        cancel->store(true);QVERIFY(searchWorkspace(root,"target",{},cancel).cancelled);cancel->store(false);root->revoke();QVERIFY(searchWorkspace(root,"target",{},cancel).hits.isEmpty());
    }
};
int main(int argc,char**argv){initializeGuiRuntime();QApplication app(argc,argv);configureGuiApplication();ProductToolsTests tests;return QTest::qExec(&tests,argc,argv);}
#include "product_tools_tests.moc"
