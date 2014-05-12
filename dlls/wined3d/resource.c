/*
 * Copyright 2002-2004 Jason Edmeades
 * Copyright 2003-2004 Raphael Junqueira
 * Copyright 2004 Christian Costa
 * Copyright 2005 Oliver Stieber
 * Copyright 2009-2010 Henri Verbeet for CodeWeavers
 * Copyright 2006-2008, 2013 Stefan Dösinger for CodeWeavers
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

#include "config.h"
#include "wine/port.h"
#include "wined3d_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

struct private_data
{
    struct list entry;

    GUID tag;
    DWORD flags; /* DDSPD_* */

    union
    {
        void *data;
        IUnknown *object;
    } ptr;

    DWORD size;
};

static DWORD resource_access_from_pool(enum wined3d_pool pool)
{
    switch (pool)
    {
        case WINED3D_POOL_DEFAULT:
            return WINED3D_RESOURCE_ACCESS_GPU;

        case WINED3D_POOL_MANAGED:
            return WINED3D_RESOURCE_ACCESS_GPU | WINED3D_RESOURCE_ACCESS_CPU;

        case WINED3D_POOL_SCRATCH:
        case WINED3D_POOL_SYSTEM_MEM:
            return WINED3D_RESOURCE_ACCESS_CPU;

        default:
            FIXME("Unhandled pool %#x.\n", pool);
            return 0;
    }
}

static void resource_check_usage(DWORD usage)
{
    static const DWORD handled = WINED3DUSAGE_RENDERTARGET
            | WINED3DUSAGE_DEPTHSTENCIL
            | WINED3DUSAGE_DYNAMIC
            | WINED3DUSAGE_AUTOGENMIPMAP
            | WINED3DUSAGE_STATICDECL
            | WINED3DUSAGE_OVERLAY
            | WINED3DUSAGE_TEXTURE;

    if (usage & ~handled)
        FIXME("Unhandled usage flags %#x.\n", usage & ~handled);
}

HRESULT resource_init(struct wined3d_resource *resource, struct wined3d_device *device,
        enum wined3d_resource_type type, const struct wined3d_format *format,
        enum wined3d_multisample_type multisample_type, UINT multisample_quality,
        DWORD usage, enum wined3d_pool pool, UINT width, UINT height, UINT depth, UINT size,
        void *parent, const struct wined3d_parent_ops *parent_ops,
        const struct wined3d_resource_ops *resource_ops)
{
    const struct wined3d *d3d = device->wined3d;

    resource_check_usage(usage);
    if (pool != WINED3D_POOL_SCRATCH)
    {
        if ((usage & WINED3DUSAGE_RENDERTARGET) && !(format->flags & WINED3DFMT_FLAG_RENDERTARGET))
            return WINED3DERR_INVALIDCALL;
        if ((usage & WINED3DUSAGE_DEPTHSTENCIL) && !(format->flags & (WINED3DFMT_FLAG_DEPTH | WINED3DFMT_FLAG_STENCIL)))
            return WINED3DERR_INVALIDCALL;
        if ((usage & WINED3DUSAGE_TEXTURE) && !(format->flags & WINED3DFMT_FLAG_TEXTURE))
            return WINED3DERR_INVALIDCALL;
    }

    resource->ref = 1;
    resource->device = device;
    resource->type = type;
    resource->format = format;
    resource->multisample_type = multisample_type;
    resource->multisample_quality = multisample_quality;
    resource->usage = usage;
    resource->pool = pool;
    resource->access_flags = resource_access_from_pool(pool);
    if (usage & WINED3DUSAGE_DYNAMIC)
        resource->access_flags |= WINED3D_RESOURCE_ACCESS_CPU;
    resource->width = width;
    resource->height = height;
    resource->depth = depth;
    resource->size = size;
    resource->priority = 0;
    resource->parent = parent;
    resource->parent_ops = parent_ops;
    resource->resource_ops = resource_ops;
    list_init(&resource->privateData);

    if (size)
    {
        if (!wined3d_resource_allocate_sysmem(resource))
        {
            ERR("Failed to allocate system memory.\n");
            return E_OUTOFMEMORY;
        }
        resource->heap_memory = resource->map_heap_memory;
    }
    else
    {
        resource->heap_memory = NULL;
    }
    resource->allocatedMemory = resource->heap_memory;

    /* Check that we have enough video ram left */
    if (pool == WINED3D_POOL_DEFAULT && d3d->flags & WINED3D_VIDMEM_ACCOUNTING)
    {
        if (size > wined3d_device_get_available_texture_mem(device))
        {
            ERR("Out of adapter memory\n");
            wined3d_resource_free_sysmem(resource);
            return WINED3DERR_OUTOFVIDEOMEMORY;
        }
        adapter_adjust_memory(device->adapter, size);
    }

    device_resource_add(device, resource);

    return WINED3D_OK;
}

