#!/usr/bin/env python3
"""Build and run the AI GoogleTests against an already-built Linux engine.

Build the official ``oceanbase`` CMake target first. Compiler settings come
from that build's compile_commands.json; no engine implementation is mocked.
"""

import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    source = Path(__file__).resolve().with_name("test_ai_function_contracts.cpp")
    library = build / "src/observer/liboceanbase.so"
    if not library.is_file():
        parser.error("Build the oceanbase target before running these tests")
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = None
    for candidate in commands:
        path = Path(candidate["file"])
        if path.name == "ob_ai_func_utils.cpp" or (
                "Unity" in path.parts and path.is_file()
                and "ob_ai_func_utils.cpp" in path.read_text()):
            entry = candidate
            break
    if entry is None:
        parser.error("No AI implementation entry found in compile_commands.json")
    command = entry.get("arguments") or shlex.split(entry["command"])
    compiler = Path(command[0])
    root = next(parent for parent in compiler.parents if parent.name == "3rd")
    sdk = root / "usr/local/oceanbase/deps/devel"
    gtest = next((sdk / "lib").glob("libgtest.a"), None)
    if gtest is None:
        gtest = next((sdk / "lib64").glob("libgtest.a"), None)
    if gtest is None or not gtest.with_name("libgtest_main.a").is_file():
        parser.error("GoogleTest libraries are missing from the pinned SDK")
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "test_ai_function_contracts"
    flags = []
    index = 1
    while index < len(command):
        argument = command[index]
        if argument in ("-o", "-MF", "-MT", "-MQ"):
            index += 2
            continue
        if argument not in ("-c", "-MD", "-MMD", "-MP", entry["file"]):
            flags.append(argument)
        index += 1
    link = [str(compiler), *flags, str(source), "-o", str(binary),
            "-I" + str(sdk / "include"),
            "--ld-path=" + str(compiler.with_name("ld.lld")),
            "-L" + str(library.parent), "-Wl,-rpath," + str(library.parent),
            "-loceanbase", str(gtest.with_name("libgtest_main.a")), str(gtest),
            "-pthread", "-ldl", "-static-libstdc++", "-static-libgcc"]
    with (output / "compile.log").open("w") as log:
        subprocess.run(link, cwd=entry["directory"], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    with (output / "test.log").open("w") as log:
        result = subprocess.run([str(binary), "--gtest_output=xml:" + str(output / "results.xml")],
                                cwd=output, stdout=log, stderr=subprocess.STDOUT)
    print((output / "test.log").read_text())
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
