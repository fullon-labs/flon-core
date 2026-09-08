#!/usr/bin/env python3
"""Isolated node startup/RPC smoke test; never connects to a live chain."""
import argparse
import json
import pathlib
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--produce", action="store_true", help="Produce isolated development-chain blocks")
    parser.add_argument("--driver", type=pathlib.Path, help="Test-only transaction/gap fixture executable")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--history", action="store_true")
    group.add_argument("--no-history", action="store_true")
    args = parser.parse_args()
    binary = args.binary.resolve()
    help_text = subprocess.check_output([str(binary), "--help"], text=True)
    assert ("transaction-history-dir" in help_text) == args.history

    with tempfile.TemporaryDirectory(prefix="flon-profile-smoke-") as directory:
        root = pathlib.Path(directory)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        command = [str(binary), "--data-dir", str(root / "data"),
                   "--config-dir", str(root / "config"),
                   "--p2p-listen-endpoint", "", "--plugin=eosio::chain_api_plugin",
                   f"--http-server-address=127.0.0.1:{port}",
                   "--chain-state-db-size-mb=128", "--chain-state-db-guard-size-mb=16",
                   "--resource-monitor-space-threshold=99",
                   "--enable-account-queries=true", "--wasm-runtime=eos-vm"]
        if args.history:
            command.append("--plugin=eosio::transaction_history_api_plugin")
        if args.produce:
            # Public development key only; P2P is disabled for this temporary chain.
            command.extend(["--producer-name=flon", "--enable-stale-production",
                            "--signature-provider=FU6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV=KEY:5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"])

        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        def rpc(path, body):
            request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/{path}",
                                             data=json.dumps(body).encode(),
                                             headers={"Content-Type": "application/json"})
            with opener.open(request, timeout=2) as response:
                return json.load(response)

        with (root / "node.log").open("w+") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            try:
                until = time.monotonic() + 30
                while True:
                    if process.poll() is not None:
                        raise RuntimeError(f"node exited with {process.returncode}")
                    try:
                        info = rpc("chain/get_info", {})
                        break
                    except (OSError, urllib.error.URLError):
                        if time.monotonic() >= until:
                            raise
                        time.sleep(0.1)
                assert "chain_id" in info
                if args.produce:
                    initial_head = info["head_block_num"]
                    until = time.monotonic() + 30
                    while info["head_block_num"] <= initial_head:
                        if time.monotonic() >= until:
                            raise RuntimeError("development BP did not produce a block")
                        time.sleep(0.2)
                        info = rpc("chain/get_info", {})
                # get_accounts_by_authorizers exercises the enabled account index.
                accounts = rpc("chain/get_accounts_by_authorizers", {"accounts": [], "keys": []})
                assert "accounts" in accounts
                if args.driver and args.produce:
                    fixture = json.loads(subprocess.check_output([
                        str(args.driver.resolve()), "transaction", info["chain_id"], info["head_block_id"]]))
                    rpc("chain/push_transaction", fixture["packed"])
                    until = time.monotonic() + 15
                    while True:
                        try:
                            rpc("chain/get_account", {"account_name": "smokeacct"})
                            indexed = rpc("chain/get_accounts_by_authorizers", {
                                "accounts": [],
                                "keys": ["FU6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"]})
                            if "smokeacct" in json.dumps(indexed):
                                break
                            if time.monotonic() >= until:
                                raise RuntimeError("new account is missing from the authorizer index")
                            time.sleep(0.1)
                        except urllib.error.HTTPError:
                            if time.monotonic() >= until:
                                raise
                            time.sleep(0.1)
                if args.history:
                    if args.driver and args.produce:
                        until = time.monotonic() + 15
                        while True:
                            try:
                                transaction = rpc("history/get_transaction", {"id": fixture["id"]})
                                assert transaction["account_index_complete"] is True
                                assert transaction["traces"][0]["act"]["name"] == "newaccount"
                                break
                            except urllib.error.HTTPError:
                                if time.monotonic() >= until:
                                    raise
                                time.sleep(0.1)
                    actions = rpc("history/get_actions", {"account_name": "flon", "offset": 1})
                    assert actions["history_status"]["recording_healthy"] is True
                    assert "indexed_through_block" in actions["history_status"]
                    if args.driver and args.produce:
                        assert actions["actions"], "account action index is empty"
                print(json.dumps({"history": args.history, "rpc": "ok", "account_query": "ok",
                                  "produced": args.produce, "head": info["head_block_num"]}))
            except Exception:
                log.flush()
                log.seek(0)
                print(log.read()[-12000:])
                raise
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            assert process.returncode == 0, f"unclean shutdown: {process.returncode}"

        if args.history and args.driver:
            driver = str(args.driver.resolve())
            database = str(root / "data" / "transaction_history")
            if args.produce:
                # Inject only while the temporary database is closed. A later
                # transaction must disable recording without overwriting old indexes.
                subprocess.check_call([driver, "corrupt-sequence", database])
                with (root / "sequence-restart.log").open("w+") as log:
                    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        until = time.monotonic() + 30
                        while True:
                            if process.poll() is not None:
                                raise RuntimeError("sequence test node exited before RPC")
                            try:
                                info = rpc("chain/get_info", {})
                                break
                            except OSError:
                                if time.monotonic() >= until:
                                    raise
                                time.sleep(0.1)
                        fixture = json.loads(subprocess.check_output([
                            driver, "transaction", info["chain_id"], info["head_block_id"], "smokeacctb"]))
                        rpc("chain/push_transaction", fixture["packed"])
                        while True:
                            actions = rpc("history/get_actions", {"account_name": "flon", "offset": 1})
                            if not actions["history_status"]["recording_healthy"]:
                                break
                            if time.monotonic() >= until:
                                raise RuntimeError("corrupt counter did not disable history recording")
                            time.sleep(0.1)
                        assert rpc("chain/get_account", {"account_name": "smokeacctb"})["account_name"] == "smokeacctb"
                    except Exception:
                        log.flush()
                        log.seek(0)
                        print(log.read()[-12000:])
                        raise
                    finally:
                        if process.poll() is None:
                            process.send_signal(signal.SIGTERM)
                            try:
                                process.wait(timeout=30)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()
                    assert process.returncode == 0
                subprocess.check_call([driver, "check-sequence", database])
                print(json.dumps({"corrupt_sequence": "recording_disabled_old_index_preserved_chain_healthy"}))
            subprocess.check_call([driver, "seed-gap", database])
            with (root / "gap-restart.log").open("w+") as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
                    until = time.monotonic() + 30
                    while True:
                        if process.poll() is not None:
                            raise RuntimeError("gap restart exited before RPC became available")
                        try:
                            actions = rpc("history/get_actions", {"account_name": "flon", "offset": 1})
                            assert actions["history_status"]["recording_healthy"] is False
                            break
                        except (OSError, urllib.error.URLError):
                            if time.monotonic() >= until:
                                raise
                            time.sleep(0.1)
                except Exception:
                    log.flush()
                    log.seek(0)
                    print(log.read()[-12000:])
                    raise
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGINT)
                        try:
                            process.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                assert process.returncode == 0
            subprocess.check_call([driver, "check-gap", database])
            print(json.dumps({"gap_restart_preserves_history": "ok"}))


if __name__ == "__main__":
    main()