static void wined3d_resource_free_bo(struct wined3d_resource *resource)
{
    struct wined3d_context *context = context_acquire(resource->device, NULL);

    if (resource->buffer != resource->map_buffer)
        ERR("Releasing resource buffer with buffer != map_buffer.\n");

    wined3d_device_release_bo(resource->device, resource->buffer, context);
    resource->buffer = NULL;
    resource->map_buffer = NULL;

    context_release(context);
}

void wined3d_resource_cleanup_cs(struct wined3d_resource *resource)
{
    if (resource->buffer)
        wined3d_resource_free_bo(resource);

    wined3d_resource_free_sysmem(resource);
    resource->allocatedMemory = NULL;
    resource->map_heap_memory = NULL;
}

void resource_cleanup(struct wined3d_resource *resource)
{
    const struct wined3d *d3d = resource->device->wined3d;
    struct private_data *data;
    struct list *e1, *e2;
    HRESULT hr;

    TRACE("Cleaning up resource %p.\n", resource);

    if (resource->pool == WINED3D_POOL_DEFAULT && d3d->flags & WINED3D_VIDMEM_ACCOUNTING)
    {
        TRACE("Decrementing device memory pool by %u.\n", resource->size);
        adapter_adjust_memory(resource->device->adapter, 0 - resource->size);
    }

    LIST_FOR_EACH_SAFE(e1, e2, &resource->privateData)
    {
        data = LIST_ENTRY(e1, struct private_data, entry);
        hr = wined3d_resource_free_private_data(resource, &data->tag);
        if (FAILED(hr))
            ERR("Failed to free private data when destroying resource %p, hr = %#x.\n", resource, hr);
    }

    wined3d_cs_emit_resource_cleanup(resource->device->cs, resource);

    device_resource_released(resource->device, resource);
}

void resource_unload(struct wined3d_resource *resource)
{
    if (resource->map_count)
        ERR("Resource %p is being unloaded while mapped.\n", resource);

    if (resource->buffer)
        wined3d_resource_free_bo(resource);

    context_resource_unloaded(resource->device,
            resource, resource->type);
}

static struct private_data *resource_find_private_data(const struct wined3d_resource *resource, REFGUID tag)
{
    struct private_data *data;
    struct list *entry;

    TRACE("Searching for private data %s\n", debugstr_guid(tag));
    LIST_FOR_EACH(entry, &resource->privateData)
    {
        data = LIST_ENTRY(entry, struct private_data, entry);
        if (IsEqualGUID(&data->tag, tag)) {
            TRACE("Found %p\n", data);
            return data;
        }
    }
    TRACE("Not found\n");
    return NULL;
}

HRESULT CDECL wined3d_resource_set_private_data(struct wined3d_resource *resource, REFGUID guid,
        const void *data, DWORD data_size, DWORD flags)
{
    struct private_data *d;

    TRACE("resource %p, riid %s, data %p, data_size %u, flags %#x.\n",
            resource, debugstr_guid(guid), data, data_size, flags);

    wined3d_resource_free_private_data(resource, guid);

    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return E_OUTOFMEMORY;

    d->tag = *guid;
    d->flags = flags;

    if (flags & WINED3DSPD_IUNKNOWN)
    {
        if (data_size != sizeof(IUnknown *))
        {
            WARN("IUnknown data with size %u, returning WINED3DERR_INVALIDCALL.\n", data_size);
            HeapFree(GetProcessHeap(), 0, d);
            return WINED3DERR_INVALIDCALL;
        }
        d->ptr.object = (IUnknown *)data;
        d->size = sizeof(IUnknown *);
        IUnknown_AddRef(d->ptr.object);
    }
    else
    {
        d->ptr.data = HeapAlloc(GetProcessHeap(), 0, data_size);
        if (!d->ptr.data)
        {
            HeapFree(GetProcessHeap(), 0, d);
            return E_OUTOFMEMORY;
        }
        d->size = data_size;
        memcpy(d->ptr.data, data, data_size);
    }
    list_add_tail(&resource->privateData, &d->entry);

    return WINED3D_OK;
}

