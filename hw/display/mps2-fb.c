/*
 * NeXT Cube/Station Framebuffer Emulation
 *
 * Copyright (c) 2011 Bryce Lanham
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "hw/loader.h"
#include "framebuffer.h"
#include "ui/pixel_ops.h"
#include "qom/object.h"
#include "qemu/log.h"

#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"

#define TYPE_MPS2FB "mps2-fb"
OBJECT_DECLARE_SIMPLE_TYPE(MPS2FBState, MPS2FB)

#define CONTROL_REGION_SIZE 4096
#define TOUCH_CTRL_OFFSET   0
#define TOUCH_HEADER_OFFSET 4

/* Point structure offsets (relative to start of point data) */
#define POINT_BASE_OFFSET   8
#define POINT_SIZE          16  /* 4 fields * 4 bytes each */
#define POINT_X_OFFSET      0
#define POINT_Y_OFFSET      4
#define POINT_PRESS_OFFSET  8
#define POINT_ID_OFFSET     12

/* Maximum number of touch points supported */
#define MAX_TOUCH_POINTS    10

// by default, slot_id/track_id from qemu start from 1, now make slot_id start from 0
// make mouse and first touch point use same slot
#define MULTI_TOUCH_SLOT_OFFSET -1
#define MOUSE_SLOT 0

/* Touch header bit definitions */
#define TOUCH_HEADER_NUM_POINTS_MASK  0x1F  /* Bits 0-4: Number of active points */
#define TOUCH_HEADER_RESERVED_MASK    (~TOUCH_HEADER_NUM_POINTS_MASK)

/* Control register bit definitions */
#define CONTROL_ENABLE_IRQ_MASK  (1u << 0)
#define CONTROL_RESERVED_MASK    (~CONTROL_ENABLE_IRQ_MASK)

/* Touch point structure */
typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t pressed;
    int32_t track_id;
} MPS2FBTouchPoint;

/* Control register structure */
typedef struct {
    unsigned int enable_irq :1;     /* Bit 0: Enable touch interrupt */
    unsigned int reserved   :31;    /* Bits 1-31: Reserved for future features */
} MPS2FBCtrl;

/* Touch header structure */
typedef struct {
    unsigned int points_mask :16;     /* Bits 0-16: Point mask */
    unsigned int reserved   :16;    /* Bits 5-31: Reserved for future features */
} MPS2FBTouchHeader;

struct MPS2FBState {
    SysBusDevice parent_obj;

    /* Control region (touch data) */
    MemoryRegion control_mr;

    /* Framebuffer memory */
    MemoryRegion fb_mr;
    MemoryRegionSection fbsection;


    QemuConsole *con;

    uint32_t cols;
    uint32_t rows;
    int invalidate;

    /* Touch state */
    QemuInputHandlerState *touch_handler;

    /* Multi-touch state */
    MPS2FBTouchHeader touch_header;
    MPS2FBTouchPoint touch_points[MAX_TOUCH_POINTS];

    /* IRQ support */
    qemu_irq touch_irq;
    MPS2FBCtrl ctrl;
};

// Add property handling prototypes
static const Property mps2fb_properties[] = {
    DEFINE_PROP_UINT32("cols", MPS2FBState, cols, 640),
    DEFINE_PROP_UINT32("rows", MPS2FBState, rows, 480),
};

static void mps2fb_update_irq(MPS2FBState *s)
{
    if (s->ctrl.enable_irq) {
        qemu_irq_pulse(s->touch_irq);
    }
}


static uint64_t control_region_read(void *opaque, hwaddr addr, unsigned size)
{
    MPS2FBState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case TOUCH_CTRL_OFFSET:
        val = *(uint32_t *)&s->ctrl;
        break;
    case TOUCH_HEADER_OFFSET:
        val = *(uint32_t *)&s->touch_header;
        break;
    default:
        /* Check if address is within point data region */
        if (addr >= POINT_BASE_OFFSET &&
            addr < POINT_BASE_OFFSET + (MAX_TOUCH_POINTS * POINT_SIZE)) {
            int point_index = (addr - POINT_BASE_OFFSET) / POINT_SIZE;
            int point_offset = (addr - POINT_BASE_OFFSET) % POINT_SIZE;

            if (point_index < MAX_TOUCH_POINTS) {
                MPS2FBTouchPoint *point = &s->touch_points[point_index];

                switch (point_offset) {
                case POINT_X_OFFSET:
                    val = point->x;
                    break;
                case POINT_Y_OFFSET:
                    val = point->y;
                    break;
                case POINT_PRESS_OFFSET:
                    val = point->pressed;
                    break;
                case POINT_ID_OFFSET:
                    val = *(uint32_t *)&point->track_id;
                    break;
                default:
                    qemu_log_mask(LOG_UNIMP,
                        "%s: unimplemented point field read at 0x%"HWADDR_PRIx"\n",
                        __func__, addr);
                    break;
                }
            }
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%"HWADDR_PRIx"\n",
                          __func__, addr);
        }
        break;
    }

    return val;
}

