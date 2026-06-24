#!/usr/bin/env python3
"""Signal-level codec for the CAN test runner.

Lets test cases be written by SIGNAL instead of by raw bytes:
  * encode(channel, id, {signal: value})  -> payload bytes to send
  * decode(channel, id, payload)          -> {signal: value} actually received
  * check (channel, id, payload, expected)-> (ok, [(sig, got, want), ...])

The check compares ONLY the signals you list, so the rest of the frame is
"don't care" - much less brittle than a full-byte comparison.

Requires: pip install cantools
"""
import os
import cantools

# ---------------------------------------------------------------------------
# Map the YAML "channel:" number to a DBC file. Edit to match your buses.
# ---------------------------------------------------------------------------
DBC_DIR = os.path.dirname(os.path.abspath(__file__))
CHANNEL_DBC = {
    1: "740D_CANFD_G2M-1.dbc",
    2: "740D_CANFD_G2M-2.dbc",
    3: "740D_CANFD_G5M.dbc",
}


class SignalCodec:
    def __init__(self, channel_dbc=None, dbc_dir=DBC_DIR, strict=False):
        channel_dbc = channel_dbc or CHANNEL_DBC
        self.dbs = {}
        for ch, fname in channel_dbc.items():
            path = os.path.join(dbc_dir, fname)
            if os.path.exists(path):
                self.dbs[ch] = cantools.database.load_file(path, strict=strict)

    # -- lookup -------------------------------------------------------------
    def _msg(self, channel, ident):
        db = self.dbs.get(channel)
        if db is None:
            raise KeyError("no DBC loaded for channel %r" % (channel,))
        if isinstance(ident, str):
            try:
                return db.get_message_by_name(ident)
            except KeyError:
                ident = int(ident, 0)              # "0x601" / "1537"
        return db.get_message_by_frame_id(ident)

    def message(self, channel, ident):
        """Public message lookup by name or CAN ID."""
        return self._msg(channel, ident)

    def frame_id(self, channel, ident):
        """Return the numeric CAN ID for a message name or id."""
        return self._msg(channel, ident).frame_id

    # -- encode: signals -> bytes ------------------------------------------
    def encode(self, channel, ident, signals, fill=0):
        """Build a full payload. Signals you omit default to their DBC initial
        value (or `fill`). Returns bytes of length = message DLC."""
        msg = self._msg(channel, ident)
        values = {}
        for s in msg.signals:
            init = getattr(s, "initial", None)
            values[s.name] = init if init is not None else fill
        values.update(signals)
        return msg.encode(values, scaling=True, padding=True, strict=False)

    def encode_hex(self, channel, ident, signals, fill=0):
        """Same as encode() but returns ['0x11','0x22',...] like your YAML."""
        return ["0x%02X" % b for b in self.encode(channel, ident, signals, fill)]

    # -- decode: bytes -> signals ------------------------------------------
    def decode(self, channel, ident, payload):
        msg = self._msg(channel, ident)
        if isinstance(payload, (list, tuple)):       # ['0x11', 34, ...] -> bytes
            payload = bytes(int(b, 0) if isinstance(b, str) else int(b)
                            for b in payload)
        elif isinstance(payload, str):
            payload = bytes.fromhex(payload.replace("0x", "").replace(" ", ""))
        return msg.decode(payload[:msg.length], decode_choices=False,
                          allow_truncated=True)

    # -- check: compare only the listed signals ----------------------------
    def check(self, channel, ident, payload, expected, tol=1e-9):
        """Return (ok, mismatches). `expected` = {signal: value}."""
        got = self.decode(channel, ident, payload)
        bad = []
        for sig, want in expected.items():
            have = got.get(sig)
            if isinstance(want, str):                # named/enum value
                want = int(want, 0) if want.lower().startswith("0x") else want
            try:
                ok = abs(float(have) - float(want)) <= tol
            except (TypeError, ValueError):
                ok = (have == want)
            if not ok:
                bad.append((sig, have, want))
        return (not bad, bad)


# ---------------------------------------------------------------------------
# Helper to fold signal-style cases into your existing byte-style runner.
# Call this on each test-case dict right after yaml.safe_load().
# ---------------------------------------------------------------------------
def expand_send(test_case, codec):
    """If a case has `send_signals`, populate `send` with the encoded bytes so
    the rest of your runner can stay byte-based. Returns the case (mutated)."""
    if "send_signals" in test_case:
        ch = test_case.get("channel", 1)
        ident = test_case.get("message") or test_case["can_id"]
        test_case["send"] = codec.encode_hex(ch, ident, test_case["send_signals"])
    return test_case


if __name__ == "__main__":
    # quick self-test / debugging from the command line
    import sys
    codec = SignalCodec()
    if len(sys.argv) >= 4 and sys.argv[1] == "encode":
        ch, ident = int(sys.argv[2]), sys.argv[3]
        sigs = dict(p.split("=") for p in sys.argv[4:])
        sigs = {k: int(v, 0) for k, v in sigs.items()}
        print(" ".join(codec.encode_hex(ch, ident, sigs)))
    elif len(sys.argv) >= 5 and sys.argv[1] == "decode":
        ch, ident, hexstr = int(sys.argv[2]), sys.argv[3], sys.argv[4]
        for k, v in codec.decode(ch, ident, hexstr).items():
            print("%-16s = %s" % (k, v))
    else:
        print("usage:\n  python signal_codec.py encode 1 0x601 PWL_EQ=1"
              "\n  python signal_codec.py decode 1 0x354 <hexpayload>")
