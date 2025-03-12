## Overview

This how-to guide provides instructions on how to create a new FullOn blockchain account using the `focli` CLI tool. You can use accounts to deploy smart contracts and perform other related blockchain operations. Create one or multiple accounts as part of your development environment setup.

The example in this how-to guide creates a new account named **bob**, authorized by the default system account **eosio**, using the `focli` CLI tool.

## Before you Begin

Make sure you meet the following requirements:

* Install the currently supported version of `focli`.
[[info | Note]]
| The focli tool is bundled with the FullOn software. [Installing FullOn](../../00_install/index.md) will also install the focli tool.
* Learn about [FullOn Accounts and Permissions](/protocol-guides/04_accounts_and_permissions.md)
* Learn about Asymmetric Cryptography - [public key](/glossary.md#public-key) and [private key](/glossary.md#private-key) pairs.
* Create public/private keypairs for the `owner` and `active` permissions of an account.

## Command Reference

See the following reference guide for `focli` command line usage and related options:
* [`focli create account`](../03_command-reference/create/account.md) command and its parameters

## Procedure

The following step shows how to create a new account **bob** authorized by the default system account **flonian**.

1. Run the following command to create the new account **bob**:

```sh
focli create account flonian bob EOS87TQktA5RVse2EguhztfQVEh6XXxBmgkU8b4Y5YnGvtYAoLGNN
```
**Where**:
* `flonian` = the system account that authorizes the creation of a new account
* `bob` = the name of the new account conforming to [account naming conventions](/protocol-guides/04_accounts_and_permissions.md#2-accounts)
* `FU87TQ...AoLGNN` = the owner public key or permission level for the new account (**required**)
[[info | Note]]
| To create a new account in the FullOn blockchain, an existing account, also referred to as a creator account, is required to authorize the creation of a new account. For a newly created FullOn blockchain, the default system account used to create a new account is **flonian**.

**Example Output**

```console
executed transaction: 4d65a274de9f809f9926b74c3c54aadc0947020bcfb6dd96043d1bcd9c46604c  200 bytes  166 us
#         eosio <= eosio::newaccount            {"creator":"flonian","name":"bob","owner":{"threshold":1,"keys":[{"key":"FU87TQktA5RVse2EguhztfQVEh6X...
warning: transaction executed locally, but may not be confirmed by the network yet         ]
```

### Summary

By following these instructions, you are able to create a new FullOn account in your blockchain environment.
