#!/usr/bin/env python3
"""Smoke-test a running BLESS control listener using only stdlib sockets."""
import os
import json
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor

host, _, port_text = os.environ.get("BLESS_CONTROL_ADDR", "127.0.0.1:8000").partition(":")
port = int(port_text or "8000")
key = os.environ.get("BLESS_API_KEY", "")

def request(data, complete=True):
    try:
        with socket.create_connection((host, port), timeout=3) as s:
            s.sendall(data)
            if not complete:
                return s.recv(4096)
            chunks = []
            response = b""
            content_length = None
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
                response = b"".join(chunks)
                if content_length is None and b"\r\n\r\n" in response:
                    header = response.split(b"\r\n\r\n", 1)[0]
                    for line in header.split(b"\r\n"):
                        if line.lower().startswith(b"content-length:"):
                            content_length = int(line.split(b":", 1)[1].strip())
                            break
                if content_length is not None:
                    body = response.split(b"\r\n\r\n", 1)[1]
                    if len(body) >= content_length:
                        break
            return response
    except OSError as exc:
        raise SystemExit(
            "cannot connect to BLESS control listener at "
            f"{host}:{port}; start BLESS first or set BLESS_CONTROL_ADDR "
            f"(underlying error: {exc})"
        ) from exc

metrics = request(b"GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
if b" 200 " not in metrics:
    raise SystemExit("/metrics did not return HTTP 200")
stats = request(b"GET /api/stats HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
if b" 200 " not in stats:
    raise SystemExit("/api/stats did not return HTTP 200")
try:
    headers, body = stats.split(b"\r\n\r\n", 1)
    document = json.loads(body.decode("utf-8"))
except (ValueError, UnicodeDecodeError) as exc:
    raise SystemExit(f"/api/stats returned invalid JSON: {exc}") from exc
if not isinstance(document, dict):
    raise SystemExit("/api/stats JSON root is not an object")
for line in headers.split(b"\r\n"):
    if line.lower().startswith(b"content-length:"):
        declared = int(line.split(b":", 1)[1].strip())
        if declared != len(body):
            raise SystemExit("/api/stats Content-Length does not match body")
        break

def check_stats_path(path):
    response = request(("GET %s HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n" % path).encode())
    if b" 200 " not in response:
        raise RuntimeError(path + " did not return HTTP 200")
    return response

with ThreadPoolExecutor(max_workers=8) as pool:
    futures = [pool.submit(check_stats_path, "/api/stats" if i % 2
                           else "/metrics") for i in range(8)]
    for future in futures:
        future.result()

def ws(query):
    req = ("GET /wsURL?%s HTTP/1.1\r\nHost: localhost\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: SGVsbG9TdGF0ZUtleQ==\r\n\r\n" % query).encode()
    return request(req, complete=False)

def hold_ws(query, seconds=2):
    req = ("GET /wsURL?%s HTTP/1.1\r\nHost: localhost\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: SGVsbG9TdGF0ZUtleQ==\r\n\r\n" % query).encode()
    try:
        s = socket.create_connection((host, port), timeout=3)
        s.sendall(req)
        response = s.recv(4096)
        if b" 101 " not in response:
            raise SystemExit("valid WebSocket hold client was rejected")
        time.sleep(seconds)  # deliberately do not read broadcast frames
        if os.environ.get("BLESS_REQUIRE_STATS_FRAME") == "1":
            s.settimeout(2)
            try:
                frame = s.recv(4096)
            except socket.timeout as exc:
                raise SystemExit("no WebSocket stats frame received") from exc
            if not frame or frame[0] & 0x0F != 0x1:
                raise SystemExit("received WebSocket frame is not text")
        s.close()
    except OSError as exc:
        raise SystemExit(f"slow-reader WebSocket failed: {exc}") from exc

if b" 101 " in ws("api_key=invalid"):
    raise SystemExit("invalid API key was accepted")
if key:
    encoded = key.replace("%", "%25").replace(" ", "%20")
    if b" 101 " not in ws("api_key=" + encoded):
        raise SystemExit("valid API key was rejected")
    # Keep this smoke test intentionally single-client.  It verifies the
    # production broadcast path without turning the core test into a
    # multi-client capacity benchmark.
    hold_ws("api_key=" + encoded)

print("live control socket: PASS")
