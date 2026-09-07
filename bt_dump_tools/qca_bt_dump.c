// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#ifndef BTPROTO_HCI
#define BTPROTO_HCI 1
#endif

#ifndef N_TTY
#define N_TTY 0
#endif

#ifndef N_HCI
#define N_HCI 15
#endif

#define HCIUARTSETPROTO _IOW('U', 200, int)
#define HCIUARTGETDEVICE _IOR('U', 202, int)
#define HCIUARTSETFLAGS _IOW('U', 203, int)
#define HCIUARTTRIGGERDUMP _IO('U', 205)

#define HCI_UART_QCA 8
#define HCI_UART_EXT_CONFIG 4
#define QCA_IBS_WAKE_IND 0xfd
#define QCA_IBS_WAKE_ACK 0xfc

#define HCIDEVUP _IOW('H', 201, int)
#define HCIDEVDOWN _IOW('H', 202, int)

#define BT_CMD_PWR_CTRL 0xbfad
#define BT_POWER_ENABLE 1

#define DEFAULT_TTY "/dev/ttyHS0"
#define DEFAULT_BTPOWER "/dev/btpower"
#define DEFAULT_SYSFS "/sys/class/devcoredump"
#define DEFAULT_BAUD 3000000U
#define DEFAULT_TIMEOUT 30U
#define DEFAULT_INIT_DELAY_MS 200U
#define DEFAULT_MAX_DUMP (512U * 1024U * 1024U)
#define MAX_BASELINE_DUMPS 256U

struct dump_names {
	char names[MAX_BASELINE_DUMPS][NAME_MAX + 1];
	size_t count;
};

struct options {
	const char *tty_path;
	const char *btpower_path;
	const char *sysfs_path;
	const char *output_path;
	unsigned int baud;
	unsigned int timeout;
	unsigned int init_delay_ms;
	size_t max_dump;
	bool power_on;
	bool trigger;
	bool trigger_during_init;
	bool force;
	bool hold;
};

struct session {
	int tty_fd;
	int hci_fd;
	int btpower_fd;
	int old_ldisc;
	int hci_id;
	bool ldisc_changed;
	bool termios_saved;
	bool hci_up;
	struct termios old_termios;
};

struct hci_up_worker {
	int hci_fd;
	int hci_id;
	int result;
	int error;
};

static volatile sig_atomic_t stop_requested;

static void signal_handler(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void usage(FILE *stream)
{
	fprintf(stream,
		"Usage:\n"
		"  qca-bt-dump collect --output PATH [options]\n"
		"  qca-bt-dump hold [options]\n"
		"\n"
		"The process keeps ttyHS0 attached to N_HCI until collection,\n"
		"timeout, or a signal. Detach is performed during cleanup.\n"
		"\n"
		"Options:\n"
		"  --tty PATH          UART device (default /dev/ttyHS0)\n"
		"  --btpower PATH      btpower device (default /dev/btpower)\n"
		"  --sysfs PATH        devcoredump class directory\n"
		"  --baud RATE         current controller baud (default 3000000)\n"
		"  --timeout SECONDS   collection timeout (default 30)\n"
		"  --max-size BYTES    dump size limit (default 536870912)\n"
		"  --power-on          request BT_POWER_ENABLE before attach\n"
		"  --trigger           ask QCA driver to send its crash buffer\n"
		"  --trigger-during-init\n"
		"                      trigger while HCIDEVUP runs in a thread\n"
		"  --init-delay-ms N   delay before init-time trigger (default 200)\n"
		"  --force             replace an existing output file\n"
		"  --help              show this text\n");
}

static int parse_uint(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno || end == text || *end != '\0' || parsed > UINT_MAX)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

static int parse_size(const char *text, size_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno || end == text || *end != '\0' || parsed > SIZE_MAX)
		return -1;
	*value = (size_t)parsed;
	return 0;
}

