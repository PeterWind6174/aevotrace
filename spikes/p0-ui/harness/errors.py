"""Dependency-free exception types shared by the legacy and engine GUI paths.

Keeping these out of guest_pe.py prevents the engine integration path from
transitively importing Unicorn/pefile just to construct the backends.
"""
from __future__ import annotations


class GuestExitThread(Exception):
    """Guest called ExitThread(); this unwinds the legacy spike runner."""


class UnsupportedApi(RuntimeError):
    """An API/message outside the declared P0 subset was reached."""


class GuestFault(RuntimeError):
    """The legacy guest runner stopped for a reason other than normal return."""
