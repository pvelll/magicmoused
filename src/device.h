#ifndef MM_DEVICE_H
#define MM_DEVICE_H

#define MM_MAX_GRABS 8

typedef struct {
    int  hid_fd;
    int  ev_fd[MM_MAX_GRABS];
    int  nev;
    char hidraw[64];
    char sysname[64];
    char name[128];
    int  bus, vendor, product;
    int  multitouch;
} mm_device;

int  mm_device_open(mm_device *d, const char *path, int grab);
int  mm_device_grab(mm_device *d);
void mm_device_close(mm_device *d);

#endif
