"""Backward-compatible wrapper exporting TashkeelEngine and BarqEngine."""

from barq_py import BarqEngine

TashkeelEngine = BarqEngine

__all__ = ["BarqEngine", "TashkeelEngine"]
