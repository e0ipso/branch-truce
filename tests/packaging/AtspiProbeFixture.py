"""Runs tools/atspi-probe.py against a fake gi.repository.Atspi.

    python3 -I -B AtspiProbeFixture.py PROBE selftest|dump|text

No D-Bus, AT-SPI registry or display is used. The fake replaces the gi module
before the probe is loaded, and a fake clock replaces its time module, so waits
and timeouts finish at once. Modes:

    selftest  checks argument handling, application lookup, the tree walk,
              state names, dump format and the wait, text, focus and press
              commands, then prints the assertion count
    dump      prints the probe's dump of a tree with the controls that
              tools/validate-installed-artifact.sh checks
    text      prints the probe's text output for an editable result holding
              the text that validation types, with Qt's paragraph separator
"""
import contextlib
import importlib.util
import io
import sys
import types

assertions = 0


def check(condition, message):
    global assertions
    if not condition:
        sys.exit("FAIL " + message)
    assertions += 1


def equal(actual, expected, message):
    check(actual == expected, "%s: expected %r, got %r" % (message, expected, actual))


class Role:
    def __init__(self, nick):
        self.value_nick = nick


ROLES = {nick: Role(nick) for nick in (
    "application", "frame", "label", "text", "list-item", "check-box", "combo-box", "push-button")}


class StateType:
    pass


for _state in ("FOCUSABLE", "FOCUSED", "EDITABLE", "READ_ONLY", "ENABLED", "SHOWING", "VISIBLE", "SELECTED"):
    setattr(StateType, _state, _state.lower())


class StateSet:
    def __init__(self, names):
        self.names = set(names)

    def contains(self, state):
        return state in self.names


class TextIface:
    def __init__(self, content):
        self.content = content


class Text:
    @staticmethod
    def get_character_count(text):
        return len(text.content)

    @staticmethod
    def get_text(text, start, end):
        return text.content[start:end]


class Action:
    def __init__(self, names, log):
        self.names = names
        self.log = log

    def get_n_actions(self):
        return len(self.names)

    def get_action_name(self, index):
        return self.names[index]

    def do_action(self, index):
        self.log.append(index)
        return True


class Component:
    def __init__(self, result):
        self.result = result
        self.calls = 0

    def grab_focus(self):
        self.calls += 1
        return self.result


class Node:
    def __init__(self, role, name, states=(), children=(), text=None, editable=False, actions=None,
                 component=None):
        self.role = role
        self.name = name
        self.states = set(states)
        self.children = list(children)
        self.text = text
        self.editable = editable
        self.actions = actions
        self.pressed = []
        self.component = component

    def get_name(self):
        return self.name

    def get_role(self):
        return ROLES[self.role]

    def get_child_count(self):
        return len(self.children)

    def get_child_at_index(self, index):
        return self.children[index]

    def get_state_set(self):
        return StateSet(self.states)

    def get_text_iface(self):
        return None if self.text is None else TextIface(self.text)

    def get_editable_text_iface(self):
        return object() if self.editable else None

    def get_component_iface(self):
        return self.component

    def get_action_iface(self):
        return Action(self.actions or [], self.pressed)


class Clock:
    """monotonic() advances only through sleep(); hooks run after each sleep."""

    def __init__(self):
        self.now = 1000.0
        self.sleeps = []
        self.hooks = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds
        for hook in list(self.hooks):
            hook(len(self.sleeps))


desktop = Node("application", "main")
required_versions = []
Atspi = types.SimpleNamespace(get_desktop=lambda index: desktop, Role=types.SimpleNamespace(
    PUSH_BUTTON=ROLES["push-button"]), StateType=StateType, Text=Text)
gi = types.ModuleType("gi")
gi.require_version = lambda name, version: required_versions.append((name, version))
gi.repository = types.ModuleType("gi.repository")
gi.repository.Atspi = Atspi
sys.modules["gi"] = gi
sys.modules["gi.repository"] = gi.repository


