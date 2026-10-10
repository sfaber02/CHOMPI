#!/usr/bin/env python3
"""Send a firmware image to the CHOMPI launcher over USB MIDI, store it in a slot and run it.

Power CHOMPI on so the launcher is up (picker, or any fault pulse), then:

    ./midi-send.py --slot 5 ../chompi-tape/code/src/build/CHOMPI.bin

The keybed fills in cyan as the image arrives and amber as it is written to
/FIRMWARE/05_TAPE.bin on the card, replacing whatever was in slot 5. Then the
firmware starts. Send to the same slot again to replace it.

The name defaults to the project folder the image was built in (chompi-tape
-> TAPE); override it with --name.

With --stay the launcher only stores it and keeps showing the picker, so more
can follow. --list shows what is on each key, --clear N empties key N. These
three need a launcher that has them; older ones say so.

To start a firmware that is already on the card, as its key would:

    ./midi-send.py --run 5

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
PING, BEGIN, DATA, END, RUN, LIST, CLEAR = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
REPLY = 0x40

FEATURE_LIST, FEATURE_CLEAR, FEATURE_STAY, FEATURE_LAUNCHER, FEATURE_RUN = 1, 2, 4, 8, 16
END_STAY = 1

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
    12: "the card is full",
    13: "that image is not a launcher",
    14: "another .bin in the card root would be installed instead; remove it first",
}

LAUNCHER_SLOT = 127

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

    def recv(self, cmd, timeout, match=None):
        """Next reply to cmd that match() accepts, or None on timeout.
        Anything else is skipped, e.g. a late reply to an earlier resend."""
        deadline = time.monotonic() + timeout
        while True:
            start = self.buf.find(b"\xF0")
            end = self.buf.find(b"\xF7", start) if start >= 0 else -1
            if start >= 0 and end >= 0:
                msg = bytes(self.buf[start:end + 1])
                del self.buf[:end + 1]
                if msg[:4] == HEADER and len(msg) > 5 and msg[4] == cmd | REPLY:
                    if match is None or match(msg[5:-1]):
                        return msg[5:-1]
                continue
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                self.buf.extend(os.read(self.fd, 4096))

    def call(self, cmd, payload=b"", timeout=0.3, retries=10, required=True, match=None):
        for attempt in range(retries):
            self.send(cmd, payload)
            reply = self.recv(cmd, timeout, match)
            if reply is not None:
                return reply
            if self.verbose:
                print(f"\n  no reply to 0x{cmd:02X}, retrying", file=sys.stderr)
        if not required:
            return None
        sys.exit("no reply from the launcher -- is CHOMPI on and showing the picker?")


def for_slot(slot):
    """LIST and CLEAR replies name their slot; failures are the status only."""
    return lambda r: r[0] != 0 or len(r) < 2 or r[1] == slot


def check(reply, what):
    if not reply or reply[0] != 0:
        status = STATUS.get(reply[0], f"status {reply[0]}") if reply else "empty reply"
        sys.exit(f"{what} failed: {status}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?", help="firmware .bin to send")
    ap.add_argument("--slot", type=int,
                    help="slot to store it in, 1-15; replaces what is there")
    ap.add_argument("--launcher", action="store_true",
                    help="the image is a new launcher: install it (CHOMPI restarts)")
    ap.add_argument("--stay", action="store_true",
                    help="store only; the launcher stays in the picker")
    ap.add_argument("--list", action="store_true", help="show what is on each key")
    ap.add_argument("--clear", type=int, metavar="SLOT", help="empty that key")
    ap.add_argument("--run", type=int, metavar="SLOT",
                    help="start the firmware already in this slot instead of sending one")
    ap.add_argument("--name", help="name on the card, 1-16 of A-Z 0-9 - _ "
                    "(default: from the project folder, e.g. TAPE)")
    ap.add_argument("--chunk", type=int, help="bytes per DATA message (default: the most the launcher takes)")
    ap.add_argument("--device", help="raw MIDI node, e.g. /dev/snd/midiC1D0 (default: find CHOMPI)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.run is not None and (args.image or args.slot or args.list or args.clear):
        ap.error("--run takes nothing else")
    if args.image and args.slot is None and not args.launcher:
        ap.error("--slot is needed to send an image")
    if args.launcher and not args.image:
        ap.error("--launcher needs the launcher's CHOMPI.bin")
    if not (args.image or args.list or args.clear or args.run is not None):
        ap.error("nothing to do: give an image and --slot, or --list, --clear or --run")

    if args.image:
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
    features = reply[7] if len(reply) > 7 else 0
    lver = ".".join(str(b) for b in reply[8:11]) if len(reply) > 10 else "unknown"
    if args.chunk:
        max_chunk = max(1, min(args.chunk, max_chunk))
    print(f"launcher {lver} on {device} (protocol {version}, {max_chunk}-byte chunks)")

    def need(feature, what):
        if not features & feature:
            sys.exit(f"this launcher cannot {what} -- put the newer launcher on the card")

    if args.run is not None:
        need(FEATURE_RUN, "start a key")
        run(link, args.run, slots)
        return

    if args.clear is not None:
        need(FEATURE_CLEAR, "clear a key")
        if not 1 <= args.clear <= slots:
            sys.exit(f"slot must be 1-{slots}")
        reply = link.call(CLEAR, bytes([args.clear]), timeout=2.0, match=for_slot(args.clear))
        check(reply, "clear")
        removed = len(reply) < 3 or reply[2]
        print(f"  key {args.clear} " + ("cleared" if removed else "was already empty"))

    if args.image and args.launcher:
        need(FEATURE_LAUNCHER, "update itself")
        send(link, image, "LAUNCHER", LAUNCHER_SLOT, max_chunk, False, launcher=True)
        return

    if args.image:
        if not 1 <= args.slot <= slots:
            sys.exit(f"slot must be 1-{slots}")
        if args.stay:
            need(FEATURE_STAY, "store without starting")
        send(link, image, name, args.slot, max_chunk, args.stay)

    if args.list:
        need(FEATURE_LIST, "list its keys")
        for slot in range(1, slots + 1):
            reply = link.call(LIST, bytes([slot]), timeout=2.0, match=for_slot(slot))
            check(reply, f"list {slot}")
            n = reply[2]
            fname = reply[3:3 + n].decode("ascii")
            size = get7(reply[3 + n:7 + n])
            print(f"  key {slot:2d}: {fname} ({size} bytes)" if n else f"  key {slot:2d}: -")


def send(link, image, name, slot, max_chunk, stay, launcher=False):
    if launcher:
        print("  storing as the new launcher, CHOMPI.bin; the current one becomes CHOMPI.old")
    else:
        target = f"{slot:02d}_{name}.bin"
        print(f"  storing as FIRMWARE/{target}, replacing anything in slot {slot}")

    check(link.call(BEGIN, put7(len(image), 4) + bytes([slot, len(name)])
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
    flags = bytes([END_STAY]) if stay else b""
    started = time.monotonic()
    reply = link.call(END, put7(zlib.crc32(image), 5) + flags, timeout=10.0,
                      retries=2, required=False)
    stored_in = time.monotonic() - started
    if reply is None:
        sys.exit("no reply to END -- the firmware was probably stored and started; "
                 "check the card")
    check(reply, "end")
    print(f"  written and checked in {stored_in:.1f} s")
    if launcher:
        print("stored -- CHOMPI restarts and installs it (rainbow), then the keys breathe white")
    else:
        print(f"stored in slot {slot}" + ("" if stay else " -- starting firmware"))


def run(link, slot, slots):
    if not 1 <= slot <= slots:
        sys.exit(f"slot must be 1-{slots}")
    # The launcher starts the firmware right after its reply, so a resend after
    # a lost reply goes unanswered: no reply means it most likely started.
    reply = link.call(RUN, bytes([slot]), timeout=1.0, retries=2, required=False)
    if reply is None:
        sys.exit("no reply to RUN -- the firmware has probably started")
    if reply[0] == 1:
        sys.exit("this launcher does not know RUN -- update it")
    check(reply, f"run slot {slot}")
    print(f"starting slot {slot}")


if __name__ == "__main__":
    main()
