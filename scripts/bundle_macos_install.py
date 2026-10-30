#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Make a local macOS installation independent of Homebrew at runtime."""
import argparse
import json
import pathlib
import shutil
import sys

from macos_libraries import bundle_libraries, identity


def install(prefix, binary):
    if sys.platform != "darwin":
        raise ValueError("macOS is required for library relocation and signing")
    prefix = prefix.absolute()
    executable = prefix / "bin/openFPGALoader"
    executable.parent.mkdir(parents=True, exist_ok=True)
    # Always start from the unmodified build, including on repeated installs.
    # CMake may otherwise consider the previously signed copy up to date.
    shutil.copy2(binary, executable)
    executable.chmod(0o755)
    libraries = prefix / "lib/openFPGALoader"
    mapped = bundle_libraries(executable, executable, libraries)
    notices = prefix / "share/licenses/openFPGALoader/macos-libraries"
    notices.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for source, destination in mapped.items():
        # Homebrew installs its license texts and build recipe with each keg.
        roots = [root for root in source.parents if (root / ".brew").is_dir()]
        root = roots[0] if roots else source.parent.parent
        documents = [path for path in root.iterdir() if path.is_file() and
                     path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE"))]
        if (root / ".brew").is_dir():
            documents.extend((root / ".brew").glob("*.rb"))
        for document in documents:
            output = notices / destination.name / document.relative_to(root)
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(document, output)
        manifest[destination.name] = {
            "original": identity(source), "bundled": identity(destination),
            "notices": [str(path.relative_to(root)) for path in documents],
        }
    (notices / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Bundled %d macOS libraries in %s" % (len(mapped), libraries))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=pathlib.Path, required=True)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    args = parser.parse_args()
    install(args.prefix, args.binary)


if __name__ == "__main__":
    main()
