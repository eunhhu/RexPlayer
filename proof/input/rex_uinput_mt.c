#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define DEVICE_NAME "RexPlayer Virtual Multi-Touch Proof"

struct event_spec {
    unsigned short type;
    unsigned short code;
    int value;
};

static const struct event_spec emit_sequence[] = {
    {EV_KEY, BTN_TOUCH, 1},
    {EV_ABS, ABS_MT_SLOT, 0},
    {EV_ABS, ABS_MT_TRACKING_ID, 42},
    {EV_ABS, ABS_MT_POSITION_X, 100},
    {EV_ABS, ABS_MT_POSITION_Y, 200},
    {EV_ABS, ABS_X, 100},
    {EV_ABS, ABS_Y, 200},
    {EV_SYN, SYN_REPORT, 0},
    {EV_ABS, ABS_MT_POSITION_X, 400},
    {EV_ABS, ABS_MT_POSITION_Y, 500},
    {EV_ABS, ABS_X, 400},
    {EV_ABS, ABS_Y, 500},
    {EV_SYN, SYN_REPORT, 0},
    {EV_ABS, ABS_MT_TRACKING_ID, -1},
    {EV_KEY, BTN_TOUCH, 0},
    {EV_SYN, SYN_REPORT, 0},
};

/* ABS_MT_SLOT=0 is unchanged from the initial slot and is filtered by evdev. */
static const struct event_spec expected_sequence[] = {
    {EV_KEY, BTN_TOUCH, 1},
    {EV_ABS, ABS_MT_TRACKING_ID, 42},
    {EV_ABS, ABS_MT_POSITION_X, 100},
    {EV_ABS, ABS_MT_POSITION_Y, 200},
    {EV_ABS, ABS_X, 100},
    {EV_ABS, ABS_Y, 200},
    {EV_SYN, SYN_REPORT, 0},
    {EV_ABS, ABS_MT_POSITION_X, 400},
    {EV_ABS, ABS_MT_POSITION_Y, 500},
    {EV_ABS, ABS_X, 400},
    {EV_ABS, ABS_Y, 500},
    {EV_SYN, SYN_REPORT, 0},
    {EV_ABS, ABS_MT_TRACKING_ID, -1},
    {EV_KEY, BTN_TOUCH, 0},
    {EV_SYN, SYN_REPORT, 0},
};

static unsigned env_delay_ms(const char *name) {
    const char *value = getenv(name);
    if (!value || !*value) return 0;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed > 60000UL) {
        fprintf(stderr, "invalid %s (expected 0..60000): %s\n", name, value);
        exit(64);
    }
    return (unsigned)parsed;
}

static bool env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && strcmp(value, "1") == 0;
}

static int create_event_node_from_sysfs(const char *event_node) {
    const char *event_name = strrchr(event_node, '/');
    event_name = event_name ? event_name + 1 : event_node;
    if (strncmp(event_name, "event", 5) != 0) {
        errno = EINVAL;
        return -1;
    }

    char dev_path[256];
    if (snprintf(dev_path, sizeof(dev_path), "/sys/class/input/%s/dev", event_name) >=
        (int)sizeof(dev_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *dev_file = fopen(dev_path, "r");
    if (!dev_file) return -1;
    unsigned major_num = 0, minor_num = 0;
    int scanned = fscanf(dev_file, "%u:%u", &major_num, &minor_num);
    fclose(dev_file);
    if (scanned != 2) {
        errno = EINVAL;
        return -1;
    }

    if (mkdir("/dev/input", 0755) < 0 && errno != EEXIST) return -1;
    if (mknod(event_node, S_IFCHR | 0660, makedev(major_num, minor_num)) < 0) return -1;
    if (chmod(event_node, 0660) < 0 || chown(event_node, 0, 1004) < 0) {
        int saved_errno = errno;
        unlink(event_node);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int emit(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event ev = {0};
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return (write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) ? 0 : -1;
}

static int setup_abs(int fd, unsigned short code, int min, int max) {
    if (ioctl(fd, UI_SET_ABSBIT, code) < 0) return -1;
    struct uinput_abs_setup abs = {0};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.resolution = 1;
    return ioctl(fd, UI_ABS_SETUP, &abs);
}

static int find_event_node(const char *sysname, char *out, size_t out_len) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/devices/virtual/input/%s", sysname);
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *ent;
    int rc = -1;
    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "event", 5) == 0) {
            const char prefix[] = "/dev/input/";
            size_t prefix_len = sizeof(prefix) - 1;
            size_t name_len = strlen(ent->d_name);
            if (prefix_len + name_len + 1 > out_len) continue;
            memcpy(out, prefix, prefix_len);
            memcpy(out + prefix_len, ent->d_name, name_len + 1);
            rc = 0;
            break;
        }
    }
    closedir(dir);
    return rc;
}

