#!/usr/bin/env python3
"""Read the three 740D Multi-Media CAN-FD DBCs (G2M-1, G2M-2, G5M).

Quick start:
    pip install cantools
    python read_740d_dbc.py                  # load all 3, print a summary
    python read_740d_dbc.py --dir ./dbcs     # look for the files in ./dbcs
    python read_740d_dbc.py --show 0x354     # show one message's signals
    python read_740d_dbc.py --decode 0x354 0011223344556677...   # decode hex

Import in a pytest framework:
    from read_740d_dbc import load_all
    db = load_all()                          # {'G2M-1': Channel, ...}
    sigs = db.decode(0x354, bytes.fromhex("...."))   # searches all channels
    data = db["G2M-1"].encode(0x354, {"I_PMD": 12})
"""
import argparse
import glob
import os
import sys

try:
    import cantools
except ImportError:
    sys.exit("cantools is required:  pip install cantools")

# channel name -> dbc filename
CHANNELS = {
    "G2M-1": "740D_CANFD_G2M-1.dbc",
    "G2M-2": "740D_CANFD_G2M-2.dbc",
    "G5M":   "740D_CANFD_G5M.dbc",
}


class Channel:
    """One CAN channel / bus."""

    def __init__(self, name, path, strict=False):
        self.name = name
        self.path = path
        self.db = cantools.database.load_file(path, strict=strict)

    def message(self, ident):
        """Look up a message by name (str) or CAN ID (int / '0x354' / '852')."""
        if isinstance(ident, str):
            try:
                return self.db.get_message_by_name(ident)
            except KeyError:
                ident = int(ident, 0)
        return self.db.get_message_by_frame_id(ident)

    def decode(self, ident, data):
        msg = self.message(ident)
        if isinstance(data, str):
            data = bytes.fromhex(data.replace(" ", ""))
        return msg.decode(data, decode_choices=False, allow_truncated=True)

    def encode(self, ident, values):
        return self.message(ident).encode(values, padding=True)

    def __iter__(self):
        return iter(self.db.messages)

    def __len__(self):
        return len(self.db.messages)


class DbcSet:
    """All loaded channels, with cross-channel lookup."""

    def __init__(self, channels):
        self.channels = channels  # {name: Channel}

    def __getitem__(self, name):
        return self.channels[name]

    def items(self):
        return self.channels.items()

    def find(self, ident):
        """Return (channel_name, message) for the first channel that has it."""
        for name, ch in self.channels.items():
            try:
                return name, ch.message(ident)
            except (KeyError, ValueError):
                continue
        raise KeyError("message %r not found on any channel" % (ident,))

    def decode(self, ident, data):
        name, _ = self.find(ident)
        return self.channels[name].decode(ident, data)


def load_all(directory=".", strict=False):
    """Load whichever of the three channel DBCs exist in `directory`."""
    channels = {}
    for name, fname in CHANNELS.items():
        path = os.path.join(directory, fname)
        if os.path.exists(path):
            channels[name] = Channel(name, path, strict=strict)
    if not channels:
        found = glob.glob(os.path.join(directory, "*.dbc"))
        sys.exit("No 740D channel DBCs found in %r.\n.dbc files there: %s"
                 % (os.path.abspath(directory), found or "none"))
    return DbcSet(channels)


def _print_summary(db):
    for name, ch in db.items():
        fd = sum(1 for m in ch if getattr(m, "is_fd", False))
        sig = sum(len(m.signals) for m in ch)
        print("%-7s %-24s %4d messages (%d CAN-FD), %d signals"
              % (name, os.path.basename(ch.path), len(ch), fd, sig))


def _print_message(msg):
    print("0x%X  %s   %d bytes%s"
          % (msg.frame_id, msg.name, msg.length,
             "  [CAN-FD]" if getattr(msg, "is_fd", False) else ""))
    for s in sorted(msg.signals, key=lambda x: x.start):
        order = "LE" if s.byte_order == "little_endian" else "BE"
        sign = "s" if s.is_signed else "u"
        scale = "" if (s.scale == 1 and s.offset == 0) \
            else "  *%g +%g" % (s.scale, s.offset)
        unit = ("  " + s.unit) if s.unit else ""
        print("   %-16s start %3d  len %2d  %s/%s  [%s..%s]%s%s"
              % (s.name, s.start, s.length, order, sign,
                 s.minimum, s.maximum, scale, unit))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dir", default=".", help="folder containing the .dbc files")
    p.add_argument("--show", metavar="ID", help="show a message by CAN ID or name")
    p.add_argument("--decode", nargs=2, metavar=("ID", "HEX"),
                   help="decode a hex payload for a message")
    p.add_argument("--strict", action="store_true",
                   help="reject overlapping signals when loading")
    args = p.parse_args(argv)

    db = load_all(args.dir, strict=args.strict)

    if args.show:
        _, msg = db.find(args.show)
        _print_message(msg)
    elif args.decode:
        ident, hexstr = args.decode
        name, _ = db.find(ident)
        print("# channel %s" % name)
        for k, v in db[name].decode(ident, hexstr).items():
            print("%-16s = %s" % (k, v))
    else:
        _print_summary(db)


if __name__ == "__main__":
    main()
