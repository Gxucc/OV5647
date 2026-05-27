#ifndef DRAW_BOX_H
#define DRAW_BOX_H

#include <stdint.h>
#include "person_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

void draw_box_on_lcd(uint8_t *buf, int buf_w, int buf_h,
                     const person_box_t *box, int cam_w, int cam_h);

#ifdef __cplusplus
}
#endif

#endif