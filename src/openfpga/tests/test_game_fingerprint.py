"""Local assets and container builds must not inject a private Memory identity."""
from pathlib import Path
import os
import subprocess
import tempfile

port = Path(__file__).resolve().parents[1]
sdk = os.environ.get('SDK_ROOT', str(port / 'out/savestate-sdk-strg'))
source = (port / 'Makefile').read_text()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'Makefile').write_text(source + '\n.PHONY: identity-fixture\nidentity-fixture:\n\t@echo $(BS_DEFS)\n')

    def flags(game, *args):
        return subprocess.check_output(['make', '--no-print-directory', 'identity-fixture',
            f'SDK_ROOT={sdk}', 'SAVESTATES=1', f'GAME={game}', *args], cwd=root, text=True)

    for game in ('undertale', 'deltarune'):
        public = flags(game)
        data = root / 'games' / game / 'data.win'
        data.parent.mkdir(parents=True)
        data.write_bytes(b'private fixture game data')
        assert flags(game) == public, 'Local game data changed the build identity'
        assert flags(game, 'STATE_GAME_ID=0123456789abcdef') == public
        assert flags(game, 'STATE_GAME_ID=') == public

    # Native builds must work without host game files, just like the public
    # pipeline. An OCI stub captures the real wrapper's invocation boundary.
    wrapper_port = root / 'repo/src/openfpga'
    (wrapper_port / 'tools').mkdir(parents=True)
    wrapper = wrapper_port / 'tools/native-sdk-container.sh'
    wrapper.write_text((port / 'tools/native-sdk-container.sh').read_text())
    stub_sdk = root / 'stub-sdk'
    (stub_sdk / 'tools').mkdir(parents=True)
    (stub_sdk / 'tools/oci.sh').write_text('oci_run() { printf "%s\\n" "$@"; }\n')
    forwarded = subprocess.check_output(['bash', str(wrapper), 'make', 'GAME=deltarune'],
        cwd=wrapper_port, env={**os.environ, 'SDK_ROOT': str(stub_sdk),
                              'OF_STATE_DATA': str(root / 'absent-game-data')}, text=True)
    assert 'GAME=deltarune' in forwarded
    assert 'STATE_GAME_ID=' not in forwarded
print('PASS: public/local build flags match and native wrapper needs no game fingerprint')
