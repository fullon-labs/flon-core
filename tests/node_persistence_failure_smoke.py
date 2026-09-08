#!/usr/bin/env python3
"""Inject shutdown metadata failures in isolated dev nodes, never live data."""
import argparse
import json
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    binary = str(args.binary.resolve())
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    results = {}
    for relative in ("state/chain_head.dat", "blocks/reversible/fork_db.dat"):
        with tempfile.TemporaryDirectory(prefix="flon-save-failure-") as folder:
            root = Path(folder)
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            command = [binary, "--data-dir", str(root / "data"),
                       "--config-dir", str(root / "config"),
                       "--p2p-listen-endpoint", "", f"--http-server-address=127.0.0.1:{port}",
                       "--plugin=eosio::chain_api_plugin", "--chain-state-db-size-mb=128",
                       "--chain-state-db-guard-size-mb=16", "--resource-monitor-space-threshold=99",
                       "--wasm-runtime=eos-vm", "--producer-name=flon", "--enable-stale-production",
                       "--signature-provider=FU6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV=KEY:5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"]
            with (root / "node.log").open("w") as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 30
                    while True:
                        if process.poll() is not None:
                            raise RuntimeError((root / "node.log").read_text())
                        try:
                            with opener.open(f"http://127.0.0.1:{port}/v1/chain/get_info", timeout=2) as response:
                                if json.load(response)["head_block_num"] >= 3:
                                    break
                        except OSError:
                            pass
                        if time.monotonic() > deadline:
                            raise RuntimeError("isolated node failed to produce")
                        time.sleep(0.1)
                    target = root / "data" / relative
                    assert not target.exists(), target
                    target.mkdir()  # deterministic rename failure, including when run as root
                    (target / "keep").write_text("do not remove on failed save")
                    process.send_signal(signal.SIGTERM)
                    assert process.wait(timeout=30) == 3, (root / "node.log").read_text()
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=30)
            output = (root / "node.log").read_text()
            assert "successfully exiting" not in output, output
            assert "refusing to mark state clean" in output, output
            assert (target / "keep").read_text() == "do not remove on failed save"
            # Remove only the injected obstacle; dirty state must still refuse startup.
            (target / "keep").unlink()
            target.rmdir()
            restart = subprocess.run(command, capture_output=True, text=True, timeout=30)
            assert restart.returncode == 2, restart.stdout + restart.stderr
            results[relative] = "exit_3_dirty_preserved_restart_refused"
    print(json.dumps(results))


if __name__ == "__main__":
    main()
