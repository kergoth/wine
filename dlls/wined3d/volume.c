/*
 * Copyright 2002-2005 Jason Edmeades
 * Copyright 2002-2005 Raphael Junqueira
 * Copyright 2005 Oliver Stieber
 * Copyright 2009-2011 Henri Verbeet for CodeWeavers
 * Copyright 2013 Stefan Dösinger for CodeWeavers
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

WINE_DEFAULT_DEBUG_CHANNEL(d3d_surface);
WINE_DECLARE_DEBUG_CHANNEL(d3d_perf);

/* Context activation is done by the caller. */
static void volume_bind_and_dirtify(const struct wined3d_volume *volume,
        struct wined3d_context *context, BOOL srgb)
{
    struct wined3d_texture *container = volume->container;
    DWORD active_sampler;

    /* We don't need a specific texture unit, but after binding the texture the current unit is dirty.
     * Read the unit back instead of switching to 0, this avoids messing around with the state manager's
     * gl states. The current texture unit should always be a valid one.
     *
     * To be more specific, this is tricky because we can implicitly be called
     * from sampler() in state.c. This means we can't touch anything other than
     * whatever happens to be the currently active texture, or we would risk
     * marking already applied sampler states dirty again. */
    active_sampler = context->rev_tex_unit_map[context->active_texture];

    if (active_sampler != WINED3D_UNMAPPED_STAGE)
        context_invalidate_state(context, STATE_SAMPLER(active_sampler));

    container->texture_ops->texture_bind(container, context, srgb);
}

void volume_set_container(struct wined3d_volume *volume, struct wined3d_texture *container)
{
    TRACE("volume %p, container %p.\n", volume, container);

    volume->container = container;
}

/* Context activation is done by the caller. */
static void wined3d_volume_allocate_texture(struct wined3d_volume *volume,
        const struct wined3d_context *context, BOOL srgb)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    const struct wined3d_format *format = volume->resource.format;
    void *mem = NULL;

    if (gl_info->supported[APPLE_CLIENT_STORAGE] && !format->convert
            && wined3d_resource_prepare_system_memory(&volume->resource))
    {
        TRACE("Enabling GL_UNPACK_CLIENT_STORAGE_APPLE for volume %p\n", volume);
        gl_info->gl_ops.gl.p_glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
        checkGLcall("glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE)");
        mem = volume->resource.heap_memory;
        volume->flags |= WINED3D_VFLAG_CLIENT_STORAGE;
    }

    GL_EXTCALL(glTexImage3DEXT(GL_TEXTURE_3D, volume->texture_level,
            srgb ? format->glGammaInternal : format->glInternal,
            volume->resource.width, volume->resource.height, volume->resource.depth,
            0, format->glFormat, format->glType, mem));
    checkGLcall("glTexImage3D");

    if (mem)
    {
        gl_info->gl_ops.gl.p_glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
        checkGLcall("glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE)");
    }
}

/* Context activation is done by the caller. */
void wined3d_volume_upload_data(struct wined3d_volume *volume, const struct wined3d_context *context,
        const struct wined3d_bo_address *data)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    const struct wined3d_format *format = volume->resource.format;
    UINT width = volume->resource.width;
    UINT height = volume->resource.height;
    UINT depth = volume->resource.depth;
    BYTE *mem = data->addr;

    TRACE("volume %p, context %p, level %u, format %s (%#x).\n",
            volume, context, volume->texture_level, debug_d3dformat(format->id),
            format->id);

    if (format->convert)
    {
        UINT dst_row_pitch, dst_slice_pitch;
        UINT src_row_pitch, src_slice_pitch;
        UINT alignment = volume->resource.device->surface_alignment;

        if (data->buffer_object)
            ERR("Loading a converted volume from a PBO.\n");
        if (format->flags & WINED3DFMT_FLAG_BLOCKS)
            ERR("Converting a block-based format.\n");

        dst_row_pitch = width * format->conv_byte_count;
        dst_row_pitch = (dst_row_pitch + alignment - 1) & ~(alignment - 1);
        dst_slice_pitch = dst_row_pitch * height;

        wined3d_resource_get_pitch(&volume->resource, &src_row_pitch, &src_slice_pitch);

        mem = HeapAlloc(GetProcessHeap(), 0, dst_slice_pitch * depth);
        format->convert(data->addr, mem, src_row_pitch, src_slice_pitch,
                dst_row_pitch, dst_slice_pitch, width, height, depth);
    }

    if (data->buffer_object)
    {
        GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, data->buffer_object));
        checkGLcall("glBindBufferARB");
    }

    GL_EXTCALL(glTexSubImage3DEXT(GL_TEXTURE_3D, volume->texture_level, 0, 0, 0,
            width, height, depth,
            format->glFormat, format->glType, mem));
    checkGLcall("glTexSubImage3D");

    if (data->buffer_object)
    {
        GL_EXTCALL(glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0));
        checkGLcall("glBindBufferARB");
    }

    if (mem != data->addr)
        HeapFree(GetProcessHeap(), 0, mem);
}

