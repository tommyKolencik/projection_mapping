"""Linux integration test: real HTTP/WebSockets and a pseudo-terminal Nano.

Run: python3 tests/integration.py build/projection_backend
Test-only dependency: python3-websockets (or pip install websockets in a venv).
"""
import asyncio
import json
import os
from pathlib import Path
import pty
import socket
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import websockets

# websockets 10 (Ubuntu 24.04) and current releases name this error differently.
try:
    from websockets.asyncio.client import connect
    from websockets.exceptions import InvalidStatus as HandshakeError
except ImportError:
    from websockets import connect
    from websockets.exceptions import InvalidStatusCode as HandshakeError


async def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/projection_backend").resolve()
    root = Path(__file__).resolve().parents[1]
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    master, slave = pty.openpty()
    with tempfile.TemporaryDirectory() as tmp:
        link = Path(tmp) / "nano"
        link.symlink_to(os.ttyname(slave))
        os.close(slave)
        process = subprocess.Popen([str(binary), "--serial", str(link), "--port", str(port), "--frontend", str(root / "frontend")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        def state():
            with urllib.request.urlopen(base + "/api/state", timeout=1) as response:
                return json.load(response)

        async def wait_for(predicate, seconds=6):
            deadline = asyncio.get_running_loop().time() + seconds
            while asyncio.get_running_loop().time() < deadline:
                try:
                    current = state()
                    if predicate(current):
                        return current
                except (OSError, urllib.error.URLError):
                    pass
                if process.poll() is not None:
                    raise AssertionError("Backend exited unexpectedly")
                await asyncio.sleep(.05)
            raise AssertionError("Timed out waiting for backend state")

        def packet(seq, event="telemetry", **extra):
            value = dict(schema_version=1, event=event, seq=seq, uptime_ms=seq*100,
                         temperature_c=23.25, temperature_available=True,
                         button_available=True, button_pressed=False)
            value.update(extra)
            return json.dumps(value).encode() + b"\n"

        try:
            await wait_for(lambda s: s["serial_connected"])
            os.write(master, packet(1))
            await wait_for(lambda s: s["device_online"] and s["received_messages"] == 1)
            async with connect(f"ws://127.0.0.1:{port}/ws", origin=base) as one, connect(f"ws://127.0.0.1:{port}/ws", origin=base) as two:
                assert json.loads(await one.recv())["scene_index"] == 0
                assert json.loads(await two.recv())["scene_index"] == 0
                os.write(master, packet(2, "short_press"))
                await wait_for(lambda s: s["scene_index"] == 1)
                os.write(master, packet(3, "long_press"))
                await wait_for(lambda s: s["blackout"])
                os.write(master, packet(3, "long_press"))
                await asyncio.sleep(.15)
                assert state()["blackout"] and state()["received_messages"] == 3
                await one.send(json.dumps({"action": "toggle_blackout"}))
                await wait_for(lambda s: not s["blackout"])
                async def second_browser_updated():
                    for _ in range(20):
                        s = json.loads(await asyncio.wait_for(two.recv(), 2))
                        if s["scene_index"] == 1 and not s["blackout"]:
                            return
                    raise AssertionError("Other browser didn't receive updated state")
                await second_browser_updated()
                os.write(master, b"bad json\n" + packet(4, temperature_c=999))
                await wait_for(lambda s: s["invalid_messages"] == 2)
                assert state()["device"]["temperature_c"] == 23.25
                partial = packet(5)
                os.write(master, partial[:30]); await asyncio.sleep(.1)
                assert state()["received_messages"] == 3
                os.write(master, partial[30:])
                await wait_for(lambda s: s["received_messages"] == 4)
                os.write(master, b"X"*5000 + b"\n" + packet(6))
                await wait_for(lambda s: s["invalid_messages"] == 3 and s["received_messages"] == 5)
                os.write(master, json.dumps({"event": "short_press"}).encode()+b"\n")
                await wait_for(lambda s: s["invalid_messages"] == 4)
                assert state()["scene_index"] == 1
                await wait_for(lambda s: not s["device_online"], seconds=7)
                os.close(master); master = -1
                await wait_for(lambda s: not s["serial_connected"])
                link.unlink()
                master, slave = pty.openpty()
                link.symlink_to(os.ttyname(slave)); os.close(slave)
                await wait_for(lambda s: s["serial_connected"])
                os.write(master, packet(0, "ready"))
                await wait_for(lambda s: s["device_online"])
                assert state()["scene_index"] == 1
            try:
                async with connect(f"ws://127.0.0.1:{port}/ws", origin="http://unrelated.example"):
                    raise AssertionError("Foreign origin accepted")
            except HandshakeError as error:
                assert getattr(getattr(error, "response", None), "status_code", getattr(error, "status_code", None)) == 403
            try:
                urllib.request.urlopen(base + "/../.git/config")
                raise AssertionError("Private path was served")
            except urllib.error.HTTPError as error:
                assert error.code == 404
            print("PASS: serial framing, validation, gestures, duplicate suppression, two browsers, stale data, USB reconnect, origin and route checks")
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
            if master >= 0:
                os.close(master)


asyncio.run(main())
