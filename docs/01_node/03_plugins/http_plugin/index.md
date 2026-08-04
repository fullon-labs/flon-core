## Description

The `http_plugin` is a core plugin supported by both `funod` and `fuwal`. The plugin is required to enable any RPC API functionality provided by a `funod` or `fuwal` instance.

## Usage

```console
# config.ini
plugin = eosio::http_plugin
[options]
```
```sh
# command-line
funod ... --plugin eosio::http_plugin [options]
 (or)
fuwal ... --plugin eosio::http_plugin [options]
```

## Options

These can be specified from both the command-line or the `config.ini` file:

```console
Config Options for eosio::http_plugin:
  --unix-socket-path arg                The filename (relative to data-dir) to
                                        create a unix socket for HTTP RPC; set
                                        blank to disable.
  --http-server-address arg (=127.0.0.1:8888)
                                        The local IP and port to listen for
                                        incoming http connections; set blank to
                                        disable.
  --access-control-allow-origin arg     Specify the Access-Control-Allow-Origin
                                        to be returned on each request
  --access-control-allow-headers arg    Specify the Access-Control-Allow-Header
                                        s to be returned on each request
  --access-control-max-age arg          Specify the Access-Control-Max-Age to
                                        be returned on each request.
  --access-control-allow-credentials    Specify if Access-Control-Allow-Credent
                                        ials: true should be returned on each
                                        request.
  --max-body-size arg (=2097152)        The maximum body size in bytes allowed
                                        for incoming RPC requests
  --http-max-bytes-in-flight-mb arg (=500)
                                        Maximum size in megabytes http_plugin
                                        should use for processing http
                                        requests. -1 for unlimited. 503 error
                                        response when exceeded.
  --http-max-in-flight-requests arg (=1024)
                                        Maximum number of requests http_plugin
                                        should use for processing http
                                        requests. 503 error response when
                                        exceeded.
  --http-max-connections arg (=1024)    Maximum number of active TCP and Unix
                                        socket connections. -1 for unlimited.
  --http-header-timeout-ms arg (=5000) Maximum time to receive request headers.
                                        0 disables the timeout.
  --http-body-timeout-ms arg (=30000)  Maximum time to receive a request body.
                                        0 disables the timeout.
  --http-idle-timeout-ms arg (=30000)  Maximum idle time between keep-alive
                                        requests. 0 disables the timeout.
  --http-write-timeout-ms arg (=30000) Maximum time to produce and write a
                                        response. 0 disables the timeout.
  --http-max-history-requests-in-flight arg (=4)
                                        Maximum number of transaction-history
                                        requests queued or executing.
  --http-history-requests-per-second arg (=5)
                                        Shared token-bucket rate limit for
                                        transaction-history endpoints.
  --http-max-response-time-ms arg (=15) Maximum time for processing a request,
                                        -1 for unlimited
  --verbose-http-errors                 Append the error log to HTTP responses
  --http-validate-host arg (=1)         If set to false, then any incoming
                                        "Host" header is considered valid
  --http-alias arg                      Additionaly acceptable values for the
                                        "Host" header of incoming HTTP
                                        requests, can be specified multiple
                                        times.  Includes http/s_server_address
                                        by default.
  --http-threads arg (=2)               Number of worker threads in http thread
                                        pool
  --http-keep-alive arg (=1)            If set to false, do not keep HTTP
                                        connections alive, even if client
                                        requests.
```

Unix sockets are created with mode `0600`. Their parent directory must be
accessible only by the node user (for example, mode `0700`); startup refuses an
existing group- or world-accessible parent. A newly created parent directory is
made private automatically.

When category-based listeners are enabled, signing endpoints use the dedicated
`sign_transaction` category. Configure that category only on a trusted Unix
socket; the endpoints also enforce the Unix-socket requirement at request time.
Do not include `sign_transaction` on a TCP listener.

```console
plugin = eosio::sign_transaction_api_plugin
http-server-address = http-category-address
http-category-address = chain_ro,127.0.0.1:8888
http-category-address = sign_transaction,./sign-transaction.sock
```

## Dependencies

None
