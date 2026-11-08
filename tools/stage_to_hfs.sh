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

# Read-only check.  -n never rewrites anything, so this cannot damage the volume
# the way "fsck -y" did (see the header of this file).  A freshly formatted
# journaled volume always reports "Volume header needs minor repair" and often
# "was found corrupt and needs to be repaired" because the journal is not in the
# state Linux expects; XNU replays it and mounts fine.  Only a structural finding
# fails the build, and the mount below is the real gate.
check=$(fsck.hfsplus -f -n "$out" 2>&1 || true)
printf '%s\n' "$check" | grep -v 'Illegal name' || true
if printf '%s\n' "$check" | grep -Eq 'Invalid node|not a HFS|bad catalog|Invalid catalog|Invalid extent'; then
	echo "fsck.hfsplus found structural damage" >&2
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
python3 "$(dirname "$0")/check_btree_nodes.py" "$out"
ls -l "$out"
sha256sum "$out"
