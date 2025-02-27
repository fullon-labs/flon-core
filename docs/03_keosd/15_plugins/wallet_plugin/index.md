## Description

The `wallet_plugin` adds access to wallet functionality from a node.

[[caution | Caution]]
| This plugin is not designed to be loaded as a plugin on a publicly accessible node without further security measures. This is particularly true when loading the `wallet_api_plugin`, which should not be loaded on a publicly accessible node under any circumstances.

## Usage

```sh
# config.ini
plugin = eosio::wallet_plugin

# command-line
funod ... --plugin eosio::wallet_plugin
```

## Options

None

## Dependencies

* `http_plugin`

[//]: # ( THIS IS A COMMENT LINK BELOW IS BROKEN )  
[//]: # ( `http_plugin` ../http_plugin.md )  

### Load Dependency Examples

```sh
# config.ini
plugin = eosio::wallet_plugin
[options]
plugin = eosio::http_plugin
[options]

# command-line
funod ... --plugin eosio::wallet_plugin [options]  \
           --plugin eosio::http_plugin [options]
```
