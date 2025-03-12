#!/usr/bin/env python3

import os
import shutil
import time
import signal
from TestHarness import Node, TestHelper, Utils

node_id = 1
node = Node(TestHelper.LOCAL_HOST, TestHelper.DEFAULT_PORT, node_id)
data_dir = Utils.getNodeDataDir(node_id)
config_dir = Utils.getNodeConfigDir(node_id)
if os.path.exists(data_dir):
    shutil.rmtree(data_dir)
os.makedirs(data_dir)
if not os.path.exists(config_dir):
    os.makedirs(config_dir)

try:
    start_node_cmd = f"{Utils.NodeServerPath} -e -p eosio --data-dir={data_dir} --config-dir={config_dir} --blocks-log-stride 10" \
                        " --plugin=eosio::http_plugin --plugin=eosio::chain_api_plugin --http-server-address=localhost:8888"

    node.launchCmd(start_node_cmd, node_id)
    time.sleep(2)
    node.waitForBlock(30)
    node.kill(signal.SIGTERM)

    node.relaunch(chainArg="--replay-blockchain")

    time.sleep(2)
    assert node.waitForBlock(31)
finally:
    # clean up
    node.kill(signal.SIGTERM)
