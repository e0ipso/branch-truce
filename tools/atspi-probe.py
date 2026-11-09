#!/usr/bin/env python3
"""Queries a running application over AT-SPI, as a screen reader would.

    atspi-probe.py APP COMMAND [ARGS]

APP is a case-insensitive part of the accessible application name. Commands:

    wait NAME [SECONDS]   wait until a showing object named NAME exists
    wait-match REGEX [SECONDS]
                          wait until a showing object whose name contains a
                          match of the Python regular expression REGEX exists,
                          for text such as a progress count that other words
                          surround and that moves between screens
    dump                  print the tree: depth, role, name and states; roles
                          are the Atspi.Role nicks, which do not change
                          between AT-SPI releases as the display names do
    text NAME             print the text of the object named NAME
    focus NAME            ask the object named NAME to take keyboard focus
    press NAME            run the first action of the push button named NAME

SECONDS defaults to 60. tools/validate-installed-artifact.sh runs it inside the
validation container, on that container's own D-Bus session. It needs
python3-gi and gir1.2-atspi-2.0, and the application must expose AT-SPI, for
example with QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1.

Exit status 0 means success. 1 means the application or object was not found
in time, or focus or press failed; the reason is on stderr. 2 means a usage
error.
"""
import argparse
import math
import re
import sys
import time
import warnings

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib  # noqa: E402

# Older AT-SPI releases lack the replacements of the deprecated calls used here.
warnings.simplefilter("ignore", DeprecationWarning)

STATES = ("focusable", "focused", "editable", "read_only", "enabled", "showing")


class ProbeError(Exception):
    """The application or object is missing, or it refused the request."""


def application(name, timeout):
    deadline = time.monotonic() + timeout
    while True:
        desktop = Atspi.get_desktop(0)
        for index in range(desktop.get_child_count()):
            app = desktop.get_child_at_index(index)
            if app is not None and name.lower() in (app.get_name() or "").lower():
                return app
        if time.monotonic() >= deadline:
            raise ProbeError("no accessible application named %r" % name)
        time.sleep(0.5)


def walk(node, depth=0):
    """Yield the tree depth first. Children that vanish while it is read are skipped."""
    yield depth, node
    for index in range(node.get_child_count()):
        try:
            child = node.get_child_at_index(index)
        except GLib.Error:
            continue
        if child is not None:
            yield from walk(child, depth + 1)


def states(node):
    state_set = node.get_state_set()
    return [s for s in STATES if state_set.contains(getattr(Atspi.StateType, s.upper()))]


def find_by(app, accepts, role=None, showing=False):
    for _, node in walk(app):
        if not accepts(node.get_name()) or (role is not None and node.get_role() != role):
            continue
        if showing and "showing" not in states(node):
            continue
        return node
    return None


def find(app, name, role=None, showing=False):
    return find_by(app, lambda candidate: candidate == name, role, showing)


def require(app, name, role=None):
    node = find(app, name, role)
    if node is None:
        raise ProbeError("no accessible %s named %r" % (role.value_nick if role is not None else "object", name))
    return node


def one_line(text):
    return (text or "").replace("\n", "\\n").replace("\u2029", "\\u2029")


def wait(app, accepts, described, timeout):
    deadline = time.monotonic() + timeout
    while (found := find_by(app, accepts, showing=True)) is None:
        if time.monotonic() >= deadline:
            raise ProbeError("no showing accessible object %s" % described)
        time.sleep(0.5)
    return "showing %r" % found.get_name()


def dump(app):
    lines = ["application %r" % app.get_name()]
    for depth, node in walk(app):
        lines.append("%s%s | %s | %s" % ("  " * depth, node.get_role().value_nick,
                                         one_line(node.get_name()), ",".join(states(node))))
    return "\n".join(lines)


def text(app, name):
    node = require(app, name)
    iface = node.get_text_iface()
    if iface is None:
        raise ProbeError("%r has no text interface" % name)
    content = Atspi.Text.get_text(iface, 0, Atspi.Text.get_character_count(iface))
    return "text %r\neditable-text interface: %s" % (content, node.get_editable_text_iface() is not None)


def focus(app, name):
    component = require(app, name).get_component_iface()
    if component is None:
        raise ProbeError("%r has no component interface to take focus" % name)
    if not component.grab_focus():
        raise ProbeError("%r refused keyboard focus" % name)
    return "grab_focus True"


def press(app, name):
    action = require(app, name, Atspi.Role.PUSH_BUTTON).get_action_iface()
    if action is None:
        raise ProbeError("%r has no action interface" % name)
    names = [action.get_action_name(i) for i in range(action.get_n_actions())]
    if not names:
        raise ProbeError("%r has no actions" % name)
    if not action.do_action(0):
        raise ProbeError("%r did not run its action %r" % (name, names[0]))
    return "actions %s; %s: True" % (names, names[0])


def seconds(value):
    try:
        result = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not a number of seconds" % value) from None
    if not math.isfinite(result) or result < 0:
        raise argparse.ArgumentTypeError("%r is not a number of seconds" % value)
    return result


def pattern(value):
    try:
        return re.compile(value)
    except re.error as error:
        raise argparse.ArgumentTypeError("%r is not a regular expression: %s" % (value, error)) from None


def parser():
    result = argparse.ArgumentParser(prog="atspi-probe.py", description=__doc__.split("\n\n")[0],
                                     epilog=__doc__.split("\n\n", 2)[2],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    result.add_argument("app", help="case-insensitive part of the accessible application name")
    commands = result.add_subparsers(dest="command", required=True, metavar="COMMAND")
    for name, target in (("wait", "NAME"), ("wait-match", "REGEX")):
        command = commands.add_parser(name, help="wait until a showing object matches " + target)
        command.add_argument("target", metavar=target, type=pattern if name == "wait-match" else str)
        command.add_argument("timeout", metavar="SECONDS", type=seconds, nargs="?", default=60.0)
    commands.add_parser("dump", help="print the accessible tree")
    for name, purpose in (("text", "print the text of"), ("focus", "focus"), ("press", "press")):
        commands.add_parser(name, help=purpose + " the object named NAME").add_argument("target", metavar="NAME")
    return result


def run(args):
    if args.command == "wait":
        target = args.target
        return wait(application(args.app, args.timeout), lambda candidate: candidate == target,
                    "named %r" % target, args.timeout)
    if args.command == "wait-match":
        return wait(application(args.app, args.timeout),
                    lambda candidate: args.target.search(candidate or "") is not None,
                    "with a name matching %r" % args.target.pattern, args.timeout)
    app = application(args.app, 30.0)
    if args.command == "dump":
        return dump(app)
    return {"text": text, "focus": focus, "press": press}[args.command](app, args.target)


def main(argv):
    args = parser().parse_args(argv[1:])
    try:
        print(run(args))
    except ProbeError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
