# SPDX-License-Identifier: GPL-3.0-or-later
"""The errors a Client raises.

ControlError is an error the gateway sent: `code` is its wire code
(`forbidden`, `timeout`, `not_found`, ...), `message` its words and
`retryable` its flag.  EngineError is a ControlError whose code starts with
`engine.`: the engine refused the request.

TransportError is a ControlError too, for what went wrong on this side:
there is no gateway, it runs as someone else, the connection is gone, a
call took too long or the gateway broke the protocol.  Its codes are not on
the wire:

    not_running       nothing listens at the address
    not_same_user     what listens there does not run as this user
    peer_check_failed this user could not be compared with the server's
    address_too_long  the socket path does not fit (Unix)
    busy              every instance of the pipe stayed busy (Windows)
    client_timeout    a call took longer than the client's timeout; the
                      connection is closed
    connection_closed the connection is closed or went away
    protocol_error    the gateway sent something that is not the protocol;
                      the connection is closed
    io_error          the operating system refused an operation

A token is never part of an error: no message, `repr` or attribute here is
ever built from one.
"""

from __future__ import annotations


class ControlError(Exception):
    """An error with a wire code.  See the module text."""

    def __init__(self, code, message="", retryable=False):
        # All three are in `args`, so a copy or a pickle keeps them.
        super().__init__(code, message, retryable)
        self.code = code
        self.message = message
        self.retryable = bool(retryable)

    def __str__(self):
        if self.message:
            return "%s: %s" % (self.code, self.message)
        return str(self.code)

    def __repr__(self):
        return "%s(code=%r, message=%r, retryable=%r)" % (
            type(self).__name__,
            self.code,
            self.message,
            self.retryable,
        )


class EngineError(ControlError):
    """The engine refused the request; the code is `engine.<name>`."""


class TransportError(ControlError):
    """A failure on this side of the connection.  See the module text."""


def error_from_reply(code, message, retryable):
    """The exception for an error reply of the gateway."""
    if code.startswith("engine."):
        return EngineError(code, message, retryable)
    return ControlError(code, message, retryable)
