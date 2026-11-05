#ifndef MM_VINPUT_H
#define MM_VINPUT_H

#include "mm.h"

typedef struct {
    int mouse_fd;
    int tp_fd;
    int kbd_fd;

    int tp_tid[MM_VTP_SLOTS];
    int tp_next_tid;
    int tp_n;

    int wheel_acc;
    int hwheel_acc;
} mm_vinput;

int  mm_vinput_create(mm_vinput *v);
void mm_vinput_output(mm_vinput *v, mm_output *out);
void mm_vinput_destroy(mm_vinput *v);

#endif