static void control_region_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MPS2FBState *s = opaque;

    switch (addr) {
    case TOUCH_CTRL_OFFSET:
        *(uint32_t *)&s->ctrl = (uint32_t)val;
        qemu_log_mask(LOG_UNIMP,
                "enable_irq %d, reserved %d\n",
                s->ctrl.enable_irq,
                s->ctrl.reserved
                );
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%"HWADDR_PRIx"\n",
                  __func__, addr);
        break;
    }
}

static const MemoryRegionOps control_region_ops = {
    .read = control_region_read,
    .write = control_region_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void mps2fb_draw_line(void *opaque, uint8_t *d, const uint8_t *s,
                             int width, int pitch)
{
    /* Assume s is already in 32 BPP format */
    memcpy(d, s, width * 4);  // 4 bytes per pixel
}

static void mps2fb_update(void *opaque)
{
    MPS2FBState *s = MPS2FB(opaque);
    int bytes_per_pixel = 4; // 32 BPP
    int src_width = s->cols * bytes_per_pixel;
    int dest_width = src_width;
    int first = 0, last = 0;
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, &s->fb_mr, 0,
                                         s->cols * bytes_per_pixel,
                                         s->rows);
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection, s->cols, s->rows,
                               src_width, dest_width, 0, 1, mps2fb_draw_line,
                               s, &first, &last);

    dpy_gfx_update(s->con, 0, 0, s->cols, s->rows);
}

static void mps2fb_invalidate(void *opaque)
{
    MPS2FBState *s = MPS2FB(opaque);
    s->invalidate = 1;
}

static const GraphicHwOps mps2fb_ops = {
    .invalidate  = mps2fb_invalidate,
    .gfx_update  = mps2fb_update,
};


