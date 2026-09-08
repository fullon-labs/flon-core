#!/usr/bin/env python3
"""Offline physical checkpoints for funod. No live copy, dirty-bit repair or pruning.

Requires the matching funod-state-inspect and a stopped service (disable restart
policies first). Only the standard DATA/state and DATA/blocks layout is supported.
Recovery is explicit; it never starts a node or enables a producer.
"""
import argparse
from contextlib import ExitStack, contextmanager
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import uuid

FILES = ("state/shared_memory.bin", "state/chain_head.dat", "blocks/reversible/fork_db.dat")
CHUNK = 4 * 1024 * 1024
MARKERS = ("state.restore.pending", "blocks.restore.pending")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def regular(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, f"Not a private regular file: {path}")
    return info


def exists(path):
    # A dangling symlink must not be mistaken for an available destination.
    return os.path.lexists(path)


def read_json(path):
    require(regular(path).st_size <= 16 * 1024 * 1024, "Metadata file too large")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"Duplicate metadata key: {key}")
            result[key] = value
        return result

    return json.loads(path.read_text(), object_pairs_hook=unique)


def validate_layout(root):
    for name in ("state", "blocks", "blocks/reversible"):
        directory = root / name
        require(directory.is_dir() and directory.resolve() == directory,
                f"Unsupported data layout: {directory}")


def fingerprint(path):
    info = regular(path)
    return {"device": info.st_dev, "inode": info.st_ino, "size": info.st_size, "mtime_ns": info.st_mtime_ns}


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def sync_file(fd):
    os.fsync(fd)
    if sys.platform == "darwin":
        fcntl.fcntl(fd, fcntl.F_FULLFSYNC)


def write_json(path, value):
    with path.open("x", encoding="utf-8") as file:
        os.chmod(path, 0o600)
        json.dump(value, file, sort_keys=True, indent=2)
        file.write("\n")
        file.flush()
        sync_file(file.fileno())
    sync_dir(path.parent)


def publish_journal(path, value):
    # A crash while writing a marker must not expose truncated JSON as the
    # only recovery record. The caller holds both stable operation locks.
    temporary = path.with_name(path.name + ".writing-" + uuid.uuid4().hex)
    write_json(temporary, value)
    require(not exists(path), f"Recovery marker already exists: {path}")
    temporary.rename(path)
    sync_dir(path.parent)


@contextmanager
def lock(path):
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def inspect(binary, *args, dirty_ok=False):
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True, timeout=300)
    if dirty_ok and result.returncode == 2:
        return None
    require(result.returncode == 0, f"State inspection refused: {result.stderr.strip()}")
    return json.loads(result.stdout)


def ranges(fd, size):
    """Skip actual holes; fall back to a bounded-memory full scan if unsupported."""
    offset = 0
    while offset < size:
        try:
            start = os.lseek(fd, offset, os.SEEK_DATA)
            end = min(os.lseek(fd, start, os.SEEK_HOLE), size)
        except OSError as error:
            if error.errno == errno.ENXIO:
                return
            if error.errno not in (errno.EINVAL, errno.ENOTSUP):
                raise
            start, end = offset, size
        require(offset <= start < end <= size, "Invalid sparse extent")
        yield start, end
        offset = end


def copy_sparse(source, destination):
    """Digest allocated extents, retain holes, never read a huge hole as data."""
    info = regular(source)
    destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    entries = []
    with source.open("rb", buffering=0) as src, destination.open("xb", buffering=0) as dst:
        os.chmod(destination, 0o600)
        dst.truncate(info.st_size)
        for start, end in ranges(src.fileno(), info.st_size):
            digest = hashlib.sha256()
            offset = start
            while offset < end:
                data = os.pread(src.fileno(), min(CHUNK, end - offset), offset)
                require(data, f"Unexpected EOF: {source}")
                digest.update(data)
                if any(data):
                    space = os.fstatvfs(dst.fileno())
                    require(space.f_bavail * space.f_frsize > len(data) + 64 * 1024 * 1024,
                            f"Checkpoint copy stopped before exhausting disk space: {destination}")
                    dst.seek(offset)
                    view = memoryview(data)
                    while view:
                        count = dst.write(view)
                        require(count > 0, "Short checkpoint write")
                        view = view[count:]
                offset += len(data)
            entries.append({"offset": start, "length": end - start, "sha256": digest.hexdigest()})
        sync_file(dst.fileno())
    after = source.stat()
    require((info.st_size, info.st_mtime_ns, info.st_ino) ==
            (after.st_size, after.st_mtime_ns, after.st_ino), f"Source changed during checkpoint: {source}")
    sync_dir(destination.parent)
    return {"size": info.st_size, "extents": entries}


