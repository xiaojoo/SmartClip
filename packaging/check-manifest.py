# Check that a built binary's embedded manifest still carries every section we need.
# Exit 0 = all needles present, 3 = at least one missing, 1 = could not read the file
# (so a missing/corrupt exe can never be reported as "the manifest lost a section" --
# a plain `findstr /m` returns 1 for both of those, which is how the first version of
# this gate mis-fired while the exe was locked by a running instance).
#
# Both encodings are tried on purpose: the manifest itself is stored as UTF-8 text
# (measured: 'dpiAwareness' 3x as ASCII, 0x as UTF-16), while QStringLiteral-based
# strings in the app are UTF-16. Guessing one of the two makes "not there" and
# "looked in the wrong encoding" read identically.
#
# Usage: check-manifest.py <file> <needle> [<needle> ...]
import sys

if len(sys.argv) < 3:
    sys.stderr.write('usage: check-manifest.py <file> <needle> [...]\n')
    sys.exit(1)

path, needles = sys.argv[1], sys.argv[2:]
try:
    data = open(path, 'rb').read()
except OSError as exc:
    sys.stderr.write('cannot read %s: %s\n' % (path, exc))
    sys.exit(1)

missing = [n for n in needles
           if n.encode('utf-8') not in data and n.encode('utf-16-le') not in data]
if missing:
    sys.stderr.write('missing from manifest: %s\n' % ' '.join(missing))
sys.exit(3 if missing else 0)
