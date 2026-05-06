#!/usr/bin/env python3

import argparse
import socket
import struct
import sys
import threading

MAX_MSG_SIZE = 1_000_000
IO_TIMEOUT = 5.0


def recv_exact(sock: socket.socket, n: int) -> bytes:
	data = bytearray()
	while len(data) < n:
		chunk = sock.recv(n - len(data))
		if not chunk:
			raise ConnectionError("peer closed")
		data.extend(chunk)
	return bytes(data)


def recv_frame(sock: socket.socket) -> bytes:
	raw_len = recv_exact(sock, 4)
	msg_len = struct.unpack("!I", raw_len)[0]
	if msg_len > MAX_MSG_SIZE:
		raise ValueError("message too large")
	return recv_exact(sock, msg_len)


def send_frame(sock: socket.socket, payload: bytes) -> None:
	header = struct.pack("!I", len(payload))
	sock.sendall(header + payload)


def handle_client(conn: socket.socket, addr) -> None:
	conn.settimeout(IO_TIMEOUT)
	try:
		while True:
			try:
				payload = recv_frame(conn)
			except ValueError:
				send_frame(conn, b"ERR: message too large")
				break
			except (socket.timeout, ConnectionError):
				break

			if payload:
				sys.stdout.buffer.write(payload + b"\n")
				sys.stdout.buffer.flush()
			send_frame(conn, b"OK")
	finally:
		conn.close()


def run_server(host: str, port: int) -> int:
	srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
	srv.bind((host, port))
	srv.listen(128)

	print(f"listening on {host}:{port}")
	try:
		while True:
			try:
				conn, addr = srv.accept()
			except KeyboardInterrupt:
				break
			conn.settimeout(IO_TIMEOUT)
			t = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
			t.start()
	finally:
		srv.close()
	return 0


def run_client(host: str, port: int, message: str | None) -> int:
	sock = socket.create_connection((host, port), timeout=IO_TIMEOUT)
	sock.settimeout(IO_TIMEOUT)
	try:
		if message is not None:
			send_frame(sock, message.encode())
			reply = recv_frame(sock)
			print(reply.decode("utf-8", errors="replace"))
		else:
			for line in sys.stdin:
				payload = line.rstrip("\n").encode()
				if not payload:
					continue
				send_frame(sock, payload)
				reply = recv_frame(sock)
				print(reply.decode("utf-8", errors="replace"))
	finally:
		sock.close()
	return 0


def parse_args():
	parser = argparse.ArgumentParser()
	parser.add_argument("mode", choices=["server", "client"])
	parser.add_argument("host")
	parser.add_argument("port", type=int)
	parser.add_argument("message", nargs="?")
	return parser.parse_args()


def main() -> int:
	args = parse_args()
	if args.mode == "server":
		return run_server(args.host, args.port)
	return run_client(args.host, args.port, args.message)


if __name__ == "__main__":
	raise SystemExit(main())