def verify_file(path, record):
    require(regular(path).st_size == record["size"], f"Checkpoint size mismatch: {path}")
    with path.open("rb", buffering=0) as file:
        previous = 0
        gaps = []
        for entry in record["extents"]:
            start, length = entry["offset"], entry["length"]
            require(type(start) is int and type(length) is int and
                    previous <= start < start + length <= record["size"], "Invalid manifest extent")
            gaps.append((previous, start))
            digest = hashlib.sha256()
            offset = start
            while offset < start + length:
                data = os.pread(file.fileno(), min(CHUNK, start + length - offset), offset)
                require(data, "Unexpected checkpoint EOF")
                digest.update(data)
                offset += len(data)
            require(digest.hexdigest() == entry["sha256"], f"Checkpoint checksum mismatch: {path}")
            previous = start + length
        gaps.append((previous, record["size"]))
        # Do not trust skipped holes in the manifest. A changed byte in any gap
        # must be detected, including on a filesystem with different allocation.
        gap_index = 0
        for start, end in ranges(file.fileno(), record["size"]):
            while gap_index < len(gaps) and gaps[gap_index][1] <= start:
                gap_index += 1
            index = gap_index
            while index < len(gaps) and gaps[index][0] < end:
                offset, stop = max(start, gaps[index][0]), min(end, gaps[index][1])
                while offset < stop:
                    data = os.pread(file.fileno(), min(CHUNK, stop - offset), offset)
                    require(data and not any(data), f"Nonzero data in checkpoint hole: {path}")
                    offset += len(data)
                index += 1


def validate_manifest(manifest):
    require(isinstance(manifest, dict) and type(manifest.get("format")) is int and manifest["format"] == 1 and
            isinstance(manifest.get("files"), dict) and set(manifest["files"]) == set(FILES),
            "Unsupported checkpoint manifest")
    for record in manifest["files"].values():
        require(isinstance(record, dict) and type(record.get("size")) is int and
                0 < record["size"] < 2**63 and isinstance(record.get("extents"), list), "Invalid file record")
        previous = 0
        for entry in record["extents"]:
            require(isinstance(entry, dict) and type(entry.get("offset")) is int and
                    type(entry.get("length")) is int and
                    previous <= entry["offset"] < entry["offset"] + entry["length"] <= record["size"] and
                    isinstance(entry.get("sha256"), str) and re.fullmatch("[0-9a-f]{64}", entry["sha256"]),
                    "Invalid manifest extent")
            previous = entry["offset"] + entry["length"]
    state = manifest.get("state")
    require(isinstance(state, dict), "Missing checkpoint identity")
    for name in ("chain_id", "head_block_id"):
        require(isinstance(state.get(name), str) and re.fullmatch("[0-9a-f]{64}", state[name]), "Invalid chain/block identity")
    require(type(state.get("head_block_num")) is int and 0 < state["head_block_num"] < 2**32 and
            int(state["head_block_id"][:8], 16) == state["head_block_num"], "Invalid checkpoint height")


def verify(checkpoint, inspector):
    checkpoint = checkpoint.resolve(strict=True)
    validate_layout(checkpoint)
    manifest = read_json(checkpoint / "manifest.json")
    validate_manifest(manifest)
    for name in FILES:
        verify_file(checkpoint / name, manifest["files"][name])
    actual = inspect(inspector, "state", checkpoint / "state")
    require(actual == manifest["state"], "Checkpoint state identity mismatch")
    return manifest


def create(data, checkpoint, inspector):
    require(not checkpoint.exists(), "Checkpoint destination already exists; generations are never overwritten")
    identity = inspect(inspector, "state", data / "state")
    allocated = sum(regular(data / name).st_blocks * 512 for name in FILES)
    require(shutil.disk_usage(checkpoint.parent).free > allocated + 64 * 1024 * 1024,
            "Insufficient free space for checkpoint allocated data plus safety margin")
    staging = checkpoint.with_name(checkpoint.name + ".incomplete-" + uuid.uuid4().hex)
    staging.mkdir(mode=0o700)
    # A failed copy is deliberately kept as .incomplete for diagnosis. It has no
    # published manifest and is never selected automatically.
    records = {name: copy_sparse(data / name, staging / name) for name in FILES}
    write_json(staging / "manifest.json", {"format": 1, "state": identity, "files": records})
    verify(staging, inspector)
    for directory in (staging / "blocks/reversible", staging / "blocks", staging / "state", staging):
        sync_dir(directory)
    staging.rename(checkpoint)
    sync_dir(checkpoint.parent)
    return {"created": str(checkpoint), "state": identity}


