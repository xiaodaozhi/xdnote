#include "GuiRuntime.h"
#include "DocumentSearch.h"
#include "WorkspaceSearch.h"
#include "Appearance.h"
#include "SharedWidgets.h"
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
    void unicodeLiteralWordCaseAndBounds(){
        QCOMPARE(findText(QStringLiteral("中文 😀 One one alone"),"one").items.size(),3);
        QCOMPARE(findText(QStringLiteral("中文 😀 One one alone"),"one",false,true).items.size(),2);
        QCOMPARE(findText("One one","one",true).items.size(),1);
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
        cancel->store(true);QVERIFY(searchWorkspace(root,"target",{},cancel).cancelled);cancel->store(false);root->revoke();QVERIFY(searchWorkspace(root,"target",{},cancel).hits.isEmpty());
    }
};
int main(int argc,char**argv){initializeGuiRuntime();QApplication app(argc,argv);configureGuiApplication();ProductToolsTests tests;return QTest::qExec(&tests,argc,argv);}
#include "product_tools_tests.moc"
