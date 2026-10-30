#pragma once

class QQuickItem;
class QQuickWindow;

// Shared by the automated fixture and the manual desktop experiment.
bool restoreSelection(QQuickItem *editor, int start, int end, int cursor);
bool transferDocument(QQuickItem *from, QQuickItem *to);

// Alternate fixture: move one editor without replacing its document. Its
// QObject owner must outlive the windows; focus and layout remain untested.
bool reparentEditorFixture(QQuickItem *editor, QQuickWindow *destination);
