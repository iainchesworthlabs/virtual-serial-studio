# Network addresses

Under the choices in the dialogs, every endpoint is an address. The engine
accepts these forms, and you can type them in Advanced and in `vss_cli`:

| Address | Meaning |
|---|---|
| `serial://COM3?baud=115200` | A real serial device with framing options. |
| `tcp://host:port` | A TCP client: Studio connects to a server. |
| `udp://host:port` | UDP datagrams sent to and received from one address. |
| `tcp-listen://host:port` | A TCP server: Studio waits for other computers. Up to 32 can connect; what you send goes to all of them. |
| `udp-listen://host:port` | UDP datagrams from any sender; replies go to the senders heard from lately. |
| `rfc2217://host:port` | RFC 2217 serial-over-network: Studio connects to an access server. |
| `rfc2217-listen://host:port` | An RFC 2217 access server: Studio waits for serial-over-network clients such as pySerial's `rfc2217://`. |

An in-process loopback port reports its path as `loopback:<name>`.

An RFC 2217 server speaks Telnet with the COM-PORT option, so a client opens
it as a serial port. Data passes through as it would for TCP. The client's
DTR and RTS are the endpoint's output lines, so a linked port sees them as
DSR, CTS and DCD, and the CTS, DSR, DCD and RI that reach the endpoint are
sent to the client as modem state.

## An RFC 2217 client

`rfc2217://host:port` is Studio's own RFC 2217 client. It makes the connection
when the port is made, and it reports an address that cannot be reached when
you add the port. The settings go in the address after the port:
`rfc2217://lab:7001?baud=115200&data=8&parity=none&stop=1&flow=none`. Studio asks
the server to use the COM-PORT option, and once it agrees:

- The settings of the server's port are the server's to say. With no settings
  in the address, Studio asks the server for its speed, data bits, parity, stop
  bits and flow control and uses the answer, so a device on a bus that runs at
  9600 is not set to 115200. Settings in the address (`baud`, `data`, `parity`,
  `stop` or `flow`), or `settings=own`, make Studio set them on the server, and
  send them again when the port's settings change; `settings=device` asks for
  the server's own. A speed or framing that a program sets on the other side of
  the link is still sent to the server.
- DTR and RTS follow the port's. CTS, DSR, DCD and RI that the server reports
  are the port's inputs.
- A break that arrives at the port is sent as break on, a wait of the break's
  length and break off. The length on the wire is only as accurate as the
  network, so a short break can arrive longer or shorter, and a device that
  times a break closely should not be driven this way. The log says this the
  first time.
- A server that reports a break (the break bit of the line state) gives a break
  in the stream, in order with the bytes around it. RFC 2217 has no length for
  it, so it is one character time and is marked as not measured.

A server that does not offer RFC 2217 is used as a plain byte stream, and the
log says so. If the connection is lost the port stops, and a reconnect policy
opens the address again, as it does for `tcp://`. A framing, parity or overrun
error that the server reports for a byte it received badly does not
end the connection: it is counted in the port's error total and written to the
log (at most once a second), and the port stays open. The client also takes part
in the Telnet binary and suppress-go-ahead options that serial servers expect.

