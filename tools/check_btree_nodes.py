#!/usr/bin/env python3
"""Assert every HFS+ B-tree fork is a whole number of nodes.

XNU refuses a volume whose catalog, extents or attributes B-tree file length is
not a multiple of that B-tree's node size, and it does so by panicking:

    panic(cpu 3): "hfs: extentdbtree: fcb 0x... has eof 0x5a1000 not a multiple
    of 0x2000 (trims 0/0)" @hfs_btreeio.c:543

That is exactly what the v5 root image did on 2026-11-08: its catalog fork was
162,140,160 bytes = 19,792.5 nodes of 8,192.  Linux mounts such a volume without
complaint, so this check is the only thing between the build and the device.

    check_btree_nodes.py IMAGE      (raw HFS+ or Android sparse image)
"""

import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_FILL = 0xCAC2
CHUNK_SKIP = 0xCAC3
VOLUME_HEADER_OFFSET = 1024
FORK_DESCRIPTOR_BASE = 112
FORK_DESCRIPTOR_SIZE = 80
BTREE_FORKS = ((1, "extents"), (2, "catalog"), (3, "attributes"))


class Raw:
    """A plain (unsparse) image: fork offsets are image offsets."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.blk = 4096

    def read(self, offset, length):
        self.f.seek(offset)
        return self.f.read(length)


class Sparse:
    """An Android sparse image, addressed by its logical (expanded) offsets."""

    def __init__(self, path):
        self.f = open(path, "rb")
        magic, _major, _minor, fhdr, chdr, blk, _total, nchunks, _sum = struct.unpack(
            "<IHHHHIIII", self.f.read(28)
        )
        if magic != SPARSE_MAGIC:
            raise SystemExit("not an Android sparse image: %#x" % magic)
        self.blk = blk
        self.chunks = []
        pos = fhdr
        logical = 0
        for _ in range(nchunks):
            self.f.seek(pos)
            kind, res, chunk_blocks, total_bytes = struct.unpack("<HHII", self.f.read(chdr))
            self.chunks.append((logical, kind, res, chunk_blocks, pos + chdr))
            logical += chunk_blocks * blk
            pos += total_bytes

    def read(self, offset, length):
        out = bytearray()
        for logical, kind, res, chunk_blocks, data_at in self.chunks:
            span = chunk_blocks * self.blk
            if logical + span <= offset:
                continue
            if logical >= offset + length:
                break
            take_start = max(offset, logical)
            take_end = min(offset + length, logical + span)
            skip = take_start - logical
            want = take_end - take_start
            if kind == CHUNK_RAW:
                self.f.seek(data_at + skip)
                out += self.f.read(want)
            elif kind == CHUNK_FILL:
                self.f.seek(data_at)
                word = self.f.read(4)
                fill = struct.pack("<I", res) if res else word
                out += (fill * (span // 4))[skip : skip + want]
            elif kind == CHUNK_SKIP:
                out += bytes(want)
            else:
                raise SystemExit("unknown sparse chunk type %#x" % kind)
        return bytes(out)


def open_image(path):
    with open(path, "rb") as probe:
        magic = struct.unpack("<I", probe.read(4))[0]
    return Sparse(path) if magic == SPARSE_MAGIC else Raw(path)


def main(argv):
    if len(argv) != 2:
        raise SystemExit("usage: %s IMAGE" % argv[0])
    img = open_image(argv[1])
    print("reading %s image" % ("sparse" if isinstance(img, Sparse) else "raw"))

    header = img.read(VOLUME_HEADER_OFFSET, 512)
    if len(header) != 512:
        raise SystemExit("short volume header")

    ok = True
    for index, name in BTREE_FORKS:
        base = FORK_DESCRIPTOR_BASE + index * FORK_DESCRIPTOR_SIZE
        descriptor = header[base : base + FORK_DESCRIPTOR_SIZE]
        logical = struct.unpack(">Q", descriptor[0:8])[0]
        first_block = struct.unpack(">I", descriptor[16:20])[0]
        if not logical:
            print("%-10s empty" % name)
            continue
        node = img.read(first_block * img.blk, 512)
        if len(node) < 64:
            raise SystemExit("%s: cannot read its header node" % name)
        depth = struct.unpack(">H", node[14:16])[0]
        node_size = struct.unpack(">H", node[32:34])[0]
        total_nodes = struct.unpack(">I", node[36:40])[0]
        free_nodes = struct.unpack(">I", node[40:44])[0]
        aligned = node_size > 0 and logical % node_size == 0
        print(
            "%-10s logical %#x nodeSize %d nodes %d free %d depth %d %s"
            % (
                name,
                logical,
                node_size,
                total_nodes,
                free_nodes,
                depth,
                "ALIGNED" if aligned else "MISALIGNED",
            )
        )
        ok = ok and aligned

    if not ok:
        return 1
    print("all B-tree forks are whole numbers of nodes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