HRESULT CDECL wined3d_resource_get_private_data(const struct wined3d_resource *resource, REFGUID guid,
        void *data, DWORD *data_size)
{
    const struct private_data *d;

    TRACE("resource %p, guid %s, data %p, data_size %p.\n",
            resource, debugstr_guid(guid), data, data_size);

    d = resource_find_private_data(resource, guid);
    if (!d) return WINED3DERR_NOTFOUND;

    if (*data_size < d->size)
    {
        *data_size = d->size;
        return WINED3DERR_MOREDATA;
    }

    if (d->flags & WINED3DSPD_IUNKNOWN)
    {
        *(IUnknown **)data = d->ptr.object;
        if (resource->device->wined3d->dxVersion != 7)
        {
            /* D3D8 and D3D9 addref the private data, DDraw does not. This
             * can't be handled in ddraw because it doesn't know if the
             * pointer returned is an IUnknown * or just a blob. */
            IUnknown_AddRef(d->ptr.object);
        }
    }
    else
    {
        memcpy(data, d->ptr.data, d->size);
    }

    return WINED3D_OK;
}
HRESULT CDECL wined3d_resource_free_private_data(struct wined3d_resource *resource, REFGUID guid)
{
    struct private_data *data;

    TRACE("resource %p, guid %s.\n", resource, debugstr_guid(guid));

    data = resource_find_private_data(resource, guid);
    if (!data) return WINED3DERR_NOTFOUND;

    if (data->flags & WINED3DSPD_IUNKNOWN)
    {
        if (data->ptr.object)
            IUnknown_Release(data->ptr.object);
    }
    else
    {
        HeapFree(GetProcessHeap(), 0, data->ptr.data);
    }
    list_remove(&data->entry);

    HeapFree(GetProcessHeap(), 0, data);

    return WINED3D_OK;
}

DWORD resource_set_priority(struct wined3d_resource *resource, DWORD priority)
{
    DWORD prev = resource->priority;
    resource->priority = priority;
    TRACE("resource %p, new priority %u, returning old priority %u.\n", resource, priority, prev);
    return prev;
}

DWORD resource_get_priority(const struct wined3d_resource *resource)
{
    TRACE("resource %p, returning %u.\n", resource, resource->priority);
    return resource->priority;
}

void * CDECL wined3d_resource_get_parent(const struct wined3d_resource *resource)
{
    return resource->parent;
}

void CDECL wined3d_resource_get_desc(const struct wined3d_resource *resource, struct wined3d_resource_desc *desc)
{
    desc->resource_type = resource->type;
    desc->format = resource->format->id;
    desc->multisample_type = resource->multisample_type;
    desc->multisample_quality = resource->multisample_quality;
    desc->usage = resource->usage;
    desc->pool = resource->pool;
    desc->width = resource->width;
    desc->height = resource->height;
    desc->depth = resource->depth;
    desc->size = resource->size;
}

void *wined3d_resource_allocate_sysmem2(struct wined3d_resource *resource)
{
    void **p;
    SIZE_T align = RESOURCE_ALIGNMENT - 1 + sizeof(*p);
    void *mem;

    if (!(mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, resource->size + align)))
        return FALSE;

    p = (void **)(((ULONG_PTR)mem + align) & ~(RESOURCE_ALIGNMENT - 1)) - 1;
    *p = mem;

    return ++p;
}

BOOL wined3d_resource_allocate_sysmem(struct wined3d_resource *resource)
{
    void **p;
    SIZE_T align = RESOURCE_ALIGNMENT - 1 + sizeof(*p);
    void *mem;

    if (!(mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, resource->size + align)))
        return FALSE;

    p = (void **)(((ULONG_PTR)mem + align) & ~(RESOURCE_ALIGNMENT - 1)) - 1;
    *p = mem;

    resource->map_heap_memory = ++p;

    return TRUE;
}

