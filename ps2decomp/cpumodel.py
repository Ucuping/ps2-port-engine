"""Choices about the CPU model that a game makes, for the interpreter (emu.py)
and the lifter (lift.py) alike - set from its game.json as it loads
(game.load), as the memory layout is (layout.py).

* `cop0_status`: COP0's Status register as a thread sees it, rather than 0:
  IE (bit 0) set, and EIE (bit 16) set until `di` and again after `ei`; every
  other bit, and every other COP0 register, still reads 0. Off by default: it
  changes what every game's code sees, and a game turns it on once it is
  checked on that game (Tekken 5's thread start asks whether interrupts are
  enabled before it starts a thread).
"""

from __future__ import annotations

import dataclasses


@dataclasses.dataclass
class Model:
    cop0_status: bool = False


CURRENT = Model()


def configure(**values) -> Model:
    """Set the model's choices for everything that runs afterwards."""
    for name, value in values.items():
        if not hasattr(CURRENT, name):
            raise KeyError("no CPU model choice %r" % name)
        setattr(CURRENT, name, value)
    return CURRENT


# Status as a thread sees it with the model on: IE, and EIE unless `di` left
# it clear (`cleared` is that: the context's `di` word).
STATUS = 12
STATUS_IE, STATUS_EIE = 0x1, 0x10000


def status(cleared: int) -> int:
    return STATUS_IE | (0 if cleared else STATUS_EIE)
