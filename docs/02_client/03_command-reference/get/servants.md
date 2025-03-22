## Description
Retrieve accounts which are servants of a given account

## Info

**Command**

```sh
fucli get servants
```
**Output**

```console
Usage: fucli get servants account

Positionals:
  account TEXT                The name of the controlling account
```

## Command

```sh
fucli get servants inita
```

## Output

```json
{
  "controlled_accounts": [
    "tester"
  ]
}
```
