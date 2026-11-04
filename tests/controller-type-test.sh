#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
python3 -B - "${repo}" <<'PY'
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import sys
import tempfile

root = Path(sys.argv[1])
loader = importlib.machinery.SourceFileLoader(
    'controller_type', str(root / 'system_files/usr/libexec/armada/controller-type'))
spec = importlib.util.spec_from_loader(loader.name, loader)
module = importlib.util.module_from_spec(spec)
loader.exec_module(module)
module.DEVICE_ENV = str(root / 'system_files/usr/libexec/armada/device-env')
os.environ['ARMADA_DEVICE_DIR'] = str(root / 'system_files/usr/lib/armada/devices')
os.environ['ARMADA_MEM_SLEEP_PATH'] = '/dev/null'
os.environ['ARMADA_SLEEP_CONFIG'] = '/dev/null'

attempts = []
module.time.sleep = lambda _: None
module.apply_type = lambda value: attempts.append(value) or False
with tempfile.TemporaryDirectory() as temporary:
    module.CONFIG = Path(temporary) / 'controller.conf'
    for model in ('OnePlus Pad 2 Pro',
                  'Qualcomm Technologies, Inc. Sun MTP,erhai-O DVT'):
        os.environ['ARMADA_MODEL'] = model
        assert module.device_environment()['ARMADA_HAS_BUILTIN_GAMEPAD'] == '0'
        assert module.main(['controller-type', 'apply']) == 0
        assert not attempts, 'tablet attempted to configure a nonexistent gamepad'
        assert not module.CONFIG.exists(), 'automatic skip wrote a controller preference'

    # The handheld must still report a missing controller as a real failure.
    os.environ['ARMADA_MODEL'] = 'AYN Odin 3'
    assert module.device_environment()['ARMADA_HAS_BUILTIN_GAMEPAD'] == '1'
    assert module.main(['controller-type', 'apply']) == 1
    assert len(attempts) == 30
    attempts.clear()
    module.apply_type = lambda value: attempts.append(value) or True
    assert module.main(['controller-type', 'apply']) == 0
    assert attempts == ['deck-uhid']

    # An explicit request can still configure an external controller on a tablet.
    os.environ['ARMADA_MODEL'] = 'OnePlus Pad 2 Pro'
    attempts.clear()
    assert module.main(['controller-type', 'set', 'xbox-series']) == 0
    assert attempts == ['xbox-series']
    assert module.CONFIG.read_text() == 'controller_type=xbox-series\n'
    assert module.main(['controller-type', 'get']) == 0
    assert module.main(['controller-type', 'set', 'invalid']) == 2

    # Older profiles and failed environment probes retain the previous behavior.
    module.device_environment = lambda: {}
    attempts.clear()
    assert module.main(['controller-type', 'apply']) == 0
    assert attempts == ['xbox-series']
print('Controller type: tablet skip, handheld success/failure and explicit set passed')
PY
