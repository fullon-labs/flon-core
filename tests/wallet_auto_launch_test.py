#!/usr/bin/env python3

# This script tests that client launches wallet automatically when wallet is not
# running yet.

import subprocess
from TestHarness import Utils

def run_client_wallet_command(command: str, no_auto_wallet: bool):
    """Run the given client command and return subprocess.CompletedProcess."""
    args = [Utils.ClientPath]

    if no_auto_wallet:
        args.append('--no-auto-wallet')

    args += 'wallet', command

    return subprocess.run(args,
                          check=False,
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE)


def stop_wallet():
    """Stop the default wallet instance."""
    run_client_wallet_command('stop', no_auto_wallet=True)


def check_client_stderr(stderr: bytes, expected_match: bytes):
    if expected_match not in stderr:
        raise RuntimeError("'{}' not found in {}'".format(
            expected_match.decode(), stderr.decode()))


def wallet_auto_launch_test():
    """Test that keos auto-launching works but can be optionally inhibited."""
    stop_wallet()

    # Make sure that when '--no-auto-wallet' is given, wallet is not started by
    # client.
    completed_process = run_client_wallet_command('list', no_auto_wallet=True)
    assert completed_process.returncode != 0
    check_client_stderr(completed_process.stderr, b'Failed http request to wallet')

    # Verify that wallet auto-launching works.
    completed_process = run_client_wallet_command('list', no_auto_wallet=False)
    if completed_process.returncode != 0:
        raise RuntimeError("Expected that wallet would be started, "
                           "but got an error instead: {}".format(
                               completed_process.stderr.decode()))
    check_client_stderr(completed_process.stderr, b'launched')


try:
    wallet_auto_launch_test()
finally:
    stop_wallet()
