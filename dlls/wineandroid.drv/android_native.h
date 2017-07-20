/*
 * Android native system definitions
 *
 * Copyright 2013 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* Copy of some Android native structures to avoid depending on the Android source */
/* Hopefully these won't change too frequently... */

/* Native window definitions */

typedef struct native_handle
{
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;

typedef const native_handle_t *buffer_handle_t;

struct android_native_base_t
{
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(struct android_native_base_t *base);
    void (*decRef)(struct android_native_base_t *base);
};

typedef struct android_native_rect_t
{
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} android_native_rect_t;

struct ANativeWindowBuffer
{
    struct android_native_base_t common;
    int width;
    int height;
    int stride;
    int format;
    int usage;
    void *reserved[2];
    buffer_handle_t handle;
    void *reserved_proc[8];
};

struct ANativeWindow
{
    struct android_native_base_t common;
    uint32_t flags;
    int      minSwapInterval;
    int      maxSwapInterval;
    float    xdpi;
    float    ydpi;
    intptr_t oem[4];
    int (*setSwapInterval)(struct ANativeWindow *window, int interval);
    int (*dequeueBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer **buffer);
    int (*lockBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*queueBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*query)(const struct ANativeWindow *window, int what, int *value);
    int (*perform)(struct ANativeWindow *window, int operation, ... );
    int (*cancelBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*dequeueBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer **buffer, int *fenceFd);
    int (*queueBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fenceFd);
    int (*cancelBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fenceFd);
};

enum native_window_query
{
    NATIVE_WINDOW_WIDTH                     = 0,
    NATIVE_WINDOW_HEIGHT                    = 1,
    NATIVE_WINDOW_FORMAT                    = 2,
    NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS    = 3,
    NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER = 4,
    NATIVE_WINDOW_CONCRETE_TYPE             = 5,
    NATIVE_WINDOW_DEFAULT_WIDTH             = 6,
    NATIVE_WINDOW_DEFAULT_HEIGHT            = 7,
    NATIVE_WINDOW_TRANSFORM_HINT            = 8,
    NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND   = 9
};

enum native_window_perform
{
    NATIVE_WINDOW_SET_USAGE                   = 0,
    NATIVE_WINDOW_CONNECT                     = 1,
    NATIVE_WINDOW_DISCONNECT                  = 2,
    NATIVE_WINDOW_SET_CROP                    = 3,
    NATIVE_WINDOW_SET_BUFFER_COUNT            = 4,
    NATIVE_WINDOW_SET_BUFFERS_GEOMETRY        = 5,
    NATIVE_WINDOW_SET_BUFFERS_TRANSFORM       = 6,
    NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP       = 7,
    NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS      = 8,
    NATIVE_WINDOW_SET_BUFFERS_FORMAT          = 9,
    NATIVE_WINDOW_SET_SCALING_MODE            = 10,
    NATIVE_WINDOW_LOCK                        = 11,
    NATIVE_WINDOW_UNLOCK_AND_POST             = 12,
    NATIVE_WINDOW_API_CONNECT                 = 13,
    NATIVE_WINDOW_API_DISCONNECT              = 14,
    NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS = 15,
    NATIVE_WINDOW_SET_POST_TRANSFORM_CROP     = 16
};


/* Hardware module definitions */

struct hw_module_methods_t;
struct hw_device_t;
struct android_ycbcr;

struct hw_module_t
{
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint32_t reserved[32-7];
};

struct hw_module_methods_t
{
    int (*open)(const struct hw_module_t *module, const char *id, struct hw_device_t **device);
};

struct hw_device_t
{
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint32_t reserved[12];
    int (*close)(struct hw_device_t *device);
};

struct gralloc_module_t
{
    struct hw_module_t common;
    int (*registerBuffer)(struct gralloc_module_t const *module, buffer_handle_t handle);
    int (*unregisterBuffer)(struct gralloc_module_t const *module, buffer_handle_t handle);
    int (*lock)(struct gralloc_module_t const *module, buffer_handle_t handle, int usage, int l, int t, int w, int h, void **vaddr);
    int (*unlock)(struct gralloc_module_t const *module, buffer_handle_t handle);
    int (*perform)(struct gralloc_module_t const *module, int operation, ... );
    int (*lock_ycbcr)(struct gralloc_module_t const *module, buffer_handle_t handle, int usage, int l, int t, int w, int h, struct android_ycbcr *ycbcr);
    void *reserved_proc[6];
};

#define ANDROID_NATIVE_MAKE_CONSTANT(a,b,c,d) \
    (((unsigned)(a)<<24)|((unsigned)(b)<<16)|((unsigned)(c)<<8)|(unsigned)(d))

#define ANDROID_NATIVE_WINDOW_MAGIC \
    ANDROID_NATIVE_MAKE_CONSTANT('_','w','n','d')

#define ANDROID_NATIVE_BUFFER_MAGIC \
    ANDROID_NATIVE_MAKE_CONSTANT('_','b','f','r')

enum gralloc_usage
{
    GRALLOC_USAGE_SW_READ_NEVER         = 0x00000000,
    GRALLOC_USAGE_SW_READ_RARELY        = 0x00000002,
    GRALLOC_USAGE_SW_READ_OFTEN         = 0x00000003,
    GRALLOC_USAGE_SW_READ_MASK          = 0x0000000F,
    GRALLOC_USAGE_SW_WRITE_NEVER        = 0x00000000,
    GRALLOC_USAGE_SW_WRITE_RARELY       = 0x00000020,
    GRALLOC_USAGE_SW_WRITE_OFTEN        = 0x00000030,
    GRALLOC_USAGE_SW_WRITE_MASK         = 0x000000F0,
    GRALLOC_USAGE_HW_TEXTURE            = 0x00000100,
    GRALLOC_USAGE_HW_RENDER             = 0x00000200,
    GRALLOC_USAGE_HW_2D                 = 0x00000400,
    GRALLOC_USAGE_HW_COMPOSER           = 0x00000800,
    GRALLOC_USAGE_HW_FB                 = 0x00001000,
    GRALLOC_USAGE_HW_VIDEO_ENCODER      = 0x00010000,
    GRALLOC_USAGE_HW_CAMERA_WRITE       = 0x00020000,
    GRALLOC_USAGE_HW_CAMERA_READ        = 0x00040000,
    GRALLOC_USAGE_HW_CAMERA_ZSL         = 0x00060000,
    GRALLOC_USAGE_HW_CAMERA_MASK        = 0x00060000,
    GRALLOC_USAGE_HW_MASK               = 0x00071F00,
    GRALLOC_USAGE_EXTERNAL_DISP         = 0x00002000,
    GRALLOC_USAGE_PROTECTED             = 0x00004000,
    GRALLOC_USAGE_PRIVATE_0             = 0x10000000,
    GRALLOC_USAGE_PRIVATE_1             = 0x20000000,
    GRALLOC_USAGE_PRIVATE_2             = 0x40000000,
    GRALLOC_USAGE_PRIVATE_3             = 0x80000000,
    GRALLOC_USAGE_PRIVATE_MASK          = 0xF0000000,
};

#define GRALLOC_HARDWARE_MODULE_ID "gralloc"

extern int hw_get_module(const char *id, const struct hw_module_t **module);
