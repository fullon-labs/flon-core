---
content_title: fonod
---

## Introduction

`fonod` is the core service daemon that runs on every FullOn node. It can be configured to process smart contracts, validate transactions, produce blocks containing valid transactions, and confirm blocks to record them on the blockchain.

## Installation

`fonod` is distributed as part of the [FullOn software suite](https://github.com/fullon-labs/fullon). To install `fonod`, visit the [FullOn Software Installation](../00_install/index.md) section.

## Explore

Navigate the sections below to configure and use `fonod`.

* [Usage](02_usage/index.md) - Configuring and using `fonod`, node setups/environments.
* [Plugins](03_plugins/index.md) - Using plugins, plugin options, mandatory vs. optional.
* [Replays](04_replays/index.md) - Replaying the chain from a snapshot or a blocks.log file.
* [RPC APIs](05_rpc_apis/index.md) - Remote Procedure Call API reference for plugin HTTP endpoints.
* [Logging](06_logging/index.md) - Logging config/usage, loggers, appenders, logging levels.
* [Concepts](07_concepts/index.md) - `fonod` concepts, explainers, implementation aspects.
* [Troubleshooting](08_troubleshooting/index.md) - Common `fonod` troubleshooting questions.

[[info | Access Node]]
| A local or remote FullOn access node running `fonod` is required for a client application or smart contract to interact with the blockchain.
