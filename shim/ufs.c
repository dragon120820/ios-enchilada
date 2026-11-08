/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x01d84000UL	/* ufshc@1d84000, fajita/sdm845 live FDT */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));

/*
 * sweetlog (xnu 0047) keeps the console in a 4 MiB ring and copies it to the
 * last 4 MiB + 4 KiB of the root partition: block 0 is a header, the ring
 * follows. Ring byte n lives at block 1 + n / BLOCK, offset n % BLOCK, so the
 * whole area is one contiguous read.
 */
#define SWEET_LOG_SIZE		(4u << 20)
#define SWEET_LOG_BLOCKS	(1 + SWEET_LOG_SIZE / BLOCK)
#define LINE_MAX		64
/* Only the tail is shown, so the ring is read a window at a time: a 4 MiB
 * .bss object would push _sec_stacks out of adr range in start.S. */
#define LOG_WINDOW_BLOCKS	16
#define LOG_WINDOW		(LOG_WINDOW_BLOCKS * BLOCK)
static uint8_t logwin[LOG_WINDOW] __attribute__((aligned(4096)));

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/*
 * The ring keeps the oldest bytes at offset 0 while total < rsize, so the
 * first lines of the ring are the start of the boot: kernel init, then the
 * launchd exec lines in the order they happened. Print those verbatim.
 */
#define HEAD_LINES	16

static void print_head(uint32_t ring_base, uint32_t rsize, uint64_t total, int want)
{
	uint64_t limit = total < rsize ? total : rsize;
	uint32_t cnt = LOG_WINDOW_BLOCKS;
	if ((uint64_t)cnt * BLOCK > limit)
		cnt = (uint32_t)((limit + BLOCK - 1) / BLOCK);
	if (!cnt)
		return;
	if (cnt > SWEET_LOG_BLOCKS - 1)
		cnt = SWEET_LOG_BLOCKS - 1;
	if (read_blocks(ring_base + 1, cnt, logwin)) {
		con_str("SWEETLOG", "head read error");
		return;
	}
	uint64_t have = (uint64_t)cnt * BLOCK;
	if (have > limit)
		have = limit;

	char buf[LINE_MAX + 1];
	int n = 0, shown = 0;

	for (uint64_t i = 0; i < have && shown < want; i++) {
		char c = (char)logwin[i];
		if (c == '\n') {
			buf[n] = 0;
			con_line(buf);
			n = 0;
			shown++;
			continue;
		}
		if (n < LINE_MAX)
			buf[n++] = c;
	}
}

/*
 * Does s contain needle? A static array of string pointers is the one thing
 * this object cannot have: -fno-pic emits an ABS64 relocation for each element
 * and the CI check fails. Call the search with literal arguments instead, so
 * every address is PC-relative.
 */
static int contains(const char *s, const char *needle)
{
	for (const char *a = s; *a; a++) {
		const char *p = a, *b = needle;
		while (*b && *p == *b)
			p++, b++;
		if (!*b)
			return 1;
	}
	return 0;
}

static int has_needle(const char *s)
{
	return contains(s, "CRASH") || contains(s, "panic") || contains(s, "error")
		|| contains(s, "failed") || contains(s, "ASSERT");
}

/*
 * Walk the whole ring and keep the LAST lines that name a failure. The first
 * ones are boot-time noise; what matters is what was still crashing when the
 * UI stopped moving, and that is at the end of the ring.
 *
 * The history is a fixed array of char rows, not an array of pointers: under
 * -fno-pic a pointer array emits an ABS64 relocation per element and the CI
 * guard rejects the object.
 */
#define FILTER_LINES	24
#define FILTER_COLS	(LINE_MAX + 1)

static char recent[FILTER_LINES][FILTER_COLS];
static int recent_at, recent_n;

static void recent_reset(void)
{
	recent_at = 0;
	recent_n = 0;
}

static void recent_add(const char *s)
{
	char *row = recent[recent_at];
	int i = 0;

	while (s[i] && i < LINE_MAX) {
		row[i] = s[i];
		i++;
	}
	row[i] = 0;
	recent_at = (recent_at + 1) % FILTER_LINES;
	if (recent_n < FILTER_LINES)
		recent_n++;
}