def restore(data, checkpoint, inspector, apply):
    data = data.resolve(strict=True)
    require(not any(exists(data / name) for name in MARKERS), "Pending recovery exists; use resume")
    manifest = verify(checkpoint, inspector)
    state = manifest["state"]
    # Only dirty existing states may be replaced; missing/clean/incompatible
    # databases require explicit operator diagnosis, not a broader force option.
    require(inspect(inspector, "state", data / "state", dirty_ok=True) is None,
            "Target state is clean; refusing rollback")
    anchor = inspect(inspector, "anchor", data / "blocks", state["chain_id"], state["head_block_id"])
    result = {"checkpoint": str(checkpoint), "state": state, **anchor,
              "blocks_to_replay": anchor["log_head"] - state["head_block_num"], "preview_only": not apply}
    if not apply:
        return result
    # Conservative per-filesystem check; a sparse-aware copy may use less, but
    # we must not consume the node's remaining disk just to attempt recovery.
    needed = {}
    for name in FILES:
        parent = (data / name).parent
        device = parent.stat().st_dev
        total, _ = needed.get(device, (0, parent))
        needed[device] = (total + regular(checkpoint / name).st_blocks * 512, parent)
    for total, parent in needed.values():
        require(shutil.disk_usage(parent).free > total + 64 * 1024 * 1024,
                f"Insufficient free space to stage recovery on {parent}")
    token = uuid.uuid4().hex
    # Stage every replacement on the destination filesystem before any rename.
    changes = []
    for name in FILES:
        target = data / name
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        if target.exists():
            regular(target)
        staged = target.with_name(target.name + ".restore-" + token)
        copy_sparse(checkpoint / name, staged)
        verify_file(staged, manifest["files"][name])
        changes.append({"target": str(target), "staged": str(staged),
                        "backup": str(target.with_name(target.name + ".pre-restore-" + token)),
                        "name": name, "original": fingerprint(target) if exists(target) else None})
    journal = {"format": 2, "token": token, "data_dir": str(data), "checkpoint": str(checkpoint),
               "manifest": manifest, "changes": changes}
    for name in MARKERS:
        publish_journal(data / name, journal)
    return resume(data, inspector, True)


def recovery_entries(data, journal):
    require(isinstance(journal, dict) and type(journal.get("format")) is int and journal["format"] == 2,
            "Unsupported recovery journal; legacy v1 requires manual reconciliation")
    require(journal.get("data_dir") == str(data), "Recovery journal belongs to a different data directory")
    require(isinstance(journal.get("checkpoint"), str), "Missing checkpoint label in recovery journal")
    token = journal.get("token")
    require(isinstance(token, str) and re.fullmatch("[0-9a-f]{32}", token), "Invalid recovery token")
    validate_manifest(journal.get("manifest"))
    changes = journal.get("changes")
    require(isinstance(changes, list) and len(changes) == len(FILES), "Invalid recovery file set")
    entries = []
    for name, change in zip(FILES, changes):
        require(isinstance(change, dict) and change.get("name") == name and "original" in change,
                "Invalid recovery file order or missing original identity")
        target = data / name
        paths = (target, target.with_name(target.name + ".restore-" + token),
                 target.with_name(target.name + ".pre-restore-" + token))
        # Never act on arbitrary paths from a journal, even a syntactically
        # valid one. All destinations are derived from the fixed whitelist.
        require(all(change.get(key) == str(path) for key, path in zip(("target", "staged", "backup"), paths)),
                "Recovery journal path does not match its token and data directory")
        original = change.get("original")
        require(original is None or (isinstance(original, dict) and
                set(original) == {"device", "inode", "size", "mtime_ns"} and
                all(type(value) is int and value >= 0 for value in original.values())), "Invalid original file identity")
        entries.append((name, *paths, original))
    return entries