/* Context activation is done by the caller. */
static void wined3d_volume_download_data(struct wined3d_volume *volume,
        const struct wined3d_context *context, const struct wined3d_bo_address *data)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    const struct wined3d_format *format = volume->resource.format;

    if (format->convert)
    {
        FIXME("Attempting to download a converted volume, format %s.\n",
                debug_d3dformat(format->id));
        return;
    }

    if (data->buffer_object)
    {
        GL_EXTCALL(glBindBufferARB(GL_PIXEL_PACK_BUFFER_ARB, data->buffer_object));
        checkGLcall("glBindBufferARB");
    }

    gl_info->gl_ops.gl.p_glGetTexImage(GL_TEXTURE_3D, volume->texture_level,
            format->glFormat, format->glType, data->addr);
    checkGLcall("glGetTexImage");

    if (data->buffer_object)
    {
        GL_EXTCALL(glBindBufferARB(GL_PIXEL_PACK_BUFFER_ARB, 0));
        checkGLcall("glBindBufferARB");
    }

}

static void wined3d_volume_evict_sysmem(struct wined3d_volume *volume)
{
    wined3d_resource_free_sysmem(&volume->resource);
    volume->resource.map_heap_memory = NULL;
    wined3d_resource_invalidate_location(&volume->resource, WINED3D_LOCATION_SYSMEM);
}

/* Context activation is done by the caller. */
static void wined3d_volume_srgb_transfer(struct wined3d_volume *volume,
        struct wined3d_context *context, BOOL dest_is_srgb)
{
    struct wined3d_bo_address data;
    /* Optimizations are possible, but the effort should be put into either
     * implementing EXT_SRGB_DECODE in the driver or finding out why we
     * picked the wrong copy for the original upload and fixing that.
     *
     * Also keep in mind that we want to avoid using resource.heap_memory
     * for DEFAULT pool surfaces. */

    WARN_(d3d_perf)("Performing slow rgb/srgb volume transfer.\n");
    data.buffer_object = 0;
    data.addr = HeapAlloc(GetProcessHeap(), 0, volume->resource.size);
    if (!data.addr)
        return;

    volume_bind_and_dirtify(volume, context, !dest_is_srgb);
    wined3d_volume_download_data(volume, context, &data);
    volume_bind_and_dirtify(volume, context, dest_is_srgb);
    wined3d_volume_upload_data(volume, context, &data);

    HeapFree(GetProcessHeap(), 0, data.addr);
}

static BOOL wined3d_volume_can_evict(const struct wined3d_volume *volume)
{
    if (volume->resource.pool != WINED3D_POOL_MANAGED)
        return FALSE;
    if (volume->download_count >= 10)
        return FALSE;
    if (volume->resource.format->convert)
        return FALSE;
    if (volume->flags & WINED3D_VFLAG_CLIENT_STORAGE)
        return FALSE;

    return TRUE;
}

/* Context activation is done by the caller. */
static void wined3d_volume_load_location(struct wined3d_resource *resource,
        struct wined3d_context *context, DWORD location)
{
    struct wined3d_volume *volume = volume_from_resource(resource);
    DWORD required_access = wined3d_resource_access_from_location(location);

    TRACE("Volume %p, loading %s, have %s.\n", volume, wined3d_debug_location(location),
        wined3d_debug_location(volume->resource.locations));

    if ((volume->resource.access_flags & required_access) != required_access)
    {
        ERR("Operation requires %#x access, but volume only has %#x.\n",
                required_access, volume->resource.access_flags);
        return;
    }

