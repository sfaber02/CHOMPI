#!/usr/bin/env python3
"""Send a firmware image to the CHOMPI launcher over USB MIDI, store it in a slot and run it.

Power CHOMPI on so the launcher is up (picker, or any fault pulse), then:

    ./midi-send.py --slot 5 ../chompi-tape/code/src/build/CHOMPI.bin

The keybed fills in cyan as the image arrives and amber as it is written to
/FIRMWARE/05_TAPE.bin on the card, replacing whatever was in slot 5. Then the
firmware starts. Send to the same slot again to replace it.

The name defaults to the project folder the image was built in (chompi-tape
-> TAPE); override it with --name.

Linux only, no dependencies -- talks to ALSA's raw MIDI device directly.
Protocol: see PROTOCOL.md.
"""
import argparse
import glob
import os
import re
import select
import sys
import time
import zlib

HEADER = bytes([0xF0, 0x7D, 0x43, 0x48])
PING, BEGIN, DATA, END = 0x01, 0x02, 0x03, 0x04
REPLY = 0x40

PROTOCOL_VERSION = 1

STATUS = {
    0: "ok",
    1: "bad message",
    2: "bad offset",
    3: "too big",
    4: "checksum mismatch",
    5: "image rejected (vector table)",
    6: "no upload in progress",
    7: "incomplete",
    8: "bad slot",
    9: "bad name",
    10: "no SD card, or it would not mount",
    11: "writing to the card failed",
}

NAME_OK = re.compile(r"^[A-Z0-9_-]{1,16}$")


def default_name(path):
    """TAPE for .../chompi-tape/code/src/build/CHOMPI.bin; else the file's own name.

    Every build is called CHOMPI.bin, so the project folder says more."""
    parts = os.path.abspath(path).split(os.sep)
    stem = os.path.splitext(parts[-1])[0]
    if stem.upper() == "CHOMPI":
        for part in reversed(parts[:-1]):
            if part.lower().startswith("chompi-") and len(part) > 7:
                return sanitize(part[7:])
    return sanitize(stem)


def sanitize(name):
    name = re.sub(r"[^A-Z0-9_-]", "-", name.upper())[:16]
    return name or "FIRMWARE"


def put7(value, n):
    return bytes((value >> (7 * i)) & 0x7F for i in range(n))


def get7(data):
    return sum((b & 0x7F) << (7 * i) for i, b in enumerate(data))


def pack7(raw):
    """Each group of up to 7 bytes, preceded by a byte of their top bits."""
    out = bytearray()
    for g in range(0, len(raw), 7):
        group = raw[g:g + 7]
        out.append(sum(((b >> 7) & 1) << i for i, b in enumerate(group)))
        out.extend(b & 0x7F for b in group)
    return bytes(out)


def find_device():
    """The raw MIDI node of the first ALSA card that looks like CHOMPI."""
    try:
        cards = open("/proc/asound/cards").read()
    except OSError:
        cards = ""
    for m in re.finditer(r"^\s*(\d+)\s+\[.*?\]:\s*(.*)\n\s*(.*)$", cards, re.M):
        num, desc = m.group(1), m.group(2) + " " + m.group(3)
        if "chompi" in desc.lower():
            nodes = sorted(glob.glob(f"/dev/snd/midiC{num}D*"))
            if nodes:
                return nodes[0]
    return None