void wined3d_resource_free_sysmem(struct wined3d_resource *resource)
{
    void **p = resource->heap_memory;

    if (!p)
        return;

    HeapFree(GetProcessHeap(), 0, *(--p));
    resource->heap_memory = NULL;
}

DWORD wined3d_resource_sanitize_map_flags(const struct wined3d_resource *resource, DWORD flags)
{
    /* Not all flags make sense together, but Windows never returns an error.
     * Catch the cases that could cause issues. */
    if (flags & WINED3D_MAP_READONLY)
    {
        if (flags & WINED3D_MAP_DISCARD)
        {
            WARN("WINED3D_MAP_READONLY combined with WINED3D_MAP_DISCARD, ignoring flags.\n");
            return 0;
        }
        if (flags & WINED3D_MAP_NOOVERWRITE)
        {
            WARN("WINED3D_MAP_READONLY combined with WINED3D_MAP_NOOVERWRITE, ignoring flags.\n");
            return 0;
        }
    }
    else if ((flags & (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE))
            == (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE))
    {
        WARN("WINED3D_MAP_DISCARD and WINED3D_MAP_NOOVERWRITE used together, ignoring.\n");
        return 0;
    }
    else if (flags & (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE)
            && !(resource->usage & WINED3DUSAGE_DYNAMIC))
    {
        WARN("DISCARD or NOOVERWRITE map on non-dynamic buffer, ignoring.\n");
        return 0;
    }

    return flags;
}

GLbitfield wined3d_resource_gl_map_flags(DWORD d3d_flags)
{
    GLbitfield ret = 0;

    if (!(d3d_flags & WINED3D_MAP_READONLY))
        ret |= GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
    if (!(d3d_flags & (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE)))
        ret |= GL_MAP_READ_BIT;

    if (d3d_flags & WINED3D_MAP_DISCARD)
        ret |= GL_MAP_INVALIDATE_BUFFER_BIT;
    if (d3d_flags & WINED3D_MAP_NOOVERWRITE)
        ret |= GL_MAP_UNSYNCHRONIZED_BIT;

    return ret;
}

static GLenum wined3d_resource_gl_legacy_map_flags(DWORD d3d_flags)
{
    if (d3d_flags & WINED3D_MAP_READONLY)
        return GL_READ_ONLY_ARB;
    if (d3d_flags & (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE))
        return GL_WRITE_ONLY_ARB;
    return GL_READ_WRITE_ARB;
}

void wined3d_resource_validate_location(struct wined3d_resource *resource, DWORD location)
{
    TRACE("Resource %p, setting %s.\n", resource, wined3d_debug_location(location));
    resource->locations |= location;
    TRACE("new location flags are %s.\n", wined3d_debug_location(resource->locations));
}

void wined3d_resource_invalidate_location(struct wined3d_resource *resource, DWORD location)
{
    TRACE("Resource %p, setting %s.\n", resource, wined3d_debug_location(location));
    resource->locations &= ~location;
    TRACE("new location flags are %s.\n", wined3d_debug_location(resource->locations));
}

DWORD wined3d_resource_access_from_location(DWORD location)
{
    switch (location)
    {
        case WINED3D_LOCATION_DISCARDED:
            return 0;

        case WINED3D_LOCATION_SYSMEM:
        case WINED3D_LOCATION_USER:
        case WINED3D_LOCATION_DIB:
            return WINED3D_RESOURCE_ACCESS_CPU;

        case WINED3D_LOCATION_BUFFER:
        case WINED3D_LOCATION_TEXTURE_RGB:
        case WINED3D_LOCATION_TEXTURE_SRGB:
        case WINED3D_LOCATION_DRAWABLE:
        case WINED3D_LOCATION_RB_MULTISAMPLE:
        case WINED3D_LOCATION_RB_RESOLVED:
            return WINED3D_RESOURCE_ACCESS_GPU;

        default:
            FIXME("Unhandled location %#x.\n", location);
            return 0;
    }
}

