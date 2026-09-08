#!/usr/bin/env python3
"""Bounded filesystem tests, including sparse-hole tampering and failed swaps."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("checkpoint", Path(__file__).resolve().parents[1] / "scripts/state_checkpoint.py")
checkpoint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checkpoint)

IDENTITY = {"chain_id": "a" * 64, "head_block_id": "0000000a" + "b" * 56, "head_block_num": 10}


def inspector_stub(binary, action, *args, **kwargs):
    if action == "anchor":
        return {"log_head": 20}
    return None if kwargs.get("dirty_ok") else IDENTITY


def recovery_fixture(root, absent=()):
    data, saved = root / "data", root / "saved"
    records = {}
    for name in checkpoint.FILES:
        target = data / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"new " + name.encode())
        records[name] = checkpoint.copy_sparse(target, saved / name)
        if name in absent:
            target.unlink()
        else:
            target.write_bytes(b"old " + name.encode())
    manifest = {"format": 1, "state": IDENTITY, "files": records}
    checkpoint.write_json(saved / "manifest.json", manifest)
    return data, saved


class CheckpointTests(unittest.TestCase):
    def test_sparse_copy_and_hole_tampering(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, copied = root / "source", root / "copied"
            with source.open("wb") as file:
                file.write(b"state")
                file.seek(16 * 1024 * 1024)
                file.write(b"tail")
                file.truncate(32 * 1024 * 1024)
            record = checkpoint.copy_sparse(source, copied)
            checkpoint.verify_file(copied, record)
            self.assertEqual(source.read_bytes(), copied.read_bytes())
            with copied.open("r+b") as file:
                file.seek(8 * 1024 * 1024)
                file.write(b"corruption in a hole")
            with self.assertRaises(RuntimeError):
                checkpoint.verify_file(copied, record)

    def test_checksum_and_symlink_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, copied = root / "source", root / "copy"
            source.write_bytes(b"abc")
            record = checkpoint.copy_sparse(source, copied)
            copied.write_bytes(b"abd")
            with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                checkpoint.verify_file(copied, record)
            link = root / "link"
            link.symlink_to(source)
            with self.assertRaises(RuntimeError):
                checkpoint.copy_sparse(link, root / "unsafe")

    def test_failed_swap_keeps_journals_and_backup(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data, saved = root / "data", root / "saved"
            records = {}
            for name in checkpoint.FILES:
                target = data / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"old state")
                records[name] = checkpoint.copy_sparse(target, saved / name)
            identity = IDENTITY
            manifest = {"format": 1, "state": identity, "files": records}
            original_rename = Path.rename

            def fail_install(path, target):
                if ".restore-" in path.name:
                    raise OSError("injected disk failure")
                return original_rename(path, target)

            with patch.object(checkpoint, "verify", return_value=manifest), \
                 patch.object(checkpoint, "inspect", side_effect=inspector_stub), \
                 patch.object(Path, "rename", fail_install):
                with self.assertRaisesRegex(OSError, "injected disk failure"):
                    checkpoint.restore(data, saved, Path("unused"), True)
            journal = json.loads((data / "state.restore.pending").read_text())
            self.assertTrue((data / "blocks.restore.pending").exists())
            self.assertEqual(Path(journal["changes"][0]["backup"]).read_bytes(), b"old state")
            self.assertTrue(Path(journal["changes"][0]["staged"]).exists())

    def test_resume_after_each_swap_and_marker_cut(self):
        # Two marker publications + six file renames + two marker removals.
        # Fail immediately AFTER each mutation, including before its fsync.
        for cut in range(1, 11):
            with self.subTest(cut=cut), tempfile.TemporaryDirectory() as folder:
                data, saved = recovery_fixture(Path(folder))
                count = 0
                original_rename, original_unlink = Path.rename, Path.unlink

                def after_change():
                    nonlocal count
                    count += 1
                    if count == cut:
                        raise OSError("simulated interruption")

                def rename(path, target):
                    result = original_rename(path, target)
                    after_change()
                    return result

                def unlink(path, *args, **kwargs):
                    result = original_unlink(path, *args, **kwargs)
                    if path.name in checkpoint.MARKERS:
                        after_change()
                    return result

                with patch.object(checkpoint, "inspect", side_effect=inspector_stub):
                    with patch.object(Path, "rename", rename), patch.object(Path, "unlink", unlink):
                        with self.assertRaisesRegex(OSError, "simulated interruption"):
                            checkpoint.restore(data, saved, Path("unused"), True)
                    if cut == 10:
                        # All swaps were already synced; removal of the final
                        # marker completed. Nothing remains to resume.
                        self.assertFalse(any((data / name).exists() for name in checkpoint.MARKERS))
                    else:
                        before = {str(path): path.read_bytes() for path in data.rglob("*") if path.is_file()}
                        preview = checkpoint.resume(data, Path("unused"), False)
                        self.assertTrue(preview["preview_only"])
                        self.assertEqual(before, {str(path): path.read_bytes() for path in data.rglob("*") if path.is_file()})
                        with patch.object(checkpoint, "copy_sparse", side_effect=AssertionError("resume must not recopy state")):
                            result = checkpoint.resume(data, Path("unused"), True)
                        self.assertTrue(result["restored"])
                    for name in checkpoint.FILES:
                        self.assertEqual((data / name).read_bytes(), b"new " + name.encode())
                        backups = list((data / name).parent.glob((data / name).name + ".pre-restore-*"))
                        self.assertEqual(len(backups), 1)
                        self.assertEqual(backups[0].read_bytes(), b"old " + name.encode())

    def test_resume_refuses_tampering_before_any_swap(self):
        for fault in ("staged", "late_staged", "backup", "target_path", "disagreement", "legacy", "symlink"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as folder:
                data, saved = recovery_fixture(Path(folder))
                original_rename = Path.rename

                def fail_install(path, target):
                    if ".restore-" in path.name:
                        raise OSError("stop before install")
                    return original_rename(path, target)

                with patch.object(checkpoint, "inspect", side_effect=inspector_stub):
                    with patch.object(Path, "rename", fail_install):
                        with self.assertRaises(OSError):
                            checkpoint.restore(data, saved, Path("unused"), True)
                    journal = checkpoint.read_json(data / checkpoint.MARKERS[0])
                    entry = journal["changes"][0]
                    if fault == "late_staged":
                        Path(journal["changes"][-1]["staged"]).write_bytes(b"tampered late file")
                    elif fault in ("staged", "backup"):
                        Path(entry[fault]).write_bytes(b"tampered")
                    elif fault == "symlink":
                        staged = Path(entry["staged"])
                        staged.unlink()
                        staged.symlink_to(saved / checkpoint.FILES[0])
                    else:
                        if fault == "target_path":
                            entry["target"] = str(Path(folder) / "outside-state")
                        elif fault == "legacy":
                            journal["format"] = 1
                        else:
                            journal["checkpoint"] = "different generation"
                        markers = checkpoint.MARKERS[:1] if fault == "disagreement" else checkpoint.MARKERS
                        for name in markers:
                            (data / name).write_text(json.dumps(journal))
                    with patch.object(Path, "rename", side_effect=AssertionError("must not rename")), \
                         patch.object(Path, "unlink", side_effect=AssertionError("must not remove journal")):
                        with self.assertRaises(RuntimeError):
                            checkpoint.resume(data, Path("unused"), True)

    def test_resume_can_itself_be_interrupted(self):
        with tempfile.TemporaryDirectory() as folder:
            data, saved = recovery_fixture(Path(folder))
            original_rename = Path.rename

            def before_install(path, target):
                if ".restore-" in path.name:
                    raise OSError("first interruption")
                return original_rename(path, target)

            def after_install(path, target):
                result = original_rename(path, target)
                if ".restore-" in path.name:
                    raise OSError("second interruption")
                return result

            with patch.object(checkpoint, "inspect", side_effect=inspector_stub):
                with patch.object(Path, "rename", before_install):
                    with self.assertRaisesRegex(OSError, "first interruption"):
                        checkpoint.restore(data, saved, Path("unused"), True)
                with patch.object(Path, "rename", after_install):
                    with self.assertRaisesRegex(OSError, "second interruption"):
                        checkpoint.resume(data, Path("unused"), True)
                preview = checkpoint.resume(data, Path("unused"), False)
                self.assertEqual(preview["phases"][checkpoint.FILES[0]], "installed")
                result = checkpoint.resume(data, Path("unused"), True)
                for name, backup in zip(checkpoint.FILES, result["backups"]):
                    self.assertEqual((data / name).read_bytes(), b"new " + name.encode())
                    self.assertEqual(Path(backup).read_bytes(), b"old " + name.encode())

    def test_duplicate_or_truncated_metadata_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "state.restore.pending"
            path.write_text('{"format": 2, "format": 2}')
            with self.assertRaisesRegex(RuntimeError, "Duplicate metadata key"):
                checkpoint.read_json(path)
            path.write_text('{"format": 2')
            with self.assertRaises(ValueError):
                checkpoint.read_json(path)

    def test_journal_sync_failure_never_exposes_partial_marker(self):
        with tempfile.TemporaryDirectory() as folder:
            data, saved = recovery_fixture(Path(folder))
            real_write = checkpoint.write_json

            def fail_journal_sync(path, value):
                if ".restore.pending.writing-" in path.name:
                    with patch.object(checkpoint, "sync_file", side_effect=OSError("journal fsync failed")):
                        return real_write(path, value)
                return real_write(path, value)

            with patch.object(checkpoint, "inspect", side_effect=inspector_stub), \
                 patch.object(checkpoint, "write_json", fail_journal_sync):
                with self.assertRaisesRegex(OSError, "journal fsync failed"):
                    checkpoint.restore(data, saved, Path("unused"), True)
            self.assertFalse(any((data / name).exists() for name in checkpoint.MARKERS))
            self.assertEqual(len(list(data.glob("*.restore.pending.writing-*"))), 1)
            for name in checkpoint.FILES:
                self.assertEqual((data / name).read_bytes(), b"old " + name.encode())

    def test_resume_absent_original_and_missing_checkpoint(self):
        with tempfile.TemporaryDirectory() as folder:
            absent = checkpoint.FILES[1:]
            data, saved = recovery_fixture(Path(folder), absent=absent)
            original_rename = Path.rename

            def fail_install(path, target):
                if ".restore-" in path.name:
                    raise OSError("stop before install")
                return original_rename(path, target)

            with patch.object(checkpoint, "inspect", side_effect=inspector_stub):
                with patch.object(Path, "rename", fail_install):
                    with self.assertRaises(OSError):
                        checkpoint.restore(data, saved, Path("unused"), True)
                # Resume is self-contained; a detached checkpoint mount is OK.
                saved.rename(saved.with_name("offline"))
                result = checkpoint.resume(data, Path("unused"), True)
                self.assertEqual(len(result["backups"]), 1)
                for name in checkpoint.FILES:
                    self.assertEqual((data / name).read_bytes(), b"new " + name.encode())

    def test_failed_sync_never_publishes_generation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data, saved = root / "data", root / "saved"
            for name in checkpoint.FILES:
                source = data / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b"unchanged source")
            with patch.object(checkpoint, "inspect", return_value={}), \
                 patch.object(checkpoint, "sync_file", side_effect=OSError("injected fsync failure")):
                with self.assertRaisesRegex(OSError, "injected fsync failure"):
                    checkpoint.create(data, saved, Path("unused"))
            self.assertFalse(saved.exists())
            incomplete = list(root.glob("saved.incomplete-*"))
            self.assertEqual(len(incomplete), 1)
            self.assertFalse((incomplete[0] / "manifest.json").exists())
            for name in checkpoint.FILES:
                self.assertEqual((data / name).read_bytes(), b"unchanged source")


if __name__ == "__main__":
    unittest.main()
