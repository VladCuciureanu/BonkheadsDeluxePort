"""Debug helper: print the settings stored in a 'Bonkheads Deluxe Prefs' file.

usage: python -I tools/readprefs.py <path to "Bonkheads Deluxe Prefs.rsrc">
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import rsrc

if len(sys.argv) != 2:
    sys.exit(__doc__.strip())
r = rsrc.read_rsrc(open(sys.argv[1], 'rb').read())
b = r['Bonk'][0][2]
keys = [b[i] for i in range(12)]
print('player 1 keys (left, jump, right):', ['%02X' % k for k in keys[0:6:2]])
print('player 2 keys (left, jump, right):', ['%02X' % k for k in keys[6:12:2]])
print('volume item (1548):', struct.unpack_from('>h', b, 1548)[0], '(11 = off)')
print('music (1550):', b[1550])
print('difficulty (1552):', struct.unpack_from('>h', b, 1552)[0], '(0 easy, 1 medium, 2 hard)')
name = b[2067 + 1:2067 + 1 + b[2067]].decode('mac_roman')
print('level set (2067):', name)
