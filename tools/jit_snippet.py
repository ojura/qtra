#!/usr/bin/env python3
"""Compile a project-native C++ snippet, load it, run it, and await completion."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from agentctl import AgentClient, ProtocolError, default_socket_path, parse_json
from compile_snippet import (
    BuildOracleError,
    command_record,
    derive_shared_object_command,
    load_database,
    select_entry,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--compile-db", required=True, type=Path)
    parser.add_argument("--context", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--socket", default=default_socket_path())
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--optimization", default="Og", choices=("O0", "Og", "O1", "O2", "O3", "Os", "Oz"))
    parser.add_argument("--executor", default="gui", choices=("gui", "object", "render"))
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--target-name")
    target.add_argument("--target-id")
    parser.add_argument("--request")
    parser.add_argument("--request-file")
    parser.add_argument("--respect-access-control", action="store_true")
    parser.add_argument("--extra", action="append", default=[])
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--compact", action="store_true")
    return parser


def default_output(source: Path, compile_db: Path) -> Path:
    digest = hashlib.sha256(
        source.read_bytes() + str(time.time_ns()).encode("ascii")
    ).hexdigest()[:12]
    directory = compile_db.resolve().parent / "runtime-snippets"
    return directory / f"{source.stem}-{digest}.so"


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        source = args.source.resolve()
        if not source.is_file():
            raise BuildOracleError(f"source is not a regular file: {source}")
        compile_db = args.compile_db.resolve()
        output = args.output.resolve() if args.output else default_output(source, compile_db)
        output.parent.mkdir(parents=True, exist_ok=True)
        entry = select_entry(load_database(compile_db), args.context)
        command = derive_shared_object_command(
            entry,
            source,
            output,
            optimization=args.optimization,
            disable_access_control=not args.respect_access_control,
            extra=args.extra,
        )
        build = command_record(entry, command, output)
        completed = subprocess.run(command, cwd=entry.directory, check=False)
        if completed.returncode != 0:
            raise BuildOracleError(f"compiler exited with status {completed.returncode}")
        if not output.is_file():
            raise BuildOracleError(f"compiler succeeded but did not create {output}")

        result: dict[str, Any] = {"compile": build}
        if not args.compile_only:
            request = parse_json(args.request, args.request_file)
            with AgentClient(args.socket, args.timeout) as client:
                client.subscribe(["operation."])
                loaded = client.request("snippet.load", {"path": str(output)})
                params: dict[str, Any] = {
                    "moduleId": loaded["moduleId"],
                    "executor": args.executor,
                    "request": request,
                }
                if args.executor == "object":
                    if args.target_name:
                        params["target"] = {"objectName": args.target_name}
                    elif args.target_id:
                        params["target"] = {"id": args.target_id}
                    else:
                        raise ValueError("object executor requires --target-name or --target-id")
                started = client.request("snippet.run", params)
                finished = client.wait_for_operation(
                    started["operationId"], timeout=args.timeout
                )
                result["runtime"] = {
                    "loaded": loaded,
                    "started": started,
                    "finished": finished,
                }

        print(
            json.dumps(
                result,
                indent=None if args.compact else 2,
                separators=(",", ":") if args.compact else None,
                sort_keys=not args.compact,
            )
        )
        return 0
    except (BuildOracleError, ConnectionError, OSError, ProtocolError, TimeoutError, ValueError) as exc:
        print(f"jit_snippet: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
