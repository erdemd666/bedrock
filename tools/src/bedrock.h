#ifndef BEDROCK_H
#define BEDROCK_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define HEADER "BEDROCK \xc2\xb7 built by erdem6.com"
#define VERSION "0.2.0"

/* Every check ends in exactly one of these. */
enum verdict { V_PASS, V_FAIL, V_INCONCLUSIVE, V_ABSENT };

/* record.c: everything one run found, for JSON, certificates and inventory.
 * verdict() records each result; modules call fact() for identifying data
 * ("system.serial", "cpu.brand", "disk.0.model"...). Keys are flat and dotted. */
#define RECORD_MAX 512
struct result { enum verdict v; char test[64]; char detail[384]; };
struct fact { char key[48]; char val[160]; };
extern struct result g_res[RECORD_MAX];
extern int g_nres;
extern struct fact g_fact[RECORD_MAX];
extern int g_nfact;
void record_reset(void);
void record_result(enum verdict v, const char *test, const char *detail);
void fact(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const char *fact_get(const char *key);
const char *verdict_name(enum verdict v);
void json_str(FILE *o, const char *s);
void iso_time(time_t t, char *buf, size_t n);
void record_json(FILE *o);

/* report.c: shared helpers */
extern volatile sig_atomic_t g_stop; /* set by SIGINT: long tests stop early */
extern int g_count[4];
extern int g_color; /* ANSI colours in verdict lines */
void verdict(FILE *o, enum verdict v, const char *test, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
int read_file(const char *path, char *buf, size_t n);
long long read_ll(const char *path, long long def);
void report_run(FILE *o, const char *title);
int report_mount(char *mp);
int report_save(char *path, size_t n);

/* cpu.c */
struct cpuid_leaf { uint32_t leaf, sub, r[4]; }; /* r = eax, ebx, ecx, edx */
struct cpu_info {
	char vendor[13], brand[49];
	unsigned family, model, stepping;
	uint32_t f1c, f1d, f7b, f81c, f81d;
	unsigned l1d, l1i, l2, l3; /* KiB, 0 = unknown */
};
int cpu_decode(const struct cpuid_leaf *l, int n, struct cpu_info *ci);
void cpu_flags(const struct cpu_info *ci, char *buf, size_t n);
unsigned work_sieve(void);
double work_matrix(void);
double work_matrix_sse2(void);
double work_matrix_avx(void);
#define SIEVE_PRIMES 78498 /* primes below 1,000,000 */
#define MATRIX_SUM 595.0 /* see work_matrix(); checked by the host tests */
void cpu_show(FILE *o);
void cpu_stress(FILE *o, int secs);

/* mem.c */
unsigned long mem_patterns(volatile unsigned long *p, size_t n, FILE *o);
void mem_test(FILE *o, size_t cap);

/* disk.c */
struct disk {
	char name[32], model[64], serial[64], fw[24];
	unsigned long long bytes;
	int nvme, ata; /* ata: IDENTIFY via SCSI/ATA translation worked */
	uint16_t id[256];
};
int disk_list(struct disk *d, int max);
struct hist { unsigned long long b[7]; }; /* <5 <20 <50 <150 <500 >=500 ms, error */
#define HIST_ERR UINT32_MAX
void hist_add(struct hist *h, uint32_t us);
enum verdict hist_verdict(const struct hist *h, char *why, size_t n);
void disk_show(FILE *o);
void disk_scan(FILE *o, const struct disk *d, int sampled);

/* smart.c */
struct ata_smart {
	int checksum_ok;
	long long realloc, pending, uncorrect, reported_uncorrect, crc, hours, temp;
};
struct nvme_smart {
	unsigned crit, temp_k, spare, spare_thresh, used;
	unsigned long long hours, media_errors, unsafe_shutdowns;
};
enum { SAT_NONE, SAT_IN, SAT_OUT };
void sat_cdb(uint8_t cdb[16], int dir, uint8_t cmd, uint8_t feat, uint8_t lba_mid, uint8_t lba_hi);
int sat_cmd(int fd, uint8_t cmd, uint8_t feat, uint8_t lba_mid, uint8_t lba_hi, void *buf);
int sat_out(int fd, uint8_t cmd, void *buf, unsigned tmo_ms);
int scsi_serial(int fd, char *out, size_t n);
int nvme_admin(int fd, uint8_t op, uint32_t nsid, uint32_t cdw10, void *buf, unsigned len, unsigned tmo_ms);
int nvme_log(int fd, uint8_t lid, uint8_t buf[512]);
int nvme_smart_log(int fd, uint8_t buf[512]);
void smart_ata_parse(const uint8_t s[512], struct ata_smart *a);
enum verdict smart_ata_verdict(const struct ata_smart *a, char *why, size_t n);
void smart_nvme_parse(const uint8_t s[512], struct nvme_smart *m);
enum verdict smart_nvme_verdict(const struct nvme_smart *m, char *why, size_t n);
void smart_show(FILE *o, const struct disk *d);

/* board.c */
struct dimm {
	char loc[32], bank[32], maker[32], part[32], serial[24];
	const char *type;
	unsigned mb, speed; /* mb 0 = empty slot */
};
#define DIMM_UNKNOWN 0xffffffffu
struct smbios {
	char bios_vendor[64], bios_ver[64], bios_date[16];
	char sys_maker[64], sys_product[64], sys_serial[64];
	char board_maker[64], board_product[64], board_ver[32];
	char cpu_socket[32], cpu_ver[64];
	unsigned cpu_max_mhz, cpu_cur_mhz, cpu_cores, cpu_threads;
	int nslots;
	struct dimm dimm[64];
};
int smbios_parse(const uint8_t *t, size_t n, struct smbios *s);
void board_show(FILE *o);

/* gpu.c */
int pciids_lookup(FILE *f, unsigned ven, unsigned dev, char *vname, size_t vn, char *dname, size_t dn);
void gpu_show(FILE *o);
void gpu_display_test(void);

/* wipe.c */
int dev_on_disk(const char *dev, const char *disk);
int boot_disk_refuse(const char *disk, const char *mounts, const char *cmdline);
struct ata_sec { int supported, enabled, locked, frozen, expired, enhanced, normal_min, enhanced_min; };
void ata_sec_decode(const uint16_t id[256], struct ata_sec *s);
void ata_sec_data(uint8_t buf[512], int enhanced);
#define SEC_PW "wipe" /* temporary ATA user password; shown to the user if the drive stays locked */
struct nvme_erase { uint32_t sanitize, format, format_nsid; int all_ns; const char *name; };
void nvme_erase_plan(const uint8_t ctrl[4096], const uint8_t ns[4096], uint32_t nsid, struct nvme_erase *e);
int nvme_sanitize_status(const uint8_t log[512], double *pct);
struct erase_tally { int zero, filled, changed, unchanged, unread; unsigned fill; };
uint64_t sample_hash(const uint8_t *p, size_t n);
void erase_tally_add(struct erase_tally *t, const uint8_t *p, size_t n, uint64_t before);
enum verdict erase_tally_verdict(const struct erase_tally *t, char *why, size_t n);
void wipe_run(void);

/* functional.c: the hands-on checklist (menu) and presence checks (station) */
struct kbkey { uint16_t code; uint8_t row, block, col; const char *label; };
const struct kbkey *kb_layout(int *n);
int kb_index(unsigned code);
int usb_port_path(const char *portname, char *out, size_t n);
const char *usb_speed_name(double mbps);
int ask_char(const char *prompt);
void functional_presence(FILE *o);
void functional_run(void);

/* audio.c */
#define LEVEL_FLOOR -96.0
double level_dbfs(const int16_t *s, size_t n);
int level_cols(double db, int width);
enum verdict speaker_test(void);
enum verdict mic_test(void);

/* camera.c */
void yuyv_rgb(int y, int u, int v, uint8_t rgb[3]);
void fit_rect(unsigned sw, unsigned sh, unsigned dw, unsigned dh, unsigned *w, unsigned *h);
enum verdict camera_test(void);

#endif
