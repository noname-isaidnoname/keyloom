#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef int (*ioctl_fn)(int, unsigned long, ...);

static unsigned long target_request(void) {
    const char* step = getenv("KEYLOOM_FAIL_UIOCTL_STEP");
    if (!step || !*step) return 0;
    if (strcmp(step, "UI_SET_EVBIT EV_KEY") == 0) return UI_SET_EVBIT;
    if (strcmp(step, "UI_SET_EVBIT EV_REL") == 0) return UI_SET_EVBIT;
    if (strcmp(step, "UI_SET_EVBIT EV_ABS") == 0) return UI_SET_EVBIT;
    if (strcmp(step, "UI_SET_KEYBIT") == 0) return UI_SET_KEYBIT;
    if (strcmp(step, "UI_SET_RELBIT") == 0) return UI_SET_RELBIT;
    if (strcmp(step, "UI_SET_ABSBIT") == 0) return UI_SET_ABSBIT;
    if (strcmp(step, "UI_ABS_SETUP") == 0) return UI_ABS_SETUP;
    if (strcmp(step, "UI_SET_PHYS") == 0) return UI_SET_PHYS;
    if (strcmp(step, "UI_DEV_SETUP") == 0) return UI_DEV_SETUP;
    if (strcmp(step, "UI_DEV_CREATE") == 0) return UI_DEV_CREATE;
    return 0;
}

static int target_arg_matches(const char* step, unsigned long request, unsigned long arg) {
    if (strcmp(step, "UI_SET_EVBIT EV_KEY") == 0) return request == UI_SET_EVBIT && arg == EV_KEY;
    if (strcmp(step, "UI_SET_EVBIT EV_REL") == 0) return request == UI_SET_EVBIT && arg == EV_REL;
    if (strcmp(step, "UI_SET_EVBIT EV_ABS") == 0) return request == UI_SET_EVBIT && arg == EV_ABS;
    return (unsigned long)target_request() == request;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    unsigned long arg = va_arg(ap, unsigned long);
    va_end(ap);

    const char* step = getenv("KEYLOOM_FAIL_UIOCTL_STEP");
    if (step && *step) {
        if (target_arg_matches(step, request, arg)) {
            errno = EIO;
            return -1;
        }
    }

    ioctl_fn real_ioctl = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    if (!real_ioctl) { errno = ENOSYS; return -1; }
    return real_ioctl(fd, request, arg);
}
