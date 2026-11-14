#include "SettingsWindow.h"
#include "Shortcuts.h"
#include "SystemIntegration.h"
#include "Appearance.h"
#include "Preferences.h"
#include "WindowGeometry.h"
#include "SharedWidgets.h"
#include "InputModality.h"
#include "UiFonts.h"
#include "VisualTokens.h"
#include "DocumentSession.h"
#include "SourceEditor.h"
#include "NativePreviewView.h"
#include <QCheckBox>
#include <QButtonGroup>
#include <QAction>
#include <QKeySequence>
#include <QSet>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QGridLayout>
#include <QKeyEvent>
#include <QEnterEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QUrl>
#include <QPainterPath>
#include <QTextBrowser>
#include <QFile>
#include <QCoreApplication>
#include <QSvgRenderer>
#include <QScrollBar>
#include <QDir>
#include <QSignalBlocker>
#include <QTextLayout>
#include <QGlyphRun>
#include <algorithm>

namespace xdnote {
namespace {
int setting(const char *key) { return visualTokens()["settings"].toObject()[key].toInt(); }
QVBoxLayout *bodyLayout(QWidget *body) { return qobject_cast<QVBoxLayout *>(body->layout()); }
class OpticalHeading final : public QLabel {
public:
    OpticalHeading(const QString &text,QWidget *parent):QLabel(text,parent){setObjectName("settingsHeading");setTextFormat(Qt::PlainText);}
protected:
    void paintEvent(QPaintEvent *event) override {
        QTextLayout layout(text(),font(),this);QTextOption option;option.setWrapMode(QTextOption::NoWrap);layout.setTextOption(option);
        layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(width());layout.endLayout();
        QRectF ink;for(const auto &run:layout.glyphRuns()){const auto glyphs=run.glyphIndexes();const auto positions=run.positions();for(qsizetype i=0;i<glyphs.size();++i){const auto bounds=run.rawFont().boundingRect(glyphs[i]).translated(positions[i]);if(!bounds.isEmpty())ink=ink.isNull()?bounds:ink.united(bounds);}}
        if(ink.isNull()){QLabel::paintEvent(event);return;}
        QPainter painter(this);painter.setClipRect(contentsRect());painter.setPen(palette().color(QPalette::WindowText));
        layout.draw(&painter,QPointF(contentsRect().left(),contentsRect().top()+(contentsRect().height()-ink.height())/2.0-ink.top()));
    }
};
void showLicenses(Appearance *appearance,QWidget*owner){
    SharedDialog dialog(appearance,owner,visualTokens()["windows"].toObject()["licenseDialog"].toObject()["preferredWidth"].toInt());dialog.setObjectName("thirdPartyLicenses");dialog.setWindowTitle(QStringLiteral("第三方许可"));auto*l=new QVBoxLayout(dialog.surface());l->setContentsMargins(24,24,24,24);l->setSpacing(16);auto*top=new QHBoxLayout;auto*title=new QLabel(QStringLiteral("第三方组件与许可"),&dialog);auto f=uiFont(18);f.setWeight(QFont::DemiBold);title->setFont(f);top->addWidget(title,1);auto*close=new SharedButton({},"dialogClose",appearance,&dialog);close->setSymbol("close");close->setAccessibleName(QStringLiteral("关闭第三方许可"));close->setFixedWidth(32);top->addWidget(close);l->addLayout(top);QObject::connect(close,&QPushButton::clicked,&dialog,&QDialog::reject);
    auto*view=new QTextBrowser(&dialog);view->setObjectName("thirdPartyLicenseText");view->setMinimumSize(0,120);view->setOpenLinks(false);view->setFont(uiFont());
    const auto refresh=[view,appearance]{view->setStyleSheet(QStringLiteral("QTextBrowser{background:%1;color:%2;border:1px solid %3;border-radius:6px;padding:12px}").arg(appearance->color("surface").name(),appearance->color("text").name(),appearance->color("border").name())+themedScrollBars(appearance));};QObject::connect(appearance,&Appearance::changed,view,refresh);refresh();l->addWidget(view);
    const auto current=std::make_shared<QString>();
    auto load=[view,current](const QString&path){QFile file(":/xdnote/"+path);if(!file.open(QIODevice::ReadOnly))return false;*current=path;if(path.endsWith(".md"))view->setMarkdown(QString::fromUtf8(file.readAll()));else view->setPlainText(QString::fromUtf8(file.readAll()));view->verticalScrollBar()->setValue(0);return true;};load("THIRD_PARTY_NOTICES.md");
    QObject::connect(view,&QTextBrowser::anchorClicked,&dialog,[load,current](const QUrl&url){if(url.isRelative()){const auto resolved=QUrl("qrc:/xdnote/"+*current).resolved(url);const auto path=QDir::cleanPath(resolved.path()).mid(QString("/xdnote/").size());const QSet<QString> known{"LICENSE","Cargo.lock","THIRD_PARTY_NOTICES.md","docs/engineering/licensing.md","tools/desktop-dependencies.json"};if((known.contains(path)||path.startsWith("LICENSES/"))&&load(path))return;if(path.startsWith("docs/")&&!path.contains(".."))QDesktopServices::openUrl(QUrl("https://github.com/xiaodaozhi/xdnote/blob/main/"+path));}else if(url.scheme()=="https")QDesktopServices::openUrl(url);});
    view->setContextMenuPolicy(Qt::CustomContextMenu);QObject::connect(view,&QWidget::customContextMenuRequested,&dialog,[view,appearance](QPoint p){SharedMenu menu(appearance,view);auto*copy=menu.addAction(QStringLiteral("复制"));copy->setEnabled(view->textCursor().hasSelection());QObject::connect(copy,&QAction::triggered,view,&QTextBrowser::copy);menu.addAction(QStringLiteral("全选"),view,&QTextBrowser::selectAll);menu.exec(view->mapToGlobal(p));});auto*bottom=new QHBoxLayout;auto*back=new SharedButton(QStringLiteral("返回组件清单"),"licenseIndex",appearance,&dialog);back->setOutlined(true);bottom->addWidget(back);QObject::connect(back,&QPushButton::clicked,&dialog,[load]{load("THIRD_PARTY_NOTICES.md");});bottom->addStretch();auto*done=new SharedButton(QStringLiteral("完成"),"licenseDone",appearance,&dialog);done->setProperty("primary",true);bottom->addWidget(done);QObject::connect(done,&QPushButton::clicked,&dialog,&QDialog::accept);l->addLayout(bottom);dialog.exec();
}
class Toggle final : public QCheckBox {
public:
    Toggle(Appearance *appearance, QWidget *parent) : QCheckBox(parent), appearance(appearance) {
        setFixedSize(setting("toggleHitWidth"), visualTokens()["controls"].toObject()["height"].toInt());setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_MacShowFocusRect, false); setCursor(Qt::PointingHandCursor);
        connect(appearance, &Appearance::changed, this, qOverload<>(&QWidget::update));
        connect(InputModality::forApplication(), &InputModality::changed, this, qOverload<>(&QWidget::update));
        connect(this, &QCheckBox::clicked, this, [] { InputModality::forApplication()->clear(); });
    }
protected:
    bool hitButton(const QPoint &point) const override { return rect().contains(point); }
    void enterEvent(QEnterEvent *event) override { QCheckBox::enterEvent(event); update(); }
    void leaveEvent(QEvent *event) override { QCheckBox::leaveEvent(event); update(); }
    void keyPressEvent(QKeyEvent *event) override { if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { click(); event->accept(); } else QCheckBox::keyPressEvent(event); }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const int h = setting("toggleTrackHeight"), knob = setting("toggleKnob");
        const QRectF track((width()-setting("toggleWidth"))/2.0, (height() - h) / 2.0, setting("toggleWidth"), h);
        painter.setPen(Qt::NoPen); painter.setBrush(!isEnabled() ? appearance->color("border") : isChecked() ? appearance->accentColor(isDown() ? "pressed" : underMouse() ? "hover" : "solid") : appearance->color(isDown() ? "scrollbarPressed" : underMouse() ? "scrollbarHover" : "textSecondary"));
        painter.drawRoundedRect(track, h / 2.0, h / 2.0);
        painter.setBrush(isEnabled() ? (isChecked()?appearance->accentColor("foreground"):appearance->color("surface")) : appearance->color("textSecondary"));
        painter.drawEllipse(QRectF(isChecked() ? track.right() - knob - 3 : track.left()+3, (height() - knob) / 2.0, knob, knob));
        if (hasFocus() && isEnabled() && InputModality::forApplication()->keyboardNavigation()) {
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(appearance->accentColor(), 2)); painter.drawRoundedRect(track.adjusted(-2,-2,2,2),h/2.0+2,h/2.0+2);
        }
    }
