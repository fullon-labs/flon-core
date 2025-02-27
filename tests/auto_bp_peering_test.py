#!/usr/bin/env python3

import socket

from TestHarness import Cluster, TestHelper, Utils, WalletMgr

###############################################################
# auto_bp_peering_test
#
# This test sets up  a cluster with 21 producers funod, each funod is configured with only one producer and only connects to the bios node.
# Moreover, each producer funod is also configured with a list of p2p-auto-bp-peer so that each one can automatically establish p2p connections to
# their downstream two neighbors based on producer schedule on the chain and tear down the connections which are no longer in the scheduling neighborhood.
#
###############################################################

Print = Utils.Print
errorExit = Utils.errorExit
cmdError = Utils.cmdError
producerNodes = 21
producerCountInEachNode = 1
totalNodes = producerNodes

# Parse command line arguments
args = TestHelper.parse_args({
    "-v",
    "--activate-if",
    "--dump-error-details",
    "--leave-running",
    "--keep-logs",
    "--unshared"
})

Utils.Debug = args.v
activateIF=args.activate_if
dumpErrorDetails = args.dump_error_details
keepLogs = args.keep_logs

# Setup cluster and its wallet manager
walletMgr = WalletMgr(True)
cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
cluster.setWalletMgr(walletMgr)


peer_names = {}

auto_bp_peer_args = ""
for nodeId in range(0, producerNodes):
    producer_name = "defproducer" + chr(ord('a') + nodeId)
    port = cluster.p2pBasePort + nodeId
    if producer_name == 'defproducerf':
        hostname = 'ext-ip0:9999'
    elif producer_name == 'defproducerk':
        hostname = socket.gethostname() + ':9886'
    else:
        hostname = "localhost:" + str(port)
    peer_names[hostname] = producer_name
    auto_bp_peer_args += (" --p2p-auto-bp-peer " + producer_name + "," + hostname)


def neighbors_in_schedule(name, schedule):
    index = schedule.index(name)
    result = []
    num = len(schedule)
    result.append(schedule[(index+1) % num])
    result.append(schedule[(index+2) % num])
    result.append(schedule[(index-1) % num])
    result.append(schedule[(index-2) % num])
    return result.sort()


peer_names["localhost:9776"] = "bios"

testSuccessful = False
try:
    specificfunodArgs = {}
    for nodeId in range(0, producerNodes):
        specificfunodArgs[nodeId] = auto_bp_peer_args

    specificfunodArgs[5] = specificfunodArgs[5] + ' --p2p-server-address ext-ip0:9999'
    specificfunodArgs[10] = specificfunodArgs[10] + ' --p2p-server-address ""'

    TestHelper.printSystemInfo("BEGIN")
    cluster.launch(
        prodCount=producerCountInEachNode,
        totalNodes=totalNodes,
        pnodes=producerNodes,
        totalProducers=producerNodes,
        activateIF=activateIF,
        topo="./tests/auto_bp_peering_test_shape.json",
        extrafunodArgs=" --plugin eosio::net_api_plugin ",
        specificExtrafunodArgs=specificfunodArgs,
    )

    # wait until produceru is seen by every node
    for nodeId in range(0, producerNodes):
        print("Wait for node ", nodeId)
        cluster.nodes[nodeId].waitForProducer(
            "defproduceru", exitOnError=True, timeout=300)

    # retrieve the producer stable producer schedule
    scheduled_producers = []
    schedule = cluster.nodes[0].processUrllibRequest(
        "chain", "get_producer_schedule")
    for prod in schedule["payload"]["active"]["producers"]:
        scheduled_producers.append(prod["producer_name"])

    connection_check_failures = 0
    for nodeId in range(0, producerNodes):
        # retrieve the connections in each node and check if each only connects to their neighbors in the schedule
        connections = cluster.nodes[nodeId].processUrllibRequest(
            "net", "connections")
        peers = []
        for conn in connections["payload"]:
            peer_addr = conn["peer"]
            if len(peer_addr) == 0:
                if len(conn["last_handshake"]["p2p_address"]) == 0:
                    continue
                peer_addr = conn["last_handshake"]["p2p_address"].split()[0]
            if peer_names[peer_addr] != "bios":
                peers.append(peer_names[peer_addr])
                if not conn["is_bp_peer"]:
                    Utils.Print("Error: expected connection to {} with is_bp_peer as true".format(peer_addr))
                    connection_check_failures = connection_check_failures+1

        peers = peers.sort()
        name = "defproducer" + chr(ord('a') + nodeId)
        expected_peers = neighbors_in_schedule(name, scheduled_producers)
        if peers != expected_peers:
            Utils.Print("ERROR: expect {} has connections to {}, got connections to {}".format(
                name, expected_peers, peers))
            connection_check_failures = connection_check_failures+1

    testSuccessful = connection_check_failures == 0

finally:
    TestHelper.shutdown(
        cluster,
        walletMgr,
        testSuccessful,
        dumpErrorDetails
    )

exitCode = 0 if testSuccessful else 1
exit(exitCode)
