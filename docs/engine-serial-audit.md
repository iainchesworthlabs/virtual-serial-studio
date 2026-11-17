# Baud rate, framing and break through the engine

This page records what a serial program's settings, a break and the timing of its bytes do on the way from the Windows
COM port driver through the engine to the other ports, the network and the monitor. It was read from the source in
November 2026 (release candidate 7). It is an audit, not a design: the last section lists what DMX512, LIN and Modbus RTU
would need that is not there.

## Rates

Studio takes a line rate from 50 to 12000000 baud in the windows, and 50 to 40000000 in the engine
(`libs/vss_core/src/serial_settings.cpp:71`, the JSON reader in `libs/vss_config/src/document.cpp:223`, the RFC 2217
server in `libs/vss_ports/src/rfc2217.cpp:43`). The line rate of a half-duplex bus is 50 to 12000000 bit/s
(`max_bus_bit_rate`, `libs/vss_core/include/vss/core/link.hpp:31`). 12000000 is the highest rate a common USB serial
adapter offers, so the windows stop there; the engine keeps its wider limit so an older file still opens.

The system and the device decide whether a rate works. macOS refuses 921600 on a pseudo terminal, and an adapter may stop
at a lower rate; the refusal comes back when the rate is applied.

## What survives

| Question | Result | Evidence |
|---|---|---|
| (a) A non-standard rate, as the program set it (250000, 31250, 19200, 12000000) | Works on the Windows driver, the engine and the RFC 2217 server. Partly on Linux and macOS pseudo terminals | The driver stores any non-zero rate and has no ceiling (`drivers/windows/vsscom/io.c:901`, `SERIAL_BAUD_USER` at `io.c:818`) and gives it to the hub as `BaudRate` (`hub.c:177`). The engine reads it back as a `line_format` (`vcom_driver_windows.cpp:736`). The RFC 2217 server keeps the rate a client sets and answers with it (`rfc2217.cpp:373`). A POSIX pseudo terminal names only the rates up to 230400 and reports a faster one as unknown, so the GUI shows no "Program set" for it (`virtual_port_posix.cpp:42`, `:68`) |
| (a) Is the rate passed on to the other end of a link | Missing | A link moves bytes and lines. It never sets the rate or framing of the far port. The GUI only warns when the two ends differ ("Differs from X in baud rate or framing") |
| (b) Data bits 5 to 8, parity none, odd, even, mark, space, stop bits 1, 1.5, 2 from the program | Works on Windows driver ports; partly elsewhere | The driver keeps the line control as set (`io.c:927`) and the hub gets all three (`hub.c:178`), mapped at `vcom_driver_windows.cpp:736`. A Linux pseudo terminal forces 8N whatever the program asks (`virtual_port_posix.cpp:103`, `pty_keeps_framing`), and mark and space parity read back as odd or even (`virtual_port_posix.cpp:66`). macOS keeps the framing |
| (b) The same to an RFC 2217 peer | Works for a client that sets them, missing the other way | The server takes SET_DATASIZE, SET_PARITY (including mark and space) and SET_STOPSIZE with 1.5 (`rfc2217.cpp:388` to `:434`) and shows them as the program's format. Studio never sends them to a client: there is no RFC 2217 client in Studio, only the listener (`listen_transport.cpp`). A Windows program's settings are not forwarded to a network peer |
| (b) Shown to the user | Works | "Program set 19200 7E2" in the ports list and inspector (`engine_studio_backend`, row "Program speed" in `gui-design/docs/08-engine-gaps.md`) |
| (c) BREAK held by the program (SetCommBreak, ClearCommBreak) reaches the engine | Partly | The driver records a level, on or off (`io.c:1152`), and raises its state generation. The hub reads it as `Break` (`hub.c:183`, `vcom_driver_windows.cpp:692`). The driver does not time it: there is no duration and no stamp |
| (c) Break is a separate event with a time stamp | Partly | It is one bit of the control lines (`control_lines.hpp:31`) and the endpoint emits a `lines` event stamped by the hub clock when the state is read (`endpoint.cpp:868`). It is not a data byte, but the stamp is the time the hub looked, and a break that goes on and off between two reads of the state is seen as no change |
| (c) A link or bus delivers it to the other ports | Missing | `modem_wiring::apply` maps DTR and RTS to the peer's inputs and ignores BREAK (`control_lines.hpp:126`). `route_lines` forwards only that (`hub.cpp:1660` to `:1693`). Neither a link nor a bus carries it, and no serial transport sends one |
| (c) RFC 2217 SET_CONTROL break on and off | Works in, missing out | A client's break on (5) and off (6) become the listener's BREAK output (`rfc2217.cpp:330`, `listen_transport.cpp:844`), and the log shows it. It goes no further (see above). A break received from a program or a device is never sent to a client: there is no NOTIFY_LINESTATE, and the modem state notice has no break bit (`rfc2217.cpp:37`) |
| (c) Break received from a real device | Partly | A break on a real port is an error (`break_detected`, `serial_transport.cpp:326`), logged as an error and not forwarded |
| (c) Monitor and recording | Partly | The Event log and the monitor list `lines` rows and the Event log writes "BREAK" while it is held (`control_lines.cpp:61`, `apps/vss_gui/src/advanced/event_log.cpp:388`). A capture file holds data only (`libs/vss_studio/src/capture.cpp:691`) |
| (d) Anything paces data to the baud rate | Only on a half-duplex bus | A plain link, a split and a join deliver as fast as the data is written. A bus with `half_duplex` holds a message for `10 × bytes ÷ bit_rate` and reports overlaps (`bus_line.cpp:21`). The bus counts ten bits a byte whatever the framing, and uses its own rate, not the ports'. The driver's `vsscom_tx_time` (`core/vsscom_core.c:428`) counts the real framing but is used only for RTS toggling (`io.c:553`). `delay_ms` and `jitter_ms` are timers, not a model of the baud rate |
| (e) Per-chunk time stamps | Partly | A chunk is stamped with `steady_clock` in nanoseconds when the hub reads it (`hub.cpp:1423`). The driver puts program writes into one ring with no stamp of their own (`hub.c:693`), so the gaps between writes made close together are lost, and the stamp is the hub's read time, not the program's write time. A capture file keeps whole milliseconds (`capture.cpp:707`) |
| (e) Can the receive side of the driver deliver a chunk split on a gap | Missing | The hub writes into a receive ring (`hub.c:581`) and the program's read takes what is there, so two chunks that arrive apart are one read if the program reads late. The driver has a read timing plan (`vsscom_plan_read`) for the program's interval timeout but nothing that pays out bytes at the times they were sent |

