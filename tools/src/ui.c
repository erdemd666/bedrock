#include "bedrock.h"

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <unistd.h>

static void on_int(int s)
{
	(void)s;
	g_stop = 1;
}

static void screen(void)
{
	printf("\033[2J\033[H\033[1m%s\033[0m\n\n", HEADER);
}

static int ask(const char *prompt, char *buf, size_t n)
{
	printf("%s", prompt);
	fflush(stdout);
	if (!fgets(buf, n, stdin))
		return -1;
	buf[strcspn(buf, "\r\n")] = 0;
	return 0;
}

/* The VGA console has no scrollback, so long results are shown a page at a time. */
static void pager(char *text)
{
	struct winsize w;
	int rows = !ioctl(1, TIOCGWINSZ, &w) && w.ws_row > 6 ? w.ws_row : 24, line = 0;
	char in[16];
	screen();
	for (char *l = text; *l;) {
		char *nl = strchr(l, '\n');
		size_t len = nl ? (size_t)(nl - l + 1) : strlen(l);
		fwrite(l, 1, len, stdout);
		l += len;
		if (++line >= rows - 4 && *l) {
			if (ask("\n-- Enter for more, q to stop --", in, sizeof in) || in[0] == 'q')
				return;
			screen();
			line = 0;
		}
	}
	ask("\n-- Enter to return to the menu --", in, sizeof in);
}

static void run_paged(void (*fn)(FILE *))
{
	char *buf = NULL;
	size_t len = 0;
	FILE *m = open_memstream(&buf, &len);
	if (!m)
		return;
	screen();
	printf("Running... Ctrl-C stops long tests early.\n");
	fflush(stdout);
	g_stop = 0;
	fn(m);
	fclose(m);
	pager(buf);
	free(buf);
}

static void sysinfo(FILE *o) { cpu_show(o); board_show(o); gpu_show(o); }
static void stress(FILE *o) { cpu_stress(o, 60); }
static void memtest(FILE *o) { mem_test(o, 0); }
static struct disk scan_disk;
static void scan(FILE *o) { disk_scan(o, &scan_disk, 0); }

static void pick_and_scan(void)
{
	struct disk d[32];
	char in[16];
	int n = disk_list(d, 32);
	screen();
	for (int i = 0; i < n; i++)
		printf("  %d  %-8s %-28s %8.1f GB\n", i + 1, d[i].name, d[i].model, d[i].bytes / 1e9);
	if (!n)
		printf("No disks found.\n");
	if (!n || ask("\nDisk number to scan (read-only; Enter to cancel): ", in, sizeof in))
		return;
	int k = atoi(in) - 1;
	if (k < 0 || k >= n)
		return;
	scan_disk = d[k];
	run_paged(scan);
}

static void pause_enter(void)
{
	char in[16];
	ask("\nPress Enter to return to the menu.", in, sizeof in);
}

static void save_report(void)
{
	char path[512];
	screen();
	printf("Running quick checks for the report (about a minute)...\n");
	if (report_save(path, sizeof path))
		printf("Could not write the report: no writable FAT partition mounted.\n");
	else
		printf("Saved %s\n", path);
	pause_enter();
}

static void display(void) { screen(); gpu_display_test(); pause_enter(); }
static void wipe(void) { screen(); wipe_run(); pause_enter(); }
static void do_sysinfo(void) { run_paged(sysinfo); }
static void do_disks(void) { run_paged(disk_show); }
static void do_stress(void) { run_paged(stress); }
static void do_memtest(void) { run_paged(memtest); }

/* The main menu. Each feature adds one line here; keys are shown in this order. */
static const struct item {
	char key;
	const char *label;
	void (*run)(void);
} menu[] = {
	{'1', "System info: CPU, board, memory slots, sensors, PCI and GPU", do_sysinfo},
	{'2', "Disks: identify and SMART health", do_disks},
	{'3', "Disk surface scan (read-only)", pick_and_scan},
	{'4', "CPU stress test (60 s, every core)", do_stress},
	{'5', "Memory test (all free RAM)", do_memtest},
	{'6', "Display test (colours and gradients)", display},
	{'7', "Save a report to the USB stick", save_report},
	{'8', "Wipe a disk (destroys data)", wipe},
};

int main(int argc, char **argv)
{
	char in[64];
	signal(SIGINT, on_int);
	if (argc >= 2 && !strcmp(argv[1], "--report") && argc <= 3) {
		FILE *o = argc == 3 ? fopen(argv[2], "w") : stdout;
		if (!o) {
			perror(argv[2]);
			return 2;
		}
		report_run(o, "Headless report");
		if (fclose(o))
			return 2;
		return g_count[V_FAIL] ? 1 : 0;
	}
	if (argc >= 2) {
		fprintf(stderr, "usage: bedrock [--report [file]]\n");
		return 2;
	}
	g_color = 1;
	for (;;) {
		screen();
		for (size_t i = 0; i < sizeof menu / sizeof *menu; i++)
			printf("  %c  %s\n", menu[i].key, menu[i].label);
		printf("\n  r  Reboot    p  Power off    q  Quit\n\n"
		       "For a RAM test that also covers memory in use by the system, reboot and\n"
		       "choose Full memory test in the boot menu.\n\n");
		if (ask("Choice: ", in, sizeof in))
			return 0; /* no console input: nothing to wait for */
		g_stop = 0;
		for (size_t i = 0; i < sizeof menu / sizeof *menu; i++)
			if (in[0] == menu[i].key)
				menu[i].run();
		switch (in[0]) {
		case 'r': sync(); reboot(RB_AUTOBOOT); perror("reboot"); pause_enter(); break;
		case 'p': sync(); reboot(RB_POWER_OFF); perror("power off"); pause_enter(); break;
		case 'q': return 0;
		}
	}
}
