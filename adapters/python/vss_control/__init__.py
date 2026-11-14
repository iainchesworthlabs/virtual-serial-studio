# SPDX-License-Identifier: GPL-3.0-or-later
"""A client of the Virtual Serial Studio control gateway (`vss_gate`).

    from vss_control import Client, ControlError, EngineError

    with Client.connect() as client:
        print(client.hello["principal"])

Only the standard library is used, and only the local pipe or socket of this
user.  See README.md.
"""

from ._client import Client
from ._errors import ControlError, EngineError, TransportError

__all__ = ["Client", "ControlError", "EngineError", "TransportError"]
__version__ = "0.1.0"
