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

#define TYPE_MPS2FB "mps2-fb"
OBJECT_DECLARE_SIMPLE_TYPE(MPS2FBState, MPS2FB)

struct MPS2FBState {
    SysBusDevice parent_obj;

    MemoryRegion fb_mr;
    MemoryRegionSection fbsection;
    QemuConsole *con;

    uint32_t cols;
    uint32_t rows;
    int invalidate;

    QemuInputHandlerState *touch_handler;
};

// Add property handling prototypes
static const Property mps2fb_properties[] = {
    DEFINE_PROP_UINT32("cols", MPS2FBState, cols, 640),
    DEFINE_PROP_UINT32("rows", MPS2FBState, rows, 480),
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
    if (evt->type == INPUT_EVENT_KIND_ABS) {
        InputAxis axis;
        int value;

        // Extract axis and value from the event
        axis = evt->u.abs.data->axis;
        value = evt->u.abs.data->value;

        if (axis == INPUT_AXIS_X || axis == INPUT_AXIS_Y) {
            int range = (axis == INPUT_AXIS_X) ? s->cols : s->rows;
            int abs_value = (value * 0x7FFF) / range;
            (void)abs_value; // wait for feature
            // qemu_log("Touch axis: %d, value: %d\n", axis, abs_value);
            // qemu_input_queue_abs(con, axis, abs_value, 0, 0x7FFF);
        }
    }
}

// Define the input handlers for our console

static const QemuInputHandler mps2_touch_handler = {
    .name  = "mps2-touchscreen",
    .mask  = INPUT_EVENT_MASK_ABS,
    .event = mps2fb_touch_event,
};

static void mps2fb_realize(DeviceState *dev, Error **errp)
{
    MPS2FBState *s = MPS2FB(dev);
    size_t fb_size = s->cols * s->rows * 4;

    memory_region_init_ram(&s->fb_mr, OBJECT(dev), "mps2-video", fb_size, errp);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->fb_mr);

    s->invalidate = 1;
    // s->cols = 640;
    // s->rows = 480;

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
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MPS2FBState),
    .class_init    = mps2fb_class_init,
};

static void mps2fb_register_types(void)
{
    type_register_static(&mps2fb_info);
}

type_init(mps2fb_register_types)