void wined3d_resource_get_memory(const struct wined3d_resource *resource,
        DWORD location, struct wined3d_bo_address *data)
{
    if (location & WINED3D_LOCATION_BUFFER)
    {
        data->buffer_object = resource->buffer->name;
        data->addr = NULL;
        return;
    }
    if (location & WINED3D_LOCATION_SYSMEM)
    {
        data->buffer_object = 0;
        data->addr = resource->heap_memory;
        return;
    }
    if (location & WINED3D_LOCATION_USER)
    {
        data->buffer_object = 0;
        data->addr = resource->user_memory;
        return;
    }
    if (location & WINED3D_LOCATION_DIB)
    {
        data->buffer_object = 0;
        data->addr = resource->dib_memory;
        return;
    }

    ERR("Unexpected location %s.\n", wined3d_debug_location(location));
}

/* Context activation is done by the caller */
static void wined3d_resource_download_buffer(const struct wined3d_resource *resource,
        const struct wined3d_gl_info *gl_info, const struct wined3d_bo_address *dst_data)
{
    GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, resource->buffer->name));
    GL_EXTCALL(glGetBufferSubDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0, resource->size, dst_data->addr));
    GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0));
    checkGLcall("Download PBO");
}

/* Context activation is done by the caller */
static void wined3d_resource_upload_buffer(const struct wined3d_resource *resource,
        const struct wined3d_gl_info *gl_info, const struct wined3d_bo_address *src_data)
{
    GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, resource->buffer->name));
    GL_EXTCALL(glBufferSubDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0, resource->size, src_data->addr));
    GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0));
    checkGLcall("Upload PBO");
}

/* Context activation is optionally by the caller. Context may be NULL. */
static BOOL wined3d_resource_load_simple_location(struct wined3d_resource *resource,
        struct wined3d_context *context, DWORD location)
{
    struct wined3d_bo_address dst, src;
    DWORD sysmem_locations = WINED3D_LOCATION_SYSMEM | WINED3D_LOCATION_USER | WINED3D_LOCATION_DIB;

    if (resource->locations & WINED3D_LOCATION_DISCARDED)
    {
        TRACE("Resource was discarded, nothing to do.\n");
        return TRUE;
    }

    wined3d_resource_get_memory(resource, location, &dst);

    if (resource->locations & sysmem_locations)
    {
        wined3d_resource_get_memory(resource,
                resource->locations & sysmem_locations, &src);

        if (location == WINED3D_LOCATION_BUFFER)
            wined3d_resource_upload_buffer(resource, context->gl_info, &src);
        else
            memcpy(dst.addr, src.addr, resource->size);

        return TRUE;
    }
    if (resource->locations & WINED3D_LOCATION_BUFFER)
    {
        wined3d_resource_download_buffer(resource, context->gl_info, &dst);
        return TRUE;
    }

    return FALSE;
}

/* Context activation is optionally by the caller. Context may be NULL. */
void wined3d_resource_load_location(struct wined3d_resource *resource,
        struct wined3d_context *context, DWORD location)
{
    DWORD required_access = wined3d_resource_access_from_location(location);
    DWORD basic_locations = WINED3D_LOCATION_BUFFER | WINED3D_LOCATION_SYSMEM
            | WINED3D_LOCATION_USER | WINED3D_LOCATION_DIB;

    if ((resource->locations & location) == location)
    {
        TRACE("Location(s) already up to date.\n");
        return;
    }

    /* Keep this a WARN for now until surfaces are cleaned up. */
    if ((resource->access_flags & required_access) != required_access)
        WARN("Operation requires %#x access, but resource only has %#x.\n",
                required_access, resource->access_flags);

    if (location & basic_locations)
    {
        if (wined3d_resource_load_simple_location(resource, context, location))
        {
            resource->locations |= location;
            return;
        }
    }

    /* Context is NULL in ddraw-only operation without OpenGL. */
    if (!context)
        ERR("A context is required for non-sysmem operation.\n");

    resource->resource_ops->resource_load_location(resource, context, location);
}