private:
    Appearance *appearance;
};
class Card final : public QAbstractButton {
public:
    Card(Appearance *appearance, const QString &value, bool theme, QWidget *parent)
        : QAbstractButton(parent), appearance(appearance), value(value), theme(theme) {
        setCheckable(true); setFocusPolicy(Qt::StrongFocus); setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setMinimumWidth(setting("cardMinWidth")); setFixedHeight(setting(theme ? "themeCardHeight" : "accentCardHeight"));
        const auto record = visualTokens()["accents"].toObject()[value].toObject();
        setText(theme ? value == "light" ? QStringLiteral("浅色") : value == "dark" ? QStringLiteral("深色") : QStringLiteral("跟随系统") : record["label"].toString());
        setAccessibleName(text()); setObjectName((theme ? "settings-theme-" : "settings-accent-") + value);
        connect(appearance, &Appearance::changed, this, qOverload<>(&QWidget::update));
        connect(InputModality::forApplication(), &InputModality::changed, this, qOverload<>(&QWidget::update));
        connect(this, &QAbstractButton::clicked, this, [] { InputModality::forApplication()->clear(); });
    }
protected:
    void enterEvent(QEnterEvent *event) override { QAbstractButton::enterEvent(event); update(); }
    void leaveEvent(QEvent *event) override { QAbstractButton::leaveEvent(event); update(); }
    void keyPressEvent(QKeyEvent *event) override { if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { click(); event->accept(); } else QAbstractButton::keyPressEvent(event); }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(isChecked() ? appearance->accentColor() : appearance->color("border"), 1));
        painter.setBrush(isDown() ? appearance->color("controlPressed") : underMouse() ? appearance->color("controlHover") : appearance->color("surface"));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 6, 6);
        if (theme) {
            const qreal previewWidth = qMin(width() - 24, 160);
            const QRectF preview((width() - previewWidth) / 2, 12, previewWidth, 64);
            const auto miniature=[&](bool dark){painter.fillRect(preview,dark?QColor("#22272F"):QColor("#FCFCFB"));painter.fillRect(QRectF(preview.x(),preview.y(),preview.width(),10),dark?QColor("#1E232B"):QColor("#F4F5F7"));painter.fillRect(QRectF(preview.x(),preview.y()+10,24,preview.height()-10),dark?QColor("#1E232B"):QColor("#F4F5F7"));painter.fillRect(QRectF(preview.x()+3,preview.y()+23,19,7),dark?QColor("#445064"):QColor("#D6DDE7"));painter.fillRect(QRectF(preview.x()+30,preview.y()+3,35,8),dark?QColor("#343E4C"):Qt::white);painter.setPen(QPen(dark?QColor("#AAB4C0"):QColor("#667085"),1));for(int y:{24,34,44})painter.drawLine(QPointF(preview.x()+34,preview.y()+y),QPointF(preview.right()-(y==34?26:12),preview.y()+y));painter.setPen(QPen(dark?QColor("#3B424D"):QColor("#DDE2E8"),1));painter.drawLine(QPointF(preview.x(),preview.bottom()-7),QPointF(preview.right(),preview.bottom()-7));};
            painter.save();QPainterPath thumbnail;thumbnail.addRoundedRect(preview,3,3);painter.setClipPath(thumbnail);miniature(value=="dark");if(value=="system"){painter.setClipRect(QRectF(preview.center().x(),preview.y(),preview.width()/2,preview.height()),Qt::IntersectClip);miniature(true);}painter.restore();painter.setBrush(Qt::NoBrush);painter.setPen(QPen(appearance->color("border"),1));painter.drawRoundedRect(preview,3,3);
        } else {
            painter.setPen(Qt::NoPen); painter.setBrush(QColor(visualTokens()["accents"].toObject()[value].toObject()["base"].toString()));
            painter.drawEllipse(QPointF(width() / 2.0, 34), 20, 20);
        }
        painter.setPen(appearance->color("text")); painter.setFont(uiFont()); painter.drawText(QRect(4, theme ? 88 : 70, width() - 8, 24), Qt::AlignCenter, text());
        if (!theme) { painter.setFont(uiFont(12)); painter.setPen(appearance->color("textSecondary")); painter.drawText(QRect(4, 95, width() - 8, 20), Qt::AlignCenter, visualTokens()["accents"].toObject()[value].toObject()["base"].toString()); }
        if (isChecked()) { if(theme){const QRect mark(width()-24,5,18,18);painter.setBrush(appearance->accentColor());painter.setPen(Qt::NoPen);painter.drawEllipse(mark);themedIcon("check",appearance->accentColor("foreground")).paint(&painter,mark.adjusted(3,3,-3,-3));}else themedIcon("check",Qt::white).paint(&painter,QRect(width()/2-10,24,20,20)); }
        if (hasFocus() && InputModality::forApplication()->keyboardNavigation()) { painter.setBrush(Qt::NoBrush); painter.setPen(QPen(appearance->accentColor(), 2)); painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 4, 4); }
    }
