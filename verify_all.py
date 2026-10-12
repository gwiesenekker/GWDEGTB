#!/usr/bin/env python3
"""Resume move-based verification of the eight-piece DTM families in build order."""

import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


FAMILIES = ("1x1", "2x1", "2x2", "3x1", "3x2", "4x1", "3x3",
            "4x2", "5x1", "4x3", "5x2", "6x1", "4x4", "5x3", "6x2")
DEFAULT_FAMILIES = ("4x4", "5x3", "6x2")
GENERATE = re.compile(r"^generate\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$")
THROUGH = re.compile(r"verified through index (\d+)/(\d+)")
COMPLETE = re.compile(r"VERIFIED \S+: positions=(\d+) ")
ALREADY_COMPLETE = re.compile(r"checkpoint covers all (\d+) DTM values")


def family_materials(source: Path, families: list[str]) -> list[tuple[str, tuple[int, ...]]]:
    materials = []
    seen = set()
    for family in families:
        script = source / f"{family}.sh"
        found = 0
        for line in script.read_text(encoding="utf-8").splitlines():
            match = GENERATE.fullmatch(line)
            if match is None:
                continue
            counts = tuple(map(int, match.groups()))
            name = f"{counts[0]}wX-{counts[1]}wO-{counts[2]}bX-{counts[3]}bO"
            if name in seen:
                raise ValueError(f"duplicate material: {name}")
            seen.add(name)
            materials.append((name, counts))
            found += 1
        if found == 0:
            raise ValueError(f"no active generate commands in {script}")
    return materials


def input_fingerprint(directory: Path) -> str:
    """Detect collection changes during a campaign; verify_dtm validates checkpoints."""
    digest = hashlib.sha256()
    for path in sorted(directory.glob("*.dtm")):
        stat = path.stat()
        digest.update(
            f"{path.name}\t{stat.st_dev}\t{stat.st_ino}\t{stat.st_size}"
            f"\t{stat.st_mtime_ns}\t{stat.st_ctime_ns}\n".encode("utf-8")
        )
    return digest.hexdigest()


def write_report(path: Path, signature: str, rows: list[dict[str, str]]) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8", newline="") as output:
        output.write(f"# input-metadata-sha256={signature}\n")
        output.write("material\tstatus\tchecked_positions\ttotal_positions\tlog\n")
        for row in rows:
            output.write("\t".join(row[field] for field in
                                   ("material", "status", "checked", "total", "log")) + "\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    directory_fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def last_progress(log: Path) -> tuple[str, str]:
    text = log.read_text(encoding="utf-8", errors="replace")
    complete = COMPLETE.search(text)
    if complete:
        return complete.group(1), complete.group(1)
    already = ALREADY_COMPLETE.search(text)
    if already:
        total = str(int(already.group(1)) // 2)
        return total, total
    matches = list(THROUGH.finditer(text))
    if matches:
        return matches[-1].groups()
    return "0", "0"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True,
                        help="stable directory containing the target and lower-material DTMs")
    parser.add_argument("--state-directory", type=Path, required=True,
                        help="directory for checkpoints, logs and summary.tsv")
    parser.add_argument("--families", nargs="+", choices=FAMILIES,
                        default=list(DEFAULT_FAMILIES))
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--chunk-pages", type=int, default=1048576)
    parser.add_argument("--max-chunks", type=int,
                        help="pause after this many chunks of the next incomplete DTM")
    parser.add_argument("--limit", type=int, help="select only the first N materials (testing)")
    parser.add_argument("--verifier", type=Path,
                        default=Path(__file__).resolve().parent / "verify_dtm")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.threads < 1 or args.chunk_pages < 1 or (args.max_chunks is not None and
                                                     args.max_chunks < 1) or (
            args.limit is not None and args.limit < 1):
        parser.error("threads, chunk-pages, max-chunks and limit must be positive")
    source = Path(__file__).resolve().parent
    try:
        selected_families = [family for family in FAMILIES if family in args.families]
        materials = family_materials(source, selected_families)
        if args.limit is not None:
            materials = materials[:args.limit]
        directory = args.directory.resolve(strict=True)
        if not directory.is_dir():
            raise ValueError(f"not a directory: {directory}")
        if args.dry_run:
            for name, _ in materials:
                print(name)
            return 0
        missing = [name for name, _ in materials if not (directory / f"{name}.dtm").is_file()]
        if missing:
            raise ValueError("missing requested DTMs: " + ", ".join(missing))
        verifier = args.verifier.resolve(strict=True)
        if not verifier.is_file() or not os.access(verifier, os.X_OK):
            raise ValueError(f"verifier is not executable: {verifier}")
        state = args.state_directory.resolve()
        if state == directory:
            raise ValueError("state directory must be separate from the DTM directory")
        state.mkdir(parents=True, exist_ok=True)
        lock = (state / "verify_all.lock").open("a+")
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError(f"another verification driver is using {state}")
        signature = input_fingerprint(directory)
        rows = [dict(material=name, status="NOT_CHECKED", checked="0", total="0", log="")
                for name, _ in materials]
        report = state / "summary.tsv"
        write_report(report, signature, rows)
        for row, (name, counts) in zip(rows, materials):
            checkpoint = state / f"{name}.verify"
            stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
            log_fd, log_name = tempfile.mkstemp(prefix=f"{name}-{stamp}-",
                                                 suffix=".log", dir=state)
            log = Path(log_name)
            row["log"] = str(log)
            row["status"] = "NOT_VERIFIED"
            write_report(report, signature, rows)
            command = [str(verifier), "-d", str(directory), "-j", str(args.threads),
                       "--checkpoint", str(checkpoint), "--chunk-pages",
                       str(args.chunk_pages)]
            if args.max_chunks is not None:
                command += ["--max-chunks", str(args.max_chunks)]
            command += list(map(str, counts))
            print(f"Verifying {name} (log: {log})", flush=True)
            try:
                with os.fdopen(log_fd, "w", encoding="utf-8") as output:
                    result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                            check=False)
            except KeyboardInterrupt:
                row["status"] = "INTERRUPTED"
                write_report(report, signature, rows)
                print(f"Interrupted {name}; rerun to resume from its last checkpoint",
                      file=sys.stderr)
                return 130
            row["checked"], row["total"] = last_progress(log)
            if input_fingerprint(directory) != signature:
                row["status"] = "INPUTS_CHANGED"
                write_report(report, signature, rows)
                print(f"DTM collection changed during verification of {name}", file=sys.stderr)
                return 1
            if result.returncode == 0:
                if row["total"] == "0" or row["checked"] != row["total"]:
                    row["status"] = "FAILED"
                    write_report(report, signature, rows)
                    print(f"verifier exited successfully without a complete result for {name}",
                          file=sys.stderr)
                    return 1
                row["status"] = "VERIFIED"
            elif result.returncode == 2:
                row["status"] = "PARTIAL"
            elif result.returncode in (-2, 130):
                row["status"] = "INTERRUPTED"
            else:
                row["status"] = "FAILED"
            write_report(report, signature, rows)
            print(f"{row['status']} {name}: {row['checked']}/{row['total']} positions", flush=True)
            if result.returncode != 0:
                if result.returncode == 2:
                    return 2
                return 130 if row["status"] == "INTERRUPTED" else 1
        print(f"All {len(materials)} requested DTMs verified; report: {report}")
        return 0
    except (OSError, ValueError) as error:
        print(f"verify_all: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
