#!/usr/bin/env python3
"""
ipradio_server.py  -  IP-Radio Server v1.0
=====================================================================
What's new in v1.0:
  - NODE_NAME is the sole unique identifier for a client
  - Approved nodes are saved to ipradio_state.json and auto-approved
    on server restart without any admin action
  - Blocked IPs and rooms are also stored persistently
  - Duplicate connection (same node name, new IP/port) replaces the
    old session automatically
  - Web UI shows "Known nodes" with option to revoke access

Protocol (binary, UDP only):
  Header 6 bytes:
    [0-1]  Magic    0xA5 0x7B
    [2]    Type     0x01=HELLO  0x02=AUDIO  0x03=BYE
                    0x04=PING   0x05=PONG
                    0x06=REJECT    (server -> client)
                    0x07=ROOM_INFO (server -> client)
    [3]    ClientID (assigned by server, 0 in HELLO)
    [4-5]  SeqNum   (uint16 big-endian)

  HELLO    payload: node name as UTF-8 bytes (max 16 chars)
  PONG     payload: [client_id: 1B][room_id: 1B][room_name: UTF-8]
  REJECT   payload: [reason: 1B]
             0x01=not approved  0x02=blocked
             0x03=kicked        0x04=server full
  ROOM_INFO payload: [room_id: 1B][room_name: UTF-8]

Requirements:
  pip install aiohttp
Run:
  python3 ipradio_server.py
"""

import asyncio
import struct
import time
import logging
import json
import hashlib
import secrets
from pathlib import Path
from aiohttp import web

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S"
)
log = logging.getLogger("ipradio")

# -- Configuration -------------------------------------------------------------
UDP_HOST        = "0.0.0.0"
UDP_PORT        = 12345
WEB_HOST        = "0.0.0.0"
WEB_PORT        = 8080
CLIENT_TIMEOUT  = 30        # seconds without a packet before timeout
PENDING_TIMEOUT = 300       # seconds a pending client is kept in queue
MAX_CLIENTS     = 20
LOG_AUDIO       = False
DEFAULT_ROOM_ID = 0
STATE_FILE      = "ipradio_state.json"

# -- Authentication ------------------------------------------------------------
SESSION_COOKIE   = "ipradio_session"
SESSION_MAX_AGE  = 8 * 3600          # 8 hours
_sessions: dict  = {}                # token -> expires (monotonic)

def _hash_password(password: str, salt: str) -> str:
    return hashlib.sha256((salt + password).encode()).hexdigest()

def _new_session() -> str:
    token = secrets.token_hex(32)
    _sessions[token] = time.monotonic() + SESSION_MAX_AGE
    return token

def _valid_session(token: str | None) -> bool:
    if not token or token not in _sessions:
        return False
    if time.monotonic() > _sessions[token]:
        del _sessions[token]
        return False
    _sessions[token] = time.monotonic() + SESSION_MAX_AGE   # sliding window
    return True

def _purge_sessions():
    now = time.monotonic()
    expired = [t for t, exp in _sessions.items() if now > exp]
    for t in expired:
        del _sessions[t]

# Password stored in state file as {"password_hash": str, "password_salt": str}
_pw_hash: str = ""
_pw_salt: str = ""

def _load_password():
    global _pw_hash, _pw_salt
    p = Path(STATE_FILE)
    if not p.exists():
        return
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
        _pw_hash = data.get("password_hash", "")
        _pw_salt = data.get("password_salt", "")
    except Exception:
        pass

def _save_password(password: str):
    global _pw_hash, _pw_salt
    _pw_salt = secrets.token_hex(16)
    _pw_hash = _hash_password(password, _pw_salt)
    # Merge into existing state file
    p = Path(STATE_FILE)
    try:
        data = json.loads(p.read_text(encoding="utf-8")) if p.exists() else {}
    except Exception:
        data = {}
    data["password_hash"] = _pw_hash
    data["password_salt"] = _pw_salt
    p.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    log.info("Password updated")

# -- Protocol constants --------------------------------------------------------
MAGIC            = b'\xA5\x7B'
TYPE_HELLO       = 0x01
TYPE_AUDIO       = 0x02
TYPE_BYE         = 0x03
TYPE_PING        = 0x04
TYPE_PONG        = 0x05
TYPE_REJECT      = 0x06
TYPE_ROOM_INFO   = 0x07
HEADER_SIZE      = 6

REJECT_NOT_APPROVED = 0x01
REJECT_BLOCKED      = 0x02
REJECT_KICKED       = 0x03
REJECT_FULL         = 0x04


def make_header(ptype: int, client_id: int, seq: int) -> bytes:
    return MAGIC + struct.pack(">BBH", ptype, client_id, seq)


# -- Data structures -----------------------------------------------------------
class Room:
    def __init__(self, rid: int, name: str):
        self.rid  = rid
        self.name = name


class PendingClient:
    def __init__(self, addr: tuple, name: str):
        self.addr       = addr
        self.name       = name
        self.since      = time.monotonic()
        self.last_hello = time.monotonic()

    def waiting_str(self) -> str:
        s = int(time.monotonic() - self.since)
        return f"{s // 60}m {s % 60}s" if s >= 60 else f"{s}s"


class Client:
    def __init__(self, cid: int, addr: tuple, name: str, room_id: int = 0):
        self.cid       = cid
        self.addr      = addr
        self.name      = name
        self.room_id   = room_id
        self.last_seen = time.monotonic()

    def touch(self):
        self.last_seen = time.monotonic()

    def alive(self) -> bool:
        return (time.monotonic() - self.last_seen) < CLIENT_TIMEOUT


