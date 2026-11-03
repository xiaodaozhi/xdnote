#include "DocumentBreadcrumb.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include <QDir>
#include <QFileInfo>
#include <QPainter>

namespace xdnote {
DocumentBreadcrumb::DocumentBreadcrumb(Appearance *appearance, QWidget *parent) : QWidget(parent), m_appearance(appearance) {
    setObjectName("workspaceBreadcrumb"); setMinimumWidth(0); setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(appearance, &Appearance::changed, this, qOverload<>(&QWidget::update));
}
void DocumentBreadcrumb::setDocumentPath(const QString &path, const QString &root) {
    m_segments.clear();
    if (!path.isEmpty()) {
        const auto relative = root.isEmpty() ? QString() : QDir(root).relativeFilePath(path);
        const bool inside = !relative.isEmpty() && !QDir::isAbsolutePath(relative) && relative != ".." && !relative.startsWith("../");
        m_segments.append(inside ? QStringLiteral("文档") : root.isEmpty() ? QStringLiteral("文档") : QStringLiteral("工作区外"));
        if (inside) m_segments.append(relative.split('/', Qt::SkipEmptyParts)); else m_segments.append(QFileInfo(path).fileName());
    }
    setAccessibleName(fullText()); setToolTip(QDir::toNativeSeparators(path)); update();
}
void DocumentBreadcrumb::paintEvent(QPaintEvent *) {
    if (m_segments.isEmpty()) return;
    QPainter painter(this); painter.setFont(font()); const auto metrics = fontMetrics();
    auto parts = m_segments; const auto separator = QStringLiteral(" / ");
    if (metrics.horizontalAdvance(parts.join(separator)) > width() && parts.size() > 2) parts = {parts.first(), QStringLiteral("…"), parts.last()};
    int x = 0; const int baseline = (height() - metrics.height()) / 2 + metrics.ascent();
    for (int i = 0; i < parts.size(); ++i) {
        painter.setPen(m_appearance->color(i == parts.size() - 1 ? "text" : "textSecondary"));
        const auto text = metrics.elidedText(parts[i], Qt::ElideMiddle, qMax(0, width() - x)); painter.drawText(x, baseline, text); x += metrics.horizontalAdvance(text);
        if (i + 1 < parts.size()) { painter.setPen(m_appearance->color("textSecondary")); painter.drawText(x, baseline, separator); x += metrics.horizontalAdvance(separator); }
    }
}
}
