#!/usr/bin/env python3
"""Queries a running application over AT-SPI, as a screen reader would.

    atspi-probe.py APP COMMAND [ARGS]

APP is a case-insensitive part of the accessible application name. Commands:

    wait NAME [SECONDS]   wait until a showing object named NAME exists
    dump                  print the tree: depth, role, name and states; roles
                          are the Atspi.Role nicks, which do not change
                          between AT-SPI releases as the display names do
    text NAME             print the text of the object named NAME
    focus NAME            ask the object named NAME to take keyboard focus
    press NAME            run the first action of the push button named NAME

tools/validate-installed-artifact.sh runs it inside the validation container,
on that container's own D-Bus session. It needs python3-gi and
gir1.2-atspi-2.0, and the application must expose AT-SPI, for example with
QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1. Exit status 1 means the application or
object was not found in time; 2 means a usage error.
"""
import sys
import time
import warnings

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402

# Older AT-SPI releases lack the replacements of the deprecated calls used here.
warnings.simplefilter("ignore", DeprecationWarning)

STATES = ("focusable", "focused", "editable", "read_only", "enabled", "showing")


def application(name, timeout):
    deadline = time.monotonic() + timeout
    while True:
        desktop = Atspi.get_desktop(0)
        for index in range(desktop.get_child_count()):
            app = desktop.get_child_at_index(index)
            if app is not None and name.lower() in (app.get_name() or "").lower():
                return app
        if time.monotonic() >= deadline:
            sys.exit("no accessible application named %r" % name)
        time.sleep(0.5)


def walk(node, depth=0):
    yield depth, node
    for index in range(node.get_child_count()):
        child = node.get_child_at_index(index)
        if child is not None:
            yield from walk(child, depth + 1)


def states(node):
    state_set = node.get_state_set()
    return [s for s in STATES if state_set.contains(getattr(Atspi.StateType, s.upper()))]


def find(app, name, role=None, showing=False):
    for _, node in walk(app):
        if node.get_name() != name or (role is not None and node.get_role() != role):
            continue
        if showing and "showing" not in states(node):
            continue
        return node
    return None


def require(app, name, role=None):
    node = find(app, name, role)
    if node is None:
        sys.exit("no accessible %s named %r" % (role.value_nick if role is not None else "object", name))
    return node


def one_line(text):
    return (text or "").replace("\n", "\\n").replace("\u2029", "\\u2029")


def main(argv):
    if len(argv) < 3:
        sys.exit(2)
    name, command, args = argv[1], argv[2], argv[3:]
    if command == "wait":
        if not args:
            sys.exit(2)
        timeout = float(args[1]) if len(args) > 1 else 60.0
        deadline = time.monotonic() + timeout
        app = application(name, timeout)
        while find(app, args[0], showing=True) is None:
            if time.monotonic() >= deadline:
                sys.exit("no showing accessible object named %r" % args[0])
            time.sleep(0.5)
        print("showing %r" % args[0])
        return
    app = application(name, 30.0)
    if command == "dump":
        print("application %r" % app.get_name())
        for depth, node in walk(app):
            print("%s%s | %s | %s" % ("  " * depth, node.get_role().value_nick,
                                      one_line(node.get_name()), ",".join(states(node))))
    elif command == "text" and args:
        node = require(app, args[0])
        text = node.get_text_iface()
        if text is None:
            sys.exit("%r has no text interface" % args[0])
        content = Atspi.Text.get_text(text, 0, Atspi.Text.get_character_count(text))
        print("text %r" % content)
        print("editable-text interface: %s" % (node.get_editable_text_iface() is not None))
    elif command == "focus" and args:
        component = require(app, args[0]).get_component_iface()
        print("grab_focus %s" % (component is not None and component.grab_focus()))
    elif command == "press" and args:
        action = require(app, args[0], Atspi.Role.PUSH_BUTTON).get_action_iface()
        names = [action.get_action_name(i) for i in range(action.get_n_actions())]
        print("actions %s; %s: %s" % (names, names[0] if names else "none",
                                      bool(names) and action.do_action(0)))
    else:
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv)