BYTE *wined3d_resource_get_map_ptr(const struct wined3d_resource *resource,
        const struct wined3d_context *context, DWORD flags)
{
    const struct wined3d_gl_info *gl_info;
    BYTE *ptr;

    switch (resource->map_binding)
    {
        case WINED3D_LOCATION_BUFFER:
            gl_info = context->gl_info;
            GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, resource->map_buffer->name));

            if (gl_info->supported[ARB_MAP_BUFFER_RANGE])
            {
                GLbitfield mapflags = wined3d_resource_gl_map_flags(flags);
                mapflags &= ~GL_MAP_FLUSH_EXPLICIT_BIT;
                ptr = GL_EXTCALL(glMapBufferRange(GL_PIXEL_UNPACK_BUFFER_ARB,
                        0, resource->size, mapflags));
            }
            else
            {
                GLenum access = wined3d_resource_gl_legacy_map_flags(flags);
                ptr = GL_EXTCALL(glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, access));
            }

            GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0));
            checkGLcall("Map GL buffer");
            return ptr;

        case WINED3D_LOCATION_SYSMEM:
            return resource->map_heap_memory;

        case WINED3D_LOCATION_USER:
            return resource->user_memory;

        case WINED3D_LOCATION_DIB:
            return resource->dib_memory;

        default:
            ERR("Unexpected map binding %s.\n", wined3d_debug_location(resource->map_binding));
            return NULL;
    }
}

void wined3d_resource_release_map_ptr(const struct wined3d_resource *resource,
        const struct wined3d_context *context)
{
    const struct wined3d_gl_info *gl_info;

    switch (resource->map_binding)
    {
        case WINED3D_LOCATION_BUFFER:
            gl_info = context->gl_info;
            GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, resource->map_buffer->name));
            GL_EXTCALL(glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB));
            GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0));
            checkGLcall("Unmap GL buffer");
            return;

        case WINED3D_LOCATION_SYSMEM:
        case WINED3D_LOCATION_USER:
        case WINED3D_LOCATION_DIB:
            return;

        default:
            ERR("Unexpected map binding %s.\n", wined3d_debug_location(resource->map_binding));
            return;
    }
}

/* Context activation is done by the caller. */
static void wined3d_resource_prepare_bo(struct wined3d_resource *resource, struct wined3d_context *context)
{
    if (resource->buffer)
        return;

    resource->buffer = wined3d_device_get_bo(resource->device, resource->size,
            GL_STREAM_DRAW_ARB, GL_PIXEL_UNPACK_BUFFER_ARB, context);
    resource->map_buffer = resource->buffer;
    TRACE("Created GL buffer %u for resource %p.\n", resource->buffer->name, resource);
}

BOOL wined3d_resource_prepare_system_memory(struct wined3d_resource *resource)
{
    if (resource->heap_memory)
        return TRUE;

    if (!wined3d_resource_allocate_sysmem(resource))
    {
        ERR("Failed to allocate system memory.\n");
        return FALSE;
    }
    resource->heap_memory = resource->map_heap_memory;
    return TRUE;
}

/* Context activation is optionally by the caller. Context may be NULL. */
BOOL wined3d_resource_prepare_map_memory(struct wined3d_resource *resource, struct wined3d_context *context)
{
    switch (resource->map_binding)
    {
        case WINED3D_LOCATION_BUFFER:
            wined3d_resource_prepare_bo(resource, context);
            return TRUE;

        case WINED3D_LOCATION_SYSMEM:
            return wined3d_resource_prepare_system_memory(resource);

        case WINED3D_LOCATION_USER:
            return TRUE;

        case WINED3D_LOCATION_DIB:
            if (resource->type != WINED3D_RTYPE_SURFACE)
            {
                ERR("Trying to create a DIB for a non-surface resource.\n");
                return FALSE;
            }

            return wined3d_surface_prepare_dib(resource);

        default:
            ERR("Unexpected map binding %s.\n", wined3d_debug_location(resource->map_binding));
            return FALSE;
    }
}

void CDECL wined3d_resource_get_pitch(const struct wined3d_resource *resource, UINT *row_pitch,
        UINT *slice_pitch)
{
    const struct wined3d_format *format = resource->format;

    if (resource->custom_row_pitch)
    {
        *row_pitch = resource->custom_row_pitch;
        *slice_pitch = resource->custom_slice_pitch;
        return;
    }

    if (format->flags & WINED3DFMT_FLAG_BLOCKS)
    {
        /* Since compressed formats are block based, pitch means the amount of
         * bytes to the next row of block rather than the next row of pixels. */
        UINT row_block_count = (resource->width + format->block_width - 1) / format->block_width;
        UINT slice_block_count = (resource->height + format->block_height - 1) / format->block_height;
        *row_pitch = row_block_count * format->block_byte_count;
        *slice_pitch = *row_pitch * slice_block_count;
    }
    else
    {
        unsigned char alignment = resource->device->surface_alignment;
        *row_pitch = format->byte_count * resource->width;  /* Bytes / row */
        *row_pitch = (*row_pitch + alignment - 1) & ~(alignment - 1);
        *slice_pitch = *row_pitch * resource->height;
    }

    TRACE("Returning row pitch %u, slice pitch %u.\n", *row_pitch, *slice_pitch);
}

