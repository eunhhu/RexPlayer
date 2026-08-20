#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/android/binderfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int add_device(int control_fd, const char *name) {
    struct binderfs_device device = {0};

    if (snprintf(device.name, sizeof(device.name), "%s", name) >=
        (int)sizeof(device.name)) {
        fprintf(stderr, "binder device name is too long: %s\n", name);
        return 1;
    }
    if (ioctl(control_fd, BINDER_CTL_ADD, &device) < 0) {
        if (errno == EEXIST) {
            printf("BINDER_DEVICE name=%s state=EXISTS\n", name);
            return 0;
        }
        fprintf(stderr, "BINDER_CTL_ADD %s failed: %s\n", name, strerror(errno));
        return 1;
    }
    printf("BINDER_DEVICE name=%s state=CREATED major=%u minor=%u\n",
           name, device.major, device.minor);
    return 0;
}

int main(int argc, char **argv) {
    const char *control = argc > 1 ? argv[1] : "/dev/binderfs/binder-control";
    const char *names[] = {"binder", "hwbinder", "vndbinder"};
    int control_fd = open(control, O_RDWR | O_CLOEXEC);
    int result = 0;

    if (control_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", control, strerror(errno));
        return 1;
    }
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        result |= add_device(control_fd, names[index]);
    }
    if (close(control_fd) < 0) {
        fprintf(stderr, "close %s failed: %s\n", control, strerror(errno));
        result = 1;
    }
    if (result == 0) {
        printf("BINDER_DEVICES_RESULT=PASS\n");
    }
    return result;
}
