## Overview

This how-to guide provides instructions on how to list all public keys and public/private key pairs within the `fuwal` default wallet. You can use the public and private keys to authorize transactions in an FullOn blockchain.

The example in this how-to guide displays all public keys and public/private key pairs stored within the existing default wallet.

## Before you begin

Make sure you meet the following requirements:

* Create a default wallet using the `fucli wallet create` command. See the [How to Create a Wallet](../02_how-to-guides/how-to-create-a-wallet.md) section for instructions.
* [Create a keypair](../03_command-reference/wallet/create_key.md) within the default wallet.
* Familiarize with the [`fucli wallet`](../03_command-reference/wallet/index.md) commands.
* Install the currently supported version of `fucli`.
[[info | Note]]
| `fucli` is bundled with the FullOn software. [Installing FullOn](../../00_install/index.md) will also install `fucli`.
* Understand what a [public key](/glossary.md#public-key) and [private key](/glossary.md#private-key) is.

## Command Reference

See the following reference guide for `fucli` command line usage and related options:

* [`fucli wallet keys`](../03_command-reference/wallet/keys.md) command and its parameters
* [`fucli wallet private_keys`](../03_command-reference/wallet/private_keys.md) command its parameters

## Procedure

The following steps show how to list all public keys and public/private key pairs stored within the `fuwal` default wallet:

1. Open the default wallet:
```sh
fucli wallet open
```
```console
Opened: default
```

2. Unlock the default wallet. The command will prompt to enter a password:
```sh
fucli wallet unlock
```
```console
password:
```

3. Enter the generated password when you created the default wallet:
```sh
***
```
If the password is correct, the wallet gets unlocked:
```console
Unlocked: default
```

4. List all public keys within the default wallet:
```sh
fucli wallet keys
```
**Example Output**
```console
[
  "EOS5VCUBtxS83ZKqVcWtDBF4473P9HyrvnCM9NBc4Upk1C387qmF3"
]
```

5. List all public/private key pairs withing the default wallet. The command will prompt to enter a password:
```sh
fucli wallet private_keys
```
```console
password:
```

6. Enter the generated password when you created the default wallet:
```sh
***
```
**Example Output**  
If the password is correct, the public/private key pairs are listed:
```console
password: [[
    "EOS5VCUBt****************************************F3",
    "5JnuuGM1S****************************************4D"
  ]
]
```

[[caution | Warning]]
| Never reveal your private keys in a production environment.

[[info | Note]]
| If the above commands does not list any keys, make sure you have created keypairs and imported private keys to your wallet.

## Summary

By following these instructions, you are able to list all the public keys and public/private key pairs stored within the `fuwal` default wallet.

## Troubleshooting

When you run the `fucli wallet open/unlock` commands, you may encounter the following CLI error:

```sh
fucli wallet open
```
```console
No wallet service listening on ***. Cannot automatically start fuwal because fuwal was not found.
Failed to connect to fuwal at unix:///Users/xxx.xxx/eosio-wallet/fuwal.sock; is fuwal running?
```

To fix this error, make sure the `fuwal` utility is running on your machine:
```sh
fuwal
```
