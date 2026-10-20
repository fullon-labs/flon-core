# FLON Tools

This package contains command-line utilities only. It does not include a
running node service or any wallet/private-key data.

## Programs

- `fucli`: query the chain, prepare transactions, and submit signed actions.
- `fuwal`: local wallet and signing service.
- `flon-util`: block-log, snapshot, chain, and BLS maintenance commands.
- `funod-state-inspect`: inspect node state without starting `funod`.
- `funod-state-checkpoint`: create and verify state checkpoint metadata
  (requires Python 3).

Keep the `fuwal` wallet directory outside this package and mount or configure
it explicitly. Never place private keys in an image or release archive.