No clear bug was found, so no code was changed for the audit.

## What DMX512, LIN and Modbus RTU would need

All three need the data path to keep time. Studio keeps the byte values and the order.

**DMX512** (250000 baud, 8N2, a break of at least 88 µs, a mark after break, a start code, then up to 512 slots):

- A break as an event with a start time and a length, from the driver and from a real port, and delivered across a link and a
  bus to the other ports. Today it is a level the links ignore.
- A receiver port that hears the break as the start of a frame, in order with the data around it.
- The rate and framing of the sender applied to the receiver, or a stated "not the same" in the log.
- The program's 2 stop bits kept on a Linux pseudo terminal, which forces 8N.

**LIN** (up to 20 kbit/s, a break of 13 or more bit times, then a sync byte 0x55, an identifier with parity, data and a checksum):

- The same break event, with its length in bit times.
- A break detected from the receive side as a framing error with a zero byte, as a real UART reports it, and not only as the
  `break_detected` error that stops a port.
- A slave that answers inside the response space of the frame: a gap-accurate delivery, to the microsecond, in step with
  the rate. A rule in `response-timing.md` can delay a reply, but the bytes of a frame are not spaced at the rate.

**Modbus RTU** (a frame ends after 3.5 character times of silence, 1.5 between bytes of one frame):

- Delivery of a chunk on a gap, so that a frame is one chunk and the silence after it is real. The driver's ring and the
  hub's reads merge frames written close together and split a frame written slowly.
- A model of the wire time for a plain link, as a half-duplex bus has, with the real framing (11 bits for 8E1 and 8N2, not
  ten).
- Time stamps finer than a millisecond in recordings, so a 3.5-character gap at 115200 baud (about 330 µs) can be seen.
- Parity errors made on purpose. The fault injection covers them on a fixture and on a driver port; a frame that is
  correct on the wire but has a bad CRC needs no engine change.

The cheapest first step for all three is a `break` event of its own, with a start and a length, in `event_kind`, from the
driver (it already knows when the program called the two functions: a stamp in `IOCTL_SERIAL_SET_BREAK_ON` and `_OFF` would
do), carried by `hub::route_lines` to links and buses, and written to capture files. That changes the driver interface and
the capture format, so it needs the owner's decision.

The plan is in [break as a timestamped event](break-event-plan.md).
