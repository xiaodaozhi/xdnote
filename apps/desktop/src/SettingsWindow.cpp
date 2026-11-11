#include "SettingsWindow.h"
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
#include <algorithm>

namespace xdnote {
namespace {
int setting(const char *key) { return visualTokens()["settings"].toObject()[key].toInt(); }
QVBoxLayout *bodyLayout(QWidget *body) { return qobject_cast<QVBoxLayout *>(body->layout()); }
class Toggle final : public QCheckBox {
public:
    Toggle(Appearance *appearance, QWidget *parent) : QCheckBox(parent), appearance(appearance) {
        setFixedSize(setting("toggleWidth"), visualTokens()["controls"].toObject()["height"].toInt());
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
        const QRectF track(1, (height() - h) / 2.0, width() - 2, h);
        painter.setPen(Qt::NoPen); painter.setBrush(isEnabled() && isChecked() ? appearance->accentColor(isDown() ? "pressed" : underMouse() ? "hover" : "solid") : appearance->color(isEnabled() && underMouse() ? "controlHover" : "border"));
        painter.drawRoundedRect(track, h / 2.0, h / 2.0);
        painter.setBrush(isEnabled() ? Qt::white : appearance->color("textSecondary"));
        painter.drawEllipse(QRectF(isChecked() ? width() - knob - 3 : 3, (height() - knob) / 2.0, knob, knob));
        if (hasFocus() && isEnabled() && InputModality::forApplication()->keyboardNavigation()) {
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(appearance->accentColor(), 2)); painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 5, 5);
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
            painter.setPen(QPen(appearance->color("border"), 1)); painter.setBrush(value == "dark" ? QColor("#22272F") : QColor("#F4F5F7")); painter.drawRoundedRect(preview, 3, 3);
            if (value == "system") painter.fillRect(QRectF(preview.center().x(), preview.y() + 1, preview.width() / 2 - 1, preview.height() - 2), QColor("#22272F"));
            painter.fillRect(QRectF(preview.x() + 1, preview.y() + 11, 24, preview.height() - 12), value == "dark" ? QColor("#343E4C") : QColor("#E5E9F0"));
            painter.setPen(QPen(QColor("#AAB4C0"), 2));
            painter.drawLine(QPointF(preview.x() + 1, preview.y() + 10), QPointF(preview.right() - 1, preview.y() + 10));
            for (int y : {24, 36, 48}) painter.drawLine(QPointF(preview.x() + 34, preview.y() + y), QPointF(preview.right() - (y == 36 ? 26 : 12), preview.y() + y));
        } else {
            painter.setPen(Qt::NoPen); painter.setBrush(QColor(visualTokens()["accents"].toObject()[value].toObject()["base"].toString()));
            painter.drawEllipse(QPointF(width() / 2.0, 34), 20, 20);
        }
        painter.setPen(appearance->color("text")); painter.setFont(uiFont()); painter.drawText(QRect(4, theme ? 88 : 70, width() - 8, 24), Qt::AlignCenter, text());
        if (!theme) { painter.setFont(uiFont(12)); painter.setPen(appearance->color("textSecondary")); painter.drawText(QRect(4, 95, width() - 8, 20), Qt::AlignCenter, visualTokens()["accents"].toObject()[value].toObject()["base"].toString()); }
        if (isChecked()) { const QRect mark(width() - 24, 5, 18, 18); painter.setBrush(appearance->accentColor()); painter.setPen(Qt::NoPen); painter.drawEllipse(mark); themedIcon("check", appearance->accentColor("foreground")).paint(&painter, mark.adjusted(3, 3, -3, -3)); }
        if (hasFocus() && InputModality::forApplication()->keyboardNavigation()) { painter.setBrush(Qt::NoBrush); painter.setPen(QPen(appearance->accentColor(), 2)); painter.drawRoundedRect(QRectF(rect()).adjusted(3, 3, -3, -3), 4, 4); }
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
    auto *middle = new QWidget(this); auto *columns = new QHBoxLayout(middle); columns->setContentsMargins(0, 0, 0, 0); columns->setSpacing(0);
    auto *sidebar = new QWidget(middle); sidebar->setObjectName("settingsSidebar"); sidebar->setFixedWidth(size["sidebarWidth"].toInt());
    auto *nav = new QVBoxLayout(sidebar); nav->setContentsMargins(16, 16, 16, 16); nav->setSpacing(8);
    m_search = new QLineEdit(sidebar); m_search->setAttribute(Qt::WA_MacShowFocusRect, false); m_search->setObjectName("settingsSearch"); m_search->setPlaceholderText(QStringLiteral("搜索设置")); m_search->setAccessibleName(QStringLiteral("搜索设置")); m_search->setFixedHeight(32); nav->addWidget(m_search); nav->addSpacing(16);
    const QStringList titles{QStringLiteral("通用"), QStringLiteral("外观与配色"), QStringLiteral("编辑器"), QStringLiteral("阅读与渲染"), QStringLiteral("快捷键"), QStringLiteral("关于")};
    const QStringList icons{"settings", "palette", "edit", "read", "keyboard", "info"};
    for (int i = 0; i < titles.size(); ++i) { auto *button = new SharedButton(titles[i], "settingsPage-" + QString::number(i), appearance, sidebar); button->setSymbol(icons[i]); button->setFixedHeight(setting("navigationHeight")); button->setProperty("navigationRow", true); nav->addWidget(button); m_navigation.append(button); connect(button, &QPushButton::clicked, this, [this, i] { selectPage(i); }); }
    nav->addStretch(); columns->addWidget(sidebar); columns->addWidget(new SharedSeparator(Qt::Vertical, appearance, middle));
    m_pages = new QStackedWidget(middle); m_pages->setObjectName("settingsPages"); columns->addWidget(m_pages, 1); root->addWidget(middle, 1);
    auto *general = page(titles[0], QStringLiteral("基本使用行为、打开偏好与保存保护。"));
    group(general, QStringLiteral("打开与窗口"));
    row(general, QStringLiteral("双击文件时打开为"), choice("desktop", "externalOpenTarget", {{QStringLiteral("独立窗口"), "standalone"}, {QStringLiteral("工作区标签"), "workspace-tab"}}));
    auto *modes = new QWidget(general); auto *modeLayout = new QHBoxLayout(modes); modeLayout->setContentsMargins(0, 0, 0, 0); modeLayout->setSpacing(0); auto *modeGroup = new QButtonGroup(modes);
    for (const auto &option : QList<QPair<QString, QString>>{{QStringLiteral("阅读"), "reading"}, {QStringLiteral("编辑"), "source"}}) {
        auto *button = new SharedButton(option.first, "settings-default-" + option.second, appearance, modes); button->setCheckable(true); button->setFixedWidth(visualTokens()["controls"].toObject()["modeSegmentWidth"].toInt()); modeGroup->addButton(button); modeLayout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, value = option.second] { m_preferences->setValues("desktop", {{"standaloneDefaultMode", value}}); refresh(); }); m_bindings.append([this, button, value = option.second] { button->setChecked(m_preferences->value("desktop", "standaloneDefaultMode").toString() == value); });
    }
    row(general, QStringLiteral("独立窗口默认模式"), modes, QStringLiteral("只影响后续打开的新文档，不切换当前视图。"));
    row(general, QStringLiteral("重复文件"), new QLabel(QStringLiteral("聚焦已有文档"), general));
    group(general, QStringLiteral("文件关联")); auto *association = new SharedButton(QStringLiteral("设置默认应用"), "settingsAssociation", appearance, general); association->setOutlined(true); association->setEnabled(false);
    row(general, QStringLiteral("Markdown 文件关联"), association, QStringLiteral("当前开发构建暂不提供文件关联设置。"));
    group(general, QStringLiteral("自动保存")); row(general, QStringLiteral("自动保存"), toggle("editor", "autoSaveEnabled"), QStringLiteral("默认关闭；冲突、只读或保存失败时暂停，未保存内容仍受关闭提醒保护。"));
    auto *delay = choice("editor", "autoSaveDelayMs", {{QStringLiteral("0.5 秒"), 500}, {QStringLiteral("1 秒"), 1000}, {QStringLiteral("1.5 秒"), 1500}, {QStringLiteral("3 秒"), 3000}, {QStringLiteral("5 秒"), 5000}, {QStringLiteral("10 秒"), 10000}});
    row(general, QStringLiteral("停止输入后保存"), delay); m_bindings.append([this, delay] { delay->setEnabled(m_preferences->value("editor", "autoSaveEnabled").toBool()); });
    group(general, QStringLiteral("关闭保护")); row(general, QStringLiteral("始终提醒未保存的修改"), new QLabel(QStringLiteral("已启用"), general), QStringLiteral("关闭或退出前，需要先保存或明确放弃修改。"));
    auto *appearancePage = page(titles[1], QStringLiteral("主题与强调色分别设置，并同步所有窗口。"));
    group(appearancePage, QStringLiteral("界面主题")); auto *themeRow = new QWidget(appearancePage); auto *themes = new QHBoxLayout(themeRow); themes->setContentsMargins(0, 0, 0, 0); themes->setSpacing(setting("cardGap"));
    for (const auto &name : {QString("light"), QString("dark"), QString("system")}) { auto *card = new Card(appearance, name, true, themeRow); themes->addWidget(card); connect(card, &QAbstractButton::clicked, this, [this, name] { m_appearance->setPreferences(name, m_appearance->accent()); refresh(); }); m_bindings.append([this, card, name] { card->setChecked(m_appearance->theme() == name); }); } themeRow->setProperty("settingText", QStringLiteral("界面主题 浅色 深色 跟随系统")); themeRow->setProperty("pageIndex", 1); m_rows.append(themeRow); bodyLayout(appearancePage)->addWidget(themeRow);
    group(appearancePage, QStringLiteral("强调色")); auto *swatches = new QWidget(appearancePage); m_swatches = new QGridLayout(swatches); m_swatches->setContentsMargins(0, 0, 0, 0); m_swatches->setSpacing(setting("cardGap"));
    for (const auto &name : {QString("graphite"), QString("mist-blue"), QString("cobalt"), QString("mauve"), QString("terracotta"), QString("olive")}) { auto *card = new Card(appearance, name, false, swatches); m_swatchCards.append(card); connect(card, &QAbstractButton::clicked, this, [this, name] { m_appearance->setPreferences(m_appearance->theme(), name); refresh(); }); m_bindings.append([this, card, name] { card->setChecked(m_appearance->accent() == name); }); } swatches->setProperty("settingText", QStringLiteral("强调色 石墨灰 雾蓝 钴蓝 灰紫 陶土 橄榄")); swatches->setProperty("pageIndex", 1); m_rows.append(swatches); bodyLayout(appearancePage)->addWidget(swatches);
    group(appearancePage, QStringLiteral("效果预览")); auto *preview = new QWidget(appearancePage); auto *sample = new QHBoxLayout(preview); sample->setContentsMargins(16, 16, 16, 16); auto *read = new SharedButton(QStringLiteral("阅读"), "settingsExampleRead", appearance, preview); read->setCheckable(true); read->setChecked(true); auto *edit = new SharedButton(QStringLiteral("编辑"), "settingsExampleEdit", appearance, preview); edit->setCheckable(true); auto *save = new SharedButton(QStringLiteral("保存"), "settingsExampleSave", appearance, preview); save->setProperty("primary", true); sample->addWidget(read); sample->addWidget(edit); sample->addStretch(); sample->addWidget(save); bodyLayout(appearancePage)->addWidget(preview);
    connect(read, &QPushButton::clicked, this, [read, edit] { read->setChecked(true); edit->setChecked(false); }); connect(edit, &QPushButton::clicked, this, [read, edit] { read->setChecked(false); edit->setChecked(true); }); save->setToolTip(QStringLiteral("此处为按钮外观示例，不操作文档。"));
    auto *editor = page(titles[2], QStringLiteral("设置编辑器字体、排版与缩进行为。")); group(editor, QStringLiteral("字体与排版")); row(editor, QStringLiteral("编辑器字体"), fontChoice("editor"));
    QList<QPair<QString, QJsonValue>> sizes; for (int n : {10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40}) sizes.append({QString::number(n), n});
    QList<QPair<QString, QJsonValue>> heights; for (double n : {1.0, 1.2, 1.4, 1.5, 1.6, 1.8, 2.0, 2.5, 3.0}) heights.append({QString::number(n), n});
    row(editor, QStringLiteral("字号"), choice("editor", "fontSize", sizes)); row(editor, QStringLiteral("行距"), choice("editor", "lineHeight", heights)); group(editor, QStringLiteral("编辑行为"));
    row(editor, QStringLiteral("软换行"), toggle("editor", "wordWrap")); row(editor, QStringLiteral("显示行号"), toggle("editor", "lineNumbers"));
    row(editor, QStringLiteral("缩进宽度"), choice("editor", "indentWidth", {{"1", 1}, {"2", 2}, {"4", 4}, {"8", 8}})); row(editor, QStringLiteral("使用空格缩进"), toggle("editor", "useSpaces"));
    group(editor, QStringLiteral("代码示例预览")); auto *session = new DocumentSession(QStringLiteral("# 写作，从这里开始\n\n把想法写下来，让思考慢慢清晰。\n\n- [x] 记录一个新想法\n- [ ] 整理阅读笔记\n").toUtf8(), this);
    auto *editorPreview = new SourceEditor(session, editor, appearance); editorPreview->setObjectName("settingsEditorPreview"); editorPreview->setMinimumHeight(180); editorPreview->setFocusPolicy(Qt::ClickFocus); editorPreview->installEventFilter(this); session->setReadOnly(true); bodyLayout(editor)->addWidget(editorPreview);
    auto *reader = page(titles[3], QStringLiteral("阅读排版即时生效，不修改 Markdown 内容。")); auto *readerBody = new QWidget(reader); m_readerLayout = new QBoxLayout(QBoxLayout::LeftToRight, readerBody); m_readerLayout->setContentsMargins(0, 0, 0, 0); m_readerLayout->setSpacing(24);
    auto *readerControls = new QWidget(readerBody); new QVBoxLayout(readerControls); bodyLayout(readerControls)->setContentsMargins(0, 0, 0, 0); bodyLayout(readerControls)->setSpacing(16);
    group(readerControls, QStringLiteral("文字与排版")); row(readerControls, QStringLiteral("字体"), fontChoice("reader")); row(readerControls, QStringLiteral("字号"), choice("reader", "fontSize", sizes)); row(readerControls, QStringLiteral("行距"), choice("reader", "lineHeight", heights));
    QList<QPair<QString, QJsonValue>> widths; for (int n : {400, 560, 640, 760, 880, 960, 1120, 1280}) widths.append({QString::number(n), n}); row(readerControls, QStringLiteral("正文宽度"), choice("reader", "maxWidth", widths));
    group(readerControls, QStringLiteral("内容显示")); row(readerControls, QStringLiteral("代码高亮"), new QLabel(QStringLiteral("跟随界面"), readerControls)); row(readerControls, QStringLiteral("网络图片"), new QLabel(QStringLiteral("暂不加载"), readerControls), QStringLiteral("当前仅加载明确授权的本地图片，不发起网络请求。")); bodyLayout(readerControls)->addStretch();
    m_readerLayout->addWidget(readerControls, 1); auto *readerPreview = new NativePreviewView(appearance, readerBody, 16); readerPreview->setObjectName("settingsReaderPreview"); readerPreview->setMinimumSize(260, 400); m_readerLayout->addWidget(readerPreview, 1); bodyLayout(reader)->addWidget(readerBody);
    const auto source = QStringLiteral("# 写作，从这里开始\n\n把想法写下来，让思考慢慢清晰。\n\n> 少一点干扰，多一点专注。\n\n## 一个安静的写作空间\n\n无论是记录灵感、整理笔记，还是撰写长文，都从一个简单的 Markdown 文件开始。\n\n- [x] 记录一个新想法\n- [ ] 留一点时间，自由写作\n").toUtf8();
    if (auto model = NativePreviewModel::parse(source)) if (auto content = NativePreviewContent::build(std::move(*model))) { NativePreviewStamp stamp{QUuid::createUuid(), 1, 1, 1, 1}; readerPreview->expect(stamp); readerPreview->presentAsync(stamp, std::make_shared<NativePreviewContent>(std::move(*content))); }
    auto *shortcuts = page(titles[4], QStringLiteral("当前平台的常用快捷键；自定义映射尚未提供。")); group(shortcuts, QStringLiteral("文件与编辑"));
    for (const auto &entry : QList<QPair<QString, QKeySequence::StandardKey>>{{QStringLiteral("打开文档"), QKeySequence::Open}, {QStringLiteral("保存"), QKeySequence::Save}, {QStringLiteral("另存为"), QKeySequence::SaveAs}, {QStringLiteral("关闭文档"), QKeySequence::Close}, {QStringLiteral("撤销"), QKeySequence::Undo}, {QStringLiteral("重做"), QKeySequence::Redo}, {QStringLiteral("退出"), QKeySequence::Quit}}) row(shortcuts, entry.first, new QLabel(QKeySequence(entry.second).toString(QKeySequence::NativeText), shortcuts));
    row(shortcuts, QStringLiteral("搜索工作区文件名"), new QLabel(QKeySequence(Qt::CTRL | Qt::Key_K).toString(QKeySequence::NativeText), shortcuts)); row(shortcuts, QStringLiteral("收藏当前文档"), new QLabel(QKeySequence(Qt::CTRL | Qt::Key_D).toString(QKeySequence::NativeText), shortcuts)); row(shortcuts, QStringLiteral("设置"), new QLabel(QKeySequence(Qt::CTRL | Qt::Key_Comma).toString(QKeySequence::NativeText), shortcuts));
    auto *about = page(titles[5], QStringLiteral("面向本地 Markdown 文档的阅读与编辑。")); group(about, QStringLiteral("xdnote")); row(about, QStringLiteral("版本"), new QLabel(QStringLiteral("开发构建"), about)); row(about, QStringLiteral("Qt"), new QLabel(QString::fromLatin1(qVersion()), about)); row(about, QStringLiteral("自有代码"), new QLabel(QStringLiteral("Apache-2.0"), about)); row(about, QStringLiteral("文档与原创设计"), new QLabel(QStringLiteral("CC BY 4.0"), about));
    auto *repository = new SharedButton(QStringLiteral("查看源码仓库"), "settingsRepository", appearance, about); repository->setOutlined(true); connect(repository, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl("https://github.com/xiaodaozhi/xdnote")); }); row(about, QStringLiteral("项目来源"), repository);
    for (auto *body : m_bodies) bodyLayout(body)->addStretch();
    root->addWidget(new SharedSeparator(Qt::Horizontal, appearance, this)); auto *footer = new QWidget(this); footer->setObjectName("settingsFooter"); footer->setFixedHeight(size["footerHeight"].toInt()); auto *foot = new QHBoxLayout(footer); foot->setContentsMargins(24, 0, 24, 0); foot->setSpacing(16); m_status = new ElidedLabel(footer); m_status->setObjectName("settingsStatus"); m_status->setFont(uiFont(12)); foot->addWidget(m_status, 1); auto *done = new SharedButton(QStringLiteral("完成"), "settingsDone", appearance, footer); done->setProperty("primary", true); done->setFixedWidth(96); foot->addWidget(done); root->addWidget(footer); connect(done, &QPushButton::clicked, this, &QWidget::close);
    connect(m_preferences, &Preferences::changed, this, [this] { refresh(); }); connect(m_preferences, &Preferences::writeFailed, this, [this] { refresh(); }); connect(appearance, &Appearance::changed, this, &SettingsWindow::applyAppearance); connect(m_search, &QLineEdit::textChanged, this, &SettingsWindow::filter);
    m_geometry = new WindowGeometry(this, store, "settings"); connect(m_geometry, &WindowGeometry::geometryWarning, this, [this](const QString &text) { if (!text.isEmpty()) { m_status->setFullText(text); m_status->setToolTip(text); } });
    selectPage(0); applyAppearance(); refresh(); reflow();
}
SettingsWindow::~SettingsWindow() { if (m_geometry) { m_geometry->flush(); delete m_geometry; m_geometry = nullptr; } }
QWidget *SettingsWindow::page(const QString &title, const QString &description) {
    auto *scroll = new QScrollArea(m_pages); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true); scroll->setObjectName("settingsScroll");
    auto *body = new QWidget(scroll); body->setProperty("pageTitle", title); body->setProperty("pageIndex", m_pages->count()); auto *layout = new QVBoxLayout(body); const int padding = visualTokens()["windows"].toObject()["settings"].toObject()["contentPadding"].toInt(); layout->setContentsMargins(padding, 24, padding, 24); layout->setSpacing(12);
    auto *heading = new QLabel(title, body); auto headingFont = uiFont(24); headingFont.setWeight(QFont::DemiBold); heading->setFont(headingFont); layout->addWidget(heading); auto *details = new QLabel(description, body); details->setProperty("secondary", true); details->setWordWrap(true); layout->addWidget(details); scroll->setWidget(body); m_pages->addWidget(scroll); m_bodies.append(body); return body;
}
void SettingsWindow::group(QWidget *page, const QString &title) {
    auto *layout = bodyLayout(page); layout->addSpacing(setting("groupGap")); layout->addWidget(new SharedSeparator(Qt::Horizontal, m_appearance, page)); auto *label = new QLabel(title, page); auto font = uiFont(16); font.setWeight(QFont::DemiBold); label->setFont(font); layout->addWidget(label);
}
void SettingsWindow::row(QWidget *page, const QString &title, QWidget *control, const QString &description) {
    auto *row = new QWidget(page); row->setProperty("settingText", title + " " + description); auto *owner = page; while (owner && !owner->property("pageIndex").isValid()) owner = owner->parentWidget(); row->setProperty("pageIndex", owner ? owner->property("pageIndex") : QVariant());
    auto *layout = new QVBoxLayout(row); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(8); auto *line = new QHBoxLayout; line->setContentsMargins(0, 0, 0, 0); line->setSpacing(16); auto *label = new QLabel(title, row); label->setWordWrap(true); line->addWidget(label, 1); line->addWidget(control); layout->addLayout(line); control->setAccessibleName(title);
    if (!description.isEmpty()) { auto *text = new QLabel(description, row); text->setProperty("secondary", true); text->setFont(uiFont(12)); text->setWordWrap(true); layout->addWidget(text); } bodyLayout(page)->addWidget(row); m_rows.append(row);
}
SharedButton *SettingsWindow::choice(const QString &section, const QString &key, const QList<QPair<QString, QJsonValue>> &options) {
    auto *button = new SharedButton({}, "settings-" + section + "-" + key, m_appearance, this); button->setMinimumWidth(144); button->setMaximumWidth(240); button->setOutlined(true); auto *menu = new SharedMenu(m_appearance, button); menu->setObjectName(button->objectName() + "-menu"); button->setMenu(menu);
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
    if (index < 0 || index >= m_pages->count()) return; m_pages->setCurrentIndex(index);
    for (int i = 0; i < m_navigation.size(); ++i) { m_navigation[i]->setProperty("navigationSelected", i == index); m_navigation[i]->update(); } reflow();
}
void SettingsWindow::filter(const QString &query) {
    const auto needle = query.trimmed(); if (needle.isEmpty()) { m_search->setProperty("searching", false); for (auto *row : m_rows) row->show(); for (auto *button : m_navigation) button->setEnabled(true); selectPage(m_beforeSearch); refresh(); return; }
    if (!m_search->property("searching").toBool()) m_beforeSearch = m_pages->currentIndex(); m_search->setProperty("searching", true);
    QSet<int> matches; for (auto *row : m_rows) { const int index = row->property("pageIndex").toInt(); const bool match = row->property("settingText").toString().contains(needle, Qt::CaseInsensitive) || m_bodies[index]->property("pageTitle").toString().contains(needle, Qt::CaseInsensitive); row->setVisible(match); if (match) matches.insert(index); }
    for (int i = 0; i < m_navigation.size(); ++i) m_navigation[i]->setEnabled(matches.contains(i));
    if (!matches.contains(m_pages->currentIndex()) && !matches.isEmpty()) selectPage(*std::min_element(matches.cbegin(), matches.cend()));
    m_status->setFullText(matches.isEmpty() ? QStringLiteral("没有匹配的设置") : QStringLiteral("已筛选设置；清空搜索恢复全部内容。"));
}
void SettingsWindow::refresh() {
    if (m_refreshing) return; QScopedValueRollback<bool> guard(m_refreshing, true); for (const auto &binding : m_bindings) binding();
    const auto error = m_preferences->error(); m_status->setFullText(error.isEmpty() ? QStringLiteral("设置更改即时生效") : error); m_status->setToolTip(m_status->fullText()); m_status->setStyleSheet(QStringLiteral("color:%1").arg((error.isEmpty() ? m_appearance->color("textSecondary") : m_appearance->semantic("error")).name()));
}
void SettingsWindow::applyAppearance() {
    setStyleSheet(QStringLiteral("QWidget#settingsWindow,QScrollArea#settingsScroll{background:%1;color:%2} QWidget#settingsSidebar{background:%3} QLabel{color:%2;background:transparent} QLabel[secondary=true]{color:%4} QLineEdit{background:%5;color:%2;border:1px solid %6;border-radius:6px;padding:4px 8px;selection-background-color:%7} QScrollArea>QWidget>QWidget{background:%1}")
        .arg(m_appearance->color("background").name(), m_appearance->color("text").name(), m_appearance->color("sidebar").name(), m_appearance->color("textSecondary").name(), m_appearance->color("surface").name(), m_appearance->color("border").name(), m_appearance->accentColor("soft").name()) + themedScrollBars(m_appearance));
    applyNativeAppearance(); refresh();
}
void SettingsWindow::reflow() {
    if (!m_swatches || m_bodies.size() < 4) return;
    const int available = m_pages->width() - 2 * visualTokens()["windows"].toObject()["settings"].toObject()["contentPadding"].toInt() - 16;
    const auto window = visualTokens()["windows"].toObject()["settings"].toObject(); const int columns = available >= 6 * setting("cardMinWidth") + 5 * setting("cardGap") ? window["swatchColumnsWide"].toInt() : window["swatchColumnsCompact"].toInt();
    for (int i = 0; i < m_swatchCards.size(); ++i) m_swatches->addWidget(m_swatchCards[i], i / columns, i % columns);
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
