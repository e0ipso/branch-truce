"""Drive real native controls and validate the resulting scene and repository evidence."""
import json

from fixture import git, reference_bytes, unmerged
from reports import write_json
from scene import item, require, verify_binary, verify_text

SHOT_PATHS = {'text': 'src/net/retry.cpp', 'binary': 'assets/logo.png'}


def run_journey(client, out):
    """Resolve the text and release-note conflicts and exercise undo and range menus."""
    client.command(path='src/net/retry.cpp', target='choiceAccept-section/1/392/446')
    client.command('click', target='choiceAccept-section/1/392/446')
    client.command('click', target='choiceIgnore-section/2/392/450')
    client.command('click-path', target='docs/release-notes.md')
    client.command(path='docs/release-notes.md', target='choiceAccept-section/1/17/37')
    client.command('click', target='choiceAccept-section/1/17/37')
    client.command('click', target='choiceIgnore-section/2/17/46')
    client.command('toolbar-click', target='markResolved')
    client.command(target='workflowStatus', text='1 of 3 files resolved')
    client.command('click-path', target='src/net/retry.cpp')
    client.command(path='src/net/retry.cpp')
    client.command('toolbar-click', target='undoCommand')
    undone = client.command(target='choiceIgnore-section/2/392/450')
    write_json(out / 'undo-state.json', undone)
    require(any(row['name'] == 'choiceIgnore-section/2/392/450' and row.get('onScreen', True)
                for row in undone['items']), 'Undo must restore undecided controls')
    client.command('toolbar-click', target='redoCommand')
    for side, line, gesture in [('stage2', 19, 'Menu'), ('stage3', 20, 'Shift+F10'),
                                 ('stage2', 10, 'right-click')]:
        menu = client.command('source-menu', target=side + 'Text', line=line, gesture=gesture)
        require(any(row['name'].startswith('choiceResolve-') for row in menu['items']),
                f'{gesture} did not open a real range menu at {side}:{line}')
        write_json(out / f'{side}-{line}-menu-state.json', menu)
        client.command('escape')
    client.command('click', target='stage2Label')
    client.command('neutral')


def show_binary(client, out):
    """Select the binary file and check its technical details before its shot."""
    client.command('click-path', target='assets/logo.png')
    client.command('neutral', path='assets/logo.png')
    client.command('click-accessible', target='Technical details')
    details = client.command()
    diagnostic = item(details, 'selectedProblemDiagnostic')
    require(diagnostic['text'] == details['supportDetail'] and 'UTF-8' in diagnostic['text'],
            'Technical details changed or lost the captured decoder diagnostic')
    require(diagnostic['readOnly'] and diagnostic['plainText'] and diagnostic['selectByMouse'],
            'Technical details must remain selectable plain text')
    write_json(out / 'binary-details-state.json', details)
    client.command('escape')


def capture_state(client, kind, repo):
    """Obtain a settled scene and check both scene semantics and live Git evidence."""
    state = client.command('capture-ready', path=SHOT_PATHS[kind])
    if kind == 'text':
        verify_text(state, {side: reference_bytes(f'expected-{side}.cpp') for side in ('left', 'right', 'result')},
                    json.loads((repo.parent / 'fixture.json').read_text()))
        require(sorted(unmerged(repo)) == ['assets/logo.png', 'src/net/retry.cpp'], 'Unexpected unresolved Git index')
        require(git(repo, 'show', ':0:docs/release-notes.md').stdout == b'# Release notes\n\nAdd retry timeouts.\n',
                'Release notes staged bytes differ')
    else:
        verify_binary(state)
    return state
