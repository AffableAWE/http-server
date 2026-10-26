"""Small raw-socket HTTP/1.1 response reader for integration tests."""

from __future__ import annotations

import socket


class HttpResponseReader:
    """Reads framed responses while preserving bytes for the next response."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buffer = bytearray()

    def read_response(self) -> tuple[str, dict[str, str], bytes]:
        while b"\r\n\r\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("connection closed before response headers")
            self.buffer.extend(chunk)

        head, separator, rest = bytes(self.buffer).partition(b"\r\n\r\n")
        if not separator:
            raise AssertionError("response header boundary missing")

        lines = head.split(b"\r\n")
        status_line = lines[0].decode("latin1")
        headers: dict[str, str] = {}
        for line in lines[1:]:
            name, colon, value = line.partition(b":")
            if colon:
                headers[name.decode("latin1").strip().lower()] = (
                    value.decode("latin1").strip()
                )

        length_text = headers.get("content-length")
        if length_text is None:
            raise AssertionError("response has no Content-Length")
        try:
            length = int(length_text)
        except ValueError as error:
            raise AssertionError(f"invalid Content-Length {length_text!r}") from error

        while len(rest) < length:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("connection closed before response body completed")
            rest += chunk

        body = rest[:length]
        self.buffer = bytearray(rest[length:])
        return status_line, headers, body

    def read_eof(self) -> bytes:
        """Returns bytes already buffered plus any unread connection data."""
        data = bytes(self.buffer)
        self.buffer.clear()
        while True:
            try:
                chunk = self.sock.recv(65536)
            except (socket.timeout, ConnectionResetError):
                break
            if not chunk:
                break
            data += chunk
        return data