static void recent_print(void)
{
	for (int k = 0; k < recent_n; k++) {
		int idx = (recent_at - recent_n + k + FILTER_LINES * 2) % FILTER_LINES;
		con_line(recent[idx]);
	}
}

static void print_recent_failures(uint32_t ring_base, uint32_t rsize, uint64_t total)
{
	uint64_t limit = total < rsize ? total : rsize;
	char cur[FILTER_COLS];
	int n = 0;
	int matched = 0;

	recent_reset();

	for (uint64_t at = 0; at < limit; ) {
		uint32_t off = (uint32_t)(at % rsize);
		uint32_t blk = off / BLOCK;
		uint32_t cnt = LOG_WINDOW_BLOCKS;

		if (at + (uint64_t)LOG_WINDOW > limit)
			cnt = (uint32_t)((limit - at + BLOCK - 1) / BLOCK);
		if (!cnt)
			break;
		if (blk + cnt > SWEET_LOG_BLOCKS - 1)
			cnt = SWEET_LOG_BLOCKS - 1 - blk;
		if (!cnt)
			break;
		if (read_blocks(ring_base + 1 + blk, cnt, logwin)) {
			con_str("SWEETLOG", "scan read error");
			return;
		}
		uint64_t have = (uint64_t)cnt * BLOCK;
		if (at + have > limit)
			have = limit - at;
		if (!have)
			break;

		for (uint64_t i = 0; i < have; i++) {
			char c = (char)logwin[i];
			if (c == '\n') {
				cur[n] = 0;
				if (n && has_needle(cur)) {
					recent_add(cur);
					matched++;
				}
				n = 0;
				continue;
			}
			if (n < LINE_MAX)
				cur[n++] = c;
		}
		at += have;
	}

	con_hex("SWEETLOG matched", (uint64_t)matched);
	con_str("SWEETLOG", "last lines that name a failure:");
	recent_print();
}

static void dump_sweetlog(uint32_t first, uint32_t nblocks)
{
	con_clear(0xff000000);
	con_str("SWEETLOG", "console ring at the tail of userdata");
	if (nblocks < SWEET_LOG_BLOCKS) {
		con_str("SWEETLOG", "partition smaller than the log");
		return;
	}
	uint32_t base = first + nblocks - SWEET_LOG_BLOCKS;
	con_hex2("SWEETLOG", "first=", first, "blocks=", nblocks);
	con_hex2("SWEETLOG", "base=", base, "blocks=", SWEET_LOG_BLOCKS);

	if (read_blocks(base, 1, logwin)) {
		con_str("SWEETLOG", "header read error");
		return;
	}
	const uint8_t *h = logwin;
	static const char magic[8] = { 'S', 'W', 'E', 'E', 'T', 'L', 'O', 'G' };
	int ok = 1;
	for (int i = 0; i < 8; i++)
		if (h[i] != (uint8_t)magic[i])
			ok = 0;
	if (!ok) {
		con_str("SWEETLOG", "no header: log never started");
		char m[9];
		for (int i = 0; i < 8; i++)
			m[i] = (h[i] >= 32 && h[i] < 127) ? (char)h[i] : '.';
		m[8] = 0;
		con_str("magic", m);
		return;
	}
	uint32_t rsize = le32(h + 12);
	uint64_t total = le64(h + 16), seq = le64(h + 24);
	con_hex2("SWEETLOG", "vers=", le32(h + 8), "size=", rsize);
	con_hex("SWEETLOG total", total);
	con_hex("SWEETLOG seq", seq);
	if (rsize != SWEET_LOG_SIZE) {
		con_str("SWEETLOG", "ring size mismatch");
		return;
	}
	con_str("SWEETLOG", "first lines:");
	print_head(base, rsize, total, HEAD_LINES);
	print_recent_failures(base, rsize, total);
	con_str("SWEETLOG", "done");
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
		if (!__builtin_strcmp(name, "userdata"))
			dump_sweetlog((uint32_t)first, (uint32_t)(last - first + 1));
	}
}

void test_ufs(void)
{
	con_str("UFS", "enchilada milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}
