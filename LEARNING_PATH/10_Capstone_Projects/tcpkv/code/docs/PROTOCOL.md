# tcpkv wire protocol, version 1

## Transport

TCP. One connection carries any number of requests. Requests may be pipelined: the client may
send several before reading any reply; replies come back in order, one per request. The
server never sends anything unprompted.

## Encoding

All text is ASCII. Lines end with `\n`; a `\r` immediately before the `\n` is ignored.
A value (the payload of `SET` and the body of a bulk reply) is raw bytes and may contain
anything, including `\n` and `\0`; it is always preceded by its exact length.

## Requests

```
request   := inline '\n'
           | "SET" SP key SP length '\n' bytes '\n'      ; bytes is exactly `length` octets
inline    := command { SP arg }
SP        := one or more ' ' or '\t'
command   := case-insensitive name: PING GET SET DEL EXISTS KEYS EXPIRE TTL STATS QUIT
key       := 1..256 octets in 0x21..0x7E
length    := decimal 0..1048576
seconds   := decimal, optional leading '-'
```

| request                   | arguments            | reply                                   |
|---------------------------|----------------------|-----------------------------------------|
| `PING`                    | none                 | `+PONG`                                 |
| `GET key`                 | 1 key                | bulk value, or `$-1` if absent          |
| `SET key length` + bytes  | key, length, value   | `+OK`                                   |
| `DEL key...`              | 1..64 keys           | `:n` keys removed                       |
| `EXISTS key...`           | 1..64 keys           | `:n` keys present                       |
| `KEYS [prefix]`           | 0 or 1 prefix        | array of bulk, sorted ascending         |
| `EXPIRE key seconds`      | key, integer         | `:1` set, `:0` key absent; `<= 0` deletes |
| `TTL key`                 | 1 key                | `:s` seconds left, `:-1` no TTL, `:-2` absent |
| `STATS`                   | none                 | bulk: `name value\n` lines              |
| `QUIT`                    | none                 | `+BYE`, then the server closes          |

Blank lines are ignored.

## Replies

```
reply     := '+' text '\n'               ; status
           | "-ERR " text '\n'           ; error
           | ':' integer '\n'            ; integer (may be negative)
           | '$' length '\n' bytes '\n'  ; bulk
           | "$-1" '\n'                  ; null bulk
           | '*' count '\n' { bulk }     ; array of bulk strings
```

## Errors

Non-fatal (the connection stays open, the offending line is skipped):

* `unknown command 'X'`
* `wrong number of arguments for X`, `X takes no arguments`
* `invalid key`, `invalid prefix`, `invalid value length`, `invalid expire seconds`
* `too many arguments` (more than 64)

Fatal (the server sends the error, then closes):

* `value too large` (length > 1048576)
* `line too long` (4096 bytes without a newline)
* `value not terminated by newline` (byte after the value is not `\n` or `\r\n`)

Fatal errors exist because after them the server cannot know where the next request starts.

## Limits

| limit            | value       |
|------------------|-------------|
| key length       | 1..256 B    |
| value length     | 0..1 MiB    |
| line length      | 4096 B      |
| arguments        | 64          |
| EXPIRE seconds   | -3153600000..3153600000 (100 years) |

## Example session

```
C: PING
S: +PONG
C: SET greeting 11
C: hello world
S: +OK
C: GET greeting
S: $11
S: hello world
C: GET nothing
S: $-1
C: KEYS gr
S: *1
S: $8
S: greeting
C: FOO
S: -ERR unknown command 'FOO'
C: QUIT
S: +BYE
```
