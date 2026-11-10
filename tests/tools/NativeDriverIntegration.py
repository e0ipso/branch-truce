"""Exercise the preload driver against the real application's Main scene, without a compositor."""
import argparse
from pathlib import Path
import socket
import signal
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/native-replay'))
from fixture import make_fixture
from processes import OwnedResources, interrupt
from desktop import NativeClient, private_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=Path)
    parser.add_argument('--driver', required=True, type=Path)
    args = parser.parse_args()
    with OwnedResources() as owned:
        make_fixture(owned.root / 'fixture')
        parent, child = socket.socketpair()
        client = NativeClient(parent)
        env = dict(private_environment(owned.root / 'home'), QT_QPA_PLATFORM='offscreen',
                   LD_PRELOAD=str(args.driver), BRANCH_TRUCE_REPLAY_FD=str(child.fileno()))
        try:
            process, _ = owned.spawn([str(args.app), '--mergetool-path', 'src/net/retry.cpp'.encode().hex(),
                                      str(owned.root / 'fixture/retry-service')],
                                     env=env, log=owned.root / 'app.log', pass_fds=(child.fileno(),))
            child.close()
            before = client.command(path='src/net/retry.cpp', target='choiceAccept-section/1/392/446')
            assert before['path'] == 'src/net/retry.cpp' and before['items']
            assert not before['pendingInput'] and before['recoveryState'] == 0
            after = client.command('click', target='choiceAccept-section/1/392/446')
            assert after['resultText'] == before['resultText'], 'Accept duplicated an already merged local range'
            assert any(r['side'] == 'stage2' and r['scope'] == 'section' and r['sourceLineBegin'] == 19
                       and r['state'] == 'accepted' for r in after['regions'])
            assert int(after['progress']['undecided']) == int(before['progress']['undecided']) - 1
            assert not after['pendingInput'] and after['recoveryState'] == 0
            client.command('quit')
            assert process.wait_exit(10) == 0
        except BaseException:
            print((owned.root / 'app.log').read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            child.close()
            client.close()
    print('real Main readiness, snapshot and native Accept command contract passed')


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, interrupt)
    main()
