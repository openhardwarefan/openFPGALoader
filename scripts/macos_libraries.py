# SPDX-License-Identifier: Apache-2.0
"""Copy and relocate a macOS executable's non-system library dependencies."""
import hashlib
import os
import pathlib
import re
import shutil
import subprocess


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def identity(path):
    return {"bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def dependencies(path):
    ids = run("otool", "-D", str(path)).splitlines()[1:]
    own = ids[0].strip() if ids else None
    return [line.strip().split(" (", 1)[0]
            for line in run("otool", "-L", str(path)).splitlines()[1:]
            if line.strip().split(" (", 1)[0] != own]


def system_library(name):
    return name.startswith(("/usr/lib/", "/System/Library/"))


def bundle_libraries(binary, executable, libraries, library_dirs=()):
    binary = binary.resolve(strict=True)
    executable = executable.resolve(strict=True)
    libraries = libraries.absolute()
    libraries.mkdir(parents=True, exist_ok=True)
    libraries = libraries.resolve(strict=True)
    executable_prefix = "@executable_path/" + os.path.relpath(libraries, executable.parent) + "/"
    mapped, names, graph = {}, {}, {}

    def resolve(name, loader):
        if name.startswith("@loader_path/"):
            candidate = loader.parent / name[len("@loader_path/"):]
        elif name.startswith("@executable_path/"):
            candidate = binary.parent / name[len("@executable_path/"):]
        elif name.startswith("@rpath/"):
            directories = list(library_dirs)
            for origin in (loader, binary):
                paths = re.findall(r"^\s*path (.+) \(offset \d+\)$",
                                   run("otool", "-l", str(origin)), re.MULTILINE)
                for path in paths:
                    path = path.replace("@loader_path", str(origin.parent))
                    path = path.replace("@executable_path", str(binary.parent))
                    if not path.startswith("@"):
                        directories.append(pathlib.Path(path))
            matches = [p / name[len("@rpath/"):] for p in directories]
            matches = [p for p in matches if p.is_file()]
            if len({p.resolve() for p in matches}) != 1:
                raise ValueError("cannot uniquely resolve library: " + name)
            candidate = matches[0]
        else:
            candidate = pathlib.Path(name)
        return candidate.resolve(strict=True)

    def collect(source):
        refs = dependencies(source)
        graph[source] = {}
        for name in refs:
            if system_library(name):
                continue
            dependency = resolve(name, source)
            filename = pathlib.Path(name).name
            if filename in names and names[filename] != dependency:
                raise ValueError("colliding library filename: " + filename)
            names[filename] = dependency
            if dependency not in mapped:
                mapped[dependency] = libraries / filename
                shutil.copy2(dependency, mapped[dependency])
                mapped[dependency].chmod(0o755)
                collect(dependency)
            graph[source][name] = mapped[dependency].name

    collect(binary)
    for source, refs in graph.items():
        destination = executable if source == binary else mapped[source]
        for old, filename in refs.items():
            prefix = executable_prefix if source == binary else "@loader_path/"
            subprocess.run(["install_name_tool", "-change", old, prefix + filename,
                            str(destination)], check=True, capture_output=True)
        if source != binary:
            subprocess.run(["install_name_tool", "-id", "@rpath/" + destination.name,
                            str(destination)], check=True, capture_output=True)
        # All non-system references are now direct relative paths.
        while True:
            paths = re.findall(r"^\s*path (.+) \(offset \d+\)$",
                               run("otool", "-l", str(destination)), re.MULTILINE)
            if not paths:
                break
            subprocess.run(["install_name_tool", "-delete_rpath", paths[0],
                            str(destination)], check=True, capture_output=True)

    architectures = set(run("lipo", "-archs", str(executable)).split())
    for path in [*mapped.values(), executable]:
        if not architectures.issubset(set(run("lipo", "-archs", str(path)).split())):
            raise ValueError("library architecture mismatch: " + path.name)
        allowed = executable_prefix if path == executable else "@loader_path/"
        if any(not system_library(ref) and not ref.startswith(allowed)
               for ref in dependencies(path)):
            raise ValueError("unresolved external dependency: " + path.name)
        subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none",
                        str(path)], check=True, capture_output=True)
        subprocess.run(["codesign", "--verify", "--strict", str(path)],
                       check=True, capture_output=True)
    return mapped