To let other computers connect, allow the port through the host firewall and
listen on an interface they can reach (`0.0.0.0` for all interfaces). Try
`127.0.0.1` first. Studio does not use the network unless you add an endpoint
like these, apart from the check for updates, which is off until you turn it on
(see [Updates](updates.md#updates)).

## Who may connect

A listening address can carry a list of the computers that may connect, after
the port: `?allow=` and then addresses or ranges, separated by commas.

```text
rfc2217-listen://192.168.1.10:7001?allow=192.168.1.0/24,10.0.0.5
tcp-listen://0.0.0.0:4001?allow=fd00::/8
```

A computer that is not on the list is disconnected as soon as it connects,
before it can send or receive a byte, and a UDP datagram from one is ignored.
An address alone means that computer; `a.b.c.d/n` is every address that shares
the first `n` bits. IPv4 and IPv6 both work, and an IPv4 address that arrives
written as IPv6 counts as the IPv4 one. The list is not a password and does not
encrypt anything: an address can be forged, and the data crosses the network
as it is. It keeps strangers on the same network away from a port. To use it in
the network dialog, turn on Advanced and edit the address. Nothing in a saved
setup changes: the list is part of the address.

An RFC 2217 server has no password at all. The network dialog and `vss_cli
bridge` warn when such a server can be reached from another computer and has no
`allow=` list. Listen on `127.0.0.1` when the other program is on this computer.

Every connection Studio accepts is set to send what it writes at once, so a
short reply (a reply to a poll on a serial bus, for example) is not held back
waiting for more data.

## Clients that do not keep up

A `tcp-listen` or `rfc2217-listen` port keeps what it has to send to each
client separately. While every client is more than 1 MiB behind, the port
slows the device or program that sends to it, as it would for one slow reader.
When one client has caught up to within 1 MiB, any client still further behind
is disconnected, and so is a client that takes no data for 30 seconds. In
Studio, the port's log says which address and why. A client that sends RFC 2217
requests and does not read the replies is disconnected when the replies waiting
for it pass 1 MiB. The other clients and the device are not held back. A write
counts as done when the port has accepted it for the clients, so what is still
waiting is lost when the port closes.

A port takes up to 32 clients at once. The next is closed as soon as it
connects, and the log says so (once every 10 seconds at most). If the computer
cannot accept a connection, for example because the program has used up the
open connections and files the system allows it, the port waits 100 ms and then
longer, up to 5 seconds, before it tries again, and says so in the log.

## A device on another computer

A device program on another computer can be one member of a bus. The window's
**Network endpoint** dialog bridges a real device and does not make a port to
put on a bus, so write the port in a setup file, or paste it into **Topology
JSON** and press **Apply**: an `rfc2217` port whose address is
`rfc2217-listen://`, listed as a member of the bus (a complete file is in
[scenarios](../scenarios.md#put-a-device-from-another-computer-on-the-bus)). On the
other computer open it with a serial-over-network client such as pySerial's
`rfc2217://host:port`. What to expect:

- **The member is there with or without the other computer.** With nobody
  connected, what the bus sends to it is dropped without an error and nothing
  comes from it, which is what an unplugged device looks like. The port's row
  in the ports list says "Clients connected: 2", or "No remote connected" when
  nobody has joined.
- **Time is Studio's.** Bytes are stamped once, when they pass through Studio, with
  Studio's clock. The other computer's clock is not used, so a capture shows its
  device on the same time axis as the others.
- **The network is part of the reply window.** A master that waits 200 ms for an
  answer waits for the network hop as well. On one computer the hop is under a
  millisecond; across a network add its round trip to the device's own time, and
  use a [delay on the link](timing-and-control-lines.md#delay-and-jitter-on-a-link) to rehearse it. Every
  connection Studio accepts is set to send at once, so a short reply is not held
  back for more data.

## Keeping it safe

An RFC 2217 server has no password, and nothing it carries is encrypted. The list
in [Who may connect](#who-may-connect) keeps strangers on the same network away,
but an address can be forged, and the data crosses the network as it is. Choose
the least open way that does the job:

1. **The same computer.** Listen on `127.0.0.1`. Nothing else can reach it.
2. **An SSH tunnel.** Listen on `127.0.0.1` on the Studio computer, and on the other
   computer forward a port to it, then open `rfc2217://127.0.0.1:7001` there:

   ```bash
   ssh -N -L 7001:127.0.0.1:7001 user@studio-computer
   ```

   The traffic is encrypted and authenticated by SSH, and Studio sees only a
   connection from itself.
3. **A VPN.** Listen on the address the VPN gives the Studio computer, not
   `0.0.0.0`, and add `?allow=` with the VPN's range, for example
   `rfc2217-listen://10.8.0.1:7001?allow=10.8.0.0/24`. Only members of the VPN can
   reach it.
4. **A network you control.** Listen on the interface they can reach, add
   `?allow=` with the computers that need it, and allow only that port through
   the firewall. Use this only on a network whose other users you trust.

Do not listen on `0.0.0.0` on a network you do not control, and do not make the
port reachable from the Internet. The network dialog and `vss_cli bridge` warn
when a server can be reached from another computer and has no `allow=` list.
