#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
// Filter before the styled editor's pointer handlers and Keys attached object.
// Left-button selection, touch selection and the native focus frame stay with
// the TextArea; only desktop context-menu gestures belong to this host.
class SourceContextMenuHost : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *editor READ editor WRITE setEditor NOTIFY editorChanged)
public:
    explicit SourceContextMenuHost(QQuickItem *parent = nullptr);

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