static int parse_options(int argc, char **argv, struct options *opts)
{
	int i;

	memset(opts, 0, sizeof(*opts));
	opts->tty_path = DEFAULT_TTY;
	opts->btpower_path = DEFAULT_BTPOWER;
	opts->sysfs_path = DEFAULT_SYSFS;
	opts->baud = DEFAULT_BAUD;
	opts->timeout = DEFAULT_TIMEOUT;
	opts->init_delay_ms = DEFAULT_INIT_DELAY_MS;
	opts->max_dump = DEFAULT_MAX_DUMP;

	if (argc < 2)
		return -1;
	if (!strcmp(argv[1], "--help")) {
		usage(stdout);
		exit(EXIT_SUCCESS);
	}
	if (!strcmp(argv[1], "hold"))
		opts->hold = true;
	else if (strcmp(argv[1], "collect"))
		return -1;

	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--help")) {
			usage(stdout);
			exit(EXIT_SUCCESS);
		} else if (!strcmp(argv[i], "--tty") && i + 1 < argc) {
			opts->tty_path = argv[++i];
		} else if (!strcmp(argv[i], "--btpower") && i + 1 < argc) {
			opts->btpower_path = argv[++i];
		} else if (!strcmp(argv[i], "--sysfs") && i + 1 < argc) {
			opts->sysfs_path = argv[++i];
		} else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
			opts->output_path = argv[++i];
		} else if (!strcmp(argv[i], "--baud") && i + 1 < argc) {
			if (parse_uint(argv[++i], &opts->baud))
				return -1;
		} else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
			if (parse_uint(argv[++i], &opts->timeout))
				return -1;
		} else if (!strcmp(argv[i], "--init-delay-ms") && i + 1 < argc) {
			if (parse_uint(argv[++i], &opts->init_delay_ms))
				return -1;
		} else if (!strcmp(argv[i], "--max-size") && i + 1 < argc) {
			if (parse_size(argv[++i], &opts->max_dump))
				return -1;
		} else if (!strcmp(argv[i], "--power-on")) {
			opts->power_on = true;
		} else if (!strcmp(argv[i], "--trigger")) {
			opts->trigger = true;
		} else if (!strcmp(argv[i], "--trigger-during-init")) {
			opts->trigger_during_init = true;
		} else if (!strcmp(argv[i], "--force")) {
			opts->force = true;
		} else {
			return -1;
		}
	}

	if (!opts->hold && !opts->output_path)
		return -1;
	if (!opts->timeout || !opts->max_dump || !opts->baud)
		return -1;
	if (opts->trigger_during_init && (!opts->trigger || opts->hold))
		return -1;
	return 0;
}

static int baud_to_speed(unsigned int baud, speed_t *speed)
{
	switch (baud) {
	case 115200:
		*speed = B115200;
		return 0;
#ifdef B1000000
	case 1000000:
		*speed = B1000000;
		return 0;
#endif
#ifdef B2000000
	case 2000000:
		*speed = B2000000;
		return 0;
#endif
#ifdef B3000000
	case 3000000:
		*speed = B3000000;
		return 0;
#endif
	default:
		errno = EINVAL;
		return -1;
	}
}

static int configure_uart(int fd, unsigned int baud,
			  struct termios *old_termios)
{
	struct termios config;
	speed_t speed;

	if (baud_to_speed(baud, &speed))
		return -1;
	if (tcgetattr(fd, old_termios))
		return -1;
	config = *old_termios;
	cfmakeraw(&config);
	config.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
	config.c_cflag |= CS8 | CLOCAL | CREAD | CRTSCTS;
	config.c_cc[VMIN] = 1;
	config.c_cc[VTIME] = 0;
	if (cfsetispeed(&config, speed) || cfsetospeed(&config, speed))
		return -1;
	return tcsetattr(fd, TCSANOW, &config);
}

