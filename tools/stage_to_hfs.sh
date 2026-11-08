#!/bin/bash
# Turn an already-staged iOS root tree into the journaled HFS+ image that goes
# on userdata.  The tree is written once, in order: this is the only writer of
# the volume, so the Linux hfsplus driver never rewrites the catalog B-tree.
#
#   stage_to_hfs.sh STAGEDIR OUT.img [SIZE]        (SIZE default 16G)
#
# Why not mount the volume and edit it: on 2026-11-08 three images were built
# from the same tree and only the one that was never modified after mkiosroot.sh
# mounted.  Editing 42 plists on a read-write HFS+ mount made the driver rebuild
# the catalog B-tree (treeDepth 4 -> 3, rootNode 19668 -> 283, freeNodes 1393 ->
# 90) and the device then failed to mount it: v4 returned EIO from cat_idlookup,
# v5 panicked in hfs_btreeio.c:543 with "eof 0x5a1000 not a multiple of 0x2000".
set -euo pipefail

[[ $# -ge 2 ]] || { sed -n '2,10p' "$0"; exit 2; }
staged=$(realpath "$1") out=$2 size=${3:-16G}
[[ -d $staged ]] || { echo "no such tree: $staged" >&2; exit 1; }
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

mount_point=$(mktemp -d)
cleanup() {
	if mountpoint -q "$mount_point"; then
		sudo umount "$mount_point" || true
	fi
	rmdir "$mount_point" 2>/dev/null || true
}
trap cleanup EXIT

truncate -s "$size" "$out"
mkfs.hfsplus -s -J -v iOSRoot "$out"
sudo mount -t hfsplus -o loop,force "$out" "$mount_point"
echo "copying $staged -> volume (single pass, rsync -aH)"
sudo rsync -aH --numeric-ids "$staged/" "$mount_point/"
sync
df -h "$mount_point"
sudo umount "$mount_point"

# Read-only check.  -n never rewrites anything; a freshly formatted journaled
# volume always reports "Volume header needs minor repair", so accept that.
check=$(fsck.hfsplus -f -n "$out" 2>&1 || true)
printf '%s\n' "$check" | grep -v 'Illegal name' || true
if printf '%s\n' "$check" | grep -Eq 'Invalid node|damaged|not a HFS|bad catalog|Invalid catalog'; then
	echo "fsck.hfsplus found real damage" >&2
	exit 1
fi

# Prove the volume reads back: the catalog B-tree is what the device failed on.
verify=$(mktemp -d)
sudo mount -t hfsplus -o loop,ro,force "$out" "$verify"
sudo ls -l "$verify" | head -20
sudo test -f "$verify/System/Library/CoreServices/SystemVersion.plist"
sudo test -f "$verify/System/Library/xpc/launchd.plist"
sudo python3 - "$verify" <<'PYVERIFY'
import plistlib, struct, sys
root = sys.argv[1]
with open(root + '/System/Library/CoreServices/SystemVersion.plist', 'rb') as f:
    sv = plistlib.load(f)
print('SystemVersion:', sv.get('ProductVersion'), sv.get('ProductBuildVersion'))
with open(root + '/System/Library/xpc/launchd.plist', 'rb') as f:
    data = plistlib.load(f)
jobs = data.get('LaunchDaemons', {})
off = [k for k, v in jobs.items() if isinstance(v, dict) and v.get('Disabled')]
print('jobs:', len(jobs), 'disabled:', len(off))
for k in sorted(off)[:8]:
    print('  disabled', k)
PYVERIFY
sudo umount "$verify"
rmdir "$verify" 2>/dev/null || true

# The catalog B-tree file length must be a whole number of nodes, or XNU panics.
python3 - "$out" <<'PYNODES'
import struct, sys
path = sys.argv[1]


class Sparse:
    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(28)
        (magic, maj, mnr, fh, ch, blk, total, nchunks, chk) = struct.unpack('<IHHHHIIII', hdr)
        assert magic == 0xED26FF3A, hex(magic)
        self.blk = blk
        self.chunks = []
        pos = 28
        for _ in range(nchunks):
            self.f.seek(pos)
            t, res, csz, tsz = struct.unpack('<HHII', self.f.read(12))
            self.chunks.append((t, pos + 12, csz, tsz))
            pos += 12 + (tsz if t != 0xCAC3 else 0)
        self.size = sum(c[3] for c in self.chunks)

    def read(self, offset, length):
        out = bytearray()
        logical = 0
        for t, data_at, csz, tsz in self.chunks:
            if logical + tsz <= offset:
                logical += tsz
                continue
            if logical >= offset + length:
                break
            lo = max(offset - logical, 0)
            hi = min(offset + length - logical, tsz)
            if t == 0xCAC1:
                self.f.seek(data_at)
                self.f.seek(data_at + lo)
                out += self.f.read(hi - lo)
            elif t == 0xCAC2:
                self.f.seek(data_at)
                fill = self.f.read(4)
                out += fill * ((hi - lo + 3) // 4)
                del out[len(out) - (((hi - lo + 3) // 4) * 4 - (hi - lo)):]
            logical += tsz
        return bytes(out)


img = Sparse(path)
vh = img.read(1024, 512)
ok = True
for idx, name in ((1, 'extents'), (2, 'catalog'), (3, 'attributes')):
    raw = vh[112 + idx * 80:112 + idx * 80 + 80]
    logical = struct.unpack('>Q', raw[0:8])[0]
    start = struct.unpack('>I', raw[16:20])[0]
    node = img.read(start * img.blk, 4096)
    node_size = struct.unpack('>H', node[32:34])[0]
    total_nodes = struct.unpack('>I', node[36:40])[0]
    free_nodes = struct.unpack('>I', node[40:44])[0]
    depth = struct.unpack('>H', node[14:16])[0]
    good = logical % node_size == 0
    print('%-10s logical %#x nodeSize %d nodes %d free %d depth %d %s' % (
        name, logical, node_size, total_nodes, free_nodes, depth,
        'ALIGNED' if good else 'MISALIGNED'))
    ok = ok and good
if not ok:
    sys.exit('a B-tree file length is not a whole number of nodes; XNU would panic')
print('all B-tree forks are whole numbers of nodes')
PYNODES

ls -l "$out"
sha256sum "$out"