class Link:
    def __init__(self, path, verbose=False):
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        self.buf = bytearray()
        self.verbose = verbose

    def send(self, cmd, payload=b""):
        msg = HEADER + bytes([cmd]) + payload + b"\xF7"
        os.write(self.fd, msg)

    def recv(self, cmd, timeout):
        """Next reply to cmd, or None on timeout. Anything else is skipped."""
        deadline = time.monotonic() + timeout
        while True:
            start = self.buf.find(b"\xF0")
            end = self.buf.find(b"\xF7", start) if start >= 0 else -1
            if start >= 0 and end >= 0:
                msg = bytes(self.buf[start:end + 1])
                del self.buf[:end + 1]
                if msg[:4] == HEADER and len(msg) > 5 and msg[4] == cmd | REPLY:
                    return msg[5:-1]
                continue
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                self.buf.extend(os.read(self.fd, 4096))

    def call(self, cmd, payload=b"", timeout=0.3, retries=10, required=True):
        for attempt in range(retries):
            self.send(cmd, payload)
            reply = self.recv(cmd, timeout)
            if reply is not None:
                return reply
            if self.verbose:
                print(f"\n  no reply to 0x{cmd:02X}, retrying", file=sys.stderr)
        if not required:
            return None
        sys.exit("no reply from the launcher -- is CHOMPI on and showing the picker?")


def check(reply, what):
    if not reply or reply[0] != 0:
        status = STATUS.get(reply[0], f"status {reply[0]}") if reply else "empty reply"
        sys.exit(f"{what} failed: {status}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", help="firmware .bin to send")
    ap.add_argument("--slot", type=int, required=True,
                    help="slot to store it in, 1-15; replaces what is there")
    ap.add_argument("--name", help="name on the card, 1-16 of A-Z 0-9 - _ "
                    "(default: from the project folder, e.g. TAPE)")
    ap.add_argument("--device", help="raw MIDI node, e.g. /dev/snd/midiC1D0 (default: find CHOMPI)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    image = open(args.image, "rb").read()
    name = args.name.upper() if args.name else default_name(args.image)
    if not NAME_OK.match(name):
        sys.exit(f"bad name {name!r}: use 1-16 of A-Z 0-9 - _")
    device = args.device or find_device()
    if not device:
        sys.exit("CHOMPI not found in /proc/asound/cards; plug it in or pass --device")

    link = Link(device, args.verbose)

    reply = link.call(PING)
    check(reply, "ping")
    version, max_chunk = reply[1], get7(reply[2:6])
    if version != PROTOCOL_VERSION or len(reply) < 7:
        sys.exit(f"launcher speaks protocol {version}, this script {PROTOCOL_VERSION}"
                 " -- update the launcher or this script")
    slots = reply[6]
    print(f"launcher on {device} (protocol {version}, {max_chunk}-byte chunks)")
    if not 1 <= args.slot <= slots:
        sys.exit(f"slot must be 1-{slots}")

    target = f"{args.slot:02d}_{name}.bin"
    print(f"  storing as FIRMWARE/{target}, replacing anything in slot {args.slot}")

    check(link.call(BEGIN, put7(len(image), 4) + bytes([args.slot, len(name)])
                    + name.encode("ascii")), "begin")

    started = time.monotonic()
    offset = 0
    while offset < len(image):
        chunk = image[offset:offset + max_chunk]
        reply = link.call(DATA, put7(offset, 4) + pack7(chunk))
        check(reply, f"data at {offset}")
        # The launcher says how far it has got; resume from there.
        offset = get7(reply[1:5])
        pct = 100 * offset // len(image)
        print(f"\r  {offset}/{len(image)} bytes  {pct}%", end="", flush=True)

    elapsed = time.monotonic() - started
    print(f"\r  {len(image)} bytes in {elapsed:.1f} s ({len(image) / elapsed / 1024:.0f} KB/s)")

    # The reply comes once the image is on the card. One resend covers an END
    # that got lost; if it was the reply that got lost, the launcher has
    # already started the firmware and the resend goes unanswered too.
    print("  writing to the card...")
    reply = link.call(END, put7(zlib.crc32(image), 5), timeout=10.0, retries=2,
                      required=False)
    if reply is None:
        sys.exit("no reply to END -- the firmware was probably stored and started; "
                 "check the card")
    check(reply, "end")
    print(f"stored in slot {args.slot} -- starting firmware")


if __name__ == "__main__":
    main()
