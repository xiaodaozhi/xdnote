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
#include "UiFonts.h"
#include "DocumentBreadcrumb.h"
#include "NavigationStore.h"
#include "WorkspaceNavigation.h"
#include "WorkspaceTree.h"
#include "WindowGeometry.h"
#include "DocumentSearch.h"
#include "VisualTokens.h"
#include "file_fixtures.h"
#include <QAbstractItemModel>
#include <QAccessible>
#include <QAction>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QFontInfo>
#include <QPainter>
#include <QTextLayout>
#include <QGlyphRun>
#include <QRawFont>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QStyleOptionFrame>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QScreen>
#include <QScrollArea>
#include <QScopeGuard>
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
QJsonObject textInkSample(QWidget *widget, const QString &kind, const QString &value, const QColor &foreground, QRect line = {}, int left = 5, int right = 5) {
    widget->ensurePolished(); QApplication::processEvents();
    const auto image = widget->grab().toImage().convertToFormat(QImage::Format_ARGB32); const qreal dpr = image.devicePixelRatio();
    if (line.isNull()) line = widget->rect();
    // A tight label can put ink on every edge. Use the modal surface color
    // inside the text area instead of guessing one background pixel.
    QHash<QRgb,int> colors;QRgb backgroundRgb=0;int largest=0;
    for(int y=qMax(0,qRound(line.top()*dpr));y<qMin(image.height(),qRound((line.bottom()+1)*dpr));++y)
        for(int x=qMax(0,qRound(left*dpr));x<qMin(image.width(),qRound((widget->width()-right)*dpr));++x){const auto rgb=image.pixel(x,y);const int count=++colors[rgb];if(count>largest){largest=count;backgroundRgb=rgb;}}
    const QColor background=QColor::fromRgba(backgroundRgb);
    const double dr=foreground.red()-background.red(),dg=foreground.green()-background.green(),db=foreground.blue()-background.blue(),den=dr*dr+dg*dg+db*db;
    int top=image.height(),bottom=-1,x0=image.width(),x1=-1; double weight=0;
    for(int y=qMax(0,qRound((line.top()+1)*dpr));y<qMin(image.height(),qRound((line.bottom())*dpr));++y)
        for(int x=qMax(0,qRound(left*dpr));x<qMin(image.width(),qRound((widget->width()-right)*dpr));++x){
            const auto c=image.pixelColor(x,y);const double r=c.red()-background.red(),g=c.green()-background.green(),b=c.blue()-background.blue();
            const double alpha=den>0?(r*dr+g*dg+b*db)/den:0;
            const double error=qAbs(r-alpha*dr)+qAbs(g-alpha*dg)+qAbs(b-alpha*db);
            // Windows ClearType colors edge pixels independently in RGB. A
            // grayscale-only match drops thin strokes and invents a shift.
            if(alpha>=.30&&alpha<=1.1&&error<120){top=qMin(top,y);bottom=qMax(bottom,y);x0=qMin(x0,x);x1=qMax(x1,x);weight+=alpha;}
        }
    const auto font=widget->font(); const QFontMetricsF metrics(font,widget); const qreal center=line.top()+line.height()/2.0;
    QJsonObject result{{"kind",kind},{"text",value},{"class",widget->metaObject()->className()},{"font",QFontInfo(font).family()},{"pixelSize",font.pixelSize()},{"weight",int(font.weight())},{"dpr",dpr},{"width",widget->width()},{"height",widget->height()},{"targetCenter",center},{"ascent",metrics.ascent()},{"descent",metrics.descent()},{"lineHeight",metrics.height()},{"found",bottom>=top},{"inkWeight",weight}};
    QTextLayout shaped(value,font,widget);shaped.beginLayout();auto shapedLine=shaped.createLine();if(shapedLine.isValid())shapedLine.setLineWidth(widget->width());shaped.endLayout();
    QRectF bounds;QJsonArray families;
    for(const auto &run:shaped.glyphRuns()){families.append(run.rawFont().familyName());const auto ids=run.glyphIndexes();const auto positions=run.positions();for(qsizetype i=0;i<ids.size();++i){const auto box=run.rawFont().boundingRect(ids[i]).translated(positions[i]);if(!box.isEmpty())bounds=bounds.isNull()?box:bounds.united(box);}}
    result["glyphFonts"]=families;result["layoutHeight"]=shapedLine.isValid()?shapedLine.height():0;result["layoutInkTop"]=bounds.top();result["layoutInkBottom"]=bounds.bottom();
    if(bottom>=top){const qreal actual=(top+bottom+1)/(2*dpr);result["inkTop"]=top/dpr;result["inkBottom"]=(bottom+1)/dpr;result["inkCenter"]=actual;result["offsetDown"]=actual-center;}
    const auto dir=qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");if(!dir.isEmpty()){const auto name=kind+"-"+widget->objectName()+"-"+QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(),QCryptographicHash::Sha256).toHex().left(10))+"-"+QString::number(font.pixelSize());QDir().mkpath(dir+"/text-ink");const auto relative="text-ink/"+name+".png";image.save(dir+"/"+relative);result["capture"]=relative;}
    return result;
}
}
class ControlConsistencyTests final: public QObject {
    Q_OBJECT
private slots:
    void licenseDialogFitsAvailableArea() {
        QTemporaryDir dir;Appearance appearance(nullptr,dir.filePath("settings.json"));SettingsWindow w(&appearance,nullptr);w.show();w.selectPage(5);
        for(const auto theme:{QString("light"),QString("dark")}){
            QVERIFY(appearance.setPreferences(theme,"graphite"));bool seen=false;
            QTimer::singleShot(50,&w,[&]{auto*dialog=w.findChild<QDialog*>("thirdPartyLicenses");if(!dialog)return;const auto close=qScopeGuard([dialog]{dialog->reject();});seen=true;QVERIFY(dialog->screen()->availableGeometry().contains(dialog->geometry()));
                auto*scroll=dialog->findChild<QScrollArea*>("dialogContentScroll");QVERIFY(scroll);
                for(auto*button:dialog->findChildren<SharedButton*>())if(button->isVisible()){scroll->ensureWidgetVisible(button);QApplication::processEvents();QVERIFY2(scroll->viewport()->rect().contains(button->mapTo(scroll->viewport(),button->rect().center())),qPrintable(button->objectName()));}
                evidence(dialog,theme+"-license-available-area");dialog->reject();});
            QTest::mouseClick(w.findChild<SharedButton*>("settingsThirdPartyLicenses"),Qt::LeftButton);QVERIFY(seen);
        }
    }
    void breadcrumbMatchesSameTextLabelBaseline() {
        QTemporaryDir dir;Appearance appearance(nullptr,dir.filePath("settings.json"));QWidget owner;owner.setAttribute(Qt::WA_DontShowOnScreen);owner.resize(600,120);owner.show();
        for(int size:{12,14})for(const auto &name:{QStringLiteral("写作 ABC.md"),QStringLiteral("gypq 123.md")}) {
            DocumentBreadcrumb breadcrumb(&appearance,&owner);breadcrumb.setFont(uiFont(size));breadcrumb.setGeometry(0,0,580,32);breadcrumb.setDocumentPath("/workspace/"+name,"/workspace");breadcrumb.show();
            QLabel reference(breadcrumb.fullText(),&owner);reference.setFont(uiFont(size));reference.setAlignment(Qt::AlignLeft|Qt::AlignVCenter);reference.setGeometry(0,40,580,32);reference.show();
            const auto actual=textInkSample(&breadcrumb,"breadcrumb-same-text",breadcrumb.fullText(),appearance.color("text"),{},0,0);const auto expected=textInkSample(&reference,"breadcrumb-reference",reference.text(),appearance.color("text"),{},0,0);
            QVERIFY(actual["found"].toBool()&&expected["found"].toBool());QVERIFY(qAbs(actual["inkCenter"].toDouble()-expected["inkCenter"].toDouble())<=1.0);
        }
    }
    void allUiIconsRenderAcrossSizesAndDensities() {
        const QDir assets(QStringLiteral(":/xdnote/design/assets/ui-v2/icons/svg"));
        const auto files=assets.entryList({"*.svg"},QDir::Files);QVERIFY(files.size()>=90);
        for(const auto &file:files)for(const QColor color:{QColor("#1F2937"),QColor("#E4E7EC")})
            for(int size:{12,16,20,24})for(qreal dpr:{1.0,1.5,2.0,3.0}){
                const auto icon=themedIcon(file.chopped(4),color);QVERIFY2(!icon.isNull(),qPrintable(file));
                const auto pixmap=icon.pixmap(QSize(size,size),dpr);const auto image=pixmap.toImage();QVERIFY(!image.isNull());
                QCOMPARE(image.width(),qRound(size*dpr));QCOMPARE(image.height(),qRound(size*dpr));
                bool ink=false,transparent=false;
                for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){const int alpha=qAlpha(image.pixel(x,y));ink|=alpha>100;transparent|=alpha==0;}
                QVERIFY2(ink&&transparent,qPrintable(file+": empty glyph or opaque background"));
                const auto disabled=icon.pixmap(QSize(size,size),dpr,QIcon::Disabled);QVERIFY(!disabled.isNull());
            }
        QCOMPARE(themedIcon("status-dirty",Qt::red).pixmap(16,16).toImage(),themedIcon("dirty",Qt::red).pixmap(16,16).toImage());
        QCOMPARE(colorSwatch(Qt::red).pixmap(16,16).toImage(),themedIcon("accent-swatch",Qt::red).pixmap(16,16).toImage());
        QVERIFY(themedIcon("does-not-exist",Qt::red).isNull());
    }
    void uiTextAlignmentSurvey() {
        QTemporaryDir dir; Appearance appearance(nullptr,dir.filePath("settings.json")); QWidget fixture; fixture.setAttribute(Qt::WA_DontShowOnScreen);fixture.resize(640,320); fixture.setFont(uiFont()); fixture.setAutoFillBackground(true);
        auto palette=fixture.palette();palette.setColor(QPalette::Window,appearance.color("background"));palette.setColor(QPalette::WindowText,appearance.color("text"));fixture.setPalette(palette);fixture.setFocusPolicy(Qt::StrongFocus);fixture.show();fixture.setFocus();
        QJsonArray samples;const QStringList texts{QStringLiteral("保存"),QStringLiteral("打开文件"),QStringLiteral("Save"),QStringLiteral("100%"),QStringLiteral("写作 ABC"),QStringLiteral("gypq")};
        for(int size:{12,14,16})for(const auto &value:texts){
            const auto font=uiFont(size);
            const auto key=value==texts[0]?QString("han2"):value==texts[1]?QString("han4"):value==texts[2]?QString("latin"):value==texts[3]?QString("digits"):value==texts[4]?QString("mixed"):QString("descenders");
            SharedButton button(value,"textSurveyButton",&appearance,&fixture);button.setFont(font);button.setGeometry(20,20,220,32);button.show();
            samples.append(textInkSample(&button,"button-"+key,value,appearance.color("text")));
            button.setProperty("primary",true);samples.append(textInkSample(&button,"primary-"+key,value,appearance.accentColor("foreground")));
            button.setEnabled(false);samples.append(textInkSample(&button,"disabled-"+key,value,appearance.color("textSecondary")));button.hide();
            SharedLineEdit input(&appearance,&fixture);input.setFont(font);input.setText(value);input.setGeometry(20,20,220,32);input.show();input.clearFocus();fixture.setFocus();
            samples.append(textInkSample(&input,"input-"+key,value,appearance.color("text"),{},12,32));input.hide();
            SharedCheckBox check(value,&appearance,&fixture);check.setFont(font);check.setGeometry(20,20,220,32);check.show();samples.append(textInkSample(&check,"checkbox-"+key,value,appearance.color("text"),{},28,8));check.hide();
            QLabel label(value,&fixture);label.setFont(font);label.setAlignment(Qt::AlignCenter);label.setGeometry(20,20,220,32);label.show();samples.append(textInkSample(&label,"label-"+key,value,appearance.color("text")));label.hide();
            if(size==12){SharedButton footer(value,"textSurveyFooter",&appearance,&fixture);footer.setFont(font);footer.setProperty("footerControl",true);footer.setFixedHeight(20);footer.setGeometry(20,20,220,20);footer.show();samples.append(textInkSample(&footer,"footer-"+key,value,appearance.color("text")));footer.hide();}
        }
        QCOMPARE(samples.size(),114);
        for(const auto &item:samples){
            const auto sample=item.toObject();QVERIFY2(sample["found"].toBool(),qPrintable(sample["kind"].toString()));
            for(const auto &reference:samples){const auto label=reference.toObject();
                if(label["kind"].toString().startsWith("label-")&&label["text"]==sample["text"]&&label["pixelSize"]==sample["pixelSize"])
                    QVERIFY2(qAbs(sample["offsetDown"].toDouble()-label["offsetDown"].toDouble())<=1.0,qPrintable(sample["kind"].toString()+": baseline differs from native QLabel reference"));
            }
        }
        const auto output=qEnvironmentVariable("XDNOTE_TEST_EVIDENCE",dir.path());QDir().mkpath(output);QFile file(output+"/text-alignment-survey.json");QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(QJsonObject{{"kind","controlled Qt raster survey; offsets are observations, not automatic defect classifications"},{"qpa",QGuiApplication::platformName()},{"samples",samples}}).toJson());
    }
    void uiTextAlignmentSurfaces() {
        QTemporaryDir dir;
        Appearance appearance(nullptr,dir.filePath("settings.json"));
        WindowGeometryStore geometry(nullptr,dir.filePath("geometry.json"));
        NavigationStore navigation(nullptr,dir.filePath("navigation.json"));
        FileManager manager;
        const auto path=dir.filePath(QStringLiteral("文字居中.md"));
        QVERIFY(fileFixtures::write(path,QStringLiteral("# 文字居中\n\n正文\n\n```cpp\nint x = 1;\n```\n").toUtf8()));
        manager.open(path); QTRY_COMPARE(manager.documentCount(),1);
        QVERIFY(navigation.recordOpen(path));
        QJsonArray samples, blocks;
        QString surface, theme;
        auto append=[&](QWidget *widget,const QString &kind,const QString &text,QColor color,QRect line=QRect(),int left=5,int right=5){
            auto sample=textInkSample(widget,theme+"-"+surface+"-"+kind,text,color,line,left,right);sample["kind"]=kind;
            sample["surface"]=surface;sample["theme"]=theme;sample["name"]=widget->objectName();
            const auto point=widget->mapTo(widget->window(),QPoint());sample["windowX"]=point.x();sample["windowY"]=point.y();
            samples.append(sample);
        };
        auto collect=[&](QWidget *window){
            QApplication::processEvents();
            for(auto *button:window->findChildren<SharedButton *>())if(button->isVisible()&&!button->text().isEmpty()){
                const bool solid=button->property("primary").toBool()||button->isChecked();
                append(button,"surface-button",button->text(),!button->isEnabled()?appearance.color("textSecondary"):solid?appearance.accentColor("foreground"):appearance.color("text"),{},button->property("navigationRow").toBool()?38:5,button->menu()?32:5);
            }
            for(auto *label:window->findChildren<QLabel *>())if(label->isVisible()&&!label->text().isEmpty()){
                const bool multiline=label->text().contains('\n')||(label->wordWrap()&&label->fontMetrics().boundingRect(QRect(0,0,label->contentsRect().width(),10000),Qt::TextWordWrap,label->text()).height()>label->fontMetrics().height()+1);
                if(multiline||label->textFormat()==Qt::RichText||!(label->alignment()&Qt::AlignVCenter)){
                    blocks.append(QJsonObject{{"surface",surface},{"theme",theme},{"name",label->objectName()},{"text",label->text()},{"reason","paragraph/wrapped or intentionally non-centered label; inspect its line layout separately"}});continue;
                }
                append(label,"surface-label",label->text(),label->palette().color(QPalette::WindowText),{},0,0);
            }
            for(auto *field:window->findChildren<QLineEdit *>())if(field->isVisible()){
                // A repeated full-height Han reference separates baseline placement
                // from the shapes of short numbers, punctuation or descenders.
                const QSignalBlocker blocked(field);const auto previous=field->text();field->setText(QStringLiteral("保存"));field->clearFocus();
                append(field,"surface-input",field->text(),appearance.color("text"),{},4,32);
                field->setText(previous);
            }
            for(auto *card:window->findChildren<QAbstractButton *>())if(card->isVisible()&&!qobject_cast<SharedButton *>(card)&&!card->text().isEmpty()&&card->height()>100){
                const bool isTheme=card->objectName().startsWith("settings-theme-");
                append(card,"surface-card",card->text(),appearance.color("text"),QRect(4,isTheme?88:70,card->width()-8,24),4,4);
            }
            evidence(window,theme+"-text-"+surface);
        };
        SettingsWindow settings(&appearance,&geometry);settings.setAttribute(Qt::WA_DontShowOnScreen);settings.resize(1080,820);
        SingleFileWindow standalone(&manager,&appearance,&geometry);standalone.setAttribute(Qt::WA_DontShowOnScreen);standalone.resize(1000,760);standalone.setDocument(manager.documents().first());
        WorkspaceWindow workspace(&appearance,&geometry,&navigation,&manager);workspace.setAttribute(Qt::WA_DontShowOnScreen);workspace.resize(1200,820);workspace.setRootPath(dir.path());
        auto *pane=new SingleFileWindow(&manager,&appearance,&geometry);pane->setDocument(manager.documents().first());QVERIFY(workspace.addPane(pane));
        for(const auto &scheme:{QString("light"),QString("dark")}){
            theme=scheme;QVERIFY(appearance.setPreferences(scheme,"graphite"));
            settings.show();
            for(int page=0;page<6;++page){settings.selectPage(page);surface="settings-"+QString::number(page);collect(&settings);}
            settings.hide();standalone.show();
            for(const auto mode:{SingleFileWindow::ViewMode::Reading,SingleFileWindow::ViewMode::Source,SingleFileWindow::ViewMode::Split}){
                standalone.setViewMode(mode);surface="standalone-"+QString::number(int(mode));collect(&standalone);
            }
            auto *find=standalone.findChild<DocumentSearch *>();QVERIFY(find);find->show();surface="document-search";collect(find);find->hide();standalone.hide();
            workspace.show();
            for(const auto mode:{SingleFileWindow::ViewMode::Reading,SingleFileWindow::ViewMode::Source,SingleFileWindow::ViewMode::Split}){
                pane->setViewMode(mode);surface="workspace-"+QString::number(int(mode));collect(&workspace);
            }
            surface="workspace";
            auto *tabs=workspace.findChild<QTabBar *>("workspaceTabs");QVERIFY(tabs);QVERIFY(tabs->count()>0);
            const auto r=tabs->tabRect(0);const auto tokens=visualTokens()["tabs"].toObject();
            append(tabs,"tab-title",tabs->tabText(0),appearance.color("text"),r,r.left()+tokens["padding"].toInt()+24,tabs->width()-r.right()+tokens["closeHit"].toInt()+tokens["closeInset"].toInt()+4);
            auto *crumb=workspace.findChild<DocumentBreadcrumb *>();QVERIFY(crumb);append(crumb,"breadcrumb",crumb->fullText(),appearance.color("text"));
            auto *tree=workspace.tree();QTRY_VERIFY(tree->model()->rowCount()>0);
            const auto index=tree->model()->index(0,0);const auto row=tree->visualRect(index);QVERIFY(!row.isEmpty());
            append(tree->viewport(),"tree-title",index.data().toString(),appearance.color("text"),row,row.left()+28,12);
            workspace.hide();
            WorkspaceNavigation recent(&appearance,&navigation,nullptr);recent.setAttribute(Qt::WA_DontShowOnScreen);recent.setFont(uiFont());recent.resize(700,420);recent.showPage(WorkspaceNavigation::Page::Recent,dir.path());recent.show();surface="recent";collect(&recent);
            auto *list=recent.findChild<QListView *>("navigationResults");QVERIFY(list);QVERIFY(list->model()->rowCount()>1);const auto item=list->model()->index(1,0);const auto itemRect=list->visualRect(item);
            append(list->viewport(),"recent-title",item.data().toString(),appearance.color("text"),QRect(itemRect.left(),itemRect.top()+12,itemRect.width(),22),56,140);
            recent.hide();
            SharedMenu menu(&appearance,&workspace);menu.setAttribute(Qt::WA_DontShowOnScreen);auto *action=menu.addAction(QStringLiteral("保存"));action->setShortcut(QKeySequence::Save);menu.addAction(QStringLiteral("打开文件"));menu.resize(menu.sizeHint());menu.show();surface="menu";
            const auto actionRect=menu.actionGeometry(action);QVERIFY(!actionRect.isEmpty());append(&menu,"menu-title",action->text(),appearance.color("text"),actionRect,actionRect.left()+8,menu.width()-actionRect.left()-44);evidence(&menu,theme+"-text-menu");menu.hide();
        }
        QVERIFY(samples.size()>100);
        for(const auto &item:samples)QVERIFY2(item.toObject()["found"].toBool(),qPrintable(item.toObject()["surface"].toString()+"/"+item.toObject()["name"].toString()));
        const auto output=qEnvironmentVariable("XDNOTE_TEST_EVIDENCE",dir.path());QDir().mkpath(output);QFile file(output+"/text-alignment-surfaces.json");QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"kind","hidden controlled Qt widgets; no interaction with user windows or settings"},{"qpa",QGuiApplication::platformName()},{"samples",samples},{"paragraphLabels",blocks}}).toJson());
    }
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