    switch (location)
    {
        case WINED3D_LOCATION_TEXTURE_RGB:
        case WINED3D_LOCATION_TEXTURE_SRGB:
            if ((location == WINED3D_LOCATION_TEXTURE_RGB
                    && !(volume->flags & WINED3D_VFLAG_ALLOCATED))
                    || (location == WINED3D_LOCATION_TEXTURE_SRGB
                    && !(volume->flags & WINED3D_VFLAG_SRGB_ALLOCATED)))
                ERR("Trying to load (s)RGB texture without prior allocation.\n");

            if (volume->resource.locations & WINED3D_LOCATION_DISCARDED)
            {
                TRACE("Volume previously discarded, nothing to do.\n");
                wined3d_resource_invalidate_location(&volume->resource, WINED3D_LOCATION_DISCARDED);
            }
            else if (volume->resource.locations & WINED3D_LOCATION_SYSMEM)
            {
                struct wined3d_bo_address data = {0, volume->resource.heap_memory};
                wined3d_volume_upload_data(volume, context, &data);
            }
            else if (volume->resource.locations & WINED3D_LOCATION_BUFFER)
            {
                struct wined3d_bo_address data = {volume->resource.buffer->name, NULL};
                wined3d_volume_upload_data(volume, context, &data);
            }
            else if (volume->resource.locations & WINED3D_LOCATION_TEXTURE_RGB)
            {
                wined3d_volume_srgb_transfer(volume, context, TRUE);
            }
            else if (volume->resource.locations & WINED3D_LOCATION_TEXTURE_SRGB)
            {
                wined3d_volume_srgb_transfer(volume, context, FALSE);
            }
            else
            {
                FIXME("Implement texture loading from %s.\n", wined3d_debug_location(volume->resource.locations));
                return;
            }
            wined3d_resource_validate_location(&volume->resource, location);

            if (wined3d_volume_can_evict(volume))
                wined3d_volume_evict_sysmem(volume);

            break;

        case WINED3D_LOCATION_SYSMEM:
            if (!volume->resource.heap_memory)
                ERR("Trying to load WINED3D_LOCATION_SYSMEM without setting it up first.\n");

            if (volume->resource.locations & (WINED3D_LOCATION_TEXTURE_RGB | WINED3D_LOCATION_TEXTURE_SRGB))
            {
                struct wined3d_bo_address data = {0, volume->resource.heap_memory};

                if (volume->resource.locations & WINED3D_LOCATION_TEXTURE_RGB)
                    volume_bind_and_dirtify(volume, context, FALSE);
                else
                    volume_bind_and_dirtify(volume, context, TRUE);

                volume->download_count++;
                wined3d_volume_download_data(volume, context, &data);
            }
            else
            {
                FIXME("Implement WINED3D_LOCATION_SYSMEM loading from %s.\n",
                        wined3d_debug_location(volume->resource.locations));
                return;
            }
            wined3d_resource_validate_location(&volume->resource, WINED3D_LOCATION_SYSMEM);
            break;

        case WINED3D_LOCATION_BUFFER:
            if (!volume->resource.buffer || volume->resource.map_binding != WINED3D_LOCATION_BUFFER)
                ERR("Trying to load WINED3D_LOCATION_BUFFER without setting it up first.\n");

            if (volume->resource.locations & (WINED3D_LOCATION_TEXTURE_RGB | WINED3D_LOCATION_TEXTURE_SRGB))
            {
                struct wined3d_bo_address data = {volume->resource.buffer->name, NULL};

                if (volume->resource.locations & WINED3D_LOCATION_TEXTURE_RGB)
                    volume_bind_and_dirtify(volume, context, FALSE);
                else
                    volume_bind_and_dirtify(volume, context, TRUE);

                wined3d_volume_download_data(volume, context, &data);
            }
            else
            {
                FIXME("Implement WINED3D_LOCATION_BUFFER loading from %s.\n",
                        wined3d_debug_location(volume->resource.locations));
                return;
            }
            wined3d_resource_validate_location(&volume->resource, WINED3D_LOCATION_BUFFER);
            break;

        default:
            FIXME("Implement %s loading from %s.\n", wined3d_debug_location(location),
                    wined3d_debug_location(volume->resource.locations));
    }
}

/* Context activation is done by the caller. */
void wined3d_volume_load(struct wined3d_volume *volume, struct wined3d_context *context, BOOL srgb_mode)
{
    volume_bind_and_dirtify(volume, context, srgb_mode);

    if (srgb_mode)
    {
        if (!(volume->flags & WINED3D_VFLAG_SRGB_ALLOCATED))
        {
            wined3d_volume_allocate_texture(volume, context, TRUE);
            volume->flags |= WINED3D_VFLAG_SRGB_ALLOCATED;
        }

        wined3d_resource_load_location(&volume->resource, context, WINED3D_LOCATION_TEXTURE_SRGB);
    }
    else
    {
        if (!(volume->flags & WINED3D_VFLAG_ALLOCATED))
        {
            wined3d_volume_allocate_texture(volume, context, FALSE);
            volume->flags |= WINED3D_VFLAG_ALLOCATED;
        }

        wined3d_resource_load_location(&volume->resource, context, WINED3D_LOCATION_TEXTURE_RGB);
    }
}

