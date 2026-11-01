"""Find AX7021 peers on directly attached IPv4 networks using read-only probes."""
import ctypes
import ipaddress
import os
import re
import secrets
import select
import socket
import struct
import time
from concurrent.futures import ThreadPoolExecutor

PORT = 37021
DIRECT = 'http://169.254.70.21'


def broadcast_routes():
    """Windows GetIpAddrTable supplies an address and netmask for each NIC."""
    if os.name != 'nt':
        return [('', '255.255.255.255')]
    size = ctypes.c_ulong()
    get_table = ctypes.windll.iphlpapi.GetIpAddrTable
    get_table(None, ctypes.byref(size), False)
    table = ctypes.create_string_buffer(size.value)
    routes = []
    if size.value >= 4 and get_table(table, ctypes.byref(size), False) == 0:
        # MIB_IPADDRTABLE: DWORD count followed by 24-byte MIB_IPADDRROW entries.
        raw = table.raw
        for i in range(struct.unpack_from('<I', raw)[0]):
            address, _, mask = struct.unpack_from('<4sI4s', raw, 4 + i*24)
            ip = ipaddress.IPv4Address(address)
            if ip.is_loopback or ip.is_unspecified:
                continue
            net = ipaddress.IPv4Network((str(ip), str(ipaddress.IPv4Address(mask))), strict=False)
            if net.prefixlen < 31:
                routes.append((str(ip), str(net.broadcast_address)))
    return routes or [('', '255.255.255.255')]


def scan(routes=None, *, port=PORT, timeout=.7):
    nonce = secrets.token_hex(8)
    query = ('AX7021_DISCOVER_V1 ' + nonce).encode('ascii')
    pattern = re.compile(rb'AX7021_MONITOR_V1 ' + nonce.encode() + rb' ([0-9a-fA-F]{12})')
    peers = {}
    sockets = []
    try:
        for address, destination in routes if routes is not None else broadcast_routes():
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                sock.bind((address, 0))
                sock.sendto(query, (destination, port))
            except OSError:
                sock.close()
                continue
            sockets.append(sock)
        until = time.monotonic() + timeout
        while sockets and time.monotonic() < until and len(peers) <= 8:
            ready = select.select(sockets, [], [], max(0, until-time.monotonic()))[0]
            for sock in ready:
                try:
                    reply, (ip, source_port) = sock.recvfrom(128)
                except OSError:
                    sockets.remove(sock)
                    sock.close()
                    continue
                match = pattern.fullmatch(reply)
                if match and source_port == port:
                    peers['http://' + ip] = match[1].decode('ascii').lower()
        return peers
    finally:
        for sock in sockets:
            sock.close()


def find_boards(handshake, known=None, current=None, locked=False, scanner=scan):
    known = known or {}
    if locked:
        peers = {current: known.get('mac')} if current else {}
    else:
        peers = scanner()
        if len(peers) > 8:
            return {'state':'multiple', 'boards':[], 'board':None}
        for origin in (current, known.get('board'), DIRECT):
            if origin:
                peers.setdefault(origin, None)
    if not peers:
        return {'state':'waiting', 'boards':[], 'board':None}
    with ThreadPoolExecutor(max_workers=len(peers)) as pool:
        passing = [origin for origin, ok in zip(peers, pool.map(handshake, peers)) if ok]
    if len(passing) == 1:
        origin = passing[0]
        # A different device on a LAN after disconnect requires explicit selection.
        if current and current != origin and (not known.get('mac') or peers[origin] != known['mac']):
            return {'state':'select', 'boards':passing, 'board':None}
        return {'state':'found', 'boards':passing, 'board':origin, 'mac':peers[origin]}
    return {'state':'multiple' if passing else 'waiting', 'boards':passing, 'board':None}
