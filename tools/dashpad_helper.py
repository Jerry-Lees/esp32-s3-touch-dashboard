#!/usr/bin/env python3
"""
DashPad PC helper: runs the commands behind the display's "Run on PC"
buttons.

The display never sends commands, only an action *name* ("terminal"). This
helper looks the name up in its own config file and runs what's listed
there, so nobody on the network can make it run anything else. Every
request is signed with a shared secret (HMAC-SHA256 over the request, with
a timestamp and a one-time nonce): the secret itself never crosses the
network and a recorded request can't be replayed.

Files (created on first run, readable only by you):
  ~/.config/dashpad/helper.json   port and actions (start: dashpad/helper.json)
  ~/.config/dashpad/token         the shared secret

Usage:
  python3 tools/dashpad_helper.py serve        # run it (keep it running)
  python3 tools/dashpad_helper.py autostart    # start it with your desktop (KDE/GNOME)
  python3 tools/dashpad_helper.py actions      # list the configured actions
  python3 tools/dashpad_helper.py run NAME     # try an action locally
  ~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/dashpad_helper.py pair
                                               # send address + secret to the display (USB)
  python3 tools/dashpad_helper.py new-token    # replace the secret (then pair again)

An action is a list of alternative commands; the first one whose program is
installed is run (no shell, so no quoting or injection surprises):
  "terminal": { "run": [["konsole"], ["gnome-terminal"]] }
Optional "state": a command whose exit code 0 means "on" (for two-state
buttons, e.g. a muted microphone).
"""
import hashlib
import hmac
import json
import os
import secrets
import shutil
import socket
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONF_DIR = os.path.expanduser("~/.config/dashpad")
CONF = os.path.join(CONF_DIR, "helper.json")
TOKEN = os.path.join(CONF_DIR, "token")
EXAMPLE = os.path.join(ROOT, "dashpad", "helper.json")
MAX_SKEW = 60            # seconds the display's clock may differ from the PC's
MAX_BODY = 4096


def private_write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    os.chmod(os.path.dirname(path), 0o700)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text)


def load():
    if not os.path.exists(CONF):
        private_write(CONF, open(EXAMPLE).read())
        print(f"Created {CONF} from the example; edit it to change the actions.")
    if not os.path.exists(TOKEN):
        private_write(TOKEN, secrets.token_hex(32) + "\n")
        print(f"Created a new secret in {TOKEN}. Pair the display (see --help).")
    conf = json.load(open(CONF))
    token = open(TOKEN).read().strip()
    if len(token) < 32:
        sys.exit(f"{TOKEN} looks wrong; run: dashpad_helper.py new-token")
    return conf, token


def pick(alternatives):
    """First alternative whose program is installed, or None."""
    for argv in alternatives:
        if argv and shutil.which(argv[0]):
            return argv
    return None


def run_action(conf, name):
    act = conf.get("actions", {}).get(name)
    if not act:
        return False, f'no action "{name}" in {CONF}'
    argv = pick(act.get("run", []))
    if not argv:
        return False, f'none of the programs for "{name}" is installed'
    try:
        # Detached: the program keeps running after the helper answers.
        subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
    except OSError as e:
        return False, f"{argv[0]}: {e.strerror}"
    return True, ""


def state_of(conf, name):
    act = conf.get("actions", {}).get(name, {})
    argv = pick([act["state"]]) if act.get("state") else None
    if not argv:
        return None
    try:
        return subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=3).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return None


class Handler(BaseHTTPRequestHandler):
    conf = None
    token = b""
    seen = {}                       # nonce -> time, for replay protection

    def log_message(self, fmt, *args):
        pass                        # quiet; errors are printed below

    def answer(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def verified(self):
        """The request body, if it's signed with our secret, fresh and not a replay."""
        n = int(self.headers.get("Content-Length") or 0)
        if not 0 < n <= MAX_BODY:
            return None
        body = self.rfile.read(n)
        sig = self.headers.get("X-DashPad-Sig", "")
        good = hmac.new(self.token, body, hashlib.sha256).hexdigest()
        if not hmac.compare_digest(sig, good):
            print(f"{self.client_address[0]}: bad signature (wrong secret? pair again)")
            return None
        try:
            req = json.loads(body)
            ts, nonce = int(req["ts"]), str(req["nonce"])
        except (ValueError, KeyError, TypeError):
            return None
        now = time.time()
        if abs(now - ts) > MAX_SKEW:
            print(f"{self.client_address[0]}: clock differs by {int(now - ts)} s; refused")
            return None
        for k in [k for k, t in self.seen.items() if now - t > 2 * MAX_SKEW]:
            del self.seen[k]
        if nonce in self.seen:
            print(f"{self.client_address[0]}: replayed request refused")
            return None
        self.seen[nonce] = now
        return req

    def do_POST(self):
        req = self.verified()
        if req is None:
            return self.answer(403, {"ok": False, "error": "not signed by the paired display"})
        if self.path == "/run":
            name = str(req.get("action", ""))
            ok, err = run_action(self.conf, name)
            print(f"run {name}: {'ok' if ok else err}")
            return self.answer(200, {"ok": ok, "error": err})
        if self.path == "/actions":
            return self.answer(200, {"ok": True,
                                     "actions": sorted(self.conf.get("actions", {}))})
        if self.path == "/state":
            names = [str(n) for n in req.get("names", [])][:32]
            return self.answer(200, {"ok": True, "states": {n: state_of(self.conf, n)
                                                            for n in names}})
        return self.answer(404, {"ok": False, "error": "unknown request"})


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))     # no packet is sent; picks the LAN interface
        return s.getsockname()[0]
    finally:
        s.close()


def cmd_serve():
    sys.stdout.reconfigure(line_buffering=True)        # log lines show up at once
    conf, token = load()
    Handler.conf, Handler.token = conf, token.encode()
    port = int(conf.get("port", 8765))
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"DashPad helper on {lan_ip()}:{port}, actions: {', '.join(sorted(conf['actions']))}")
    print("If the display can't reach it, open the port in the firewall (see README).")
    srv.serve_forever()


def cmd_pair():
    conf, token = load()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import sdcard                                   # needs pyserial (ESP-IDF's Python)
    port = int(conf.get("port", 8765))
    d = sdcard.Display("/dev/ttyACM0")
    d.hello()
    d.request(f"dashpad {lan_ip()} {port} {token}")
    print(f"Paired: the display will use {lan_ip()}:{port}.")


def cmd_autostart():
    path = os.path.expanduser("~/.config/autostart/dashpad-helper.desktop")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=DashPad helper\n"
                f"Exec=python3 {os.path.abspath(__file__)} serve\n"
                "X-GNOME-Autostart-enabled=true\nNoDisplay=true\n")
    print(f"Wrote {path}: the helper starts when you log in.")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "--help"
    if cmd == "serve":
        cmd_serve()
    elif cmd == "pair":
        cmd_pair()
    elif cmd == "autostart":
        cmd_autostart()
    elif cmd == "actions":
        conf, _ = load()
        for name, act in sorted(conf["actions"].items()):
            argv = pick(act.get("run", []))
            print(f"  {name:12} {' '.join(argv) if argv else '(not installed)'}")
    elif cmd == "run" and len(sys.argv) > 2:
        conf, _ = load()
        ok, err = run_action(conf, sys.argv[2])
        print("ok" if ok else err)
    elif cmd == "new-token":
        private_write(TOKEN, secrets.token_hex(32) + "\n")
        print("New secret written. Restart the helper and pair the display again.")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
