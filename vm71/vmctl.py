#!/usr/bin/env python3
"""vmctl.py - Remote-control harness for the FTP4DOS QEMU test VM.

Talks to QEMU's QMP socket to inject keystrokes and capture the screen as an
image, giving a closed control loop: send key -> screenshot -> inspect -> repeat.

Usage (called from PowerShell/Bash):
    py vmctl.py key f2                 # one key
    py vmctl.py key ctrl c             # key combo (pressed together)
    py vmctl.py type "10.0.2.2"        # type a string
    py vmctl.py enter                  # press Return
    py vmctl.py shot boot              # screendump -> vm/shots/boot.png
    py vmctl.py keys f2 / p u b enter  # several discrete key presses

Environment:
    QMP_HOST (default 127.0.0.1), QMP_PORT (default 4444)
"""
import json
import os
import socket
import sys
import time

HOST = os.environ.get("QMP_HOST", "127.0.0.1")
PORT = int(os.environ.get("QMP_PORT", "4444"))
SHOTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")

# Map a single character to (modifiers, QKeyCode) for QEMU `send-key`.
# US keyboard layout. Modifiers are extra qcodes pressed simultaneously.
_BASE = {
    " ": "spc", "\t": "tab", "\n": "ret",
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    "\\": "backslash", ";": "semicolon", "'": "apostrophe",
    "`": "grave_accent", ",": "comma", ".": "dot", "/": "slash",
}
_SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
    "{": "bracket_left", "}": "bracket_right", "|": "backslash",
    ":": "semicolon", '"': "apostrophe", "~": "grave_accent",
    "<": "comma", ">": "dot", "?": "slash",
}
# Friendly aliases accepted on the command line for special keys.
_KEY_ALIASES = {
    "enter": "ret", "return": "ret", "esc": "esc", "escape": "esc",
    "space": "spc", "tab": "tab", "backspace": "backspace", "bksp": "backspace",
    "del": "delete", "ins": "insert",
    "pgup": "pgup", "pgdn": "pgdn", "pageup": "pgup", "pagedown": "pgdn",
}


def char_to_keys(ch):
    """Return list of qcodes to press together to produce character `ch`."""
    if ch.isalpha():
        code = ch.lower()
        return ["shift", code] if ch.isupper() else [code]
    if ch.isdigit():
        return [ch]
    if ch in _BASE:
        return [_BASE[ch]]
    if ch in _SHIFTED:
        return ["shift", _SHIFTED[ch]]
    raise ValueError("no key mapping for %r" % ch)


class Qmp:
    def __init__(self, host=HOST, port=PORT, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.fp = self.sock.makefile("rwb", buffering=0)
        self._read_json()                 # server greeting {"QMP": ...}
        self.execute("qmp_capabilities")  # leave negotiation mode

    def _read_json(self):
        while True:
            line = self.fp.readline()
            if not line:
                raise EOFError("QMP connection closed")
            line = line.strip()
            if line:
                return json.loads(line)

    def execute(self, command, **args):
        msg = {"execute": command}
        if args:
            msg["arguments"] = args
        self.fp.write((json.dumps(msg) + "\r\n").encode())
        while True:
            reply = self._read_json()
            if "event" in reply:          # async event, ignore
                continue
            if "error" in reply:
                raise RuntimeError("QMP error: %s" % reply["error"])
            return reply.get("return")

    def send_key(self, qcodes, hold_ms=0):
        keys = [{"type": "qcode", "data": c} for c in qcodes]
        args = {"keys": keys}
        if hold_ms:
            args["hold-time"] = hold_ms
        self.execute("send-key", **args)

    def type_text(self, text, per_key_delay=0.03):
        for ch in text:
            self.send_key(char_to_keys(ch))
            time.sleep(per_key_delay)

    def screendump(self, name):
        if not name.lower().endswith((".png", ".ppm")):
            name += ".png"
        path = name if os.path.isabs(name) else os.path.join(SHOTS_DIR, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # Prefer PNG; fall back to PPM if this QEMU build rejects the format arg.
        try:
            self.execute("screendump", filename=path, format="png")
            return path
        except RuntimeError:
            ppm = os.path.splitext(path)[0] + ".ppm"
            self.execute("screendump", filename=ppm)
            png = self._ppm_to_png(ppm, path)
            return png or ppm

    @staticmethod
    def _ppm_to_png(ppm_path, png_path):
        try:
            from PIL import Image
        except ImportError:
            return None
        Image.open(ppm_path).save(png_path)
        return png_path

    def close(self):
        try:
            self.fp.close()
            self.sock.close()
        except OSError:
            pass


def _resolve_key(token):
    t = token.lower()
    if t in _KEY_ALIASES:
        return _KEY_ALIASES[t]
    return t  # assume it's already a valid qcode (a-z, 0-9, f1..f12, up, ...)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    q = Qmp()
    try:
        if cmd == "key":
            # remaining tokens are pressed together as one combo
            q.send_key([_resolve_key(t) for t in argv[2:]])
        elif cmd == "keys":
            # each token is a separate, sequential key press
            for t in argv[2:]:
                q.send_key([_resolve_key(t)])
                time.sleep(0.05)
        elif cmd == "type":
            q.type_text(argv[2])
        elif cmd == "enter":
            q.send_key(["ret"])
        elif cmd == "shot":
            path = q.screendump(argv[2] if len(argv) > 2 else "shot")
            print(path)
        else:
            print("unknown command: %s" % cmd, file=sys.stderr)
            return 2
    finally:
        q.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