private:
    Appearance *appearance;
    QString value;
    bool theme;
};
}
SettingsWindow::SettingsWindow(Appearance *appearance, WindowGeometryStore *store)
    : ApplicationWindow(appearance), m_appearance(appearance), m_preferences(appearance->preferences()) {
    setObjectName("settingsWindow"); setWindowTitle(QStringLiteral("设置 — xdnote")); setAttribute(Qt::WA_DeleteOnClose);
    setFont(uiFont()); const auto size = visualTokens()["windows"].toObject()["settings"].toObject();
    resize(size["defaultWidth"].toInt(), size["defaultHeight"].toInt()); setMinimumSize(size["minWidth"].toInt(), size["minHeight"].toInt());
    auto *root = new QVBoxLayout(this); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0); root->setSizeConstraint(QLayout::SetNoConstraint);
    auto *header = new WindowTitleBar(appearance, this); header->setTitle(QStringLiteral("设置"), false); setTitleBar(header); root->addWidget(header);
    m_feedback=new QWidget(this);m_feedback->setObjectName("settingsFeedback");auto*feedbackRow=new QHBoxLayout(m_feedback);feedbackRow->setContentsMargins(24,8,24,8);feedbackRow->setSpacing(12);auto*errorIcon=new QLabel(m_feedback);errorIcon->setObjectName("settingsErrorIcon");errorIcon->setFixedSize(16,16);feedbackRow->addWidget(errorIcon);m_feedbackText=new QLabel(m_feedback);m_feedbackText->setWordWrap(true);feedbackRow->addWidget(m_feedbackText,1);m_retrySettings=new SharedButton(QStringLiteral("重试"),"settingsRetry",appearance,m_feedback);m_retrySettings->setOutlined(true);feedbackRow->addWidget(m_retrySettings);auto*dismiss=new SharedButton({},"settingsDismissError",appearance,m_feedback);dismiss->setSymbol("close");dismiss->setAccessibleName(QStringLiteral("关闭提示"));dismiss->setFixedWidth(32);feedbackRow->addWidget(dismiss);root->addWidget(m_feedback);m_feedback->hide();connect(dismiss,&QPushButton::clicked,this,[this]{m_hiddenError=m_preferences->error();m_feedback->hide();});connect(m_retrySettings,&QPushButton::clicked,this,[this]{m_preferences->retryWrite();refresh();});
    auto *middle = new QWidget(this); auto *columns = new QHBoxLayout(middle); columns->setContentsMargins(0, 0, 0, 0); columns->setSpacing(0);
    m_sidebarScroll=new QScrollArea(middle);m_sidebarScroll->setObjectName("settingsNavigationScroll");m_sidebarScroll->setFrameShape(QFrame::NoFrame);m_sidebarScroll->setWidgetResizable(true);m_sidebarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);m_sidebarScroll->setFixedWidth(size["sidebarWidth"].toInt());m_sidebarScroll->setMinimumHeight(0);
    auto *sidebar = new QWidget(m_sidebarScroll); sidebar->setObjectName("settingsSidebar");m_sidebarScroll->setWidget(sidebar);
    auto *nav = new QVBoxLayout(sidebar); nav->setContentsMargins(16, 16, 16, 16); nav->setSpacing(8);
    m_search = new SharedLineEdit(appearance,sidebar);m_search->setClearButtonEnabled(true);auto*searchAction=m_search->addAction(themedIcon("search",appearance->color("textSecondary")),QLineEdit::LeadingPosition);connect(appearance,&Appearance::changed,m_search,[appearance,searchAction]{searchAction->setIcon(themedIcon("search",appearance->color("textSecondary")));});m_search->setObjectName("settingsSearch"); m_search->setPlaceholderText(QStringLiteral("搜索设置")); m_search->setAccessibleName(QStringLiteral("搜索设置")); nav->addWidget(m_search); nav->addSpacing(16);
    const QStringList titles{QStringLiteral("通用"), QStringLiteral("外观与配色"), QStringLiteral("编辑器"), QStringLiteral("阅读与渲染"), QStringLiteral("快捷键"), QStringLiteral("关于")};
    const QStringList icons{"settings", "palette", "edit", "read", "keyboard", "info"};
    for (int i = 0; i < titles.size(); ++i) { auto *button = new SharedButton(titles[i], "settingsPage-" + QString::number(i), appearance, sidebar); button->setSymbol(icons[i]); button->setFixedHeight(setting("navigationHeight")); button->setProperty("navigationRow", true); nav->addWidget(button); m_navigation.append(button); connect(button, &QPushButton::clicked, this, [this, i] { selectPage(i); }); }
    nav->addStretch(); columns->addWidget(m_sidebarScroll); columns->addWidget(new SharedSeparator(Qt::Vertical, appearance, middle));
    m_content=new QStackedWidget(middle);m_content->setObjectName("settingsContent");m_pages = new QStackedWidget(m_content); m_pages->setObjectName("settingsPages");m_content->addWidget(m_pages);m_searchResults=new QWidget(m_content);m_searchResults->setObjectName("settingsSearchResults");m_content->addWidget(m_searchResults);columns->addWidget(m_content, 1); root->addWidget(middle, 1);
    auto *general = page(titles[0], QStringLiteral("基本使用行为、打开偏好与保存保护。"));
    group(general, QStringLiteral("打开与窗口"));
    auto *externalTarget = choice("desktop", "externalOpenTarget", {{QStringLiteral("独立窗口"), "standalone"}, {QStringLiteral("工作区标签"), "workspace-tab"}});
    externalTarget->setToolTip(QStringLiteral("工作区标签仅适用于已打开工作区目录内的文件；目录外文件和菜单打开使用独立窗口。"));
    row(general, QStringLiteral("双击文件时打开为"), externalTarget);
    auto *modes = new SharedSegment(appearance,general);modes->setObjectName("settingsDefaultMode");auto *modeGroup = new QButtonGroup(modes);
    for (const auto &option : QList<QPair<QString, QString>>{{QStringLiteral("阅读"), "reading"}, {QStringLiteral("编辑"), "source"}}) {
        auto *button = new SharedButton(option.first, "settings-default-" + option.second, appearance, modes); button->setCheckable(true); modeGroup->addButton(button);modes->addButton(button);
        connect(button, &QPushButton::clicked, this, [this, value = option.second] { m_preferences->setValues("desktop", {{"standaloneDefaultMode", value}}); refresh(); }); m_bindings.append([this, button, value = option.second] { button->setChecked(m_preferences->value("desktop", "standaloneDefaultMode").toString() == value); });
    }
    row(general, QStringLiteral("独立窗口默认模式"), modes, QStringLiteral("只影响后续打开的新文档，不切换当前视图。"));
    row(general, QStringLiteral("重复文件"), new QLabel(QStringLiteral("聚焦已有文档"), general));
    group(general, QStringLiteral("文件关联")); auto *association = new SharedButton(QStringLiteral("设置默认应用"), "settingsAssociation", appearance, general); association->setOutlined(true); association->setEnabled(true);connect(association,&QPushButton::clicked,this,[this]{if(!openDefaultApplicationSettings())m_status->setFullText(associationStatus());});
    row(general, QStringLiteral("Markdown 文件关联"), association, QStringLiteral("默认应用由系统管理，注册打开方式不会替换当前默认值。"));
    auto *registerTypes=new SharedButton(QStringLiteral("注册此副本"),"settingsRegisterTypes",appearance,general);registerTypes->setOutlined(true);row(general,QStringLiteral("Markdown 打开方式"),registerTypes,associationStatus());auto *registrationRow=m_rows.last();connect(registerTypes,&QPushButton::clicked,this,[this,registrationRow]{m_status->setFullText(registerFileAssociations());for(auto *label:registrationRow->findChildren<QLabel*>())if(label->property("secondary").toBool())label->setText(associationStatus());});
    group(general, QStringLiteral("自动保存")); row(general, QStringLiteral("自动保存"), toggle("editor", "autoSaveEnabled"), QStringLiteral("默认关闭；冲突、只读或保存失败时暂停，未保存内容仍受关闭提醒保护。"));
    auto *delay = choice("editor", "autoSaveDelayMs", {{QStringLiteral("0.5 秒"), 500}, {QStringLiteral("1 秒"), 1000}, {QStringLiteral("1.5 秒"), 1500}, {QStringLiteral("3 秒"), 3000}, {QStringLiteral("5 秒"), 5000}, {QStringLiteral("10 秒"), 10000}});
    row(general, QStringLiteral("停止输入后保存"), delay); m_bindings.append([this, delay] { delay->setEnabled(m_preferences->value("editor", "autoSaveEnabled").toBool()); });
    group(general, QStringLiteral("关闭保护"));auto*protection=new QWidget(general);auto*protectionRow=new QHBoxLayout(protection);protectionRow->setContentsMargins(0,0,0,0);protectionRow->setSpacing(8);auto*lock=new QLabel(protection);lock->setObjectName("settingsCloseProtectionIcon");lock->setFixedSize(16,16);auto updateLock=[lock,appearance]{lock->setPixmap(themedIcon("lock",appearance->color("textSecondary")).pixmap(QSize(16,16),lock->devicePixelRatioF()));};connect(appearance,&Appearance::changed,lock,updateLock);updateLock();protectionRow->addWidget(lock);protectionRow->addWidget(new QLabel(QStringLiteral("已启用"),protection));row(general, QStringLiteral("始终提醒未保存的修改"),protection, QStringLiteral("关闭或退出前，需要先保存或明确放弃修改。"));
    auto *appearancePage = page(titles[1], QStringLiteral("主题与强调色分别设置，并同步所有窗口。"));
    group(appearancePage, QStringLiteral("界面主题")); auto *themeRow = new QWidget(appearancePage); m_themes = new QGridLayout(themeRow); m_themes->setContentsMargins(0, 0, 0, 0); m_themes->setSpacing(setting("cardGap"));
    for (const auto &name : {QString("light"), QString("dark"), QString("system")}) { auto *card = new Card(appearance, name, true, themeRow); m_themes->addWidget(card,0,m_themeCards.size());m_themeCards.append(card); connect(card, &QAbstractButton::clicked, this, [this, name] { m_appearance->setPreferences(name, m_appearance->accent()); refresh(); }); m_bindings.append([this, card, name] { card->setChecked(m_appearance->theme() == name); }); } themeRow->setProperty("settingText", QStringLiteral("界面主题 浅色 深色 跟随系统")); themeRow->setProperty("pageIndex", 1); m_rows.append(themeRow); bodyLayout(appearancePage)->addWidget(themeRow);
    group(appearancePage, QStringLiteral("强调色")); auto *swatches = new QWidget(appearancePage); m_swatches = new QGridLayout(swatches); m_swatches->setContentsMargins(0, 0, 0, 0); m_swatches->setSpacing(setting("cardGap"));
    for (const auto &name : {QString("graphite"), QString("mist-blue"), QString("cobalt"), QString("mauve"), QString("terracotta"), QString("olive")}) { auto *card = new Card(appearance, name, false, swatches); m_swatchCards.append(card); connect(card, &QAbstractButton::clicked, this, [this, name] { m_appearance->setPreferences(m_appearance->theme(), name); refresh(); }); m_bindings.append([this, card, name] { card->setChecked(m_appearance->accent() == name); }); } swatches->setProperty("settingText", QStringLiteral("强调色 石墨灰 雾蓝 钴蓝 灰紫 陶土 橄榄")); swatches->setProperty("pageIndex", 1); m_rows.append(swatches); bodyLayout(appearancePage)->addWidget(swatches);
    group(appearancePage, QStringLiteral("效果预览")); auto *preview = new SharedPanel(appearance,appearancePage);preview->setObjectName("settingsAppearancePreview");auto*sampleBody=new QVBoxLayout(preview);sampleBody->setContentsMargins(16,16,16,16);sampleBody->setSpacing(12);auto *sample = new QHBoxLayout;auto*exampleModes=new SharedSegment(appearance,preview);auto*examplesGroup=new QButtonGroup(exampleModes);auto *read = new SharedButton(QStringLiteral("阅读"), "settingsExampleRead", appearance, exampleModes); read->setCheckable(true); read->setChecked(true); auto *edit = new SharedButton(QStringLiteral("编辑"), "settingsExampleEdit", appearance, exampleModes);edit->setCheckable(true);examplesGroup->addButton(read);examplesGroup->addButton(edit);exampleModes->addButton(read);exampleModes->addButton(edit);auto *save = new SharedButton(QStringLiteral("保存"), "settingsExampleSave", appearance, preview); save->setProperty("primary", true);sample->addWidget(exampleModes);sample->addStretch();sample->addWidget(save);sampleBody->addLayout(sample);auto*exampleDocument=new SharedButton(QStringLiteral("写作，从这里开始.md"),"settingsExampleDocument",appearance,preview);exampleDocument->setSymbol("document");exampleDocument->setProperty("navigationRow",true);exampleDocument->setProperty("navigationSelected",true);exampleDocument->setFixedHeight(36);sampleBody->addWidget(exampleDocument);auto*exampleToggle=new Toggle(appearance,preview);exampleToggle->setObjectName("settingsExampleToggle");exampleToggle->setChecked(true);auto*toggleRow=new QHBoxLayout;toggleRow->addWidget(new QLabel(QStringLiteral("自动保存"),preview),1);toggleRow->addWidget(exampleToggle);sampleBody->addLayout(toggleRow);bodyLayout(appearancePage)->addWidget(preview);
    connect(read, &QPushButton::clicked, this, [read, edit] { read->setChecked(true); edit->setChecked(false); }); connect(edit, &QPushButton::clicked, this, [read, edit] { read->setChecked(false); edit->setChecked(true); }); save->setToolTip(QStringLiteral("此处为按钮外观示例，不操作文档。"));
    auto *editor = page(titles[2], QStringLiteral("设置编辑器字体、排版与缩进行为。")); group(editor, QStringLiteral("字体与排版")); row(editor, QStringLiteral("编辑器字体"), fontChoice("editor"));
    QList<QPair<QString, QJsonValue>> sizes; for (int n : {10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40}) sizes.append({QString::number(n), n});
    QList<QPair<QString, QJsonValue>> heights; for (double n : {1.0, 1.2, 1.4, 1.5, 1.6, 1.8, 2.0, 2.5, 3.0}) heights.append({QString::number(n), n});
    row(editor, QStringLiteral("字号"), choice("editor", "fontSize", sizes)); row(editor, QStringLiteral("行距"), choice("editor", "lineHeight", heights)); group(editor, QStringLiteral("编辑行为"));
    row(editor, QStringLiteral("软换行"), toggle("editor", "wordWrap")); row(editor, QStringLiteral("显示行号"), toggle("editor", "lineNumbers"));
    row(editor, QStringLiteral("缩进宽度"), choice("editor", "indentWidth", {{"1", 1}, {"2", 2}, {"4", 4}, {"8", 8}})); row(editor, QStringLiteral("使用空格缩进"), toggle("editor", "useSpaces"));
    group(editor, QStringLiteral("代码示例预览")); auto *session = new DocumentSession(QStringLiteral("# 写作，从这里开始\n\n把想法写下来，让思考慢慢清晰。\n\n- [x] 记录一个新想法\n- [ ] 整理阅读笔记\n").toUtf8(), this);
    auto*editorPanel=new SharedPanel(appearance,editor);editorPanel->setObjectName("settingsEditorPanel");auto*editorPanelLayout=new QVBoxLayout(editorPanel);editorPanelLayout->setContentsMargins(12,12,12,12);auto *editorPreview = new SourceEditor(session, editorPanel, appearance); editorPreview->setObjectName("settingsEditorPreview"); editorPreview->setMinimumHeight(180); editorPreview->setFocusPolicy(Qt::ClickFocus); editorPreview->installEventFilter(this); session->setReadOnly(true);editorPanelLayout->addWidget(editorPreview);bodyLayout(editor)->addWidget(editorPanel);
    auto *reader = page(titles[3], QStringLiteral("阅读排版即时生效，不修改 Markdown 内容。"));group(reader,QStringLiteral("文字与排版"));auto *readerBody = new QWidget(reader); m_readerLayout = new QBoxLayout(QBoxLayout::LeftToRight, readerBody); m_readerLayout->setContentsMargins(0, 0, 0, 0); m_readerLayout->setSpacing(24);
    auto *readerControls = new QWidget(readerBody); new QVBoxLayout(readerControls); bodyLayout(readerControls)->setContentsMargins(0, 0, 0, 0); bodyLayout(readerControls)->setSpacing(16);
    bodyLayout(readerControls)->setSpacing(12);row(readerControls, QStringLiteral("字体"), fontChoice("reader")); row(readerControls, QStringLiteral("字号"), choice("reader", "fontSize", sizes)); row(readerControls, QStringLiteral("行距"), choice("reader", "lineHeight", heights));
    QList<QPair<QString, QJsonValue>> widths; for (int n : {400, 560, 640, 760, 880, 960, 1120, 1280}) widths.append({QString::number(n), n}); row(readerControls, QStringLiteral("正文宽度"), choice("reader", "maxWidth", widths));
    group(readerControls, QStringLiteral("内容显示")); row(readerControls, QStringLiteral("代码高亮"), new QLabel(QStringLiteral("跟随界面"), readerControls)); row(readerControls, QStringLiteral("网络图片"), new QLabel(QStringLiteral("暂不加载"), readerControls), QStringLiteral("当前仅加载明确授权的本地图片，不发起网络请求。")); bodyLayout(readerControls)->addStretch();
    m_readerLayout->addWidget(readerControls, 1);auto*readerPanel=new SharedPanel(appearance,readerBody);readerPanel->setObjectName("settingsReaderPanel");auto*readerPanelLayout=new QVBoxLayout(readerPanel);readerPanelLayout->setContentsMargins(16,16,16,16);readerPanelLayout->setSpacing(16);auto*readerPanelTitle=new QLabel(QStringLiteral("预览"),readerPanel);auto previewTitleFont=uiFont(16);previewTitleFont.setWeight(QFont::DemiBold);readerPanelTitle->setFont(previewTitleFont);readerPanelLayout->addWidget(readerPanelTitle);auto *readerPreview = new NativePreviewView(appearance, readerPanel, 0); readerPreview->setObjectName("settingsReaderPreview"); readerPreview->setMinimumSize(260, 400);readerPanelLayout->addWidget(readerPreview,1);m_readerLayout->addWidget(readerPanel, 1); bodyLayout(reader)->addWidget(readerBody);
    const auto source = QStringLiteral("# 写作，从这里开始\n\n把想法写下来，让思考慢慢清晰。\n\n> 少一点干扰，多一点专注。\n\n## 一个安静的写作空间\n\n无论是记录灵感、整理笔记，还是撰写长文，都从一个简单的 Markdown 文件开始。\n\n- [x] 记录一个新想法\n- [ ] 留一点时间，自由写作\n").toUtf8();
    if (auto model = NativePreviewModel::parse(source)) if (auto content = NativePreviewContent::build(std::move(*model))) { NativePreviewStamp stamp{QUuid::createUuid(), 1, 1, 1, 1}; readerPreview->expect(stamp); readerPreview->presentAsync(stamp, std::make_shared<NativePreviewContent>(std::move(*content))); }
    auto *shortcuts = page(titles[4], QStringLiteral("修改当前平台的命令快捷键，成功保存后立即生效。"));auto*shortcutSearch=new SharedLineEdit(appearance,shortcuts);shortcutSearch->setObjectName("settingsShortcutSearch");shortcutSearch->setPlaceholderText(QStringLiteral("快速查找快捷键"));shortcutSearch->setClearButtonEnabled(true);shortcutSearch->addAction(themedIcon("search",appearance->color("textSecondary")),QLineEdit::LeadingPosition);bodyLayout(shortcuts)->addWidget(shortcutSearch);group(shortcuts,QStringLiteral("文件"));
    auto *shortcutHint=new QLabel(QStringLiteral("输入组合名称，例如 Ctrl+Alt+F；留空禁用。应用前检查冲突，支持最多两段组合。"),shortcuts);shortcutHint->setWordWrap(true);bodyLayout(shortcuts)->addWidget(shortcutHint);
    for(const auto &command:commandShortcuts()){
        if(command.id=="undoAction")group(shortcuts,QStringLiteral("编辑"));if(command.id=="focusAction")group(shortcuts,QStringLiteral("视图与导航"));
        auto *control=new QWidget(shortcuts);auto *line=new QHBoxLayout(control);line->setContentsMargins(0,0,0,0);line->setSpacing(8);auto *field=new SharedLineEdit(appearance,control);field->setObjectName("shortcut-"+command.id);field->setAccessibleName(command.title+QStringLiteral("快捷键"));field->setMaxLength(80);field->setMinimumWidth(130);field->setShortcutDisplay(true);field->setPlaceholderText(QStringLiteral("未分配"));connect(field,&QLineEdit::textChanged,field,[field](const QString&value){const auto native=QKeySequence::fromString(value,QKeySequence::PortableText).toString(QKeySequence::NativeText);field->setToolTip(value.isEmpty()?QStringLiteral("未分配"):native.isEmpty()?value:native);});line->addWidget(field,1);
        auto *apply=new SharedButton(QStringLiteral("应用"),"apply-"+command.id,appearance,control);apply->setOutlined(true);line->addWidget(apply);auto *reset=new SharedButton(QStringLiteral("默认"),"reset-"+command.id,appearance,control);reset->setOutlined(true);line->addWidget(reset);
        connect(apply,&QPushButton::clicked,this,[this,field,id=command.id]{m_preferences->setValues("shortcuts",{{id,field->text().trimmed()}});});connect(reset,&QPushButton::clicked,this,[this,field,id=command.id]{if(m_preferences->setValues("shortcuts",{{id,shortcutDefaults().value(id)}}))field->setText(m_preferences->value("shortcuts",id).toString());});
        m_bindings.append([this,field,id=command.id]{if(!field->hasFocus())field->setText(m_preferences->value("shortcuts",id).toString());});row(shortcuts,command.title,control);m_rows.last()->setProperty("shortcutText",command.title+" "+command.standard.toString(QKeySequence::NativeText));
    }
    connect(shortcutSearch,&QLineEdit::textChanged,shortcuts,[shortcuts](const QString&q){for(auto*w:shortcuts->findChildren<QWidget*>())if(w->property("shortcutText").isValid())w->setVisible(w->property("shortcutText").toString().contains(q.trimmed(),Qt::CaseInsensitive));});

    auto *about = page(titles[5], QStringLiteral("面向本地 Markdown 文档的阅读与编辑。"));auto*hero=new QWidget(about);auto*heroLayout=new QVBoxLayout(hero);heroLayout->setContentsMargins(24,24,24,24);heroLayout->setSpacing(12);auto*mark=new QLabel(hero);mark->setFixedSize(80,80);auto updateMark=[mark]{QSvgRenderer svg(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/app-icon.svg"));QPixmap image(QSize(80,80)*mark->devicePixelRatioF());image.setDevicePixelRatio(mark->devicePixelRatioF());image.fill(Qt::transparent);QPainter p(&image);svg.render(&p);mark->setPixmap(image);};connect(appearance,&Appearance::changed,mark,updateMark);updateMark();heroLayout->addWidget(mark,0,Qt::AlignHCenter);auto*brand=new QLabel("xdnote",hero);auto brandFont=uiFont(32);brandFont.setWeight(QFont::DemiBold);brand->setFont(brandFont);brand->setAlignment(Qt::AlignCenter);heroLayout->addWidget(brand);auto*tagline=new QLabel(QStringLiteral("本地文档，专注阅读与写作。"),hero);tagline->setAlignment(Qt::AlignCenter);tagline->setProperty("secondary",true);heroLayout->addWidget(tagline);bodyLayout(about)->addWidget(hero);group(about,QStringLiteral("版本与许可"));row(about, QStringLiteral("版本"), new QLabel(QStringLiteral("开发构建"), about)); row(about, QStringLiteral("Qt"), new QLabel(QString::fromLatin1(qVersion()), about)); row(about, QStringLiteral("自有代码"), new QLabel(QStringLiteral("Apache-2.0"), about)); row(about, QStringLiteral("文档与原创设计"), new QLabel(QStringLiteral("CC BY 4.0"), about));auto*licenses=new SharedButton(QStringLiteral("查看第三方许可"),"settingsThirdPartyLicenses",appearance,about);licenses->setSymbol("document");licenses->setOutlined(true);row(about,QStringLiteral("第三方组件"),licenses);connect(licenses,&QPushButton::clicked,this,[this]{showLicenses(m_appearance,this);});
    auto *repository = new SharedButton(QStringLiteral("查看源码仓库"), "settingsRepository", appearance, about); repository->setOutlined(true); connect(repository, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl("https://github.com/xiaodaozhi/xdnote")); }); row(about, QStringLiteral("项目来源"), repository);
    for (auto *body : m_bodies) bodyLayout(body)->addStretch();
    root->addWidget(new SharedSeparator(Qt::Horizontal, appearance, this)); auto *footer = new QWidget(this); footer->setObjectName("settingsFooter"); footer->setFixedHeight(size["footerHeight"].toInt()); auto *foot = new QHBoxLayout(footer); foot->setContentsMargins(24, 0, 24, 0); foot->setSpacing(8);m_statusIcon=new QLabel(footer);m_statusIcon->setFixedSize(16,16);m_statusIcon->setObjectName("settingsStatusIcon");foot->addWidget(m_statusIcon);m_status = new ElidedLabel(footer); m_status->setObjectName("settingsStatus"); m_status->setFont(uiFont(12)); foot->addWidget(m_status, 1); auto *done = new SharedButton(QStringLiteral("完成"), "settingsDone", appearance, footer); done->setProperty("primary", true); done->setFixedWidth(96); foot->addWidget(done); root->addWidget(footer); connect(done, &QPushButton::clicked, this, &QWidget::close);
    connect(m_preferences, &Preferences::changed, this, [this] { refresh(); }); connect(m_preferences, &Preferences::writeFailed, this, [this] { m_hiddenError.clear();refresh(); }); connect(appearance, &Appearance::changed, this, &SettingsWindow::applyAppearance); connect(m_search, &QLineEdit::textChanged, this, &SettingsWindow::filter);
    m_geometry = new WindowGeometry(this, store, "settings",QSize(size["compactMinWidth"].toInt(),size["compactMinHeight"].toInt())); connect(m_geometry, &WindowGeometry::geometryWarning, this, [this](const QString &text) { if (!text.isEmpty()) { m_status->setFullText(text); m_status->setToolTip(text); } });
    selectPage(0); applyAppearance(); refresh(); reflow();
}
SettingsWindow::~SettingsWindow() { if (m_geometry) { m_geometry->flush(); delete m_geometry; m_geometry = nullptr; } }
QWidget *SettingsWindow::page(const QString &title, const QString &description) {
    auto *scroll = new QScrollArea(m_pages); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true); scroll->setObjectName("settingsScroll");
    auto *body = new QWidget(scroll); body->setProperty("pageTitle", title); body->setProperty("pageIndex", m_pages->count()); auto *layout = new QVBoxLayout(body); const int padding = visualTokens()["windows"].toObject()["settings"].toObject()["contentPadding"].toInt(); layout->setContentsMargins(padding, 24, padding, 24); layout->setSpacing(12);
    auto *heading = new OpticalHeading(title, body); auto headingFont = uiFont(24); headingFont.setWeight(QFont::DemiBold); heading->setFont(headingFont); layout->addWidget(heading); auto *details = new QLabel(description, body); details->setProperty("secondary", true); details->setWordWrap(true); layout->addWidget(details); scroll->setWidget(body); m_pages->addWidget(scroll); m_bodies.append(body); return body;
}
void SettingsWindow::group(QWidget *page, const QString &title) {
    auto *layout = bodyLayout(page); layout->addSpacing(setting("groupGap")); layout->addWidget(new SharedSeparator(Qt::Horizontal, m_appearance, page)); auto *label = new QLabel(title, page);label->setProperty("settingsGroupTitle",true);auto font = uiFont(16); font.setWeight(QFont::DemiBold); label->setFont(font); layout->addWidget(label);
}
void SettingsWindow::row(QWidget *page, const QString &title, QWidget *control, const QString &description) {
    auto *row = new QWidget(page); row->setProperty("settingsFormRow",true);row->setProperty("settingText", title + " " + description); auto *owner = page; while (owner && !owner->property("pageIndex").isValid()) owner = owner->parentWidget(); row->setProperty("pageIndex", owner ? owner->property("pageIndex") : QVariant());
    auto *layout = new QVBoxLayout(row); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(8); auto *line = new QHBoxLayout; line->setContentsMargins(0, 0, 0, 0); line->setSpacing(16); auto *label = new QLabel(title, row); label->setWordWrap(true); line->addWidget(label, 1); line->addWidget(control); layout->addLayout(line); control->setAccessibleName(title);
    if (!description.isEmpty()) { auto *text = new QLabel(description, row); text->setProperty("secondary", true); text->setFont(uiFont(12)); text->setWordWrap(true); layout->addWidget(text); } bodyLayout(page)->addWidget(row); m_rows.append(row);
}
SharedButton *SettingsWindow::choice(const QString &section, const QString &key, const QList<QPair<QString, QJsonValue>> &options) {
    auto *button = new SharedButton({}, "settings-" + section + "-" + key, m_appearance, this);button->setProperty("fieldDropdown",true);button->setProperty("showMenuIndicator",true);button->setMinimumWidth(160); button->setMaximumWidth(240); button->setOutlined(true); auto *menu = new SharedMenu(m_appearance, button); menu->setObjectName(button->objectName() + "-menu"); button->setMenu(menu);connect(menu,&QMenu::aboutToShow,button,[button,menu]{menu->setMinimumWidth(button->width()+36);});
    for (const auto &option : options) { auto *action = menu->addAction(option.first); action->setCheckable(true); action->setData(option.second.toVariant()); connect(action, &QAction::triggered, this, [this, section, key, value = option.second] { m_preferences->setValues(section, {{key, value}}); refresh(); }); }
    m_bindings.append([this, button, menu, section, key] { const auto value = m_preferences->value(section, key); QString label = value.toVariant().toString(); for (auto *action : menu->actions()) { const bool match = QJsonValue::fromVariant(action->data()) == value; action->setChecked(match); if (match) label = action->text(); } button->setText(label); button->setToolTip(label); }); return button;
}
SharedButton *SettingsWindow::fontChoice(const QString &section) {
    QList<QPair<QString, QJsonValue>> options{{section == "editor" ? QStringLiteral("系统等宽字体") : QStringLiteral("系统字体"), ""}};
    for (const auto &family : QFontDatabase::families()) options.append({family, family}); return choice(section, "fontFamily", options);
}
QWidget *SettingsWindow::toggle(const QString &section, const QString &key) {
    auto *button = new Toggle(m_appearance, this); button->setObjectName("settings-" + section + "-" + key); connect(button, &QCheckBox::clicked, this, [this, section, key](bool checked) { m_preferences->setValues(section, {{key, checked}}); refresh(); }); m_bindings.append([this, button, section, key] { button->setChecked(m_preferences->value(section, key).toBool()); }); return button;
}
void SettingsWindow::selectPage(int index) {
    if (index < 0 || index >= m_pages->count()) return;if(!m_search->text().isEmpty()){QSignalBlocker blocker(m_search);m_search->clear();m_search->setProperty("searching",false);}m_content->setCurrentWidget(m_pages); m_pages->setCurrentIndex(index);
    for (int i = 0; i < m_navigation.size(); ++i) { m_navigation[i]->setProperty("navigationSelected", i == index); m_navigation[i]->update(); } reflow();
}
void SettingsWindow::filter(const QString &query) {
    const auto needle = query.trimmed(); if (needle.isEmpty()) { m_search->setProperty("searching", false);selectPage(m_beforeSearch);refresh();return; }
    if (!m_search->property("searching").toBool()) m_beforeSearch = m_pages->currentIndex(); m_search->setProperty("searching", true);
    auto*l=qobject_cast<QVBoxLayout*>(m_searchResults->layout());if(!l){l=new QVBoxLayout(m_searchResults);l->setContentsMargins(0,0,0,0);}while(auto*item=l->takeAt(0)){if(item->widget()){item->widget()->hide();item->widget()->setParent(nullptr);item->widget()->deleteLater();}delete item;}
    auto*scroll=new QScrollArea(m_searchResults);scroll->setObjectName("settingsResultsScroll");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);auto*body=new QWidget(scroll);auto*results=new QVBoxLayout(body);results->setContentsMargins(40,24,40,24);results->setSpacing(12);auto*heading=new OpticalHeading(QStringLiteral("搜索结果"),body);auto headingFont=uiFont(24);headingFont.setWeight(QFont::DemiBold);heading->setFont(headingFont);results->addWidget(heading);auto*count=new QLabel(body);count->setProperty("secondary",true);results->addWidget(count);int matches=0;
    for(auto*row:m_rows){const int index=row->property("pageIndex").toInt();if(index<0||index>=m_bodies.size())continue;const auto text=row->property("settingText").toString();const auto category=m_bodies[index]->property("pageTitle").toString();if(!text.contains(needle,Qt::CaseInsensitive)&&!category.contains(needle,Qt::CaseInsensitive))continue;++matches;auto*card=new SharedPanel(m_appearance,body);auto*cardLayout=new QVBoxLayout(card);cardLayout->setContentsMargins(16,12,16,12);cardLayout->setSpacing(8);auto*path=new QLabel(category+QStringLiteral(" › 设置"),card);path->setProperty("secondary",true);path->setFont(uiFont(12));cardLayout->addWidget(path);auto highlighted=text.toHtmlEscaped();highlighted.replace(needle.toHtmlEscaped(),"<b>"+needle.toHtmlEscaped()+"</b>",Qt::CaseInsensitive);auto*label=new QLabel(highlighted,card);label->setWordWrap(true);label->setTextFormat(Qt::RichText);cardLayout->addWidget(label);auto*go=new SharedButton(QStringLiteral("前往设置"),"settingsSearchResult",m_appearance,card);go->setSymbol("chevron-right");cardLayout->addWidget(go,0,Qt::AlignRight);connect(go,&QPushButton::clicked,this,[this,index,row]{m_search->clear();selectPage(index);if(auto*s=qobject_cast<QScrollArea*>(m_pages->widget(index)))s->ensureWidgetVisible(row,0,32);});results->addWidget(card);}
    count->setText(QStringLiteral("找到 %1 个设置项").arg(matches));if(!matches){auto*empty=new QLabel(QStringLiteral("没有找到相关设置"),body);empty->setAlignment(Qt::AlignCenter);auto f=uiFont(18);f.setWeight(QFont::DemiBold);empty->setFont(f);results->addSpacing(48);results->addWidget(empty);auto*help=new QLabel(QStringLiteral("试试其他关键词，或清除搜索返回全部设置。"),body);help->setAlignment(Qt::AlignCenter);help->setProperty("secondary",true);help->setWordWrap(true);results->addWidget(help);auto*clear=new SharedButton(QStringLiteral("清除搜索"),"settingsClearSearch",m_appearance,body);clear->setOutlined(true);results->addWidget(clear,0,Qt::AlignHCenter);connect(clear,&QPushButton::clicked,m_search,&QLineEdit::clear);}results->addStretch();scroll->setWidget(body);l->addWidget(scroll);m_content->setCurrentWidget(m_searchResults);
}
void SettingsWindow::refresh() {
    if (m_refreshing) return; QScopedValueRollback<bool> guard(m_refreshing, true); for (const auto &binding : m_bindings) binding();
    const auto error = m_preferences->error(); m_status->setFullText(error.isEmpty() ? QStringLiteral("设置更改即时生效") : error); m_status->setToolTip(m_status->fullText()); m_status->setStyleSheet(QStringLiteral("color:%1").arg((error.isEmpty() ? m_appearance->color("textSecondary") : m_appearance->semantic("error")).name()));
    m_statusIcon->setPixmap(themedIcon(error.isEmpty()?"check-circle":"error-message",error.isEmpty()?m_appearance->semantic("success"):m_appearance->semantic("error")).pixmap(QSize(16,16),devicePixelRatioF()));m_feedback->setVisible(!error.isEmpty()&&error!=m_hiddenError);m_feedbackText->setText(error.section(QChar(0xff1a),0,0));m_feedbackText->setToolTip(error);m_retrySettings->setVisible(m_preferences->canRetryWrite());if(auto*icon=m_feedback->findChild<QLabel*>("settingsErrorIcon"))icon->setPixmap(themedIcon("error-message",m_appearance->semantic("error")).pixmap(QSize(16,16),devicePixelRatioF()));
}
void SettingsWindow::applyAppearance() {
    setStyleSheet(QStringLiteral("QWidget#settingsWindow,QScrollArea#settingsScroll{background:%1;color:%2} QWidget#settingsSidebar{background:%3} QLabel{color:%2;background:transparent} QLabel[secondary=true]{color:%4} QLineEdit{background:%5;color:%2;border:1px solid %6;border-radius:6px;padding:4px 8px;selection-background-color:%7} QScrollArea>QWidget>QWidget{background:%1}")
        .arg(m_appearance->color("background").name(), m_appearance->color("text").name(), m_appearance->color("sidebar").name(), m_appearance->color("textSecondary").name(), m_appearance->color("surface").name(), m_appearance->color("border").name(), m_appearance->accentColor("soft").name()) + themedScrollBars(m_appearance));
    m_feedback->setStyleSheet(QStringLiteral("QWidget#settingsFeedback{background:%1;border-bottom:1px solid %2}").arg(m_appearance->color("sidebar").name(),m_appearance->semantic("error").name()));applyNativeAppearance(); refresh();
}
void SettingsWindow::reflow() {
    if (!m_swatches || m_bodies.size() < 4) return;
    const auto window = visualTokens()["windows"].toObject()["settings"].toObject();
    const bool compact=width()<window["compactBelowWidth"].toInt();
    const int padding=window[compact?"compactContentPadding":"contentPadding"].toInt();
    m_sidebarScroll->setFixedWidth(window[compact?"compactSidebarWidth":"sidebarWidth"].toInt());
    for(auto*body:m_bodies)bodyLayout(body)->setContentsMargins(padding,24,padding,24);
    const int available=qMax(0,m_pages->width()-2*padding-16);
    const int fitting=qMax(1,(available+setting("cardGap"))/(setting("cardMinWidth")+setting("cardGap")));
    const int columns=fitting>=6?6:qMin(window["swatchColumnsCompact"].toInt(),fitting);
    for (int i = 0; i < m_swatchCards.size(); ++i) m_swatches->addWidget(m_swatchCards[i], i / columns, i % columns);
    const int themeColumns=qMin(3,fitting);for(int i=0;i<m_themeCards.size();++i)m_themes->addWidget(m_themeCards[i],i/themeColumns,i%themeColumns);
    for(auto*row:m_rows)if(row->property("settingsFormRow").toBool())if(auto*line=qobject_cast<QBoxLayout*>(row->layout()->itemAt(0)->layout()))line->setDirection(available<window["stackRowsBelowWidth"].toInt()?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);
    if(auto*preview=findChild<NativePreviewView*>("settingsReaderPreview"))preview->setMinimumWidth(qMin(260,qMax(0,available-32)));
    if (m_readerLayout) m_readerLayout->setDirection(available < 680 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}
void SettingsWindow::resizeEvent(QResizeEvent *event) { ApplicationWindow::resizeEvent(event); reflow(); }
void SettingsWindow::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::Close)) { InputModality::forApplication()->clear(); close(); event->accept(); }
    else if (event->key() == Qt::Key_Escape) { InputModality::forApplication()->clear(); if (!m_search->text().isEmpty()) m_search->clear(); else close(); event->accept(); }
    else ApplicationWindow::keyPressEvent(event);
}
bool SettingsWindow::eventFilter(QObject *object, QEvent *event) {
    if (object->objectName() == "settingsEditorPreview" && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) return focusNextPrevChild(key->key() != Qt::Key_Backtab && !(key->modifiers() & Qt::ShiftModifier));
    }
    return ApplicationWindow::eventFilter(object, event);
}
}
