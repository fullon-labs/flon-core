#!/usr/bin/env python3
"""Run signed interop RPC smoke cases on a disposable or test network."""

import argparse
import json
import subprocess
import urllib.error
import urllib.request
from pathlib import Path


def rpc(url, method, body=None):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(
        f"{url.rstrip('/')}/v1/interop/{method}",
        data,
        {"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=40) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def expect(response, status):
    actual, result = response
    if actual != status:
        raise RuntimeError(f"expected HTTP {status}, got {actual}: {result}")
    return result


def sign(command, prepared):
    result = subprocess.run(
        command, input=json.dumps(prepared), text=True, capture_output=True,
        check=True, timeout=30,
    )
    fields = json.loads(result.stdout)
    if not isinstance(fields.get("signature"), str):
        raise ValueError("wallet adapter must return a signature string")
    return {
        **prepared["submit_args"],
        "signature": fields["signature"],
        "wallet_fields": fields.get("wallet_fields", {}),
    }


def fill_account(value, account):
    if isinstance(value, dict):
        return {key: fill_account(item, account) for key, item in value.items()}
    if isinstance(value, list):
        return [fill_account(item, account) for item in value]
    return account if value == "$account" else value


def run_case(url, routes, case, timeout_url=None):
    source, address = case["source"], case["address"]
    if not routes[source].get("bind") or not routes[source].get("execute"):
        raise RuntimeError(f"{source} must advertise bind and execute")
    common = {"source": source, "address": address}
    bind = {"operation": "bind", **common}
    prepared = expect(rpc(url, "prepare", bind), 200)
    binding = expect(rpc(url, "submit", sign(case["signer"], prepared)), 202)
    account = prepared["submit_args"]["account"]
    execute = {
        "operation": "execute", **common,
        "txns": fill_account(case["actions"], account),
    }
    prepared = expect(rpc(url, "prepare", execute), 200)
    signed = sign(case["signer"], prepared)
    expect(rpc(url, "submit", {**signed, "expires_at": 1}), 410)

    # Ask the wallet to sign unrelated text, retaining its normal signature encoding.
    message = prepared["signing_message"]
    unrelated = {**prepared, "signing_message": message[:-2] + ("01" if message.endswith("00") else "00")}
    result = expect(rpc(url, "submit", sign(case["signer"], unrelated)), 500)
    if result.get("error", {}).get("name") != "eosio_assert_message_exception":
        raise RuntimeError(f"{source}: expected a contract assertion, got {result}")
    fresh = expect(rpc(url, "prepare", execute), 200)
    if fresh["submit_args"]["nonce"] != prepared["submit_args"]["nonce"]:
        raise RuntimeError(f"{source}: rejected request changed the nonce")

    tx = expect(rpc(url, "submit", signed), 202)
    expect(rpc(url, "submit", signed), 409)
    print(f"{source}: bind={binding['transaction_id']} execute={tx['transaction_id']}")
    if timeout_url:
        prepared = expect(rpc(timeout_url, "prepare", execute), 200)
        timeout = expect(rpc(timeout_url, "submit", sign(case["signer"], prepared)), 504)
        if timeout.get("code") != "submission_timeout" or len(timeout.get("transaction_id", "")) != 64:
            raise RuntimeError(f"{source}: did not receive timeout with transaction ID: {timeout}")
        print(f"{source}: timeout transaction={timeout['transaction_id']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="TCP RPC node on a disposable or test network")
    parser.add_argument("--cases", type=Path, required=True, help="JSON list of cases with signer commands")
    parser.add_argument("--mismatch-url", help="Optional node configured with the wrong signing protocol")
    parser.add_argument("--timeout-url", help="Optional same-chain relayer with a deliberately stalled signer")
    args = parser.parse_args()
    cases = json.loads(args.cases.read_text())
    routes = expect(rpc(args.url, "get_routes"), 200)["routes"]
    if len({routes[case["source"]]["signature_type"] for case in cases}) < 2:
        parser.error("supply at least two supported signature types")
    if args.mismatch_url:
        error = expect(rpc(args.mismatch_url, "get_routes"), 503)
        if error.get("code") != "signing_protocol_mismatch":
            raise RuntimeError(f"expected signing protocol mismatch: {error}")
    for case in cases:
        run_case(args.url, routes, case, args.timeout_url)


if __name__ == "__main__":
    main()
