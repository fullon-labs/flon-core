## Overview

This how-to guide provides instructions on how to query infomation of an FullOn account. The example in this how-to guide retrieves information of the `flonian` account.

## Before you begin

* Install the currently supported version of `fucli`

[[info | Note]]
| The fucli tool is bundled with the FullOn software. [Installing FullOn](../../00_install/index.md) will also install the fucli tool.

* Acquire functional understanding of [FullOn Accounts and Permissions](/protocol-guides/04_accounts_and_permissions.md)

## Command Reference

See the following reference guide for command line usage and related options for the fucli command:

* [`fucli get account`](../03_command-reference/get/account.md) command and its parameters

## Procedure

The following step shows how to query information of the `flonian` account:

1. Run the following command:

```sh
fucli get account flonian
```
**Where**:

* `flonian` = The name of the default system account in the FullOn blockchain.

**Example Output**

```console
created: 2018-06-01T12:00:00.000
privileged: true
permissions:
     owner     1:    1 FU6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV
        active     1:    1 FU6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV
memory:
     quota:       unlimited  used:     3.004 KiB

net bandwidth:
     used:               unlimited
     available:          unlimited
     limit:              unlimited

cpu bandwidth:
     used:               unlimited
     available:          unlimited
     limit:              unlimited
```

[[info | Account Fields]]
| Depending on the FullOn network you are connected, you might see different fields associated with an account. That depends on which system contract has been deployed on the network.
