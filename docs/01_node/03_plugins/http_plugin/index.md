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

## Dependencies

None
