"""Pure assertions about native journey observations and their pinned expectations."""

def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def item(state, name):
    matches = [row for row in state['items'] if row['name'] == name and row.get('onScreen', True)]
    require(len(matches) == 1, f'{name}: expected one reachable control, observed {matches}')
    return matches[0]


def verify_prominent_actions(state):
    for name in ('saveDraft', 'markResolved'):
        control = item(state, name)
        require(control['x'] >= 0 and control['y'] >= 0
                and control['x'] + control['width'] <= state['width']
                and control['y'] + control['height'] <= state['height'],
                f'{name} must remain prominent and fully reachable: {control}')


def verify_window(state):
    pitch = item(state, 'comparisonView')['rowHeight']
    for name in ('stage2Viewport', 'resultViewport', 'stage3Viewport'):
        pane = item(state, name)
        require(pitch > 0 and pane['height'] >= 8 * pitch, f'{name} has fewer than eight rows: {pane}')
    for name in ('stage2Identity', 'stage3Identity'):
        identity = item(state, name)
        require(not identity['truncated'], f'{name} truncated at {state["width"]}x{state["height"]}: {identity}')


def verify_line_cue(state, pane, line, kind, fill=True):
    details = state['renderedDetails']
    glyphs = [row for row in details if row.get('line') == line and row.get('kind')
              and pane + 'GutterMarkerSlot' in row.get('ancestors', [])]
    require(len(glyphs) == 1 and glyphs[0]['kind'] == kind,
            f'{pane}:{line} must have exactly one {kind} glyph: {glyphs}')
    backgrounds = [row for row in details if row.get('line') == line
                   and pane + 'BackgroundsLines' in row.get('ancestors', [])]
    require((len(backgrounds) == 1 and backgrounds[0].get('kind') == kind) if fill else not backgrounds,
            f'{pane}:{line} background differs from {kind}, fill={fill}: {backgrounds}')


def verify_text(state, expected, commits):
    require(state['path'] == 'src/net/retry.cpp', 'Wrong text file selected')
    verify_prominent_actions(state)
    verify_window(state)
    for scope in ('section/1/392/446', 'section/2/392/450'):
        for kind in ('Accept', 'Ignore', 'Menu'):
            require(not any(row['name'] == f'choice{kind}-{scope}' and row.get('onScreen', True)
                            for row in state['items']), f'Decided range retains {kind} chrome: {scope}')
    for pane, line, kind, fill in [('stage3', 4, 'added', True), ('result', 4, 'added', True),
                                   ('stage2', 16, 'changed', True), ('result', 22, 'changed', True),
                                   ('stage2', 19, 'accepted', True), ('result', 25, 'accepted', True),
                                   ('stage3', 20, 'ignored', False)]:
        verify_line_cue(state, pane, line, kind, fill)
    require(int(state['progress']['conflicts']) == 1, 'Expected one actual conflict region')
    require(int(state['progress']['undecided']) == 8, 'Separate source decisions changed')
    require(item(state, 'undecidedCounter')['text'] == '1 conflict left', 'Conflict stepper count differs')
    for name, side in [('stage2Text', 'left'), ('stage3Text', 'right')]:
        require(item(state, name)['text'].encode() == expected[side], f'{side} source bytes differ')
    require(state['resultText'].encode() == expected['result'], 'Result bytes differ')
    require(len(state['resultText'].splitlines()) == 30, 'Expected 30 result lines')
    blocks = state['markerBlocks']
    require(len(blocks) == 1 and blocks[0]['folded'] and blocks[0]['resultLineBegin'] == 11
            and blocks[0]['resultLineEnd'] == 18, 'Expected folded marker lines 11 through 17')
    numbers = [r['number'] for r in item(state, 'resultGutter')['rows']]
    require(11 in numbers and numbers[numbers.index(11)+1] == 18, 'Visible gutter must jump from 11 to 18')
    for side, line, decision in [('stage2', 19, 'accepted'), ('stage3', 20, 'ignored')]:
        require(any(r['side'] == side and r['scope'] == 'section' and r['sourceLineBegin'] == line
                    and r['state'] == decision for r in state['regions']), f'Missing {decision} contribution')
    require(item(state, 'workflowStatus')['text'] == '1 of 3 files resolved', 'Release notes not resolved')
    require(all(item(state, name).get('contentY') == 0 for name in ('stage2Viewport', 'stage3Viewport', 'resultViewport')),
            'Comparison panes did not return to the top')
    for side, branch in [('stage2', 'main'), ('stage3', 'incoming')]:
        require(commits[branch][:10] in item(state, side + 'Identity')['text'], 'Source commit identity differs')
    require(not any(r['name'] == 'commandMessage' for r in state['items']), 'Unexpected command failure')


def verify_binary(state):
    require(state['path'] == 'assets/logo.png', 'Wrong binary file selected')
    verify_prominent_actions(state)
    require(item(state, 'selectedProblem')['text'] == 'Binary files are resolved outside Branch Truce',
            'Binary heading lost its exclusion meaning')
    explanation = item(state, 'selectedProblem')['explanation']
    require('logo.png' in explanation and 'git add assets/logo.png' in explanation
            and 'Git' in explanation, 'Concrete binary guidance missing')
    require(not item(state, 'markResolved')['enabled'] and not item(state, 'saveDraft')['enabled'],
            'Binary save/resolution must stay disabled')
    require(item(state, 'workflowStatus')['text'] == '1 of 3 files resolved', 'Resolved count changed')
    require(item(state, 'fileFacts')['text'] == 'PNG image · 18.20 KiB', 'Wrong binary facts')