def load(path):
    spec = importlib.util.spec_from_file_location("atspi_probe", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def application_tree():
    """The accessible names validate-installed-artifact.sh looks for."""
    common = ("enabled", "showing")
    return Node("application", "Branch Truce", children=[
        Node("frame", "Branch Truce", common, children=[
            Node("list-item", "tracked.txt", ("focusable", "selected") + common),
            Node("text", "Left source", ("focusable", "read_only") + common, text="main\n"),
            Node("text", "Editable result", ("focusable", "editable") + common,
                 text="merged é ü ", editable=True, component=Component(True)),
            Node("text", "Right source", ("focusable", "read_only") + common, text="side\n"),
            Node("text", "Passage search", ("focusable", "editable") + common, text="", editable=True),
            Node("combo-box", "Search scope", ("focusable",) + common),
            Node("check-box", "Wrap lines", ("focusable",) + common),
            Node("push-button", "Save", ("focusable",) + common, actions=["Press"]),
            Node("push-button", "Mark resolved", ("focusable",) + common, actions=["Press"]),
        ]),
    ])


def run(probe, *argv):
    out = io.StringIO()
    code = 0
    with contextlib.redirect_stdout(out):
        try:
            probe.main(["atspi-probe.py"] + list(argv))
        except SystemExit as exit_:
            code = exit_.code
    return code, out.getvalue()


def selftest(probe, clock):
    global desktop
    equal(required_versions, [("Atspi", "2.0")], "required Atspi version")
    equal(probe.one_line("a\nb c"), "a\\nb\\u2029c", "one_line escapes")
    equal(probe.one_line(None), "", "one_line of no text")

    app = application_tree()
    desktop = Node("application", "main", children=[None, Node("application", "plasmashell"), app])

    # Usage errors exit 2 before any lookup.
    for argv in ((), ("Branch Truce",), ("Branch Truce", "wait")):
        equal(run(probe, *argv), (2, ""), "usage %r" % (argv,))
    equal(clock.sleeps, [], "usage errors do not wait")
    # Unknown commands and commands without their name argument are usage errors too.
    for argv in (("Branch Truce", "bogus"), ("Branch Truce", "text"), ("Branch Truce", "focus"),
                 ("Branch Truce", "press")):
        equal(run(probe, *argv), (2, ""), "usage %r" % (argv,))

    # The application is a case-insensitive part of the name; empty desktop slots are skipped.
    check(probe.application("branch TRUCE", 0) is app, "case-insensitive application lookup")
    before = clock.now
    equal(run(probe, "Kate", "dump"), ("no accessible application named 'Kate'", ""), "missing application")
    equal(clock.now - before, 30.0, "missing application waits 30 s")
    check(all(seconds == 0.5 for seconds in clock.sleeps), "application polls every 0.5 s")
    # An application that registers late is found by a later poll.
    del clock.sleeps[:]
    desktop = Node("application", "main")
    clock.hooks.append(lambda count: count == 3 and desktop.children.append(app))
    check(probe.application("Branch Truce", 10) is app, "late application")
    equal(len(clock.sleeps), 3, "polls until the application registers")
    del clock.hooks[:]

    # The walk is depth-first with depths, and skips missing children.
    walked = [(depth, node.get_name()) for depth, node in probe.walk(
        Node("frame", "root", children=[Node("label", "a", children=[None, Node("label", "b")]), None,
                                        Node("label", "c")]))]
    equal(walked, [(0, "root"), (1, "a"), (2, "b"), (1, "c")], "walk order")
    # States come in the probe's fixed order and only from its list.
    equal(probe.states(Node("text", "x", ("showing", "selected", "read_only", "focusable"))),
          ["focusable", "read_only", "showing"], "state order")

    # find matches the exact name, then the role, then showing.
    hidden = Node("push-button", "Confirm resolution", ("enabled",), actions=["Press"])
    label = Node("label", "Confirm resolution", ("showing",))
    shown = Node("push-button", "Confirm resolution", ("enabled", "showing"), actions=["Press"])
    dialog = Node("application", "Branch Truce", children=[label, hidden, shown])
    check(probe.find(dialog, "Confirm resolution") is label, "first match by name")
    check(probe.find(dialog, "Confirm resolution", ROLES["push-button"]) is hidden, "role filter")
    check(probe.find(dialog, "Confirm resolution", ROLES["push-button"], showing=True) is shown,
          "role and showing filter")
    check(probe.find(dialog, "confirm resolution") is None, "names match exactly")

    # dump prints depth, role nick, one-line name and states.
    desktop = Node("application", "main", children=[Node("application", "Branch Truce", ("showing",), children=[
        Node("label", "two\nlines", ("enabled",)), Node("push-button", None, ("focusable", "enabled"))])])
    equal(run(probe, "Branch Truce", "dump"), (0, "application 'Branch Truce'\n"
                                                  "application | Branch Truce | showing\n"
                                                  "  label | two\\nlines | enabled\n"
                                                  "  push-button |  | focusable,enabled\n"), "dump")

    # wait succeeds once the object shows and times out with the given seconds.
    target = Node("push-button", "Confirm resolution", ("enabled",), actions=["Press"])
    desktop = Node("application", "main", children=[Node("application", "Branch Truce", children=[target])])
    del clock.sleeps[:]
    clock.hooks.append(lambda count: count == 2 and target.states.add("showing"))
    equal(run(probe, "Branch Truce", "wait", "Confirm resolution", "5"), (0, "showing 'Confirm resolution'\n"),
          "wait for a showing object")
    equal(len(clock.sleeps), 2, "wait polls until the object shows")
    del clock.hooks[:]
    target.states.discard("showing")
    before = clock.now
    equal(run(probe, "Branch Truce", "wait", "Confirm resolution", "1.5"),
          ("no showing accessible object named 'Confirm resolution'", ""), "wait timeout")
    equal(clock.now - before, 1.5, "wait honours its timeout")
    before = clock.now
    equal(run(probe, "Branch Truce", "wait", "Confirm resolution")[0],
          "no showing accessible object named 'Confirm resolution'", "default wait timeout")
    equal(clock.now - before, 60.0, "wait defaults to 60 s")

    # text prints the content with repr and whether it is editable.
    desktop = Node("application", "main", children=[application_tree()])
    equal(run(probe, "Branch Truce", "text", "Left source"),
          (0, "text 'main\\n'\neditable-text interface: False\n"), "read-only text")
    equal(run(probe, "Branch Truce", "text", "Save"), ("'Save' has no text interface", ""), "no text interface")
    equal(run(probe, "Branch Truce", "text", "Missing"), ("no accessible object named 'Missing'", ""),
          "text of a missing object")

    # focus reports the component's answer, or False without a component.
    result = probe.find(desktop.children[0], "Editable result")
    equal(run(probe, "Branch Truce", "focus", "Editable result"), (0, "grab_focus True\n"), "focus")
    equal(result.component.calls, 1, "focus asked the component once")
    equal(run(probe, "Branch Truce", "focus", "Wrap lines"), (0, "grab_focus False\n"), "focus without component")

    # press runs the first action of a push button only.
    desktop = Node("application", "main", children=[dialog])
    equal(run(probe, "Branch Truce", "press", "Confirm resolution"),
          (0, "actions ['Press']; Press: True\n"), "press")
    equal((label.pressed, hidden.pressed, shown.pressed), ([], [0], []), "press acts on the first push button")
    hidden.actions = []
    equal(run(probe, "Branch Truce", "press", "Confirm resolution"),
          (0, "actions []; none: False\n"), "press without actions")
    desktop = Node("application", "main", children=[Node("application", "Branch Truce", children=[label])])
    equal(run(probe, "Branch Truce", "press", "Confirm resolution"),
          ("no accessible push-button named 'Confirm resolution'", ""), "press needs a push button")
    print("%d atspi-probe assertions passed" % assertions)


def main(argv):
    global desktop
    if len(argv) != 3 or argv[2] not in ("selftest", "dump", "text"):
        sys.exit("usage: AtspiProbeFixture.py PROBE selftest|dump|text")
    probe = load(argv[1])
    clock = Clock()
    probe.time = types.SimpleNamespace(monotonic=clock.monotonic, sleep=clock.sleep)
    if argv[2] == "selftest":
        selftest(probe, clock)
        return
    desktop = Node("application", "main", children=[application_tree()])
    command = ["dump"] if argv[2] == "dump" else ["text", "Editable result"]
    code, out = run(probe, "Branch Truce", *command)
    sys.stdout.write(out)
    if code:
        sys.exit(code)


main(sys.argv)