static int wake_controller(int fd)
{
	struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
	uint8_t wake = QCA_IBS_WAKE_IND;
	uint8_t ack;
	int poll_result;
	ssize_t length;

	if (tcflush(fd, TCIFLUSH))
		return -1;
	length = write(fd, &wake, sizeof(wake));
	if (length < 0)
		return -1;
	if (length != sizeof(wake)) {
		errno = EIO;
		return -1;
	}
	if (tcdrain(fd))
		return -1;
	do {
		poll_result = poll(&poll_fd, 1, 1000);
	} while (poll_result < 0 && errno == EINTR);
	if (!poll_result) {
		fprintf(stderr,
			"QCA IBS wake timed out waiting for ACK 0x%02x\n",
			QCA_IBS_WAKE_ACK);
		errno = ETIMEDOUT;
		return -1;
	}
	if (poll_result < 0)
		return -1;
	if (!(poll_fd.revents & POLLIN)) {
		fprintf(stderr, "QCA IBS wake poll failed, revents=0x%x\n",
			poll_fd.revents);
		errno = EIO;
		return -1;
	}
	length = read(fd, &ack, sizeof(ack));
	if (length < 0)
		return -1;
	if (length != sizeof(ack)) {
		errno = EIO;
		return -1;
	}
	if (ack != QCA_IBS_WAKE_ACK) {
		fprintf(stderr,
			"QCA IBS wake expected ACK 0x%02x, received 0x%02x\n",
			QCA_IBS_WAKE_ACK, ack);
		errno = EPROTO;
		return -1;
	}
	return 0;
}

static bool dump_name_present(const struct dump_names *dumps,
			      const char *name)
{
	size_t i;

	for (i = 0; i < dumps->count; i++) {
		if (!strcmp(dumps->names[i], name))
			return true;
	}
	return false;
}

static int snapshot_dumps(const char *sysfs, struct dump_names *dumps)
{
	struct dirent *entry;
	DIR *dir;

	memset(dumps, 0, sizeof(*dumps));
	dir = opendir(sysfs);
	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		size_t name_len;

		if (strncmp(entry->d_name, "devcd", 5))
			continue;
		if (dumps->count == MAX_BASELINE_DUMPS) {
			closedir(dir);
			errno = EOVERFLOW;
			return -1;
		}
		name_len = strnlen(entry->d_name, NAME_MAX);
		memcpy(dumps->names[dumps->count], entry->d_name, name_len);
		dumps->names[dumps->count][name_len] = '\0';
		dumps->count++;
	}
	closedir(dir);
	return 0;
}

static bool dump_matches_hci(const char *sysfs, const char *name, int hci_id)
{
	char expected[32];
	char link_path[PATH_MAX];
	char target[PATH_MAX];
	const char *base;
	ssize_t length;

	if (snprintf(link_path, sizeof(link_path), "%s/%s/failing_device",
		     sysfs, name) >= (int)sizeof(link_path))
		return false;
	length = readlink(link_path, target, sizeof(target) - 1);
	if (length < 0)
		return false;
	target[length] = '\0';
	base = strrchr(target, '/');
	base = base ? base + 1 : target;
	snprintf(expected, sizeof(expected), "hci%d", hci_id);
	return !strcmp(base, expected);
}

static int find_new_dump(const char *sysfs, const struct dump_names *baseline,
			 int hci_id, char *path, size_t path_size)
{
	struct dirent *entry;
	DIR *dir;
	int found = 0;

	dir = opendir(sysfs);
	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, "devcd", 5) ||
		    dump_name_present(baseline, entry->d_name) ||
		    !dump_matches_hci(sysfs, entry->d_name, hci_id))
			continue;
		if (snprintf(path, path_size, "%s/%s/data", sysfs,
			     entry->d_name) >= (int)path_size) {
			errno = ENAMETOOLONG;
			found = -1;
			break;
		}
		found = 1;
		break;
	}
	closedir(dir);
	return found;
}

