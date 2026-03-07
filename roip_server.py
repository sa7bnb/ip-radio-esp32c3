#!/usr/bin/env python3
"""
roip_server.py  –  Simple UDP relay for ESP32 walkie-talkie
============================================================
No users, no passwords.
One client transmits → all others hear.

Protocol (binary, UDP only):
  Header 6 bytes:
    [0-1]  Magic    0xA5 0x7B
    [2]    Type     0x01=HELLO  0x02=AUDIO  0x03=BYE  0x04=PING
    [3]    ClientID (assigned by server, 0 on HELLO)
    [4-5]  SeqNum   (uint16 BE, increments per packet)
  + Payload (PCM 16-bit mono 8 kHz for AUDIO packets)

Start: python3 roip_server.py
"""

import asyncio
import struct
import time
import logging

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S"
)
log = logging.getLogger("roip")

# ── Configuration ───────────────────────────────────────────
UDP_HOST       = "0.0.0.0"
UDP_PORT       = 12345
CLIENT_TIMEOUT = 30        # seconds without packet → forget client
MAX_CLIENTS    = 20
LOG_AUDIO      = False     # True = log every audio packet (spammy)

# ── Protocol constants ──────────────────────────────────────
MAGIC          = b'\xA5\x7B'
TYPE_HELLO     = 0x01
TYPE_AUDIO     = 0x02
TYPE_BYE       = 0x03
TYPE_PING      = 0x04
TYPE_PONG      = 0x05
HEADER_SIZE    = 6

def make_header(ptype: int, client_id: int, seq: int) -> bytes:
    return MAGIC + struct.pack(">BBH", ptype, client_id, seq)

# ── Client registry ─────────────────────────────────────────
class Client:
    def __init__(self, cid: int, addr: tuple):
        self.cid       = cid
        self.addr      = addr   # (ip, port)
        self.last_seen = time.monotonic()
        self.rx_seq    = 0
        self.name      = f"ESP{cid}"

    def touch(self):
        self.last_seen = time.monotonic()

    def alive(self):
        return (time.monotonic() - self.last_seen) < CLIENT_TIMEOUT

# ── UDP Server ──────────────────────────────────────────────
class RoIPServer(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport   = None
        self.clients     = {}      # cid -> Client
        self.addr_to_cid = {}      # (ip,port) -> cid
        self.next_cid    = 1
        self.relay_seq   = 0

    def connection_made(self, transport):
        self.transport = transport
        log.info(f"Server listening on UDP {UDP_HOST}:{UDP_PORT}")

    def datagram_received(self, data: bytes, addr: tuple):
        # Check minimum size + magic
        if len(data) < HEADER_SIZE:
            return
        if data[0:2] != MAGIC:
            return

        ptype     = data[2]
        client_id = data[3]
        seq       = struct.unpack(">H", data[4:6])[0]
        payload   = data[HEADER_SIZE:]

        if ptype == TYPE_HELLO:
            self._handle_hello(addr)

        elif ptype == TYPE_AUDIO:
            self._handle_audio(addr, payload)

        elif ptype == TYPE_PING:
            self._handle_ping(addr, client_id)

        elif ptype == TYPE_BYE:
            self._handle_bye(addr)

    def _get_or_create(self, addr) -> Client | None:
        """Return existing client or None if unknown."""
        return self.clients.get(self.addr_to_cid.get(addr))

    def _handle_hello(self, addr):
        existing = self._get_or_create(addr)
        if existing:
            existing.touch()
            # Send back the same ID
            pong = make_header(TYPE_PONG, existing.cid, 0) + \
                   struct.pack(">B", existing.cid)
            self.transport.sendto(pong, addr)
            return

        if len(self.clients) >= MAX_CLIENTS:
            log.warning(f"Max clients ({MAX_CLIENTS}) reached, rejecting {addr}")
            return

        cid = self.next_cid
        self.next_cid += 1
        client = Client(cid, addr)
        self.clients[cid] = client
        self.addr_to_cid[addr] = cid

        # Reply with assigned ID
        pong = make_header(TYPE_PONG, cid, 0) + struct.pack(">B", cid)
        self.transport.sendto(pong, addr)

        log.info(f"+ New client #{cid} {addr[0]}:{addr[1]}  "
                 f"(total: {len(self.clients)})")
        self._cleanup()

    def _handle_audio(self, addr, payload: bytes):
        client = self._get_or_create(addr)
        if not client:
            # Unknown client – send HELLO prompt
            self.transport.sendto(make_header(TYPE_PONG, 0, 0) +
                                  struct.pack(">B", 0), addr)
            return

        client.touch()

        if not payload:
            return

        if LOG_AUDIO:
            log.debug(f"Audio #{client.cid} {len(payload)} bytes")

        # Relay to all other active clients
        self.relay_seq = (self.relay_seq + 1) & 0xFFFF
        out_hdr = make_header(TYPE_AUDIO, client.cid, self.relay_seq)
        packet  = out_hdr + payload

        dead = []
        for cid, c in self.clients.items():
            if cid == client.cid:
                continue
            if not c.alive():
                dead.append(cid)
                continue
            self.transport.sendto(packet, c.addr)

        for cid in dead:
            self._remove(cid)

    def _handle_ping(self, addr, client_id: int):
        client = self._get_or_create(addr)
        if client:
            client.touch()
        pong = make_header(TYPE_PONG, client_id, 0)
        self.transport.sendto(pong, addr)

    def _handle_bye(self, addr):
        cid = self.addr_to_cid.get(addr)
        if cid:
            log.info(f"- Client #{cid} sent BYE")
            self._remove(cid)

    def _remove(self, cid: int):
        client = self.clients.pop(cid, None)
        if client:
            self.addr_to_cid.pop(client.addr, None)
            log.info(f"- Client #{cid} removed (total: {len(self.clients)})")

    def _cleanup(self):
        """Remove clients that have not been heard from."""
        dead = [cid for cid, c in self.clients.items() if not c.alive()]
        for cid in dead:
            log.info(f"- Client #{cid} timed out")
            self._remove(cid)

    def error_received(self, exc):
        log.error(f"UDP error: {exc}")

# ── Periodic housekeeping + status ──────────────────────────
async def housekeeping(server: RoIPServer):
    while True:
        await asyncio.sleep(10)
        server._cleanup()
        if server.clients:
            ids = ", ".join(f"#{c.cid} {c.addr[0]}"
                            for c in server.clients.values())
            log.info(f"Active clients: {ids}")
        else:
            log.info("No clients connected")

# ── Main ────────────────────────────────────────────────────
async def main():
    log.info("=== RoIP Server starting ===")
    loop = asyncio.get_running_loop()

    server_proto = RoIPServer()
    transport, _ = await loop.create_datagram_endpoint(
        lambda: server_proto,
        local_addr=(UDP_HOST, UDP_PORT)
    )

    try:
        await housekeeping(server_proto)
    finally:
        transport.close()

if __name__ == "__main__":
    asyncio.run(main())
