#!/usr/bin/env python3
"""Preview a bounded recovery-snapshot schedule; write only with --apply."""
import argparse
import ipaddress
import json
import urllib.parse
import urllib.request

MAX_BLOCK = 2**32 - 1
DESCRIPTION = "managed-recovery-points-v1"


def make_plan(head, spacing, count):
    if spacing <= 0 or not 2 <= count <= 10:
        raise ValueError("block spacing must be positive; count must be between 2 and 10")
    start = head + 1
    end = start + spacing * (count - 1)
    if start <= 0 or end > MAX_BLOCK:
        raise ValueError("snapshot schedule exceeds the block-number range")
    return {"block_spacing": spacing, "start_block_num": start, "end_block_num": end,
            "snapshot_description": DESCRIPTION}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8888")
    parser.add_argument("--block-spacing", type=int, required=True,
                        help="Actual blocks, not wall-clock seconds; account for elastic production")
    parser.add_argument("--count", type=int, default=3, help="Bounded pilot: 2-10 snapshots, default 3")
    parser.add_argument("--apply", action="store_true", help="Register the displayed schedule on the node")
    args = parser.parse_args()
    parsed = urllib.parse.urlsplit(args.url)
    if parsed.scheme not in ("http", "https") or parsed.username or parsed.password:
        parser.error("use an HTTP(S) loopback endpoint, without credentials in the URL")
    try:
        loopback = parsed.hostname == "localhost" or ipaddress.ip_address(parsed.hostname).is_loopback
    except (TypeError, ValueError):
        loopback = False
    if not loopback:
        parser.error("management RPC must be loopback; use an SSH tunnel for remote nodes")
    # Validate before making any request.
    make_plan(0, args.block_spacing, args.count)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def rpc(method, body=None):
        request = urllib.request.Request(args.url.rstrip("/") + "/v1/" + method,
                                         data=json.dumps(body or {}).encode(),
                                         headers={"Content-Type": "application/json"})
        with opener.open(request, timeout=30) as response:
            return json.load(response)

    info = rpc("chain/get_info")
    existing = rpc("producer/get_snapshot_requests")["snapshot_requests"]
    managed = [item for item in existing if item.get("snapshot_description") == DESCRIPTION]
    if managed:
        print(json.dumps({"chain_id": info["chain_id"], "status": "existing_schedule_unchanged",
                          "requests": managed}, indent=2))
        return
    plan = make_plan(info["head_block_num"], args.block_spacing, args.count)
    print(json.dumps({"chain_id": info["chain_id"], "head": info["head_block_num"],
                      "mode": "apply" if args.apply else "preview_only", "request": plan}, indent=2))
    if args.apply:
        print(json.dumps({"registered": rpc("producer/schedule_snapshot", plan)}, indent=2))
    else:
        print("No changes made. Verify disk capacity, snapshot duration, and block retention before --apply.")


if __name__ == "__main__":
    main()