static void volume_unload(struct wined3d_resource *resource)
{
    struct wined3d_volume *volume = volume_from_resource(resource);
    struct wined3d_device *device = volume->resource.device;
    struct wined3d_context *context;

    if (volume->resource.pool == WINED3D_POOL_DEFAULT)
        ERR("Unloading DEFAULT pool volume.\n");

    TRACE("texture %p.\n", resource);

    if (wined3d_resource_prepare_system_memory(&volume->resource))
    {
        context = context_acquire(device, NULL);
        wined3d_resource_load_location(&volume->resource, context, WINED3D_LOCATION_SYSMEM);
        context_release(context);
        wined3d_resource_invalidate_location(&volume->resource, ~WINED3D_LOCATION_SYSMEM);
    }
    else
    {
        ERR("Out of memory when unloading volume %p.\n", volume);
        wined3d_resource_validate_location(&volume->resource, WINED3D_LOCATION_DISCARDED);
        wined3d_resource_invalidate_location(&volume->resource, ~WINED3D_LOCATION_DISCARDED);
    }

    /* The texture name is managed by the container. */
    volume->flags &= ~(WINED3D_VFLAG_ALLOCATED | WINED3D_VFLAG_SRGB_ALLOCATED
            | WINED3D_VFLAG_CLIENT_STORAGE);

    resource_unload(resource);
}

ULONG CDECL wined3d_volume_incref(struct wined3d_volume *volume)
{
    ULONG refcount;

    if (volume->container)
    {
        TRACE("Forwarding to container %p.\n", volume->container);
        return wined3d_texture_incref(volume->container);
    }

    refcount = InterlockedIncrement(&volume->resource.ref);

    TRACE("%p increasing refcount to %u.\n", volume, refcount);

    return refcount;
}

void wined3d_volume_cleanup_cs(struct wined3d_volume *volume)
{
    HeapFree(GetProcessHeap(), 0, volume);
}

ULONG CDECL wined3d_volume_decref(struct wined3d_volume *volume)
{
    ULONG refcount;

    if (volume->container)
    {
        TRACE("Forwarding to container %p.\n", volume->container);
        return wined3d_texture_decref(volume->container);
    }

    refcount = InterlockedDecrement(&volume->resource.ref);

    TRACE("%p decreasing refcount to %u.\n", volume, refcount);

    if (!refcount)
    {
        struct wined3d_device *device = volume->resource.device;

        resource_cleanup(&volume->resource);

        volume->resource.parent_ops->wined3d_object_destroyed(volume->resource.parent);
        wined3d_cs_emit_volume_cleanup(device->cs, volume);
    }

    return refcount;
}

void * CDECL wined3d_volume_get_parent(const struct wined3d_volume *volume)
{
    TRACE("volume %p.\n", volume);

    return volume->resource.parent;
}

DWORD CDECL wined3d_volume_set_priority(struct wined3d_volume *volume, DWORD priority)
{
    return resource_set_priority(&volume->resource, priority);
}

DWORD CDECL wined3d_volume_get_priority(const struct wined3d_volume *volume)
{
    return resource_get_priority(&volume->resource);
}

void CDECL wined3d_volume_preload(struct wined3d_volume *volume)
{
    FIXME("volume %p stub!\n", volume);
}

struct wined3d_resource * CDECL wined3d_volume_get_resource(struct wined3d_volume *volume)
{
    TRACE("volume %p.\n", volume);

    return &volume->resource;
}

static BOOL wined3d_volume_check_box_dimensions(const struct wined3d_volume *volume,
        const struct wined3d_box *box)
{
    if (!box)
        return TRUE;

    if (box->left >= box->right)
        return FALSE;
    if (box->top >= box->bottom)
        return FALSE;
    if (box->front >= box->back)
        return FALSE;
    if (box->right > volume->resource.width)
        return FALSE;
    if (box->bottom > volume->resource.height)
        return FALSE;
    if (box->back > volume->resource.depth)
        return FALSE;

    return TRUE;
}

HRESULT CDECL wined3d_volume_map(struct wined3d_volume *volume,
        struct wined3d_map_desc *map_desc, const struct wined3d_box *box, DWORD flags)
{
    HRESULT hr;
    const struct wined3d_format *format = volume->resource.format;

