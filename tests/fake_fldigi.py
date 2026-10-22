# SPDX-License-Identifier: GPL-2.0-or-later
# Scripted stand-in for fldigi's XML-RPC server (default :7362) so
# FldigiClient can be exercised without audio or a display:
#   python3 tests/fake_fldigi.py [port]
# Serves modem.get_name/get_carrier/set_carrier and the text.get_rx pair;
# the RX text grows a few characters per poll like a live decoder would.
import sys
from xmlrpc.server import SimpleXMLRPCServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 17362
STORY = "CQ CQ DE W1AW W1AW PSE K "

state = {"carrier": 1000, "served": 0}

srv = SimpleXMLRPCServer(("127.0.0.1", PORT), logRequests=False,
                         allow_none=True)
srv.register_function(lambda: "BPSK31", "modem.get_name")
srv.register_function(lambda: state["carrier"], "modem.get_carrier")


def set_carrier(hz):
    state["carrier"] = hz
    print("set_carrier", hz, flush=True)
    return hz


def rx_length():
    # Three more characters become "decoded" every time length is polled.
    state["served"] = min(state["served"] + 3, len(STORY))
    return state["served"]


srv.register_function(set_carrier, "modem.set_carrier")
srv.register_function(rx_length, "text.get_rx_length")
srv.register_function(lambda s, n: STORY[s:s + n], "text.get_rx")
print("fake fldigi on", PORT, flush=True)
srv.serve_forever()
