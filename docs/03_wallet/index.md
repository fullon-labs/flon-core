---
content_title: fuwal
---

## Introduction

`fuwal` is a key manager service daemon for storing private keys and signing digital messages. It provides a secure key storage medium for keys to be encrypted at rest in the associated wallet file. `fuwal` also defines a secure enclave for signing transaction created by `fucli` or a third part library.

## Installation

`fuwal` is distributed as part of the [FullOn software suite](https://github.com/fullon-labs/fullon). To install `fuwal` just visit the [FullOn Software Installation](../00_install/index.md) section.

## Operation

When a wallet is unlocked with the corresponding password, `fucli` can request `fuwal` to sign a transaction with the appropriate private keys. Also, `fuwal` provides support for hardware-based wallets such as Secure Encalve and YubiHSM.

[[info | Audience]]
| `fuwal` is intended to be used by FullOn developers only.
