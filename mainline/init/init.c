// SPDX-License-Identifier: GPL-2.0
/*
 * Milestone-1 init for the Galaxy A5 (2017) on mainline Linux.
 * Built with the kernel's nolibc (tools/include/nolibc), static, no libc.
 *
 * What it proves on the phone:
 *  1. Linux reached userspace (this text is on screen at all).
 *  2. Phase A (first PHASE_A_S seconds): DECON is switched to hardware
 *     (TE) trigger with auto-mask off and we do NOT push frames ourselves.
 *     If text appears and the counter moves during phase A, the panel
 *     refreshes on its own, so a normal graphics stack will just work.
 *     (Samsung decon_reg_7880.c:384 decon_reg_configure_trigger: HW mode
 *     sets HW_TRIG_EN; decon_reg_set_trigger unmasks HW_TRIG_MASK.)
 *  3. Phase B: we push frames through /dev/mem with the software trigger
 *     proven in step 1b, so the screen is readable either way.
 *  4. After RECOVERY_AFTER_S the phone reboots with the "recovery" reason.
 *     Landing in TWRP proves the syscon reboot-mode node works.
 */

#include <linux/reboot.h>

#define DECON_BASE		0x14830000UL	/* exynos7880.dtsi decon_f */
#define HW_SW_TRIG_CONTROL	0x70		/* decon_7880/regs-decon.h:92 */
#define TRIG_IDLE		0x1000		/* AUTO_MASK */
#define TRIG_FIRE		0x1100		/* AUTO_MASK | SW_TRIG */
#define TRIG_HW_CONTINUOUS	0x0010		/* HW_TRIG_EN, unmasked, no auto-mask */

#define PHASE_A_S		15
#define RECOVERY_AFTER_S	180

static volatile unsigned int *decon;
static int sw_refresh;

static void puts_(const char *s)
{
	write(1, s, strlen(s));
}

static void read_file(const char *path, char *buf, int len)
{
	int fd = open(path, O_RDONLY);
	int n = 0;

	buf[0] = 0;
	if (fd >= 0) {
		n = read(fd, buf, len - 1);
		close(fd);
	}
	if (n < 0)
		n = 0;
	buf[n] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
}

static void first_line_of(const char *path, const char *key, char *out, int len)
{
	static char buf[4096];
	char *p;

	read_file(path, buf, sizeof(buf));
	out[0] = 0;
	p = strstr(buf, key);
	if (!p)
		return;
	for (int i = 0; i < len - 1 && p[i] && p[i] != '\n'; i++) {
		out[i] = p[i];
		out[i + 1] = 0;
	}
}

static void kick(void)
{
	if (!decon || !sw_refresh)
		return;
	decon[HW_SW_TRIG_CONTROL / 4] = TRIG_IDLE;
	decon[HW_SW_TRIG_CONTROL / 4] = TRIG_FIRE;
}

static void map_decon(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	void *p;

	if (fd < 0) {
		puts_("  /dev/mem: open failed\n");
		return;
	}
	p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DECON_BASE);
	if (p == MAP_FAILED) {
		puts_("  /dev/mem: mmap of DECON failed\n");
		return;
	}
	decon = p;
}

int main(void)
{
	struct utsname u;
	char line[160], cpus[64];

	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/dev", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

	map_decon();
	if (decon)
		decon[HW_SW_TRIG_CONTROL / 4] = TRIG_HW_CONTINUOUS;

	uname(&u);
	read_file("/sys/devices/system/cpu/online", cpus, sizeof(cpus));
	first_line_of("/proc/meminfo", "MemTotal", line, sizeof(line));

	puts_("\n\n=========================================\n");
	puts_(" Galaxy A5 (2017) - mainline Linux M1\n");
	puts_("=========================================\n");
	puts_(" kernel : "); puts_(u.release); puts_(" "); puts_(u.machine); puts_("\n");
	puts_(" cpus   : "); puts_(cpus); puts_("\n");
	puts_(" memory : "); puts_(line); puts_("\n\n");
	puts_(" PHASE A: hardware (TE) refresh only.\n");
	puts_(" If you can read this before t = 15,\n");
	puts_(" the panel refreshes by itself.\n\n");

	for (int t = 0; t < RECOVERY_AFTER_S; t++) {
		char msg[96];

		if (t == PHASE_A_S) {
			puts_("\n\n PHASE B: software refresh on\n");
			sw_refresh = 1;
			puts_(" Reboots into recovery (TWRP) at ");
			puts_(utoa(RECOVERY_AFTER_S));
			puts_(" s\n\n");
		}

		strcpy(msg, "\r t = ");
		strlcat(msg, utoa(t), sizeof(msg));
		strlcat(msg, t < PHASE_A_S ? " s  (phase A)   " : " s  (phase B)   ", sizeof(msg));
		puts_(msg);

		/* 1 s, pushing a frame every 50 ms in phase B */
		for (int i = 0; i < 20; i++) {
			kick();
			msleep(50);
		}
	}

	puts_("\n Rebooting to recovery...\n");
	for (int i = 0; i < 10; i++) {
		kick();
		msleep(50);
	}
	__nolibc_syscall4(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		    LINUX_REBOOT_CMD_RESTART2, "recovery");
	for (;;)
		sleep(1);
	return 0;
}