static void mps2fb_touch_event(DeviceState *dev,
                              QemuConsole *con,
                              InputEvent *evt)
{
    MPS2FBState *s = MPS2FB(dev);
    int touch_state_changed = 0;

    if (evt->type == INPUT_EVENT_KIND_MTT) {
        /* Multi-touch event */
        InputMultiTouchEvent *mt = evt->u.mtt.data;

        switch (mt->type) {
        case INPUT_MULTI_TOUCH_TYPE_BEGIN:
        case INPUT_MULTI_TOUCH_TYPE_UPDATE:
            if (mt->slot < MAX_TOUCH_POINTS) {
                // MPS2FBTouchPoint *point = &s->touch_points[mt->slot];
                // point->pressed = 1;
                // point->track_id = mt->tracking_id;
                // qemu_log_mask(LOG_UNIMP, "track_id assign %ld, slot id %ld\n", mt->tracking_id, mt->slot);
            }
            break;
        case INPUT_MULTI_TOUCH_TYPE_DATA:
            if (mt->slot + MULTI_TOUCH_SLOT_OFFSET < MAX_TOUCH_POINTS) {
                const int i = mt->slot + MULTI_TOUCH_SLOT_OFFSET;
                MPS2FBTouchPoint *point = &s->touch_points[i];

                point->pressed = 1;
                s->touch_header.points_mask |= (1 << i);
                point->track_id = mt->tracking_id;

                qemu_log_mask(LOG_UNIMP, "track_in data  %ld, slot id %ld\n", mt->tracking_id, mt->slot);
                uint64_t value = mt->value = (mt->value < 0) ? 0 : ((mt->value > INPUT_EVENT_ABS_MAX) ? INPUT_EVENT_ABS_MAX - 1 : mt->value);
                if (mt->axis == INPUT_AXIS_X) {
                    point->x = ((uint64_t)value * (uint64_t)s->cols) / (uint64_t)INPUT_EVENT_ABS_MAX;
                    qemu_log_mask(LOG_UNIMP, "x is %d\n", point->x);
                } else if  (mt->axis == INPUT_AXIS_Y) {
                    point->y = ((uint64_t)value * (uint64_t)s->rows) / (uint64_t)INPUT_EVENT_ABS_MAX;
                    qemu_log_mask(LOG_UNIMP, "y is %d\n", point->y);
                } else {
                    qemu_log_mask(LOG_UNIMP, "Unknow\n");
                }

                touch_state_changed = 1;
            }
            break;

        case INPUT_MULTI_TOUCH_TYPE_END:
        case INPUT_MULTI_TOUCH_TYPE_CANCEL:
            if (mt->slot + MULTI_TOUCH_SLOT_OFFSET < MAX_TOUCH_POINTS) {
                const int i = mt->slot + MULTI_TOUCH_SLOT_OFFSET;
                MPS2FBTouchPoint *point = &s->touch_points[i];

                point->pressed = 0;
                s->touch_header.points_mask &= ~(1 << i);
                point->track_id = mt->tracking_id; // == -1

                qemu_log_mask(LOG_UNIMP, "track_id release %ld, slot id %ld\n", mt->tracking_id, mt->slot);
                touch_state_changed = 1;
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unknown multi-touch type %d\n",
                    __func__, mt->type);
            break;
        }

    } else if (evt->type == INPUT_EVENT_KIND_BTN) {
        if (evt->u.btn.data->button == INPUT_BUTTON_LEFT) {
            /* Single touch - use slot 0 */
            uint8_t old_pressed = s->touch_points[MOUSE_SLOT].pressed;
            int pressed = evt->u.btn.data->down;

            s->touch_points[MOUSE_SLOT].pressed = pressed ? 1 : 0;
            s->touch_points[MOUSE_SLOT].track_id = pressed ? 0 : -1;
            if (pressed) {
                s->touch_header.points_mask |= (1 << MOUSE_SLOT);
            } else {
                s->touch_header.points_mask &= ~(1 << MOUSE_SLOT);
            }

            touch_state_changed = (old_pressed != s->touch_points[0].pressed);
        }
    } else if (evt->type == INPUT_EVENT_KIND_ABS) {
        InputAxis axis = evt->u.abs.data->axis;
        int value = evt->u.abs.data->value;
        int range = (axis == INPUT_AXIS_X) ? s->cols : s->rows;
        uint64_t scaled_value = ((uint64_t)value * (uint64_t)range) / (uint64_t)INPUT_EVENT_ABS_MAX;

        /* Single touch - use slot 0 */
        if (axis == INPUT_AXIS_X) {
            uint16_t old_x = s->touch_points[MOUSE_SLOT].x;
            s->touch_points[MOUSE_SLOT].x = scaled_value;
            touch_state_changed = (old_x != s->touch_points[MOUSE_SLOT].x);
        } else if (axis == INPUT_AXIS_Y) {
            uint16_t old_y = s->touch_points[MOUSE_SLOT].y;
            s->touch_points[MOUSE_SLOT].y = scaled_value;
            touch_state_changed = (old_y != s->touch_points[MOUSE_SLOT].y);
        }
    }

    /* Update touch state and generate IRQ if state changed */
    if (touch_state_changed) {
        mps2fb_update_irq(s);
    }
}

// Define the input handlers for our console

static const QemuInputHandler mps2_touch_handler = {
    .name  = "mps2-touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_MTT,
    .event = mps2fb_touch_event,
};

static void mps2fb_realize(DeviceState *dev, Error **errp)
{
    MPS2FBState *s = MPS2FB(dev);
    size_t fb_size = s->cols * s->rows * 4;

    /* Initialize framebuffer memory */
    memory_region_init_ram(&s->fb_mr, OBJECT(dev), "mps2-fb", fb_size, errp);

    /* Initialize control region */
    memory_region_init_io(&s->control_mr, OBJECT(dev), &control_region_ops, s,
                         "mps2-fb-control", CONTROL_REGION_SIZE);

    /* Map both regions */
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->control_mr);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->fb_mr);

    /* Initialize IRQ */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->touch_irq);

    /* Initialize touch state */
    s->ctrl.enable_irq = 0;
    s->ctrl.reserved = 0;

    s->touch_header.points_mask = 0;
    s->touch_header.reserved = 0;

    /* Initialize all touch points */
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        s->touch_points[i].x = 0;
        s->touch_points[i].y = 0;
        s->touch_points[i].pressed = 0;
        s->touch_points[i].track_id = -1;
    }

    s->invalidate = 1;
    s->con = graphic_console_init(dev, 0, &mps2fb_ops, s);
    qemu_console_resize(s->con, s->cols, s->rows);

    s->touch_handler = qemu_input_handler_register(dev, &mps2_touch_handler);
}

static void mps2fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, mps2fb_properties);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = mps2fb_realize;

    /* Note: This device does not have any state that we have to reset or migrate */
}

static const TypeInfo mps2fb_info = {
    .name          = TYPE_MPS2FB,
    .parent        = TYPE_DYNAMIC_SYS_BUS_DEVICE,
    .instance_size = sizeof(MPS2FBState),
    .class_init    = mps2fb_class_init,
};

static void mps2fb_register_types(void)
{
    type_register_static(&mps2fb_info);
}

type_init(mps2fb_register_types)
