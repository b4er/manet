#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Test CLI program to start two WebSocket servers (one Plain and one TLS).

Routes:
  /binary    :  sends 4 bytes BINARY, then closes.
  /counter   :  sends `counter={i}` for i in 0..9, then closes.
  /heartbeat :  sends love forever.
  /hello     :  sends TEXT "Hello, World!", then closes.
"""

import asyncio
import logging
import os
import signal
import ssl
import subprocess
import sys
import time
import traceback

import websockets.asyncio.server as ws
from websockets.exceptions import ConnectionClosedOK

from argparse import ArgumentParser
from pathlib import Path

# for CMake this will be {build}/tests/ws-server.{pid,log}
PID_FILE = Path("ws-server.pid")
LOG_FILE = Path("ws-server.log")

cert_dir = Path(__file__).resolve().parent / "cert" # next to this file

DEFAULT_CERT = cert_dir / "test-cert.pem"
DEFAULT_KEY = cert_dir / "test-key.pem"

# logging
logging.basicConfig(
    level=logging.DEBUG, format="%(asctime)s [%(name)s] %(levelname)s %(message)s"
)

log = logging.getLogger("websockets.handler")


async def handler_binary(ws):
    await ws.send(b"\x00\x01\x02\x03")
    await ws.close()


async def handler_heartbeat(ws):
    while True:
        await ws.send("🫀")
        await asyncio.sleep(5)


async def handler_counter(ws):
    i = 0
    while i < 10:
        await ws.send(f"counter={i}")
        i += 1
        await asyncio.sleep(0.1)


async def handler_hello(ws):
    await ws.send("Hello, World!")
    await ws.close()


async def default_handler(ws):
    await ws.close(code=1008, reason="unknown path")


ROUTES = {
    "/binary": handler_binary,
    "/counter": handler_counter,
    "/heartbeat": handler_heartbeat,
    "/hello": handler_hello,
}


async def handler(ws):
    peer = ws.remote_address
    path = ws.request.path

    log.info("peer = %s, path = %s", peer, path)

    try:
        await ROUTES.get(path, default_handler)(ws)
    except ConnectionClosedOK:
        pass
    except Exception:
        log.exception("handler failed for %s", peer)
    finally:
        log.info("stopped pushing to %s", peer)


def get_tls(keychain):
    ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_ctx.load_cert_chain(*keychain)

    return ssl_ctx


async def serve(port, tls_port, keychain):
    stop_evt = asyncio.Event()

    def request_shutdown():
        log.warning("shutting down (SIGTERM/SIGINT received)")
        stop_evt.set()

    loop = asyncio.get_running_loop()

    loop.add_signal_handler(signal.SIGTERM, request_shutdown)
    loop.add_signal_handler(signal.SIGINT, request_shutdown)

    plain = await ws.serve(
        handler, "localhost", port,
        ping_interval=1,
        ssl=None,
    )

    tls = await ws.serve(
        handler, "localhost", tls_port,
        ping_interval=1,
        ssl=get_tls(keychain),
    )

    try:
        await stop_evt.wait()
    finally:
        log.info("shutting down servers")
        plain.close()
        tls.close()

        await asyncio.gather(plain.wait_closed(), tls.wait_closed())


def is_running(pid) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def start(port, tls_port):
    if PID_FILE.exists():
        with PID_FILE.open() as file:
            pid = int(file.read())

        if is_running(pid):
            print("[ws-server] already running", file=sys.stderr)
            exit(1)
        else:
            os.unlink(PID_FILE)

    with open(LOG_FILE, "ab", buffering=0) as log_f:
        proc = subprocess.Popen(
            [
                sys.executable,
                "-u",
                str(Path(__file__).resolve()),
                "--serve",
                "--port",
                str(port),
                "--tls-port",
                str(tls_port),
            ],
            stdout=log_f,
            stderr=subprocess.STDOUT,  # merge
            start_new_session=True,
            close_fds=True,
        )

        # delay a bit such that server is ready for tests:
        time.sleep(1)

    with open(PID_FILE, "w") as file:
        file.write(str(proc.pid))

    print(f"[ws-server] started pid={proc.pid}", file=sys.stderr)


def stop():
    if os.path.exists(PID_FILE):
        with open(PID_FILE) as file:
            pid = int(file.read())

        if is_running(pid):
            os.kill(pid, signal.SIGTERM)

        for _ in range(20):
            if not is_running(pid):
                break
            time.sleep(0.1)
        else:
            print("[ws-server] SIGTERM failed; SIGKILL", file=sys.stderr)
            os.kill(pid, signal.SIGKILL)

        os.unlink(PID_FILE)
        print("[ws-server] stopped")
    else:
        print("no PID file found..")
        exit(1)


if __name__ == "__main__":
    # getopts (--start|--serve|--stop) [--port=N] [--tls-port=N]
    parser = ArgumentParser(
        prog="ws-server",
        description="test server for WebSocket protocol",
    )

    ## (--start | --serve | --stop)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--start", action="store_true", help="spawn server in background and exit"
    )
    mode.add_argument(
        "--serve", action="store_true", help="run server in foreground (internal)"
    )
    mode.add_argument("--stop", action="store_true", help="stop background server")

    ## ports
    parser.add_argument(
        "--port", type=int, default=9000, help="port to listen on (default: 9000)"
    )
    parser.add_argument(
        "--tls-port", type=int, default=9443, help="port to listen on (default: 9443)"
    )

    args = parser.parse_args()

    keychain = [DEFAULT_CERT, DEFAULT_KEY]

    if args.start:
        start(args.port, args.tls_port)
    elif args.serve:
        try:
            asyncio.run(serve(args.port, args.tls_port, keychain))
        except KeyboardInterrupt:
            log.warning("\nInterrupted (KeyboardInterrupt)")
        except Exception:
            traceback.print_exc()
    elif args.stop:
        stop()
