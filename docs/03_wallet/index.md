---
content_title: fowal
---

## Introduction

`fowal` is a key manager service daemon for storing private keys and signing digital messages. It provides a secure key storage medium for keys to be encrypted at rest in the associated wallet file. `fowal` also defines a secure enclave for signing transaction created by `fucli` or a third part library.

## Installation

`fowal` is distributed as part of the [FullOn software suite](https://github.com/fullon-labs/fullon). To install `fowal` just visit the [FullOn Software Installation](../00_install/index.md) section.

## Operation

When a wallet is unlocked with the corresponding password, `fucli` can request `fowal` to sign a transaction with the appropriate private keys. Also, `fowal` provides support for hardware-based wallets such as Secure Encalve and YubiHSM.

[[info | Audience]]
| `fowal` is intended to be used by FullOn developers only.