def resume(data, inspector, apply):
    data = data.resolve(strict=True)
    validate_layout(data)
    journals = [read_json(data / name) for name in MARKERS if exists(data / name)]
    require(journals, "No pending recovery to resume")
    journal = journals[0]
    require(all(item == journal for item in journals), "Recovery journals disagree; refusing to guess")
    entries = recovery_entries(data, journal)
    manifest, phases = journal["manifest"], []
    # Validate ALL files before making even the first rename. Presence plus
    # original inode identity disambiguates every interrupted swap phase.
    for name, target, staged, backup, original in entries:
        have_target, have_staged, have_backup = map(exists, (target, staged, backup))
        if have_staged:
            verify_file(staged, manifest["files"][name])
            if original is None:
                require(not have_target and not have_backup, "Unexpected file at originally absent target")
                phase = "ready"
            elif have_target and not have_backup:
                require(fingerprint(target) == original, "Original target changed; refusing recovery")
                phase = "ready"
            else:
                require(not have_target and have_backup and fingerprint(backup) == original,
                        "Ambiguous or changed backup during recovery")
                phase = "original_saved"
        else:
            require(have_target, "Both staged and installed checkpoint file are missing")
            verify_file(target, manifest["files"][name])
            require((original is None and not have_backup) or
                    (original is not None and have_backup and fingerprint(backup) == original),
                    "Installed checkpoint has no matching original backup")
            phase = "installed"
        phases.append(phase)
    state = manifest["state"]
    anchor = inspect(inspector, "anchor", data / "blocks", state["chain_id"], state["head_block_id"])
    result = {"checkpoint": journal["checkpoint"], "state": state, **anchor,
              "blocks_to_replay": anchor["log_head"] - state["head_block_num"], "preview_only": not apply,
              "phases": dict(zip(FILES, phases)),
              "backups": [str(entry[3]) for entry in entries if entry[4] is not None]}
    if not apply:
        return result
    # One marker can be absent after publication or completion was interrupted.
    # Re-establish both durable barriers before proceeding; never rewrite one.
    for name in MARKERS:
        if not exists(data / name):
            publish_journal(data / name, journal)
    for (_, target, staged, backup, original), phase in zip(entries, phases):
        if phase == "ready" and original is not None:
            target.rename(backup)
            sync_dir(target.parent)
        if phase != "installed":
            staged.rename(target)
        # Also sync already-installed files/directories: the previous invocation
        # may have been interrupted just after its rename, before directory sync.
        with target.open("rb") as file:
            sync_file(file.fileno())
        sync_dir(target.parent)
    require(inspect(inspector, "state", data / "state") == state, "Restored identity mismatch; journal retained")
    for name in MARKERS:
        (data / name).unlink()
        sync_dir(data)
    result.update(restored=True, preview_only=False)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify", "restore", "resume"))
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--inspector", type=Path, default=Path(shutil.which("funod-state-inspect") or "funod-state-inspect"))
    parser.add_argument("--apply", action="store_true", help="Apply restore/resume; otherwise only preview the recovery plan")
    args = parser.parse_args()
    require((args.action == "resume") == (args.checkpoint is None),
            "--checkpoint is required except for resume, which uses the pending journal")
    checkpoint = args.checkpoint.resolve() if args.checkpoint else None
    inspector = args.inspector.resolve()
    require(args.action in ("restore", "resume") or not args.apply, "--apply is only valid for restore/resume")
    if args.action == "verify":
        print(json.dumps(verify(checkpoint, inspector), indent=2))
        return
    require(args.data_dir is not None, "--data-dir is required")
    data = args.data_dir.resolve(strict=True)
    require(data != Path(data.anchor), "A filesystem root cannot be a data directory")
    if checkpoint is not None:
        require(not checkpoint.is_relative_to(data) and not data.is_relative_to(checkpoint),
                "Checkpoint must be outside the data directory")
        require(checkpoint.parent.is_dir(), "Create the checkpoint parent directory first")
    # Reject custom/symlinked subdirectories rather than accidentally using a
    # different lock name from the node or mixing distinct storage layouts.
    validate_layout(data)
    with ExitStack() as stack:
        for name in ("state", "blocks"):
            stack.enter_context(lock(data / (name + ".operation.lock")))
            require(args.action == "resume" or not exists(data / (name + ".restore.pending")),
                    "Interrupted restore journal found; use resume to preview safe continuation")
        # Reject a legacy process already holding chainbase's inode lock, too.
        # Stable operation locks are the lifetime guarantee; only matching new
        # nodes understand them. Older services MUST remain disabled throughout.
        if args.action != "resume" or exists(data / "state/shared_memory.bin"):
            regular(data / "state/shared_memory.bin")
            stack.enter_context(lock(data / "state/shared_memory.bin"))
        if args.action == "resume":
            result = resume(data, inspector, args.apply)
        else:
            result = (create(data, checkpoint, inspector) if args.action == "create" else
                      restore(data, checkpoint, inspector, args.apply))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError, TypeError, RecursionError, subprocess.SubprocessError) as error:
        print(f"Checkpoint operation refused: {error}", file=sys.stderr)
        sys.exit(1)
