#pragma once
#include <QWidget>
#include <QPointer>
#include "DocumentSession.h"
class QLineEdit;
class QLabel;
class QCheckBox;
namespace xdnote {
class Appearance;
struct TextMatch { int start = 0, length = 0; };
struct TextMatches { QVector<TextMatch> items; bool complete = true; };
TextMatches findText(const QString &text, const QString &query, bool matchCase = false, bool wholeWord = false, int limit = 10000);
class DocumentSearch final : public QWidget {
    Q_OBJECT
public:
    DocumentSearch(Appearance *appearance, QWidget *parent);
    void setSession(DocumentSession *session);
    void open(bool replace = false);
    void next(bool backwards = false);
signals:
    void locationRequested(int startUtf16, int endUtf16);
    void dismissed();
protected:
    void keyPressEvent(QKeyEvent *) override;
private:
    void refresh();
    void replace(bool all);
    QPointer<DocumentSession> m_session;
    DocumentSnapshot m_snapshot;
    TextMatches m_matches;
    int m_current = -1;
    QLineEdit *m_query, *m_replacement;
    QCheckBox *m_case, *m_word;
    QLabel *m_status;
    QWidget *m_replaceRow;
};
}
