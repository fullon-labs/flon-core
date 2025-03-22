---
content_title: Fuwal Troubleshooting
---

## How to solve the error "Failed to lock access to wallet directory; is another `fuwal` running"?

Since `fucli` may auto-launch an instance of `fuwal`, it is possible to end up with multiple instances of `fuwal` running. That can cause unexpected behavior or the error message above.

To fix this issue, you can terminate all running `fuwal` instances and restart `fuwal`. The following command will find and terminate all instances of `fuwal` running on the system:

```sh
pkill fuwal
```
