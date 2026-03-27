from __future__ import annotations

import logging
from pathlib import Path

from grpc_tools import protoc

LOGGER = logging.getLogger("proto_codegen")


def project_root() -> Path:
    current = Path(__file__).resolve().parent
    candidates = [
        current / "specs" / "connect2",
        current.parent / "specs" / "connect2",
        current.parent.parent / "specs" / "connect2",
    ]
    for candidate in candidates:
        if (candidate / "command.proto").exists() and (candidate / "event.proto").exists():
            return candidate.parent.parent
    raise FileNotFoundError("unable to locate specs/connect2 with command.proto and event.proto")


def sandbox_root() -> Path:
    return Path(__file__).resolve().parent


def output_dir() -> Path:
    return sandbox_root() / "generated_proto"


def _proto_files() -> list[Path]:
    spec_dir = project_root() / "specs" / "connect2"
    # Keep deterministic order for reproducible generation.
    return sorted(spec_dir.glob("*.proto"))


def _is_stale(proto_file: Path, generated_file: Path) -> bool:
    if not generated_file.exists():
        return True
    return proto_file.stat().st_mtime > generated_file.stat().st_mtime


def ensure_generated() -> None:
    spec_dir = project_root() / "specs" / "connect2"
    out_dir = output_dir()
    out_dir.mkdir(parents=True, exist_ok=True)

    proto_files = _proto_files()
    stale = False
    for proto_file in proto_files:
        generated_file = out_dir / f"{proto_file.stem}_pb2.py"
        if _is_stale(proto_file, generated_file):
            stale = True
            break

    if not stale:
        return

    for proto_file in proto_files:
        LOGGER.info("generating python protobuf module for %s", proto_file.name)
        rc = protoc.main(
            [
                "grpc_tools.protoc",
                f"-I{spec_dir}",
                f"--python_out={out_dir}",
                str(proto_file),
            ]
        )
        if rc != 0:
            raise RuntimeError(f"protoc failed for {proto_file}")


def main() -> int:
    logging.basicConfig(level=logging.INFO)
    ensure_generated()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
