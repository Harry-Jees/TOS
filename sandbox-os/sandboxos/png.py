"""Dependency-free PNG writer (stdlib zlib + struct only).

The sandbox has no third-party packages, so screenshots are encoded here.
"""

from __future__ import annotations

import struct
import zlib

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def write_png(width: int, height: int, rgb: bytes, level: int = 6) -> bytes:
    """Encode raw RGB bytes (w*h*3) as a truecolour PNG."""
    expected = width * height * 3
    if len(rgb) != expected:
        raise ValueError(f"expected {expected} bytes of RGB, got {len(rgb)}")
    if not (0 < width <= 4096 and 0 < height <= 4096):
        raise ValueError("invalid dimensions")

    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += rgb[y * stride : (y + 1) * stride]

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + _chunk(b"IHDR", ihdr)
        + _chunk(b"IDAT", zlib.compress(bytes(raw), level))
        + _chunk(b"IEND", b"")
    )
