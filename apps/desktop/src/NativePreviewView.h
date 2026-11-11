#pragma once

#include "NativePreviewContent.h"
#include <QAbstractScrollArea>
#include <QHash>
#include <memory>
#include <functional>

namespace xdnote {
class Appearance;
class NativePreviewView final : public QAbstractScrollArea {
    Q_OBJECT
public:
    struct Statistics {
        int cachedDocuments = 0, peakCachedDocuments = 0, layouts = 0;
        qsizetype cachedEstimatedBytes = 0, peakCachedEstimatedBytes = 0;
        qsizetype retainedContentBytes = 0, decodedImageBytes = 0;
        qsizetype peakDecodedImageBytes = 0;
        int chunks = 0, flows = 0, resourceLoads = 0, deniedResources = 0;
        bool layoutPending = false, layoutRunning = false;
    };
    explicit NativePreviewView(Appearance *appearance, QWidget *parent = nullptr, int padding = 24);
    ~NativePreviewView() override;
    // A new request revokes old resources and actions immediately. Only a result
    // for the exact document/revision/request/path/resource stamp can be shown.
    void expect(const NativePreviewStamp &stamp);
    void setActionValidator(std::function<bool(const NativePreviewStamp &)> validator);
    bool present(NativePreviewStamp stamp, std::shared_ptr<const NativePreviewContent> content,
                 QHash<int, QByteArray> admittedPng = {}, bool localDocumentsAdmitted = false);
    bool presentAsync(NativePreviewStamp stamp, std::shared_ptr<const NativePreviewContent> content,
                 QHash<int, QByteArray> admittedPng = {}, bool localDocumentsAdmitted = false);
    void showError(const NativePreviewStamp &stamp, const QString &message);
    QSizeF contentsSize() const;
    bool ready() const;
    int admittedImageCount() const;
    void setZoomFactor(qreal factor);
    qreal zoomFactor() const;
    const NativePreviewContent *content() const;
    NativePreviewStamp displayedStamp() const;
    QString selectedText() const;
    void setSelection(int anchor, int cursor);
    QPair<int, int> selection() const;
    void copy();
    void selectAll();
    bool navigateToHeading(const QString &fragment, const NativePreviewStamp &stamp);
    bool activateAction(int action);
    QRect characterRect(int offset);
    int offsetAtPoint(const QPoint &viewportPoint);
    void scrollToText(int offset);
    QPointF readingPosition() const;
    void restoreReadingPosition(const QPointF &position);
    Statistics statistics() const;
    QString error() const;
    bool keyboardOutlineVisible() const;
signals:
    void copyTextRequested(const QString &text);
    void documentLinkRequested(const QString &target, const xdnote::NativePreviewStamp &stamp);
    void fragmentNavigated(const QString &fragment);
    void navigationMessage(const QString &message);
    void selectionChanged();
    void layoutFailed(const QString &message);
    void layoutStarted();
    void layoutReady(const xdnote::NativePreviewStamp &stamp);
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void scrollContentsBy(int, int) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    bool focusNextPrevChild(bool next) override;
private:
    friend class AccessibleNativePreview;
    struct State;
    std::unique_ptr<State> d;
    void rebuild(bool keepPosition);
    void updateControls();
    void scheduleLayout();
    void finishLayout(int offset, bool restore);
    int actionAtPoint(const QPoint &point);
};
}
Q_DECLARE_METATYPE(xdnote::NativePreviewStamp)
