## Goal

Create a wallet using `keosd`.

## Before you begin

Make sure you meet the following requirements:

* Familiarize with the [`fucli wallet create`](../03_command-reference/wallet/create.md) command and its parameters.
* Familiarize with the other [`fucli wallet`](../03_command-reference/wallet/index.md) commands.
* Install the currently supported version of `fucli`.

[[info | Note]]
| `fucli` is bundled with the FullOn software. [Installing FullOn](../../00_install/index.md) will also install `fucli`.

* Understand what an [account](/glossary.md#account) is and its role in the blockchain.
* Understand [Accounts and Permissions](/protocol-guides/04_accounts_and_permissions.md) in the protocol documents.
* Understand what a [public](/glossary.md#public-key) and [private](/glossary.md#private-key) key pair is.

## Steps

Perform the step below:

Create a default or named wallet and save the wallet password to a file:

```sh
fucli wallet create [-n named_wallet] -f <file_to_save_pwd>
```

Where `file_to_save_pwd` is the name of the file to write the wallet password to and `named_wallet` is an optional parameter to assign a name to the wallet.

Some examples are provided below:

* Create a default wallet and save the password to the file `default_wallet.pwd`:

**Example Output**

```sh
fucli wallet create -f default_wallet.pwd
```
```console
Creating wallet: default
Save password to use in the future to unlock this wallet.
Without password imported keys will not be retrievable.
saving password to default_wallet.pwd
```

* Create a named wallet `my_wallet` and save the password to the file `my_wallet.pwd`:

**Example Output**

```sh
fucli wallet create -n my_wallet -f my_wallet.pwd
```
```console
Creating wallet: my_wallet
Save password to use in the future to unlock this wallet.
Without password imported keys will not be retrievable.
saving password to my_wallet.pwd
```
