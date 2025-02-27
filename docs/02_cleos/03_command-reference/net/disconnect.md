## Command
```sh
fucli net disconnect [OPTIONS] host
```

**Where:**
* [OPTIONS] = See **Options**section TBD

[//]: # ( THIS IS A COMMENT LINK BELOW IS BROKEN )  
[//]: # (in the **Command Usage** command-usage section below.)  

**Note:** The arguments and options enclosed in square brackets are optional.

## Description
Close an existing connection to a specified peer. A node operator can use this command to instruct a node to disconnect from another peer without restarting the node.

## Command Usage
The following information shows the different positionals and options you can use with the `fucli net disconnect` command:

### Positionals
* `host` _TEXT_ REQUIRED - The hostname:port to disconnect from

### Options
* `-h,--help` - Print this help message and exit

## Requirements
Make sure you meet the following requirements:

* Install the currently supported version of `fucli`.
[[info | Note]]
| `fucli` is bundled with the FullOn software. [Installing FullOn](../../../00_install/index.md) will also install the `fucli` and `keosd` command line tools.
* You have access to a producing node instance with the [`net_api_plugin`](../../../01_funod/03_plugins/net_api_plugin/index.md) loaded.

## Examples
The following examples demonstrate how to use the `fucli net disconnect` command:

* Instruct default local node (listening at default http address `http://127.0.0.1:8888`) to disconnect from peer node listening at p2p address `localhost:9002`:
```sh
fucli net disconnect localhost:9002
```
**Output:**
```console
"connection removed"
```

* Instruct local node listening at http address `http://127.0.0.1:8001` to disconnect from peer node listening at p2p address `localhost:9002`:
```sh
fucli -u http://127.0.0.1:8001 net disconnect localhost:9002
```
**Output:**
```console
"connection removed"
```

**Note:** If any of the above commands are re-executed, `fucli` returns the following message as expected:  
```console
"no known connection for host"
```
