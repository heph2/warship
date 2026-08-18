#!/usr/bin/env python3
"""End-to-end smoke test: two real warship processes on ptys, playing through a
local rendezvous server. Covers what the C tests cannot -- placement, the ICE
handshake, the READY gate and one full FIRE/RESULT round trip.

Run with `make smoke`.
"""
import os, pty, re, select, signal, subprocess, sys, time

PORT = "17791"
ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")


class Peer:
    def __init__(self, args):
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen(
            args, stdin=slave, stdout=slave, stderr=slave, start_new_session=True
        )
        os.close(slave)
        self.buf = b""

    def pump(self, timeout=0.2):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], max(0, end - time.time()))
            if not r:
                break
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                break
            if not chunk:
                break
            self.buf += chunk
        return ANSI.sub(b" ", self.buf).decode("utf8", "replace")

    def wait_for(self, needle, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.pump(0.2):
                return True
        return False

    def send(self, keys):
        os.write(self.master, keys.encode())
        time.sleep(0.12)

    def kill(self):
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
        except Exception:
            pass
        self.proc.wait(timeout=5)


def place_fleet(p):
    """Five ships in rows 1-5, all starting at column A, horizontal."""
    for i in range(5):
        p.send(" ")   # commit the current ship at the cursor
        if i < 4:
            p.send("s")  # next row
    p.send("\r")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    server = subprocess.Popen(["./rendezvous", PORT],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    host = guest = None
    try:
        base = ["--signal", "127.0.0.1:" + PORT, "--stun", "127.0.0.1:1"]
        host = Peer(["./warship", "host"] + base)
        assert host.wait_for("placing Carrier", 10), "host never showed placement"
        place_fleet(host)

        assert host.wait_for("room code:", 15), "host never printed a room code"
        m = re.search(r"room code:\s+([a-z0-9]{6})", host.pump(0.3))
        assert m, "could not read the room code"
        code = m.group(1)

        guest = Peer(["./warship", "join", code] + base)
        assert guest.wait_for("placing Carrier", 10), "guest never showed placement"
        place_fleet(guest)

        assert host.wait_for("your turn", 40), "host never got the turn"
        assert guest.wait_for("their turn", 40), "guest never saw the host's turn"

        host.send(" ")  # fire at A1, where the guest's Carrier sits
        assert host.wait_for("hit at A1", 20), "host never saw its hit"
        assert guest.wait_for("they hit your Carrier at A1", 20), \
            "guest never reported the hit"
        assert guest.wait_for("your turn", 20), "turn did not pass to the guest"

        route = re.search(r"connected over (\w+)", host.pump(0.2))
        print("smoke ok (route %s, room %s)" % (route.group(1) if route else "?", code))
        return 0
    except AssertionError as e:
        print("SMOKE FAILED:", e, file=sys.stderr)
        for name, p in (("host", host), ("guest", guest)):
            if p:
                print("--- %s ---\n%s" % (name, p.pump(0.2)[-1500:]), file=sys.stderr)
        return 1
    finally:
        for p in (host, guest):
            if p:
                p.kill()
        server.kill()
        server.wait()


if __name__ == "__main__":
    sys.exit(main())
