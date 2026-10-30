#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check a moved macOS installation's actual library loading without USB."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile


binary = pathlib.Path(sys.argv[1]).resolve()
script = pathlib.Path(__file__).resolve().parents[1] / "scripts/bundle_macos_install.py"
with tempfile.TemporaryDirectory(prefix="openfpga macOS bundle ") as temporary:
    root = pathlib.Path(temporary)
    prefix = root / "installed"
    command = [sys.executable, str(script), "--binary", str(binary), "--prefix", str(prefix)]
    # Reinstalling must start from the original executable, not a rewritten copy.
    for _ in range(2):
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
    moved = root / "Moved Installation With Spaces"
    prefix.rename(moved)
    executable = moved / "bin/openFPGALoader"
    libraries = list((moved / "lib/openFPGALoader").iterdir())
    for path in [executable, *libraries]:
        subprocess.run(["codesign", "--verify", "--strict", str(path)], check=True,
                       capture_output=True, timeout=10)
    manifest = json.loads((moved / "share/licenses/openFPGALoader/macos-libraries/manifest.json").read_text())
    assert len(manifest) == len(libraries)
    config = root / "empty-config"
    config.mkdir()
    env = dict(os.environ, PATH="/usr/bin:/bin", XDG_CONFIG_HOME=str(config),
               DYLD_PRINT_LIBRARIES="1")
    result = subprocess.run([str(executable), "--version"], env=env, cwd=root,
                            capture_output=True, text=True, timeout=10)
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert "openFPGALoader" in output
    for external in ("/opt/homebrew/", "/usr/local/Cellar/", str(binary.parent)):
        assert external not in output, output
    print("Moved installation: signatures and actual library loading pass")