BOOL wined3d_resource_check_block_align(const struct wined3d_resource *resource,
        const struct wined3d_box *box)
{
    UINT width_mask, height_mask;
    const struct wined3d_format *format = resource->format;

    if (!box)
        return TRUE;

    /* This assumes power of two block sizes, but NPOT block sizes would be
     * silly anyway.
     *
     * This also assumes that the format's block depth is 1. */
    width_mask = format->block_width - 1;
    height_mask = format->block_height - 1;

    if (box->left & width_mask)
        return FALSE;
    if (box->top & height_mask)
        return FALSE;
    if (box->right & width_mask && box->right != resource->width)
        return FALSE;
    if (box->bottom & height_mask && box->bottom != resource->height)
        return FALSE;

    return TRUE;
}

void *wined3d_resource_map_internal(struct wined3d_resource *resource, DWORD flags)
{
    struct wined3d_device *device = resource->device;
    struct wined3d_context *context = NULL;
    void *mem;

    if (device->d3d_initialized)
        context = context_acquire(device, NULL);

    if (!wined3d_resource_prepare_map_memory(resource, context))
    {
        WARN("Out of memory.\n");
        context_release(context);
        return NULL;
    }

    if (flags & WINED3D_MAP_DISCARD)
    {
        switch (resource->map_binding)
        {
            case WINED3D_LOCATION_BUFFER:
                resource->map_buffer = wined3d_device_get_bo(device, resource->size,
                        GL_STREAM_DRAW_ARB, GL_PIXEL_UNPACK_BUFFER_ARB, context);
                break;

            case WINED3D_LOCATION_SYSMEM:
                wined3d_resource_allocate_sysmem(resource);
                break;

            default:
                if (resource->access_fence)
                    ERR("Location %s does not support DISCARD maps.\n", 
                            wined3d_debug_location(resource->map_binding));
                if (resource->pool != WINED3D_POOL_DEFAULT)
                    FIXME("Discard used on %s pool resource.\n", debug_d3dpool(resource->pool));
        }
        wined3d_resource_validate_location(resource, resource->map_binding);
    }
    else
    {
        wined3d_resource_load_location(resource, context, resource->map_binding);
    }

    mem = wined3d_resource_get_map_ptr(resource, context, flags);

    if (context)
        context_release(context);

    return mem;
}

static void wined3d_resource_sync(struct wined3d_resource *resource)
{
    struct wined3d_resource *real_res = resource;
    struct wined3d_surface *surface;
    struct wined3d_volume *volume;

    switch (resource->type)
    {
        case WINED3D_RTYPE_SURFACE:
            surface = surface_from_resource(resource);
            if (surface->container)
                real_res = &surface->container->resource;
            break;

        case WINED3D_RTYPE_VOLUME:
            volume = volume_from_resource(resource);
            real_res = &volume->container->resource;
            break;

        default:
            break;
    }
    wined3d_resource_wait_fence(real_res);
}

HRESULT wined3d_resource_map(struct wined3d_resource *resource,
        struct wined3d_map_desc *map_desc, const struct wined3d_box *box, DWORD flags)
{
    struct wined3d_device *device = resource->device;
    BYTE *base_memory;
    const struct wined3d_format *format = resource->format;

    TRACE("resource %p, map_desc %p, box %p, flags %#x.\n",
            resource, map_desc, box, flags);

    if (resource->map_count)
    {
        WARN("Volume is already mapped.\n");
        return WINED3DERR_INVALIDCALL;
    }

    flags = wined3d_resource_sanitize_map_flags(resource, flags);

    if (flags & WINED3D_MAP_NOOVERWRITE)
        FIXME("WINED3D_MAP_NOOVERWRITE are not implemented yet.\n");