# -- UDP Server ----------------------------------------------------------------
class IPRadioServer(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport   = None
        self.clients     = {}    # cid  -> Client
        self.addr_to_cid = {}    # addr -> cid   (routing)
        self.name_to_cid = {}    # name -> cid   (dedup/lookup)
        self.pending     = {}    # name -> PendingClient
        self.approved    = {}    # name -> {"room_id": int}  (persistent)
        self.blocked_ips = set() # "1.2.3.4"
        self.rooms       = {0: Room(0, "General")}
        self.next_cid    = 1
        self.next_rid    = 1
        self.relay_seq   = 0
        self._sse_queues = []

        self._load_state()

    # -- Persistence -----------------------------------------------------------
    def _load_state(self):
        p = Path(STATE_FILE)
        if not p.exists():
            log.info("No saved state - starting fresh")
            return
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
            self.approved    = data.get("approved", {})
            self.blocked_ips = set(data.get("blocked_ips", []))
            self.next_rid    = data.get("next_rid", 1)
            for r in data.get("rooms", []):
                rid, name = r["rid"], r["name"]
                self.rooms[rid] = Room(rid, name)
                if rid >= self.next_rid:
                    self.next_rid = rid + 1
            log.info(f"State loaded: {len(self.approved)} approved nodes, "
                     f"{len(self.blocked_ips)} blocked IPs, "
                     f"{len(self.rooms)} rooms")
        except Exception as e:
            log.error(f"Could not read {STATE_FILE}: {e}")

    def _save_state(self):
        data = {
            "approved":    self.approved,
            "blocked_ips": sorted(self.blocked_ips),
            "next_rid":    self.next_rid,
            "rooms": [
                {"rid": r.rid, "name": r.name}
                for r in self.rooms.values() if r.rid != 0
            ],
        }
        try:
            Path(STATE_FILE).write_text(
                json.dumps(data, ensure_ascii=False, indent=2),
                encoding="utf-8"
            )
        except Exception as e:
            log.error(f"Could not save {STATE_FILE}: {e}")

    # -- SSE -------------------------------------------------------------------
    def _notify_change(self):
        state = self.get_state()
        for q in self._sse_queues:
            try:    q.put_nowait(state)
            except asyncio.QueueFull: pass

    def add_sse_queue(self, q):
        self._sse_queues.append(q)

    def remove_sse_queue(self, q):
        try:    self._sse_queues.remove(q)
        except ValueError: pass

    # -- DatagramProtocol ------------------------------------------------------
    def connection_made(self, transport):
        self.transport = transport
        log.info(f"UDP listening on {UDP_HOST}:{UDP_PORT}")

    def datagram_received(self, data: bytes, addr: tuple):
        if len(data) < HEADER_SIZE: return
        if data[0:2] != MAGIC:      return
        ptype   = data[2]
        cid     = data[3]
        payload = data[HEADER_SIZE:]

        if   ptype == TYPE_HELLO: self._handle_hello(addr, payload)
        elif ptype == TYPE_AUDIO: self._handle_audio(addr, payload)
        elif ptype == TYPE_PING:  self._handle_ping(addr, cid)
        elif ptype == TYPE_BYE:   self._handle_bye(addr)

    # -- HELLO -----------------------------------------------------------------
    def _handle_hello(self, addr: tuple, payload: bytes):
        ip = addr[0]

        if ip in self.blocked_ips:
            self._send_reject(addr, REJECT_BLOCKED)
            return

        name = payload.decode("utf-8", errors="replace").strip("\x00").strip()[:16]
        if not name:
            name = f"ESP-{ip.split('.')[-1]}"

        # Already connected with this node name?
        existing_cid = self.name_to_cid.get(name)
        if existing_cid and existing_cid in self.clients:
            c = self.clients[existing_cid]
            if c.addr == addr:
                # Same address - keepalive
                c.touch()
                self._send_pong(addr, existing_cid, c.room_id)
                return
            else:
                # New address, same node name - replace old session
                log.info(f"Reconnect '{name}': {c.addr[0]} -> {addr[0]}:{addr[1]}")
                self._remove_client(existing_cid, "replaced by new session")

        # Already pending with this node name?
        if name in self.pending:
            p = self.pending[name]
            p.addr       = addr
            p.last_hello = time.monotonic()
            return

        if len(self.clients) >= MAX_CLIENTS:
            self._send_reject(addr, REJECT_FULL)
            return

        # Known (previously approved) node -> auto-approve
        if name in self.approved:
            room_id = self.approved[name].get("room_id", DEFAULT_ROOM_ID)
            if room_id not in self.rooms:
                room_id = DEFAULT_ROOM_ID
            self._register_client(addr, name, room_id)
            log.info(f"Auto-approved: '{name}' @ {ip}:{addr[1]}")
            return

        # Unknown node -> pending queue
        self.pending[name] = PendingClient(addr, name)
        log.info(f"Pending approval: '{name}' @ {ip}:{addr[1]}")
        self._notify_change()

    def _register_client(self, addr: tuple, name: str, room_id: int):
        cid    = self.next_cid
        self.next_cid += 1
        client = Client(cid, addr, name, room_id)
        self.clients[cid]      = client
        self.addr_to_cid[addr] = cid
        self.name_to_cid[name] = cid
        self._send_pong(addr, cid, room_id)
        self._notify_change()

    # -- AUDIO -----------------------------------------------------------------
    def _handle_audio(self, addr: tuple, payload: bytes):
        client = self._get_by_addr(addr)
        if not client or not payload: return
        client.touch()
        if LOG_AUDIO:
            log.debug(f"Audio '{client.name}' {len(payload)}B")

        self.relay_seq = (self.relay_seq + 1) & 0xFFFF
        packet = make_header(TYPE_AUDIO, client.cid, self.relay_seq) + payload

        dead = []
        for cid, c in self.clients.items():
            if cid == client.cid or c.room_id != client.room_id: continue
            if not c.alive():
                dead.append(cid); continue
            self.transport.sendto(packet, c.addr)
        for cid in dead:
            self._remove_client(cid, "timeout")

    # -- PING ------------------------------------------------------------------
    def _handle_ping(self, addr: tuple, cid: int):
        c = self._get_by_addr(addr)
        if c: c.touch()
        self.transport.sendto(make_header(TYPE_PONG, cid, 0), addr)

    # -- BYE -------------------------------------------------------------------
    def _handle_bye(self, addr: tuple):
        cid = self.addr_to_cid.get(addr)
        if cid:
            name = self.clients[cid].name if cid in self.clients else "?"
            log.info(f"'{name}' sent BYE")
            self._remove_client(cid, "bye")
            self._notify_change()

    # -- Send packets ----------------------------------------------------------
    def _send_pong(self, addr, cid, room_id):
        room = self.rooms.get(room_id, self.rooms[0])
        pkt  = (make_header(TYPE_PONG, cid, 0) +
                struct.pack(">BB", cid, room_id) +
                room.name.encode("utf-8")[:15])
        self.transport.sendto(pkt, addr)

    def _send_reject(self, addr, reason):
        self.transport.sendto(
            make_header(TYPE_REJECT, 0, 0) + struct.pack(">B", reason), addr)

    def _send_room_info(self, addr, room_id):
        room = self.rooms.get(room_id, self.rooms[0])
        pkt  = (make_header(TYPE_ROOM_INFO, 0, 0) +
                struct.pack(">B", room_id) +
                room.name.encode("utf-8")[:15])
        self.transport.sendto(pkt, addr)

    # -- Admin actions ---------------------------------------------------------
    def approve(self, name: str) -> bool:
        """Approve a pending node and save it permanently."""
        p = self.pending.pop(name, None)
        if not p or p.addr[0] in self.blocked_ips:
            return False
        self.approved[name] = {"room_id": DEFAULT_ROOM_ID}
        self._save_state()
        self._register_client(p.addr, name, DEFAULT_ROOM_ID)
        log.info(f"Approved and saved: '{name}' @ {p.addr[0]}")
        return True

    def reject_pending(self, name: str) -> bool:
        """Deny a pending node (does not affect approved list)."""
        p = self.pending.pop(name, None)
        if not p: return False
        self._send_reject(p.addr, REJECT_NOT_APPROVED)
        log.info(f"Rejected: '{name}' @ {p.addr[0]}")
        self._notify_change()
        return True

    def revoke(self, name: str) -> bool:
        """Revoke approval - node must be manually approved again."""
        if name not in self.approved: return False
        del self.approved[name]
        self._save_state()
        cid = self.name_to_cid.get(name)
        if cid:
            c = self.clients.get(cid)
            if c: self._send_reject(c.addr, REJECT_NOT_APPROVED)
            self._remove_client(cid, "approval revoked")
        log.info(f"Approval revoked: '{name}'")
        self._notify_change()
        return True

    def kick(self, cid: int) -> bool:
        """Disconnect a client temporarily (keeps approved status)."""
        c = self.clients.get(cid)
        if not c: return False
        self._send_reject(c.addr, REJECT_KICKED)
        log.info(f"Kicked: '{c.name}'")
        self._remove_client(cid, "kicked")
        self._notify_change()
        return True

    def block(self, cid: int) -> bool:
        """Block IP and revoke approval."""
        c = self.clients.get(cid)
        if not c: return False
        ip, name = c.addr[0], c.name
        self.blocked_ips.add(ip)
        self.approved.pop(name, None)
        self._save_state()
        self._send_reject(c.addr, REJECT_BLOCKED)
        log.info(f"Blocked: '{name}' @ {ip}")
        self._remove_client(cid, "blocked")
        for n in [n for n, p in self.pending.items() if p.addr[0] == ip]:
            del self.pending[n]
        self._notify_change()
        return True

    def unblock(self, ip: str):
        self.blocked_ips.discard(ip)
        self._save_state()
        log.info(f"Unblocked: {ip}")
        self._notify_change()

    def create_room(self, name: str) -> int:
        rid = self.next_rid
        self.next_rid += 1
        self.rooms[rid] = Room(rid, name[:20])
        self._save_state()
        log.info(f"New room: '{name}' (ID {rid})")
        self._notify_change()
        return rid

    def delete_room(self, rid: int) -> bool:
        if rid == 0 or rid not in self.rooms: return False
        for c in self.clients.values():
            if c.room_id == rid:
                c.room_id = DEFAULT_ROOM_ID
                self._send_room_info(c.addr, DEFAULT_ROOM_ID)
        for info in self.approved.values():
            if info.get("room_id") == rid:
                info["room_id"] = DEFAULT_ROOM_ID
        del self.rooms[rid]
        self._save_state()
        log.info(f"Room {rid} deleted")
        self._notify_change()
        return True

    def assign_room(self, cid: int, room_id: int) -> bool:
        if room_id not in self.rooms: return False
        c = self.clients.get(cid)
        if not c: return False
        c.room_id = room_id
        if c.name in self.approved:
            self.approved[c.name]["room_id"] = room_id
            self._save_state()
        self._send_room_info(c.addr, room_id)
        log.info(f"'{c.name}' -> room '{self.rooms[room_id].name}'")
        self._notify_change()
        return True

    # -- Internals -------------------------------------------------------------
    def _get_by_addr(self, addr):
        return self.clients.get(self.addr_to_cid.get(addr))

    def _remove_client(self, cid: int, reason: str = ""):
        c = self.clients.pop(cid, None)
        if c:
            self.addr_to_cid.pop(c.addr, None)
            self.name_to_cid.pop(c.name, None)
            log.info(f"- '{c.name}' removed"
                     + (f" ({reason})" if reason else "")
                     + f"  (total: {len(self.clients)})")

    def cleanup(self):
        dead = [cid for cid, c in self.clients.items() if not c.alive()]
        for cid in dead:
            self._remove_client(cid, "timeout")
        stale = [n for n, p in self.pending.items()
                 if (time.monotonic() - p.last_hello) > PENDING_TIMEOUT]
        for n in stale:
            log.info(f"Pending timeout: '{n}'")
            del self.pending[n]
        if dead or stale:
            self._notify_change()

    def get_state(self) -> dict:
        now = time.monotonic()
        return {
            "clients": [
                {
                    "cid":       c.cid,
                    "name":      c.name,
                    "ip":        c.addr[0],
                    "port":      c.addr[1],
                    "room_id":   c.room_id,
                    "room_name": self.rooms.get(c.room_id, self.rooms[0]).name,
                    "age":       int(now - c.last_seen),
                    "alive":     c.alive(),
                }
                for c in self.clients.values()
            ],
            "pending": [
                {
                    "name":    p.name,
                    "ip":      p.addr[0],
                    "port":    p.addr[1],
                    "waiting": p.waiting_str(),
                }
                for p in self.pending.values()
            ],
            "approved": [
                {
                    "name":      name,
                    "room_id":   info.get("room_id", 0),
                    "room_name": self.rooms.get(
                                     info.get("room_id", 0),
                                     self.rooms[0]).name,
                    "online":    name in self.name_to_cid,
                }
                for name, info in sorted(self.approved.items())
            ],
            "rooms": [
                {
                    "rid":   r.rid,
                    "name":  r.name,
                    "count": sum(1 for c in self.clients.values()
                                 if c.room_id == r.rid),
                }
                for r in self.rooms.values()
            ],
            "blocked": sorted(self.blocked_ips),
        }

    def error_received(self, exc):
        log.error(f"UDP error: {exc}")


# -- HTML (web UI) -------------------------------------------------------------
HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IP-Radio Server</title>
<style>
:root{--bg:#0f1117;--surf:#1a1d27;--surf2:#22263a;--bdr:#2a2d3a;
--acc:#4f8ef7;--grn:#22c55e;--yel:#f59e0b;--red:#ef4444;
--txt:#e2e8f0;--muted:#64748b;--rad:8px}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);font:14px/1.5 'Segoe UI',system-ui,sans-serif;min-height:100vh}
header{background:var(--surf);border-bottom:1px solid var(--bdr);padding:12px 24px;
display:flex;align-items:center;gap:16px}
header h1{font-size:18px;font-weight:700;letter-spacing:.5px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--grn);
box-shadow:0 0 6px var(--grn);animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
.stats{margin-left:auto;display:flex;gap:20px;font-size:12px;color:var(--muted)}
.stats span b{color:var(--txt);font-size:15px}
main{padding:20px 24px;display:grid;gap:16px;
grid-template-columns:1fr 1fr;
grid-template-areas:"pending pending""clients clients""approved approved""rooms blocked"}
@media(max-width:700px){main{grid-template-columns:1fr;
grid-template-areas:"pending""clients""approved""rooms""blocked"}}
section{background:var(--surf);border:1px solid var(--bdr);border-radius:var(--rad);overflow:hidden}
.sec-header{padding:10px 16px;background:var(--surf2);border-bottom:1px solid var(--bdr);
display:flex;align-items:center;gap:8px;font-weight:600;font-size:13px}
.badge{padding:2px 8px;border-radius:99px;font-size:11px;font-weight:700}
.badge-yel{background:#78350f;color:var(--yel)}
.badge-grn{background:#14532d;color:var(--grn)}
.badge-red{background:#450a0a;color:var(--red)}
.badge-blu{background:#1e3a5f;color:var(--acc)}
.sec-body{padding:12px 16px}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;color:var(--muted);font-weight:600;font-size:11px;
text-transform:uppercase;letter-spacing:.5px;padding:4px 8px 8px}
td{padding:6px 8px;border-top:1px solid var(--bdr)}
tr:hover td{background:rgba(255,255,255,.02)}
.tag{display:inline-block;padding:1px 7px;border-radius:4px;font-size:11px;font-weight:600}
.tag-grn{background:#14532d;color:var(--grn)}
.tag-yel{background:#78350f;color:var(--yel)}
.tag-red{background:#450a0a;color:var(--red)}
.tag-blu{background:#1e3a5f;color:var(--acc)}
.tag-off{background:var(--surf2);color:var(--muted)}
.btn{display:inline-block;padding:4px 12px;border:none;border-radius:5px;
cursor:pointer;font-size:12px;font-weight:600;transition:.15s}
.btn:hover{filter:brightness(1.15)}
.btn-grn{background:#166534;color:#bbf7d0}
.btn-red{background:#7f1d1d;color:#fecaca}
.btn-yel{background:#78350f;color:#fde68a}
.btn-gry{background:var(--surf2);color:var(--muted)}
.btn+.btn{margin-left:6px}
.room-sel{background:var(--surf2);border:1px solid var(--bdr);color:var(--txt);
border-radius:4px;padding:3px 6px;font-size:12px}
.create-row{display:flex;gap:8px;margin-top:10px}
.create-row input{flex:1;background:var(--surf2);border:1px solid var(--bdr);
color:var(--txt);border-radius:5px;padding:5px 10px;font-size:13px;outline:none}
.create-row input:focus{border-color:var(--acc)}
.empty{color:var(--muted);font-size:13px;padding:8px 0;font-style:italic}
#blocked-list{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:4px}
.blocked-chip{background:var(--surf2);border:1px solid var(--bdr);border-radius:5px;
padding:4px 10px;font-size:12px;display:flex;align-items:center;gap:8px}
.blocked-chip button{background:none;border:none;color:var(--red);cursor:pointer;
font-size:14px;line-height:1;padding:0}
</style>
</head>
<body>
<header>
  <div class="dot"></div>
  <h1>&#128251; IP-Radio Server v1.0</h1>
  <div class="stats">
    <span>Online <b id="s-clients">0</b></span>
    <span>Pending <b id="s-pending">0</b></span>
    <span>Known <b id="s-approved">0</b></span>
    <span>Rooms <b id="s-rooms">0</b></span>
    <span>Blocked <b id="s-blocked">0</b></span>
  </div>
  <div style="margin-left:16px;display:flex;gap:8px">
    <a href="/change-password" style="padding:4px 12px;background:#22263a;color:#94a3b8;border-radius:5px;font-size:12px;font-weight:600;text-decoration:none">&#128274; Password</a>
    <a href="/logout" style="padding:4px 12px;background:#7f1d1d;color:#fecaca;border-radius:5px;font-size:12px;font-weight:600;text-decoration:none">Sign out</a>
  </div>
</header>
<main>

<section style="grid-area:pending">
  <div class="sec-header">
    &#9203; Pending Approval
    <span class="badge badge-yel" id="b-pending">0</span>
  </div>
  <div class="sec-body">
    <table><thead><tr>
      <th>Node Name</th><th>IP</th><th>Port</th><th>Waiting</th><th>Action</th>
    </tr></thead>
    <tbody id="tbody-pending"></tbody></table>
    <div id="empty-pending" class="empty">No pending clients</div>
  </div>
</section>

<section style="grid-area:clients">
  <div class="sec-header">
    &#128225; Connected Clients
    <span class="badge badge-grn" id="b-clients">0</span>
  </div>
  <div class="sec-body">
    <table><thead><tr>
      <th>CID</th><th>Node Name</th><th>IP</th><th>Room</th><th>Last seen</th><th>Action</th>
    </tr></thead>
    <tbody id="tbody-clients"></tbody></table>
    <div id="empty-clients" class="empty">No connected clients</div>
  </div>
</section>

<section style="grid-area:approved">
  <div class="sec-header">
    &#128273; Known Nodes <span style="font-weight:400;font-size:11px;color:var(--muted)">&nbsp;auto-approved on reconnect</span>
    <span class="badge badge-blu" id="b-approved">0</span>
  </div>
  <div class="sec-body">
    <table><thead><tr>
      <th>Node Name</th><th>Status</th><th>Room</th><th>Action</th>
    </tr></thead>
    <tbody id="tbody-approved"></tbody></table>
    <div id="empty-approved" class="empty">No known nodes yet</div>
  </div>
</section>

<section style="grid-area:rooms">
  <div class="sec-header">&#127968; Rooms</div>
  <div class="sec-body">
    <div id="rooms-list"></div>
    <div class="create-row">
      <input id="new-room-name" placeholder="New room name..." maxlength="20">
      <button class="btn btn-grn" onclick="createRoom()">+ Create room</button>
    </div>
  </div>
</section>

<section style="grid-area:blocked">
  <div class="sec-header">
    &#128683; Blocked IP Addresses
    <span class="badge badge-red" id="b-blocked">0</span>
  </div>
  <div class="sec-body">
    <div id="blocked-list"></div>
    <div id="empty-blocked" class="empty">No blocked addresses</div>
  </div>
</section>

</main>
<script>
let roomsCache = [];
const es = new EventSource('/events');
es.onmessage = e => { const s = JSON.parse(e.data); roomsCache = s.rooms; renderAll(s); };

function esc(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

function renderAll(s) {
  document.getElementById('s-clients').textContent  = s.clients.length;
  document.getElementById('s-pending').textContent  = s.pending.length;
  document.getElementById('s-approved').textContent = s.approved.length;
  document.getElementById('s-rooms').textContent    = s.rooms.length;
  document.getElementById('s-blocked').textContent  = s.blocked.length;
  document.getElementById('b-pending').textContent  = s.pending.length;
  document.getElementById('b-clients').textContent  = s.clients.length;
  document.getElementById('b-approved').textContent = s.approved.length;
  document.getElementById('b-blocked').textContent  = s.blocked.length;
  renderPending(s.pending);
  renderClients(s.clients, s.rooms);
  renderApproved(s.approved);
  renderRooms(s.rooms);
  renderBlocked(s.blocked);
}

function renderPending(pending) {
  const tbody = document.getElementById('tbody-pending');
  document.getElementById('empty-pending').style.display = pending.length ? 'none' : '';
  tbody.innerHTML = pending.map(p => `
    <tr>
      <td><b>${esc(p.name)}</b></td>
      <td style="font-family:monospace">${esc(p.ip)}</td>
      <td>${esc(p.port)}</td>
      <td><span class="tag tag-yel">${esc(p.waiting)}</span></td>
      <td>
        <button class="btn btn-grn" onclick="approve('${esc(p.name)}')">&#10003; Approve</button>
        <button class="btn btn-red" onclick="rejectPending('${esc(p.name)}')">&#10005; Reject</button>
      </td>
    </tr>`).join('');
}

function renderClients(clients, rooms) {
  const tbody = document.getElementById('tbody-clients');
  document.getElementById('empty-clients').style.display = clients.length ? 'none' : '';
  tbody.innerHTML = clients.map(c => {
    const sel = rooms.map(r =>
      `<option value="${r.rid}"${r.rid===c.room_id?' selected':''}>${esc(r.name)}</option>`
    ).join('');
    const alive = c.alive ? 'tag-grn' : 'tag-red';
    return `<tr>
      <td><span class="tag ${alive}">#${c.cid}</span></td>
      <td><b>${esc(c.name)}</b></td>
      <td style="font-family:monospace">${esc(c.ip)}</td>
      <td><select class="room-sel" onchange="assignRoom(${c.cid},this.value)">${sel}</select></td>
      <td><span style="color:var(--muted)">${c.age}s</span></td>
      <td>
        <button class="btn btn-yel" onclick="kick(${c.cid})">Kick</button>
        <button class="btn btn-red" onclick="blockClient(${c.cid})">Block</button>
      </td>
    </tr>`;
  }).join('');
}

function renderApproved(approved) {
  const tbody = document.getElementById('tbody-approved');
  document.getElementById('empty-approved').style.display = approved.length ? 'none' : '';
  tbody.innerHTML = approved.map(a => `
    <tr>
      <td><b>${esc(a.name)}</b></td>
      <td>${a.online
        ? '<span class="tag tag-grn">&#9679; Online</span>'
        : '<span class="tag tag-off">&#9675; Offline</span>'}</td>
      <td><span style="color:var(--muted)">${esc(a.room_name)}</span></td>
      <td>
        <button class="btn btn-red" onclick="revoke('${esc(a.name)}')"
          title="Revoke approval - node must be re-approved manually">
          Revoke
        </button>
      </td>
    </tr>`).join('');
}

function renderRooms(rooms) {
  const el = document.getElementById('rooms-list');
  if (!rooms.length) { el.innerHTML=''; return; }
  el.innerHTML = `<table style="margin-bottom:8px">
    <thead><tr><th>Room</th><th>Clients</th><th></th></tr></thead>
    <tbody>${rooms.map(r=>`
      <tr>
        <td><b>${esc(r.name)}</b></td>
        <td><span class="tag tag-blu">${r.count}</span></td>
        <td>${r.rid!==0
          ?`<button class="btn btn-gry" onclick="deleteRoom(${r.rid})">Delete</button>`
          :'<span style="color:var(--muted);font-size:11px">default</span>'}</td>
      </tr>`).join('')}
    </tbody></table>`;
}

function renderBlocked(blocked) {
  const el=document.getElementById('blocked-list');
  const emp=document.getElementById('empty-blocked');
  emp.style.display = blocked.length ? 'none' : '';
  el.innerHTML = blocked.map(ip=>`
    <div class="blocked-chip">
      <span style="font-family:monospace">${esc(ip)}</span>
      <button onclick="unblock('${esc(ip)}')" title="Unblock">&#10005;</button>
    </div>`).join('');
}

async function api(url, body) {
  const r = await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  return r.json();
}

async function approve(name)       { await api('/api/approve',     {name}); }
async function rejectPending(name) { await api('/api/reject',      {name}); }
async function revoke(name)        { if(confirm(`Revoke approval for "${name}"?\nThe node will need manual re-approval.`)) await api('/api/revoke',{name}); }
async function kick(cid)           { await api('/api/kick',        {cid}); }
async function blockClient(cid)    { if(confirm('Block this IP?\nApproval will also be revoked.')) await api('/api/block',{cid}); }
async function unblock(ip)         { await api('/api/unblock',     {ip}); }
async function assignRoom(cid,rid) { await api('/api/room/assign', {cid:+cid,rid:+rid}); }
async function deleteRoom(rid)     { if(confirm('Delete this room? Clients will be moved to General.')) await api('/api/room/delete',{rid}); }
async function createRoom() {
  const inp=document.getElementById('new-room-name');
  const name=inp.value.trim(); if(!name) return;
  await api('/api/room/create',{name}); inp.value='';
}
document.getElementById('new-room-name').addEventListener('keydown',e=>{if(e.key==='Enter')createRoom();});
</script>
</body>
</html>
"""

# -- Auth pages ----------------------------------------------------------------
_AUTH_STYLE = """
<style>
:root{{--bg:#0f1117;--surf:#1a1d27;--bdr:#2a2d3a;--acc:#4f8ef7;
--txt:#e2e8f0;--muted:#64748b;--red:#ef4444;--grn:#22c55e}}
*{{box-sizing:border-box;margin:0;padding:0}}
body{{background:var(--bg);color:var(--txt);font:14px/1.5 'Segoe UI',system-ui,sans-serif;
min-height:100vh;display:flex;align-items:center;justify-content:center}}
.card{{background:var(--surf);border:1px solid var(--bdr);border-radius:10px;
padding:36px 40px;width:360px;max-width:95vw}}
h1{{font-size:20px;font-weight:700;margin-bottom:6px}}
.sub{{color:var(--muted);font-size:13px;margin-bottom:28px}}
label{{display:block;font-size:12px;font-weight:600;color:var(--muted);
text-transform:uppercase;letter-spacing:.5px;margin-bottom:6px}}
input{{width:100%;background:#0f1117;border:1px solid var(--bdr);color:var(--txt);
border-radius:6px;padding:9px 12px;font-size:14px;outline:none;margin-bottom:18px}}
input:focus{{border-color:var(--acc)}}
button{{width:100%;background:var(--acc);color:#fff;border:none;border-radius:6px;
padding:10px;font-size:14px;font-weight:600;cursor:pointer;transition:.15s}}
button:hover{{filter:brightness(1.1)}}
.err{{background:#450a0a;color:var(--red);border-radius:6px;
padding:9px 12px;font-size:13px;margin-bottom:16px}}
.logo{{font-size:28px;margin-bottom:12px}}
</style>"""

HTML_LOGIN = _AUTH_STYLE + """
<div class="card">
  <div class="logo">&#128251;</div>
  <h1>IP-Radio Server</h1>
  <p class="sub">Sign in to access the admin panel</p>
  {error}
  <label>Password</label>
  <form method="post" action="/login">
    <input type="password" name="password" autofocus placeholder="Enter password">
    <button type="submit">Sign in</button>
  </form>
</div>"""

HTML_SETUP = _AUTH_STYLE + """
<div class="card">
  <div class="logo">&#128251;</div>
  <h1>Welcome to IP-Radio</h1>
  <p class="sub">First run – set an admin password to protect the web interface</p>
  {error}
  <form method="post" action="/setup">
    <label>Password</label>
    <input type="password" name="password" autofocus placeholder="Choose a password" minlength="6">
    <label>Confirm password</label>
    <input type="password" name="confirm" placeholder="Repeat password">
    <button type="submit">Set password &amp; continue</button>
  </form>
</div>"""

HTML_CHANGE_PW = _AUTH_STYLE + """
<div class="card">
  <div class="logo">&#128251;</div>
  <h1>Change Password</h1>
  <p class="sub">IP-Radio Server admin</p>
  {error}
  <form method="post" action="/change-password">
    <label>Current password</label>
    <input type="password" name="current" autofocus placeholder="Current password">
    <label>New password</label>
    <input type="password" name="password" placeholder="New password" minlength="6">
    <label>Confirm new password</label>
    <input type="password" name="confirm" placeholder="Repeat new password">
    <button type="submit">Update password</button>
  </form>
</div>"""

# -- Web routes ----------------------------------------------------------------
_server: IPRadioServer = None


def _auth_redirect(req):
    """Return a redirect to login, preserving the original path."""
    return web.HTTPFound(f"/login?next={req.path}")

def _check_auth(req):
    """Return True if request carries a valid session cookie."""
    return _valid_session(req.cookies.get(SESSION_COOKIE))


# -- Setup (first run) ---------------------------------------------------------
async def route_setup_get(req):
    if _pw_hash:                          # already set - go to login
        raise web.HTTPFound("/login")
    return web.Response(
        text=HTML_SETUP.format(error=""), content_type="text/html")

async def route_setup_post(req):
    if _pw_hash:
        raise web.HTTPFound("/login")
    data     = await req.post()
    password = data.get("password", "")
    confirm  = data.get("confirm", "")
    if len(password) < 6:
        err = '<div class="err">Password must be at least 6 characters.</div>'
        return web.Response(text=HTML_SETUP.format(error=err), content_type="text/html")
    if password != confirm:
        err = '<div class="err">Passwords do not match.</div>'
        return web.Response(text=HTML_SETUP.format(error=err), content_type="text/html")
    _save_password(password)
    log.info("Admin password set on first run")
    token = _new_session()
    resp  = web.HTTPFound("/")
    resp.set_cookie(SESSION_COOKIE, token, max_age=SESSION_MAX_AGE, httponly=True)
    return resp


# -- Login / logout ------------------------------------------------------------
async def route_login_get(req):
    if not _pw_hash:
        raise web.HTTPFound("/setup")
    if _check_auth(req):
        raise web.HTTPFound("/")
    return web.Response(
        text=HTML_LOGIN.format(error=""), content_type="text/html")

async def route_login_post(req):
    if not _pw_hash:
        raise web.HTTPFound("/setup")
    data     = await req.post()
    password = data.get("password", "")
    if _hash_password(password, _pw_salt) == _pw_hash:
        token = _new_session()
        dest  = req.rel_url.query.get("next", "/")
        resp  = web.HTTPFound(dest)
        resp.set_cookie(SESSION_COOKIE, token, max_age=SESSION_MAX_AGE, httponly=True)
        log.info(f"Admin logged in from {req.remote}")
        return resp
    err = '<div class="err">Incorrect password.</div>'
    return web.Response(text=HTML_LOGIN.format(error=err), content_type="text/html")

async def route_logout(req):
    token = req.cookies.get(SESSION_COOKIE)
    if token:
        _sessions.pop(token, None)
    resp = web.HTTPFound("/login")
    resp.del_cookie(SESSION_COOKIE)
    log.info(f"Admin logged out from {req.remote}")
    return resp


# -- Change password -----------------------------------------------------------
async def route_change_pw_get(req):
    if not _check_auth(req): raise _auth_redirect(req)
    return web.Response(
        text=HTML_CHANGE_PW.format(error=""), content_type="text/html")

async def route_change_pw_post(req):
    if not _check_auth(req): raise _auth_redirect(req)
    data    = await req.post()
    current = data.get("current", "")
    new_pw  = data.get("password", "")
    confirm = data.get("confirm", "")
    if _hash_password(current, _pw_salt) != _pw_hash:
        err = '<div class="err">Current password is incorrect.</div>'
        return web.Response(text=HTML_CHANGE_PW.format(error=err), content_type="text/html")
    if len(new_pw) < 6:
        err = '<div class="err">New password must be at least 6 characters.</div>'
        return web.Response(text=HTML_CHANGE_PW.format(error=err), content_type="text/html")
    if new_pw != confirm:
        err = '<div class="err">Passwords do not match.</div>'
        return web.Response(text=HTML_CHANGE_PW.format(error=err), content_type="text/html")
    _save_password(new_pw)
    # Invalidate all other sessions
    _sessions.clear()
    token = _new_session()
    resp  = web.HTTPFound("/")
    resp.set_cookie(SESSION_COOKIE, token, max_age=SESSION_MAX_AGE, httponly=True)
    log.info("Admin password changed")
    return resp


# -- Protected routes ----------------------------------------------------------
async def route_index(req):
    if not _pw_hash:      raise web.HTTPFound("/setup")
    if not _check_auth(req): raise _auth_redirect(req)
    return web.Response(text=HTML, content_type="text/html")

async def route_state(req):
    if not _check_auth(req): return web.json_response({"error": "unauthorized"}, status=401)
    return web.json_response(_server.get_state())

async def route_events(req):
    if not _check_auth(req): raise web.HTTPFound("/login")
    resp = web.StreamResponse(headers={
        "Content-Type":      "text/event-stream",
        "Cache-Control":     "no-cache",
        "X-Accel-Buffering": "no",
    })
    await resp.prepare(req)
    q = asyncio.Queue(maxsize=4)
    _server.add_sse_queue(q)
    await resp.write(f"data: {json.dumps(_server.get_state())}\n\n".encode())
    try:
        while True:
            try:    state = await asyncio.wait_for(q.get(), timeout=5.0)
            except asyncio.TimeoutError:
                await resp.write(b": ping\n\n"); continue
            await resp.write(f"data: {json.dumps(state)}\n\n".encode())
    except (ConnectionResetError, asyncio.CancelledError): pass
    finally: _server.remove_sse_queue(q)
    return resp


async def _json(req):
    try:    return await req.json()
    except: return {}

def _api(fn):
    """Decorator: checks auth before calling an API handler."""
    async def wrapper(req):
        if not _check_auth(req):
            return web.json_response({"error": "unauthorized"}, status=401)
        return await fn(req)
    return wrapper

@_api
async def route_approve(req):
    d = await _json(req); ok = _server.approve(d.get("name", ""))
    return web.json_response({"ok": ok})

@_api
async def route_reject(req):
    d = await _json(req); ok = _server.reject_pending(d.get("name", ""))
    return web.json_response({"ok": ok})

@_api
async def route_revoke(req):
    d = await _json(req); ok = _server.revoke(d.get("name", ""))
    return web.json_response({"ok": ok})

@_api
async def route_kick(req):
    d = await _json(req); ok = _server.kick(int(d.get("cid", -1)))
    return web.json_response({"ok": ok})

@_api
async def route_block(req):
    d = await _json(req); ok = _server.block(int(d.get("cid", -1)))
    return web.json_response({"ok": ok})

@_api
async def route_unblock(req):
    d = await _json(req); _server.unblock(d.get("ip", ""))
    return web.json_response({"ok": True})

@_api
async def route_room_create(req):
    d = await _json(req); rid = _server.create_room(d.get("name", "New Room"))
    return web.json_response({"ok": True, "rid": rid})

@_api
async def route_room_delete(req):
    d = await _json(req); ok = _server.delete_room(int(d.get("rid", -1)))
    return web.json_response({"ok": ok})

@_api
async def route_room_assign(req):
    d = await _json(req)
    ok = _server.assign_room(int(d.get("cid", -1)), int(d.get("rid", 0)))
    return web.json_response({"ok": ok})


# -- Periodic housekeeping -----------------------------------------------------
async def housekeeping(server: IPRadioServer):
    while True:
        await asyncio.sleep(10)
        server.cleanup()
        if server.clients:
            ids = ", ".join(f"'{c.name}'({c.addr[0]})"
                            for c in server.clients.values())
            log.info(f"Active: {ids}")
        else:
            log.info("No clients connected")
        if server.pending:
            log.info(f"Pending: {', '.join(server.pending)}")


# -- Main ----------------------------------------------------------------------
async def main():
    global _server
    log.info("=== IP-Radio Server v1.0 starting ===")
    _load_password()
    if not _pw_hash:
        log.info(f"No password set - open http://localhost:{WEB_PORT}/setup to configure")
    loop = asyncio.get_running_loop()

    _server = IPRadioServer()
    transport, _ = await loop.create_datagram_endpoint(
        lambda: _server, local_addr=(UDP_HOST, UDP_PORT))

    app = web.Application()
    # Auth routes (public)
    app.router.add_get ("/setup",           route_setup_get)
    app.router.add_post("/setup",           route_setup_post)
    app.router.add_get ("/login",           route_login_get)
    app.router.add_post("/login",           route_login_post)
    app.router.add_get ("/logout",          route_logout)
    # Protected routes
    app.router.add_get ("/",                route_index)
    app.router.add_get ("/change-password", route_change_pw_get)
    app.router.add_post("/change-password", route_change_pw_post)
    app.router.add_get ("/events",          route_events)
    app.router.add_get ("/api/state",       route_state)
    app.router.add_post("/api/approve",     route_approve)
    app.router.add_post("/api/reject",      route_reject)
    app.router.add_post("/api/revoke",      route_revoke)
    app.router.add_post("/api/kick",        route_kick)
    app.router.add_post("/api/block",       route_block)
    app.router.add_post("/api/unblock",     route_unblock)
    app.router.add_post("/api/room/create", route_room_create)
    app.router.add_post("/api/room/delete", route_room_delete)
    app.router.add_post("/api/room/assign", route_room_assign)

    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, WEB_HOST, WEB_PORT).start()
    log.info(f"Web UI: http://{WEB_HOST}:{WEB_PORT}/")

    try:
        await housekeeping(_server)
    finally:
        transport.close()
        await runner.cleanup()


if __name__ == "__main__":
    asyncio.run(main())
