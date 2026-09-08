#!/usr/bin/env python3
"""Isolated shutdown/snapshot recovery checks. Never connects to a live chain."""
import argparse
import importlib.util
import json
from pathlib import Path
import signal
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
from unittest.mock import patch


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--inspector", type=Path, required=True)
    args = parser.parse_args()
    binary, driver = str(args.binary.resolve()), str(args.driver.resolve())
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with tempfile.TemporaryDirectory(prefix="flon-shutdown-smoke-") as folder:
        root = Path(folder)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        command = [binary, "--data-dir", str(root / "data"), "--config-dir", str(root / "config"),
                   "--p2p-listen-endpoint", "", f"--http-server-address=127.0.0.1:{port}",
                   "--plugin=eosio::chain_api_plugin", "--plugin=eosio::producer_api_plugin",
                   "--chain-state-db-size-mb=128", "--chain-state-db-guard-size-mb=16",
                   "--resource-monitor-space-threshold=99", "--enable-account-queries=true",
                   "--wasm-runtime=eos-vm", "--producer-name=flon", "--enable-stale-production",
                   "--signature-provider=FU6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV=KEY:5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"]

        def rpc(method, body=None):
            request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/{method}",
                                             data=json.dumps(body or {}).encode(),
                                             headers={"Content-Type": "application/json"})
            with opener.open(request, timeout=30) as response:
                return json.load(response)

        process = None
        log = None

        def stop(sig):
            process.send_signal(sig)
            code = process.wait(timeout=30)
            log.close()
            assert code == (-signal.SIGKILL if sig == signal.SIGKILL else 0), code

        def start(label, extra=()):
            nonlocal process, log
            log = (root / f"{label}.log").open("w")
            process = subprocess.Popen(command + list(extra), stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 30
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"{label} exited with {process.returncode}")
                try:
                    return rpc("chain/get_info")
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.1)

        def check_account():
            assert rpc("chain/get_account", {"account_name": "smokeacct"})["account_name"] == "smokeacct"

        try:
            info = start("initial")
            original_chain_id = info["chain_id"]
            deadline = time.monotonic() + 30
            while rpc("chain/get_info")["head_block_num"] < 3:
                if time.monotonic() >= deadline:
                    raise RuntimeError("test chain did not start producing")
                time.sleep(0.1)
            checkpoint_helper = Path(__file__).resolve().parents[1] / "scripts/state_checkpoint.py"
            checkpoint = root / "physical-checkpoint"
            checkpoint_command = [sys.executable, str(checkpoint_helper),
                                  "--data-dir", str(root / "data"), "--checkpoint", str(checkpoint),
                                  "--inspector", str(args.inspector.resolve())]
            live_copy = subprocess.run(checkpoint_command + ["create"], capture_output=True, text=True)
            assert live_copy.returncode != 0 and not checkpoint.exists()
            stop(signal.SIGTERM)
            physical = json.loads(subprocess.check_output(checkpoint_command + ["create"], text=True))
            checkpoint_height = physical["state"]["head_block_num"]
            assert physical["state"]["undo_first_revision"] < physical["state"]["undo_last_revision"]
            clean_restore = subprocess.run(checkpoint_command + ["restore", "--apply"], capture_output=True, text=True)
            assert clean_restore.returncode != 0 and "Target state is clean" in clean_restore.stderr
            info = start("after-checkpoint")
            fixture = json.loads(subprocess.check_output(
                [driver, "transaction", info["chain_id"], info["head_block_id"]]))
            pushed = rpc("chain/push_transaction", fixture["packed"])
            transaction_block = pushed["processed"]["block_num"]
            deadline = time.monotonic() + 30
            while rpc("chain/get_info")["last_irreversible_block_num"] < transaction_block:
                if time.monotonic() >= deadline:
                    raise RuntimeError("test transaction did not become irreversible")
                time.sleep(0.1)
            check_account()
            stop(signal.SIGINT)
            start("after-sigint")
            check_account()
            stop(signal.SIGTERM)
            start("after-sigterm")
            check_account()
            helper = Path(__file__).resolve().parents[1] / "scripts/plan_recovery_snapshots.py"
            plan_command = [sys.executable, str(helper), "--url", f"http://127.0.0.1:{port}",
                            "--block-spacing", "2", "--count", "2"]
            before = rpc("producer/get_snapshot_requests")
            preview = subprocess.check_output(plan_command, text=True)
            assert "preview_only" in preview
            assert rpc("producer/get_snapshot_requests") == before
            subprocess.check_output(plan_command + ["--apply"], text=True)
            repeated = subprocess.check_output(plan_command + ["--apply"], text=True)
            assert "existing_schedule_unchanged" in repeated
            deadline = time.monotonic() + 30
            while (rpc("producer/get_snapshot_requests")["snapshot_requests"] or
                   len(list((root / "data/snapshots").glob("snapshot-*.bin"))) < 2):
                if time.monotonic() >= deadline:
                    raise RuntimeError("bounded recovery schedule did not produce two finalized snapshots")
                time.sleep(0.1)
            snapshot = rpc("producer/create_snapshot")
            snapshot_path = Path(snapshot["snapshot_name"]).resolve()
            assert snapshot_path.is_relative_to(root.resolve())
            assert snapshot_path.is_file() and snapshot_path.stat().st_size > 0
            assert rpc("chain/get_info")["last_irreversible_block_num"] >= snapshot["head_block_num"]
            stop(signal.SIGKILL)
            dirty = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, timeout=30)
            assert dirty.returncode == 2 and "database dirty flag set" in dirty.stdout
            # Only this disposable test state is moved, never deleted/repaired in place.
            (root / "data/state").rename(root / "dirty-state-preserved")
            info = start("snapshot-recovery", ["--snapshot", str(snapshot_path)])
            assert info["chain_id"] == original_chain_id
            assert info["head_block_num"] >= snapshot["head_block_num"]
            check_account()
            stop(signal.SIGTERM)
            # This checkpoint predates smokeacct. Replaying the existing log,
            # rather than the checkpoint alone, must recover that transaction.
            start("before-physical-recovery")
            stop(signal.SIGKILL)
            safety = root / "data/state/safety.dat"
            safety.write_bytes(b"test sentinel: recovery must never replace signing safety")
            preview = json.loads(subprocess.check_output(checkpoint_command + ["restore"], text=True))
            assert preview["preview_only"] and preview["blocks_to_replay"] > 0
            anchor_command = [str(args.inspector.resolve()), "anchor", str(root / "data/blocks")]
            anchor_id = physical["state"]["head_block_id"]
            wrong_id = anchor_id[:-1] + ("0" if anchor_id[-1] != "0" else "1")
            wrong_chain = original_chain_id[:-1] + ("0" if original_chain_id[-1] != "0" else "1")
            assert subprocess.run(anchor_command + [original_chain_id, wrong_id], capture_output=True).returncode != 0
            assert subprocess.run(anchor_command + [wrong_chain, anchor_id], capture_output=True).returncode != 0
            # Exercise the controller's own guard, not just the restore helper.
            # This disposable clone has a deliberately mismatching next block.
            bad_data = root / "bad-replay"
            shutil.copytree(checkpoint, bad_data)
            for name in ("blocks.log", "blocks.index"):
                shutil.copyfile(root / "data/blocks" / name, bad_data / "blocks" / name)
            with (bad_data / "blocks/blocks.index").open("rb") as index:
                index.seek(checkpoint_height * 8)  # fresh test log starts at block 1
                position = struct.unpack("<Q", index.read(8))[0]
            with (bad_data / "blocks/blocks.log").open("r+b") as block_log:
                block_log.seek(position + 45)  # last byte of the packed header's previous ID
                value = block_log.read(1)
                block_log.seek(position + 45)
                block_log.write(bytes([value[0] ^ 1]))
            bad_command = list(command)
            bad_command[bad_command.index("--data-dir") + 1] = str(bad_data)
            bad_command[bad_command.index("--config-dir") + 1] = str(root / "bad-config")
            bad = subprocess.run(bad_command, capture_output=True, text=True, timeout=30)
            assert bad.returncode != 0 and "undo history was not discarded" in bad.stdout + bad.stderr
            after_bad = json.loads(subprocess.check_output(
                [str(args.inspector.resolve()), "state", str(bad_data / "state")], text=True))
            assert after_bad == physical["state"]  # includes unchanged undo range
            recovered = json.loads(subprocess.check_output(checkpoint_command + ["restore", "--apply"], text=True))
            assert recovered["restored"] and any(Path(p).exists() for p in recovered["backups"])
            assert safety.read_bytes() == b"test sentinel: recovery must never replace signing safety"
            info = start("physical-recovery")
            assert info["chain_id"] == original_chain_id
            assert info["head_block_num"] > checkpoint_height
            check_account()
            stop(signal.SIGTERM)
            start("before-interrupted-restore")
            stop(signal.SIGKILL)
            # Fault injection belongs only to this test, not the production CLI.
            spec = importlib.util.spec_from_file_location("checkpoint_helper", checkpoint_helper)
            recovery = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(recovery)
            real_rename = Path.rename

            def interrupt_install(path, destination):
                if ".restore-" in path.name:
                    raise OSError("test interruption after original backup")
                return real_rename(path, destination)

            with recovery.lock((root / "data/state.operation.lock").resolve()), \
                 recovery.lock((root / "data/blocks.operation.lock").resolve()), \
                 patch.object(Path, "rename", interrupt_install):
                try:
                    recovery.restore(root / "data", checkpoint, args.inspector.resolve(), True)
                    raise AssertionError("interruption was not injected")
                except OSError as error:
                    assert "test interruption" in str(error)
            assert not (root / "data/state/shared_memory.bin").exists()
            interrupted = subprocess.run(command, capture_output=True, text=True, timeout=30)
            assert interrupted.returncode != 0 and "Interrupted physical recovery" in interrupted.stdout + interrupted.stderr
            resume_command = [sys.executable, str(checkpoint_helper), "resume", "--data-dir", str(root / "data"),
                              "--inspector", str(args.inspector.resolve())]
            resume_preview = json.loads(subprocess.check_output(resume_command, text=True))
            assert resume_preview["preview_only"]
            assert resume_preview["phases"]["state/shared_memory.bin"] == "original_saved"
            assert not (root / "data/state/shared_memory.bin").exists()  # preview did not create/replace it
            # The journal embeds validated metadata; the checkpoint mount may
            # be unavailable after a reboot, without forcing another full copy.
            checkpoint.rename(root / "checkpoint-offline")
            resumed = json.loads(subprocess.check_output(resume_command + ["--apply"], text=True))
            assert resumed["restored"] and all(Path(p).exists() for p in resumed["backups"])
            assert safety.read_bytes() == b"test sentinel: recovery must never replace signing safety"
            info = start("resumed-physical-recovery")
            assert info["chain_id"] == original_chain_id
            assert info["head_block_num"] > checkpoint_height
            check_account()
            stop(signal.SIGTERM)
            print(json.dumps({"sigint_restart": "ok", "sigterm_restart": "ok",
                              "sigkill_refused_dirty": "ok", "snapshot_recovery": "ok",
                              "recovery_plan_preview_dedup_and_completion": "ok",
                              "restored_account": "smokeacct", "physical_checkpoint_tail_replay": "ok",
                              "live_copy_and_clean_rollback_refused": "ok", "interrupted_restore_refused": "ok",
                              "wrong_chain_and_same_height_wrong_id_refused": "ok",
                              "native_replay_mismatch_preserves_undo": "ok",
                              "interrupted_restore_preview_and_resume_without_checkpoint": "ok",
                              "checkpoint_block": checkpoint_height, "transaction_block": transaction_block,
                              "recovery_log_head": recovered["log_head"]}))
        except Exception:
            if log and not log.closed:
                log.flush()
            for path in sorted(root.glob("*.log")):
                print(f"--- {path.name} ---\n{path.read_text()[-8000:]}")
            raise
        finally:
            if process and process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if log and not log.closed:
                log.close()


if __name__ == "__main__":
    main()
