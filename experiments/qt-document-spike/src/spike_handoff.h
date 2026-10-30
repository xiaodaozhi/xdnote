#pragma once

class QQuickItem;

// Shared by the automated fixture and the manual desktop experiment.
bool restoreSelection(QQuickItem *editor, int start, int end, int cursor);
bool transferDocument(QQuickItem *from, QQuickItem *to);
