#include "SourceContextMenuHost.h"

#include <QKeyEvent>
#include <QMouseEvent>

namespace branch_truce {
SourceContextMenuHost::SourceContextMenuHost(QQuickItem *parent) : QQuickItem(parent)
{
    setFiltersChildMouseEvents(true);
    setAcceptedMouseButtons(Qt::RightButton);
}

void SourceContextMenuHost::setEditor(QQuickItem *editor)
{
    if (m_editor == editor)
        return;
    if (m_editor)
        m_editor->removeEventFilter(this);
    disconnect(m_editorDestroyed);
    m_editor = editor;
    if (m_editor) {
        m_editor->installEventFilter(this);
        m_editorDestroyed = connect(m_editor, &QObject::destroyed, this, [this] {
            m_editor = nullptr;
            emit editorChanged();
        });
    }
    emit editorChanged();
}

bool SourceContextMenuHost::childMouseEventFilter(QQuickItem *item, QEvent *event)
{
    if (!m_editor || (item != m_editor && !m_editor->isAncestorOf(item))
        || event->type() != QEvent::MouseButtonPress)
        return false;
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::RightButton || mouse->buttons() != Qt::RightButton)
        return false;

    // This runs before the style's short-threshold TapHandler sees the press.
    // Own the release too, so it cannot land on a menu opened during the press.
    m_menuPosition = m_editor->mapFromScene(mouse->scenePosition());
    m_rightPressed = true;
    setKeepMouseGrab(true);
    mouse->setExclusiveGrabber(mouse->point(0), this);
    mouse->accept();
    return true;
}

void SourceContextMenuHost::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::RightButton || !m_rightPressed) {
        event->ignore();
        return;
    }
    m_rightPressed = false;
    setKeepMouseGrab(false);
    event->accept();
    if (m_editor && m_editor->isVisible() && m_editor->isEnabled()
        && contains(mapFromScene(event->scenePosition()))
        && m_editor->contains(m_editor->mapFromScene(event->scenePosition())))
        emit pointerMenuRequested(m_menuPosition);
}

void SourceContextMenuHost::mouseUngrabEvent()
{
    m_rightPressed = false;
    setKeepMouseGrab(false);
}

bool SourceContextMenuHost::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_editor || (event->type() != QEvent::KeyPress
        && event->type() != QEvent::KeyRelease && event->type() != QEvent::ShortcutOverride))
        return false;
    const auto *key = static_cast<QKeyEvent *>(event);
    const bool menuKey = key->key() == Qt::Key_Menu && key->modifiers() == Qt::NoModifier;
    const bool shiftF10 = key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier;
    if (!menuKey && !shiftF10)
        return false;

    // Accepting an instance's Keys signal does not suppress the base style's
    // signal handler. Consume the event before either handler can receive it.
    event->accept();
    if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
        emit keyboardMenuRequested();
    return true;
}
}