int main(void) {
    int ufd = -1, efd = -1, rc = 1;
    char sysname[128] = {0}, event_node[256] = {0};
    bool event_node_created = false, sequence_match = true;
    unsigned pre_emit_ms = env_delay_ms("REX_PRE_EMIT_MS");
    unsigned hold_ms = env_delay_ms("REX_HOLD_MS");
    size_t expected_index = 0;
    int syn_reports = 0, total_events = 0;

    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { perror("open /dev/uinput"); goto done; }

    if (ioctl(ufd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(ufd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(ufd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        ioctl(ufd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) {
        perror("configure event bits"); goto done;
    }

    if (setup_abs(ufd, ABS_MT_SLOT, 0, 9) < 0 ||
        setup_abs(ufd, ABS_MT_TRACKING_ID, 0, 65535) < 0 ||
        setup_abs(ufd, ABS_MT_POSITION_X, 0, 1079) < 0 ||
        setup_abs(ufd, ABS_MT_POSITION_Y, 0, 1919) < 0 ||
        setup_abs(ufd, ABS_X, 0, 1079) < 0 ||
        setup_abs(ufd, ABS_Y, 0, 1919) < 0) {
        perror("configure absolute axes"); goto done;
    }

    struct uinput_setup usetup = {0};
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "%s", DEVICE_NAME);
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x5258;
    usetup.id.product = 0x0002;
    usetup.id.version = 2;
    if (ioctl(ufd, UI_DEV_SETUP, &usetup) < 0) { perror("UI_DEV_SETUP"); goto done; }
    if (ioctl(ufd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); goto done; }

    usleep(500000);
    if (ioctl(ufd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
        perror("UI_GET_SYSNAME"); goto destroy;
    }
    if (find_event_node(sysname, event_node, sizeof(event_node)) < 0) {
        fprintf(stderr, "failed to map %s to /dev/input/event*\n", sysname); goto destroy;
    }
    if (access(event_node, F_OK) < 0 && env_enabled("REX_CREATE_EVENT_NODE")) {
        if (create_event_node_from_sysfs(event_node) < 0) {
            perror("create event node"); goto destroy;
        }
        event_node_created = true;
        printf("CREATED node=%s from=sysfs\n", event_node);
    }
    for (int attempt = 0; attempt < 50; attempt++) {
        efd = open(event_node, O_RDONLY | O_NONBLOCK);
        if (efd >= 0) break;
        if (errno != ENOENT && errno != EACCES && errno != ENXIO) break;
        usleep(100000);
    }
    if (efd < 0) { perror("open event node"); goto destroy; }

    char actual_name[256] = {0};
    if (ioctl(efd, EVIOCGNAME(sizeof(actual_name)), actual_name) < 0) {
        perror("EVIOCGNAME"); goto destroy;
    }
    printf("DEVICE name=%s sysname=%s node=%s\n", actual_name, sysname, event_node);
    fflush(stdout);

    if (pre_emit_ms) {
        printf("WAIT pre_emit_ms=%u\n", pre_emit_ms);
        fflush(stdout);
        usleep((useconds_t)pre_emit_ms * 1000U);
    }

    for (size_t i = 0; i < sizeof(emit_sequence) / sizeof(emit_sequence[0]); i++) {
        if (emit(ufd, emit_sequence[i].type, emit_sequence[i].code,
                 emit_sequence[i].value) < 0) {
            perror("emit input event");
            goto destroy;
        }
    }

    struct pollfd pfd = {.fd = efd, .events = POLLIN};
    int quiet_rounds = 0;
    while (quiet_rounds < 3) {
        int pr = poll(&pfd, 1, 300);
        if (pr < 0) { perror("poll"); goto destroy; }
        if (pr == 0) { quiet_rounds++; continue; }
        struct input_event evs[64];
        ssize_t n = read(efd, evs, sizeof(evs));
        if (n < 0 && errno == EAGAIN) continue;
        if (n < 0) { perror("read"); goto destroy; }
        size_t count = (size_t)n / sizeof(struct input_event);
        for (size_t i = 0; i < count; i++) {
            const struct input_event *ev = &evs[i];
            total_events++;
            printf("EVENT type=%u code=%u value=%d\n", ev->type, ev->code, ev->value);
            if (expected_index >= sizeof(expected_sequence) / sizeof(expected_sequence[0]) ||
                ev->type != expected_sequence[expected_index].type ||
                ev->code != expected_sequence[expected_index].code ||
                ev->value != expected_sequence[expected_index].value) {
                sequence_match = false;
            }
            expected_index++;
            if (ev->type == EV_SYN && ev->code == SYN_REPORT) syn_reports++;
        }
    }

    size_t expected_count = sizeof(expected_sequence) / sizeof(expected_sequence[0]);
    bool pass = sequence_match && expected_index == expected_count &&
                total_events == (int)expected_count && syn_reports == 3;
    printf("SUMMARY events=%d expected=%zu syn_reports=%d sequence=%d\n",
           total_events, expected_count, syn_reports, sequence_match && expected_index == expected_count);
    printf("RESULT %s\n", pass ? "PASS" : "FAIL");
    if (hold_ms) {
        printf("WAIT hold_ms=%u\n", hold_ms);
        fflush(stdout);
        usleep((useconds_t)hold_ms * 1000U);
    }
    rc = pass ? 0 : 2;

destroy:
    if (ufd >= 0) ioctl(ufd, UI_DEV_DESTROY);
    if (event_node_created) unlink(event_node);
done:
    if (efd >= 0) close(efd);
    if (ufd >= 0) close(ufd);
    return rc;
}