    map_desc->data = NULL;
    if (!(volume->resource.access_flags & WINED3D_RESOURCE_ACCESS_CPU))
    {
        WARN("Volume %p is not CPU accessible.\n", volume);
        return WINED3DERR_INVALIDCALL;
    }
    if (!wined3d_volume_check_box_dimensions(volume, box))
    {
        WARN("Map box is invalid.\n");
        return WINED3DERR_INVALIDCALL;
    }
    if ((format->flags & WINED3DFMT_FLAG_BLOCKS) &&
            !wined3d_resource_check_block_align(&volume->resource, box))
    {
        WARN("Map box is misaligned for %ux%u blocks.\n",
                format->block_width, format->block_height);
        return WINED3DERR_INVALIDCALL;
    }

    hr = wined3d_resource_map(&volume->resource, map_desc, box, flags);
    if (FAILED(hr))
        return hr;

    return hr;
}

struct wined3d_volume * CDECL wined3d_volume_from_resource(struct wined3d_resource *resource)
{
    return volume_from_resource(resource);
}

HRESULT CDECL wined3d_volume_unmap(struct wined3d_volume *volume)
{
    if (volume->resource.unmap_dirtify)
        wined3d_texture_set_dirty(volume->container);

    return wined3d_resource_unmap(&volume->resource);
}

static void wined3d_volume_changed(struct wined3d_resource *resource)
{
    struct wined3d_volume *volume = volume_from_resource(resource);

    if (volume->container)
        wined3d_texture_set_dirty(volume->container);
}

static const struct wined3d_resource_ops volume_resource_ops =
{
    volume_unload,
    wined3d_volume_load_location,
    wined3d_volume_changed,
};

static HRESULT volume_init(struct wined3d_volume *volume, struct wined3d_device *device, UINT width,
        UINT height, UINT depth, UINT level, DWORD usage, enum wined3d_format_id format_id,
        enum wined3d_pool pool, void *parent, const struct wined3d_parent_ops *parent_ops)
{
    const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
    const struct wined3d_format *format = wined3d_get_format(gl_info, format_id);
    HRESULT hr;
    UINT size;

    if (!gl_info->supported[EXT_TEXTURE3D])
    {
        WARN("Volume cannot be created - no volume texture support.\n");
        return WINED3DERR_INVALIDCALL;
    }
    /* TODO: Write tests for other resources and move this check
     * to resource_init, if applicable. */
    if (usage & WINED3DUSAGE_DYNAMIC
            && (pool == WINED3D_POOL_MANAGED || pool == WINED3D_POOL_SCRATCH))
    {
        WARN("Attempted to create a DYNAMIC texture in pool %u.\n", pool);
        return WINED3DERR_INVALIDCALL;
    }

    size = wined3d_format_calculate_size(format, device->surface_alignment, width, height, depth);

    hr = resource_init(&volume->resource, device, WINED3D_RTYPE_VOLUME, format,
            WINED3D_MULTISAMPLE_NONE, 0, usage, pool, width, height, depth,
            size, parent, parent_ops, &volume_resource_ops);
    if (FAILED(hr))
    {
        WARN("Failed to initialize resource, returning %#x.\n", hr);
        return hr;
    }

    volume->texture_level = level;
    volume->resource.locations = WINED3D_LOCATION_DISCARDED;
    volume->resource.map_binding = WINED3D_LOCATION_SYSMEM;

    if (pool == WINED3D_POOL_DEFAULT && usage & WINED3DUSAGE_DYNAMIC
            && gl_info->supported[ARB_PIXEL_BUFFER_OBJECT]
            && !format->convert)
    {
        wined3d_resource_free_sysmem(&volume->resource);
        volume->resource.map_binding = WINED3D_LOCATION_BUFFER;
        volume->resource.map_heap_memory = NULL;
    }

    return WINED3D_OK;
}

HRESULT CDECL wined3d_volume_create(struct wined3d_device *device, UINT width, UINT height,
        UINT depth, UINT level, DWORD usage, enum wined3d_format_id format_id, enum wined3d_pool pool,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_volume **volume)
{
    struct wined3d_volume *object;
    HRESULT hr;

    TRACE("device %p, width %u, height %u, depth %u, usage %#x, format %s, pool %s\n",
            device, width, height, depth, usage, debug_d3dformat(format_id), debug_d3dpool(pool));
    TRACE("parent %p, parent_ops %p, volume %p.\n", parent, parent_ops, volume);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
    {
        *volume = NULL;
        return WINED3DERR_OUTOFVIDEOMEMORY;
    }

    hr = volume_init(object, device, width, height, depth, level,
            usage, format_id, pool, parent, parent_ops);
    if (FAILED(hr))
    {
        WARN("Failed to initialize volume, returning %#x.\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created volume %p.\n", object);
    *volume = object;

    return WINED3D_OK;
}