    if (flags & WINED3D_MAP_DISCARD)
    {
        switch (resource->map_binding)
        {
            case WINED3D_LOCATION_BUFFER:
            case WINED3D_LOCATION_SYSMEM:
                break;

            default:
                FIXME("Implement discard maps with %s map binding.\n",
                        wined3d_debug_location(resource->map_binding));
                wined3d_resource_sync(resource);
        }
    }
    else
        wined3d_resource_sync(resource);

    base_memory = wined3d_cs_emit_resource_map(device->cs, resource, flags);
    if (!base_memory)
    {
        WARN("Map failed.\n");
        return WINED3DERR_INVALIDCALL;
    }

    TRACE("Base memory pointer %p.\n", base_memory);

    if (format->flags & WINED3DFMT_FLAG_BROKEN_PITCH)
    {
        map_desc->row_pitch = resource->width * format->byte_count;
        map_desc->slice_pitch = map_desc->row_pitch * resource->height;
    }
    else
    {
        wined3d_resource_get_pitch(resource, &map_desc->row_pitch, &map_desc->slice_pitch);
    }

    if (!box)
    {
        TRACE("No box supplied - all is ok\n");
        map_desc->data = base_memory;
    }
    else
    {
        TRACE("Lock Box (%p) = l %u, t %u, r %u, b %u, fr %u, ba %u\n",
                box, box->left, box->top, box->right, box->bottom, box->front, box->back);

        if ((format->flags & (WINED3DFMT_FLAG_BLOCKS | WINED3DFMT_FLAG_BROKEN_PITCH)) == WINED3DFMT_FLAG_BLOCKS)
        {
            /* Compressed textures are block based, so calculate the offset of
             * the block that contains the top-left pixel of the locked rectangle. */
            map_desc->data = base_memory
                    + (box->front * map_desc->slice_pitch)
                    + ((box->top / format->block_height) * map_desc->row_pitch)
                    + ((box->left / format->block_width) * format->block_byte_count);
        }
        else
        {
            map_desc->data = base_memory
                    + (map_desc->slice_pitch * box->front)
                    + (map_desc->row_pitch * box->top)
                    + (box->left * format->byte_count);
        }
    }

    if (!(flags & WINED3D_MAP_READONLY))
        resource->unmap_dirtify = TRUE;

    resource->map_count++;

    TRACE("Returning memory %p, row pitch %d, slice pitch %d.\n",
            map_desc->data, map_desc->row_pitch, map_desc->slice_pitch);

    return WINED3D_OK;
}

void wined3d_resource_unmap_internal(struct wined3d_resource *resource)
{
    struct wined3d_device *device = resource->device;
    struct wined3d_context *context = NULL;

    if (device->d3d_initialized)
        context = context_acquire(device, NULL);
    wined3d_resource_release_map_ptr(resource, context);
    if (context)
        context_release(context);
}

HRESULT wined3d_resource_unmap(struct wined3d_resource *resource)
{
    struct wined3d_device *device = resource->device;
    TRACE("resource %p.\n", resource);

    if (!resource->map_count)
    {
        WARN("Trying to unlock an unlocked resource %p.\n", resource);
        return WINED3DERR_INVALIDCALL;
    }

    wined3d_cs_emit_resource_unmap(device->cs, resource);

    if (resource->unmap_dirtify)
    {
        wined3d_cs_emit_resource_changed(device->cs, resource,
                resource->map_buffer, resource->map_heap_memory);
    }
    resource->unmap_dirtify = FALSE;

    resource->map_count--;

    return WINED3D_OK;
}

void wined3d_resource_changed(struct wined3d_resource *resource, struct wined3d_gl_bo *swap_buffer,
        void *swap_heap_memory)
{
    struct wined3d_device *device = resource->device;

    if (swap_buffer && swap_buffer != resource->buffer)
    {
        struct wined3d_context *context = context_acquire(device, NULL);
        wined3d_device_release_bo(device, resource->buffer, context);
        context_release(context);
        resource->buffer = swap_buffer;
    }
    if (swap_heap_memory && swap_heap_memory != resource->heap_memory)
    {
        wined3d_resource_free_sysmem(resource);
        resource->heap_memory = swap_heap_memory;
    }

    resource->resource_ops->resource_changed(resource);
    wined3d_resource_invalidate_location(resource, ~resource->map_binding);
}
