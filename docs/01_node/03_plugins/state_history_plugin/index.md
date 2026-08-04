
## Description

The `state_history_plugin` is useful for capturing historical data about the blockchain state. The plugin receives blockchain data from other connected nodes and caches the data into files. The plugin listens on a socket for applications to connect and sends blockchain data back based on the plugin options specified when starting `funod`.

## Usage

```console
# config.ini
plugin = eosio::state_history_plugin
[options]
```
```sh
# command-line
funod ... --plugin eosio::state_history_plugin [operations] [options]
```

## Operations

These can only be specified from the `funod` command-line:

```console
Command Line Options for eosio::state_history_plugin:

  --delete-state-history                clear state history files
  --state-history-max-connections arg (=100)
                                        maximum number of simultaneous state
                                        history client connections
```

## Options

These can be specified from both the `funod` command-line or the `config.ini` file:

```console
Config Options for eosio::state_history_plugin:
  --state-history-dir arg (="state-history")
                                        the location of the state-history
                                        directory (absolute path or relative to
                                        application data dir)
  --trace-history                       enable trace history
  --chain-state-history                 enable chain state history
  --state-history-endpoint arg (=127.0.0.1:8080)
                                        the endpoint upon which to listen for
                                        incoming connections. Caution: only
                                        expose this port to your internal
                                        network.
  --state-history-unix-socket-path arg  the path (relative to data-dir) to
                                        create a unix socket upon which to
                                        listen for incoming connections.
  --trace-history-debug-mode            enable debug mode for trace history
  --state-history-log-retain-blocks arg if set, periodically prune the state
                                        history files to store only configured
                                        number of most recent blocks
```

Each client request is limited to 1 MiB, 4096 fork positions, and 4096 send
credits. Additional acknowledgement credits saturate at the same limit, which
keeps per-session memory and main-thread work bounded.

When a Unix socket is used, its parent directory must be private to the node
user (for example, mode `0700`).

## How-To Guides

* [How to fast start without history on existing chains](10_how-to-fast-start-without-old-history.md)
* [How to replay or resync with full history](20_how-to-replay-or-resync-with-full-history.md)
* [How to create a portable snapshot with full state history](30_how-to-create-snapshot-with-full-history.md)
* [How to restore a portable snapshot with full state history](40_how-to-restore-snapshot-with-full-history.md)