static int copy_dump(const char *input, const char *output, size_t max_dump,
		     bool force, size_t *copied)
{
	char buffer[64 * 1024];
	char temporary[PATH_MAX];
	ssize_t length;
	int input_fd = -1;
	int output_fd = -1;
	int result = -1;

	*copied = 0;
	if (!force && !access(output, F_OK)) {
		errno = EEXIST;
		return -1;
	}
	if (snprintf(temporary, sizeof(temporary), "%s.part.%ld", output,
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	input_fd = open(input, O_RDONLY | O_CLOEXEC);
	if (input_fd < 0)
		goto out;
	output_fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
			 0600);
	if (output_fd < 0)
		goto out;

	while ((length = read(input_fd, buffer, sizeof(buffer))) > 0) {
		ssize_t offset = 0;

		if ((size_t)length > max_dump - *copied) {
			errno = EFBIG;
			goto out;
		}
		while (offset < length) {
			ssize_t written = write(output_fd, buffer + offset,
						length - offset);

			if (written < 0) {
				if (errno == EINTR)
					continue;
				goto out;
			}
			offset += written;
		}
		*copied += (size_t)length;
	}
	if (length < 0)
		goto out;
	if (!*copied) {
		errno = ENODATA;
		goto out;
	}
	if (fsync(output_fd))
		goto out;
	if (close(output_fd)) {
		output_fd = -1;
		goto out;
	}
	output_fd = -1;
	if (rename(temporary, output))
		goto out;
	result = 0;

out:
	if (input_fd >= 0)
		close(input_fd);
	if (output_fd >= 0)
		close(output_fd);
	if (result)
		unlink(temporary);
	return result;
}

static int open_hci_socket(void)
{
	int fd;

	fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	return fd;
}

static int bring_hci_up(int hci_fd, int hci_id)
{
	if (!ioctl(hci_fd, HCIDEVUP, hci_id))
		return 0;
	if (errno == EALREADY)
		return 0;
	return -1;
}

static void *bring_hci_up_thread(void *opaque)
{
	struct hci_up_worker *worker = opaque;

	if (bring_hci_up(worker->hci_fd, worker->hci_id)) {
		worker->result = -1;
		worker->error = errno;
	} else {
		worker->result = 0;
		worker->error = 0;
	}
	return NULL;
}

static void sleep_milliseconds(unsigned int milliseconds)
{
	struct timespec remaining;
	struct timespec requested = {
		.tv_sec = milliseconds / 1000U,
		.tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
	};

	while (nanosleep(&requested, &remaining) && errno == EINTR) {
		if (stop_requested)
			break;
		requested = remaining;
	}
}

static int trigger_crash(int tty_fd)
{
	return ioctl(tty_fd, HCIUARTTRIGGERDUMP, 0);
}

static void cleanup_session(struct session *session)
{
	if (session->hci_fd >= 0 && session->hci_up) {
		if (ioctl(session->hci_fd, HCIDEVDOWN, session->hci_id) &&
		    errno != ENETDOWN && errno != ENODEV)
			fprintf(stderr, "warning: HCIDEVDOWN hci%d: %s\n",
				session->hci_id, strerror(errno));
	}
	if (session->hci_fd >= 0)
		close(session->hci_fd);
	if (session->tty_fd >= 0 && session->ldisc_changed) {
		if (ioctl(session->tty_fd, TIOCSETD, &session->old_ldisc))
			fprintf(stderr, "warning: restore tty line discipline: %s\n",
				strerror(errno));
	}
	if (session->tty_fd >= 0 && session->termios_saved) {
		if (tcsetattr(session->tty_fd, TCSANOW, &session->old_termios))
			fprintf(stderr, "warning: restore tty termios: %s\n",
				strerror(errno));
	}
	if (session->tty_fd >= 0)
		close(session->tty_fd);
	if (session->btpower_fd >= 0)
		close(session->btpower_fd);
}

static int start_session(const struct options *opts, struct session *session)
{
	unsigned long flags = 1UL << HCI_UART_EXT_CONFIG;
	int ldisc = N_HCI;
	int hci_id;

	memset(session, 0, sizeof(*session));
	session->tty_fd = -1;
	session->hci_fd = -1;
	session->btpower_fd = -1;
	session->old_ldisc = N_TTY;
	session->hci_id = -1;

	if (opts->power_on) {
		session->btpower_fd = open(opts->btpower_path,
					   O_RDWR | O_CLOEXEC);
		if (session->btpower_fd < 0) {
			fprintf(stderr, "open %s: %s\n", opts->btpower_path,
				strerror(errno));
			return -1;
		}
		if (ioctl(session->btpower_fd, BT_CMD_PWR_CTRL,
			  BT_POWER_ENABLE)) {
			fprintf(stderr, "BT_POWER_ENABLE: %s\n", strerror(errno));
			return -1;
		}
	}

	session->tty_fd = open(opts->tty_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (session->tty_fd < 0) {
		fprintf(stderr, "open %s: %s (stop the QTI Bluetooth HAL first)\n",
			opts->tty_path, strerror(errno));
		return -1;
	}
	if (ioctl(session->tty_fd, TIOCEXCL)) {
		fprintf(stderr, "TIOCEXCL %s: %s\n", opts->tty_path,
			strerror(errno));
		return -1;
	}
	if (ioctl(session->tty_fd, TIOCGETD, &session->old_ldisc)) {
		fprintf(stderr, "TIOCGETD %s: %s\n", opts->tty_path,
			strerror(errno));
		return -1;
	}
	if (configure_uart(session->tty_fd, opts->baud,
			   &session->old_termios)) {
		fprintf(stderr, "configure %s at %u: %s\n", opts->tty_path,
			opts->baud, strerror(errno));
		return -1;
	}
	session->termios_saved = true;
	if (wake_controller(session->tty_fd)) {
		fprintf(stderr, "wake QCA controller: %s\n", strerror(errno));
		return -1;
	}
	printf("received QCA IBS wake ACK\n");

	if (ioctl(session->tty_fd, TIOCSETD, &ldisc)) {
		fprintf(stderr, "TIOCSETD N_HCI: %s\n", strerror(errno));
		return -1;
	}
	session->ldisc_changed = true;
	if (ioctl(session->tty_fd, HCIUARTSETFLAGS, flags)) {
		fprintf(stderr, "HCIUARTSETFLAGS EXT_CONFIG: %s\n",
			strerror(errno));
		return -1;
	}
	if (ioctl(session->tty_fd, HCIUARTSETPROTO, HCI_UART_QCA)) {
		fprintf(stderr, "HCIUARTSETPROTO QCA: %s\n", strerror(errno));
		return -1;
	}
	hci_id = ioctl(session->tty_fd, HCIUARTGETDEVICE, 0);
	if (hci_id < 0) {
		fprintf(stderr, "HCIUARTGETDEVICE: %s\n", strerror(errno));
		return -1;
	}
	session->hci_id = hci_id;
	session->hci_fd = open_hci_socket();
	if (session->hci_fd < 0) {
		fprintf(stderr, "open raw hci%d socket: %s\n", hci_id,
			strerror(errno));
		return -1;
	}
	if (!opts->trigger_during_init) {
		if (bring_hci_up(session->hci_fd, hci_id)) {
			fprintf(stderr, "HCIDEVUP hci%d: %s\n", hci_id,
				strerror(errno));
			return -1;
		}
		session->hci_up = true;
	}
	return 0;
}

static int wait_for_dump(const struct options *opts,
			 const struct dump_names *baseline,
			 const struct session *session)
{
	struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = 100000000L };
	char dump_path[PATH_MAX];
	unsigned int elapsed = 0;
	int found;

	while (!stop_requested && elapsed < opts->timeout * 10U) {
		found = find_new_dump(opts->sysfs_path, baseline, session->hci_id,
				      dump_path, sizeof(dump_path));
		if (found < 0) {
			fprintf(stderr, "scan %s: %s\n", opts->sysfs_path,
				strerror(errno));
			return -1;
		}
		if (found) {
			size_t copied;

			if (copy_dump(dump_path, opts->output_path, opts->max_dump,
				      opts->force, &copied)) {
				fprintf(stderr, "copy %s: %s\n", dump_path,
					strerror(errno));
				return -1;
			}
			printf("collected %zu bytes from %s into %s\n", copied,
			       dump_path, opts->output_path);
			return 0;
		}
		nanosleep(&sleep_time, NULL);
		elapsed++;
	}
	if (stop_requested)
		errno = EINTR;
	else
		errno = ETIMEDOUT;
	return -1;
}

static int collect_during_hci_init(const struct options *opts,
				   const struct dump_names *baseline,
				   struct session *session)
{
	struct hci_up_worker worker = {
		.hci_fd = session->hci_fd,
		.hci_id = session->hci_id,
		.result = -1,
		.error = 0,
	};
	pthread_t thread;
	int dump_error = 0;
	int dump_result;
	int join_error;
	int thread_error;
	int trigger_error = 0;

	thread_error = pthread_create(&thread, NULL, bring_hci_up_thread,
				      &worker);
	if (thread_error) {
		errno = thread_error;
		fprintf(stderr, "start HCIDEVUP hci%d worker: %s\n",
			session->hci_id, strerror(errno));
		return -1;
	}

	printf("started HCIDEVUP hci%d worker; delaying %u ms before trigger\n",
	       session->hci_id, opts->init_delay_ms);
	sleep_milliseconds(opts->init_delay_ms);
	if (!stop_requested) {
		if (trigger_crash(session->tty_fd)) {
			trigger_error = errno;
			fprintf(stderr, "send crash buffer during HCI_INIT: %s\n",
				strerror(trigger_error));
		} else {
			printf("requested the QCA 1096-byte crash buffer during HCI_INIT\n");
		}
	}

	dump_result = wait_for_dump(opts, baseline, session);
	if (dump_result)
		dump_error = errno;

	join_error = pthread_join(thread, NULL);
	if (join_error) {
		errno = join_error;
		fprintf(stderr, "join HCIDEVUP hci%d worker: %s\n",
			session->hci_id, strerror(errno));
		return -1;
	}
	if (!worker.result)
		session->hci_up = true;

	if (!dump_result) {
		if (worker.result)
			fprintf(stderr,
				"HCIDEVUP hci%d returned after dump: %s\n",
				session->hci_id, strerror(worker.error));
		return 0;
	}

	fprintf(stderr, "no dump collected: %s\n", strerror(dump_error));
	if (worker.result)
		fprintf(stderr, "HCIDEVUP hci%d: %s\n", session->hci_id,
			strerror(worker.error));
	else
		fprintf(stderr, "HCIDEVUP hci%d completed successfully\n",
			session->hci_id);
	if (trigger_error)
		fprintf(stderr, "HCIUARTTRIGGERDUMP: %s\n",
			strerror(trigger_error));
	errno = dump_error;
	return -1;
}

int main(int argc, char **argv)
{
	struct sigaction action;
	struct dump_names baseline;
	struct options opts;
	struct session session;
	int result = EXIT_FAILURE;

	if (parse_options(argc, argv, &opts)) {
		usage(stderr);
		return EXIT_FAILURE;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	if (snapshot_dumps(opts.sysfs_path, &baseline)) {
		fprintf(stderr, "snapshot %s: %s\n", opts.sysfs_path,
			strerror(errno));
		return EXIT_FAILURE;
	}
	if (start_session(&opts, &session))
		goto out;
	printf("attached %s to QCA protocol as hci%d at %u baud\n",
	       opts.tty_path, session.hci_id, opts.baud);
	if (opts.trigger_during_init) {
		if (!collect_during_hci_init(&opts, &baseline, &session))
			result = EXIT_SUCCESS;
		goto out;
	}

	if (opts.trigger) {
		if (trigger_crash(session.tty_fd)) {
			fprintf(stderr, "send crash buffer: %s\n", strerror(errno));
			goto out;
		}
		printf("requested the QCA 1096-byte crash buffer\n");
	} else {
		printf("controller is ready; trigger the crash from another shell\n");
	}

	if (opts.hold) {
		while (!stop_requested)
			pause();
		result = EXIT_SUCCESS;
	} else if (!wait_for_dump(&opts, &baseline, &session)) {
		result = EXIT_SUCCESS;
	} else {
		fprintf(stderr, "no dump collected: %s\n", strerror(errno));
	}

out:
	cleanup_session(&session);
	return result;
}
