#pragma once
#include <QWidget>

namespace xdnote {
class Appearance;
class DocumentBreadcrumb final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString fullText READ fullText)
public:
    DocumentBreadcrumb(Appearance *appearance, QWidget *parent);
    void setDocumentPath(const QString &path, const QString &root);
    QString fullText() const { return m_segments.join(QStringLiteral(" / ")); }
    QStringList segments() const { return m_segments; }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    Appearance *m_appearance;
    QStringList m_segments;
};
}
