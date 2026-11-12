#pragma once
#include "WindowChrome.h"
#include <QHash>
#include <QJsonValue>
#include <functional>
class QLineEdit;
class QStackedWidget;
class QGridLayout;
class QBoxLayout;
class QLabel;
namespace xdnote {
class Preferences;
class SharedButton;
class ElidedLabel;
class WindowGeometry;
class WindowGeometryStore;
class SourceEditor;
class NativePreviewView;
class SettingsWindow final : public ApplicationWindow {
    Q_OBJECT
public:
    SettingsWindow(Appearance *appearance, WindowGeometryStore *store);
    ~SettingsWindow() override;
    void selectPage(int index);
protected:
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
private:
    QWidget *page(const QString &title, const QString &description);
    void group(QWidget *page, const QString &title);
    void row(QWidget *page, const QString &title, QWidget *control, const QString &description = {});
    SharedButton *choice(const QString &section, const QString &key, const QList<QPair<QString, QJsonValue>> &options);
    SharedButton *fontChoice(const QString &section);
    QWidget *toggle(const QString &section, const QString &key);
    void refresh();
    void applyAppearance();
    void filter(const QString &query);
    void reflow();
    Appearance *m_appearance;
    Preferences *m_preferences;
    WindowGeometry *m_geometry;
    QLineEdit *m_search;
    QStackedWidget *m_pages;
    QList<SharedButton *> m_navigation;
    QList<QWidget *> m_bodies, m_rows;
    QList<std::function<void()>> m_bindings;
    QGridLayout *m_swatches = nullptr;
    QList<QWidget *> m_swatchCards;
    ElidedLabel *m_status;
    QBoxLayout *m_readerLayout = nullptr;
    bool m_refreshing = false;
    int m_beforeSearch = 0;
    QStackedWidget *m_content = nullptr;
    QWidget *m_searchResults = nullptr, *m_feedback = nullptr;
    QLabel *m_statusIcon = nullptr, *m_feedbackText = nullptr;
    SharedButton *m_retrySettings = nullptr;
    QString m_hiddenError;
};
}
