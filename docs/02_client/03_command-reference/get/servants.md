## Description
Retrieve accounts which are servants of a given account

## Info

**Command**

```sh
focli get servants
```
**Output**

```console
Usage: focli get servants account

Positionals:
  account TEXT                The name of the controlling account
```

## Command

```sh
focli get servants inita
```

## Output

```json
{
  "controlled_accounts": [
    "tester"
  ]
}
```
