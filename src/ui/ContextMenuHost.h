// The context menu host routes desktop context-menu gestures from a text editor to the app's own menu.
#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
// Filter before the styled editor's pointer handlers and Keys attached object.
// Left-button selection, touch selection and the native focus frame stay with
// the TextArea; only desktop context-menu gestures belong to this host: a
// right click, the Menu key and Shift+F10, each without other modifiers. The
// editor's own default text menu never opens, so every pane offers the menu
// whose commands go through the app's undo history.
class ContextMenuHost : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *editor READ editor WRITE setEditor NOTIFY editorChanged)
public:
    explicit ContextMenuHost(QQuickItem *parent = nullptr);

    QQuickItem *editor() const { return m_editor; }
    void setEditor(QQuickItem *editor);

signals:
    void editorChanged();
    void pointerMenuRequested(const QPointF &position);
    void keyboardMenuRequested();

protected:
    bool childMouseEventFilter(QQuickItem *item, QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;

private:
    QPointer<QQuickItem> m_editor;
    QMetaObject::Connection m_editorDestroyed;
    QPointF m_menuPosition;
    bool m_rightPressed = false;
};
}
