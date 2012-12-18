/*
 * Server-side pipe management
 *
 * Copyright (C) 1998 Alexandre Julliard
 * Copyright (C) 2001 Mike McCormack
 * Copyright (C) 2012-2013 Adam Martinson
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
 *
 * TODO:
 *   message mode
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <time.h>
#include <unistd.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"

#include "file.h"
#include "handle.h"
#include "thread.h"
#include "request.h"

enum pipe_state
{
    ps_idle_server,
    ps_wait_open,
    ps_connected,
    ps_disconnected_client,
    ps_disconnected_server,
    ps_wait_connect
};

struct named_pipe;

struct pipe_instance
{
    struct object        obj;        /* object header */
    struct fd           *ioctl_fd;   /* file descriptor for ioctls when not connected */
    struct list          entry;      /* entry in named pipe instances list */
    enum pipe_state      state;      /* instance state */
    struct pipe_end     *server;     /* server that this instance is connected to */
    struct pipe_end     *client;     /* client that this instance is connected to */
    struct named_pipe   *pipe;
};

struct pipe_end
{
    struct object            obj;        /* object header */
    struct fd               *fd;         /* pipe file descriptor */
    struct pipe_instance    *inst;       /* instance that this end is connected to */
    unsigned int             options;    /* pipe options */
    unsigned int             flags;      /* pipe flags */
    struct timeout_user     *flush_poll;
    struct event            *event_empty;
};

struct named_pipe
{
    struct object       obj;         /* object header */
    unsigned int        sharing;
    unsigned int        maxinstances;
    unsigned int        outsize;
    unsigned int        insize;
    unsigned int        numinstances;
    timeout_t           timeout;
    struct list         instances;   /* list of instances using this pipe */
    struct async_queue *waiters;     /* list of clients waiting to connect */
};

struct named_pipe_device
{
    struct object       obj;         /* object header */
    struct fd          *fd;          /* pseudo-fd for ioctls */
    struct namespace   *pipes;       /* named pipe namespace */
};

static void named_pipe_dump( struct object *obj, int verbose );
static unsigned int named_pipe_map_access( struct object *obj, unsigned int access );
static struct object *named_pipe_open_file( struct object *obj, unsigned int access,
                                            unsigned int sharing, unsigned int options );
static void named_pipe_destroy( struct object *obj );

static const struct object_ops named_pipe_ops =
{
    sizeof(struct named_pipe),    /* size */
    named_pipe_dump,              /* dump */
    no_get_type,                  /* get_type */
    no_add_queue,                 /* add_queue */
    NULL,                         /* remove_queue */
    NULL,                         /* signaled */
    NULL,                         /* satisfied */
    no_signal,                    /* signal */
    no_get_fd,                    /* get_fd */
    named_pipe_map_access,        /* map_access */
    default_get_sd,               /* get_sd */
    default_set_sd,               /* set_sd */
    no_lookup_name,               /* lookup_name */
    named_pipe_open_file,         /* open_file */
    no_close_handle,              /* close_handle */
    named_pipe_destroy            /* destroy */
};

/* pipe instance functions */
static void pipe_instance_dump( struct object *obj, int verbose );
static struct fd *pipe_instance_get_fd( struct object *obj );
static void pipe_instance_destroy( struct object *obj);
static enum server_fd_type pipe_instance_get_fd_type( struct fd *fd );
static obj_handle_t pipe_instance_ioctl( struct fd *fd, ioctl_code_t code, const async_data_t *async,
                                       int blocking, const void *data, data_size_t size );

static const struct object_ops pipe_instance_ops =
{
    sizeof(struct pipe_instance), /* size */
    pipe_instance_dump,           /* dump */
    no_get_type,                  /* get_type */
    add_queue,                    /* add_queue */
    remove_queue,                 /* remove_queue */
    default_fd_signaled,          /* signaled */
    no_satisfied,                 /* satisfied */
    no_signal,                    /* signal */
    pipe_instance_get_fd,         /* get_fd */
    default_fd_map_access,        /* map_access */
    default_get_sd,               /* get_sd */
    default_set_sd,               /* set_sd */
    no_lookup_name,               /* lookup_name */
    no_open_file,                 /* open_file */
    fd_close_handle,              /* close_handle */
    pipe_instance_destroy         /* destroy */
};

static const struct fd_ops pipe_instance_fd_ops =
{
    default_fd_get_poll_events,   /* get_poll_events */
    default_poll_event,           /* poll_event */
    no_flush,                     /* flush */
    pipe_instance_get_fd_type,    /* get_fd_type */
    pipe_instance_ioctl,          /* ioctl */
    default_fd_queue_async,       /* queue_async */
    default_fd_reselect_async,    /* reselect_async */
    default_fd_cancel_async,      /* cancel_async */
};

/* pipe end functions */
static void pipe_end_dump( struct object *obj, int verbose );
static int pipe_end_signaled( struct object *obj, struct thread *thread );
static struct fd *pipe_end_get_fd( struct object *obj );
static void pipe_end_destroy( struct object *obj );
static void pipe_end_flush( struct fd *fd, struct event **event );
static enum server_fd_type pipe_end_get_fd_type( struct fd *fd );
static obj_handle_t pipe_end_ioctl( struct fd *fd, ioctl_code_t code, const async_data_t *async,
                                    int blocking, const void *data, data_size_t size );

static const struct object_ops pipe_end_ops =
{
    sizeof(struct pipe_end),      /* size */
    pipe_end_dump,                /* dump */
    no_get_type,                  /* get_type */
    add_queue,                    /* add_queue */
    remove_queue,                 /* remove_queue */
    pipe_end_signaled,            /* signaled */
    no_satisfied,                 /* satisfied */
    no_signal,                    /* signal */
    pipe_end_get_fd,              /* get_fd */
    default_fd_map_access,        /* map_access */
    default_get_sd,               /* get_sd */
    default_set_sd,               /* set_sd */
    no_lookup_name,               /* lookup_name */
    no_open_file,                 /* open_file */
    fd_close_handle,              /* close_handle */
    pipe_end_destroy              /* destroy */
};

static const struct fd_ops pipe_end_fd_ops =
{
    default_fd_get_poll_events,   /* get_poll_events */
    default_poll_event,           /* poll_event */
    pipe_end_flush,               /* flush */
    pipe_end_get_fd_type,         /* get_fd_type */
    pipe_end_ioctl,               /* ioctl */
    default_fd_queue_async,       /* queue_async */
    default_fd_reselect_async,    /* reselect_async */
    default_fd_cancel_async       /* cancel_async */
};

static void named_pipe_device_dump( struct object *obj, int verbose );
static struct object_type *named_pipe_device_get_type( struct object *obj );
static struct fd *named_pipe_device_get_fd( struct object *obj );
static struct object *named_pipe_device_lookup_name( struct object *obj,
    struct unicode_str *name, unsigned int attr );
static struct object *named_pipe_device_open_file( struct object *obj, unsigned int access,
                                                   unsigned int sharing, unsigned int options );
static void named_pipe_device_destroy( struct object *obj );
static enum server_fd_type named_pipe_device_get_fd_type( struct fd *fd );
static obj_handle_t named_pipe_device_ioctl( struct fd *fd, ioctl_code_t code, const async_data_t *async_data,
                                             int blocking, const void *data, data_size_t size );

static const struct object_ops named_pipe_device_ops =
{
    sizeof(struct named_pipe_device), /* size */
    named_pipe_device_dump,           /* dump */
    named_pipe_device_get_type,       /* get_type */
    no_add_queue,                     /* add_queue */
    NULL,                             /* remove_queue */
    NULL,                             /* signaled */
    no_satisfied,                     /* satisfied */
    no_signal,                        /* signal */
    named_pipe_device_get_fd,         /* get_fd */
    no_map_access,                    /* map_access */
    default_get_sd,                   /* get_sd */
    default_set_sd,                   /* set_sd */
    named_pipe_device_lookup_name,    /* lookup_name */
    named_pipe_device_open_file,      /* open_file */
    fd_close_handle,                  /* close_handle */
    named_pipe_device_destroy         /* destroy */
};

static const struct fd_ops named_pipe_device_fd_ops =
{
    default_fd_get_poll_events,       /* get_poll_events */
    default_poll_event,               /* poll_event */
    no_flush,                         /* flush */
    named_pipe_device_get_fd_type,    /* get_fd_type */
    named_pipe_device_ioctl,          /* ioctl */
    default_fd_queue_async,           /* queue_async */
    default_fd_reselect_async,        /* reselect_async */
    default_fd_cancel_async           /* cancel_async */
};

static inline int is_server_end( struct pipe_end *end )
{
    int res = (end->flags & NAMED_PIPE_SERVER_END)? 1 : 0;
    if (end->inst)
    {
        if (res)
            assert( end->inst->server == end );
        else
            assert( end->inst->client == end );
    }
    return res;
}

static void named_pipe_dump( struct object *obj, int verbose )
{
    struct named_pipe *pipe = (struct named_pipe *) obj;
    assert( obj->ops == &named_pipe_ops );
    fprintf( stderr, "Named pipe " );
    dump_object_name( &pipe->obj );
    fprintf( stderr, "\n" );
}

static unsigned int named_pipe_map_access( struct object *obj, unsigned int access )
{
    if (access & GENERIC_READ)    access |= STANDARD_RIGHTS_READ;
    if (access & GENERIC_WRITE)   access |= STANDARD_RIGHTS_WRITE | FILE_CREATE_PIPE_INSTANCE;
    if (access & GENERIC_EXECUTE) access |= STANDARD_RIGHTS_EXECUTE;
    if (access & GENERIC_ALL)     access |= STANDARD_RIGHTS_ALL;
    return access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

static void pipe_instance_dump( struct object *obj, int verbose )
{
    struct pipe_instance *inst = (struct pipe_instance *) obj;
    assert( obj->ops == &pipe_instance_ops );
    fprintf( stderr, "Named pipe instance pipe=%p state=%d\n", inst->pipe, inst->state );
}

static void pipe_end_dump( struct object *obj, int verbose )
{
    struct pipe_end *end = (struct pipe_end *) obj;
    assert( obj->ops == &pipe_end_ops );
    fprintf( stderr, "Named pipe end instance=%p\n", end->inst );
}

static int pipe_end_signaled( struct object *obj, struct thread *thread )
{
    struct pipe_end *end = (struct pipe_end *) obj;

    return end->fd && is_fd_signaled(end->fd);
}

static void named_pipe_destroy( struct object *obj)
{
    struct named_pipe *pipe = (struct named_pipe *) obj;

    assert( list_empty( &pipe->instances ) );
    assert( !pipe->numinstances );
    free_async_queue( pipe->waiters );
}

static struct fd *pipe_end_get_fd( struct object *obj )
{
    struct pipe_end *end = (struct pipe_end *) obj;
    if (end->fd)
        return (struct fd *) grab_object( end->fd );
    else if ((end->flags & NAMED_PIPE_SERVER_END) && end->inst)
        return (struct fd *) grab_object( end->inst->ioctl_fd );
    set_error( STATUS_PIPE_DISCONNECTED );
    return NULL;
}

static void set_instance_state( struct pipe_instance *inst, enum pipe_state state )
{
    inst->state = state;

    switch(state)
    {
    case ps_connected:
        assert( inst->server );
        assert( inst->server->fd );
        assert( inst->client );
        assert( inst->client->fd );
        break;
    case ps_wait_open:
    case ps_idle_server:
        assert( inst->server );
        assert( !inst->server->fd );
        assert( !inst->client );
        set_no_fd_status( inst->ioctl_fd, STATUS_PIPE_LISTENING );
        break;
    case ps_disconnected_client:
        assert( !inst->server );
        assert( inst->client );
        break;
    case ps_disconnected_server:
        assert( inst->server );
        assert( !inst->client );
        set_no_fd_status( inst->ioctl_fd, STATUS_PIPE_DISCONNECTED );
        break;
    case ps_wait_connect:
        assert( inst->server );
        assert( !inst->server->fd );
        assert( !inst->client );
        set_no_fd_status( inst->ioctl_fd, STATUS_PIPE_DISCONNECTED );
        break;
    }
}

static struct fd *pipe_instance_get_fd( struct object *obj )
{
    struct pipe_instance *inst = (struct pipe_instance *) obj;

    return (struct fd *)grab_object( inst->ioctl_fd );
}


static void notify_empty( struct pipe_end *end )
{
    if (!end->flush_poll)
        return;
    assert( end->inst && end->inst->state == ps_connected );
    assert( end->event_empty );
    remove_timeout_user( end->flush_poll );
    end->flush_poll = NULL;
    set_event( end->event_empty );
    release_object( end->event_empty );
    end->event_empty = NULL;
}

static void do_disconnect_end( struct pipe_end *end, int is_shutdown )
{
    assert( end->fd );

    if (!is_shutdown)
        shutdown( get_unix_fd( end->fd ), SHUT_RDWR );
    release_object( end->fd );
    end->fd = NULL;
}

static void do_disconnect( struct pipe_instance *inst )
{
    int is_shutdown = 0;
    /* we may only have 1 end */
    if (inst->server)
    {
        do_disconnect_end( inst->server, 0 );
        is_shutdown = 1;
    }

    if (inst->client)
    {
        do_disconnect_end( inst->client, is_shutdown );
    }
}

static void pipe_instance_destroy( struct object *obj )
{
    struct pipe_instance *inst = (struct pipe_instance *)obj;

    assert( obj->ops == &pipe_instance_ops );

    assert( !inst->server );
    assert( !inst->client );
    assert( inst->pipe->numinstances );
    inst->pipe->numinstances--;

    if (inst->ioctl_fd) release_object( inst->ioctl_fd );
    list_remove( &inst->entry );
    release_object( inst->pipe );
}

static void pipe_end_destroy( struct object *obj )
{
    struct pipe_end *end = (struct pipe_end *)obj;
    struct pipe_instance *inst = end->inst;

    assert( obj->ops == &pipe_end_ops );
    if (end->event_empty)
        notify_empty( end );

    if (inst)
    {
        if (is_server_end( end ))
        {
            if (!inst->client)
                assert( inst->obj.refcount == 1 );
            inst->server = NULL;
        }
        else
        {
           if (!inst->server)
                assert( inst->obj.refcount == 1 );
            inst->client = NULL;
        }

        switch(inst->state)
        {
        case ps_connected:
            if (end->flags & NAMED_PIPE_SERVER_END)
                set_instance_state( inst, ps_disconnected_client );
            else
                set_instance_state( inst, ps_disconnected_server );
            break;
        case ps_disconnected_server:
        case ps_disconnected_client:
        case ps_idle_server:
        case ps_wait_open:
        case ps_wait_connect:
            break;
        }

        release_object( inst );
    }
    if (end->fd) release_object( end->fd );
}

static void named_pipe_device_dump( struct object *obj, int verbose )
{
    assert( obj->ops == &named_pipe_device_ops );
    fprintf( stderr, "Named pipe device\n" );
}

static struct object_type *named_pipe_device_get_type( struct object *obj )
{
    static const WCHAR name[] = {'D','e','v','i','c','e'};
    static const struct unicode_str str = { name, sizeof(name) };
    return get_object_type( &str );
}

static struct fd *named_pipe_device_get_fd( struct object *obj )
{
    struct named_pipe_device *device = (struct named_pipe_device *)obj;
    return (struct fd *)grab_object( device->fd );
}

static struct object *named_pipe_device_lookup_name( struct object *obj, struct unicode_str *name,
                                                     unsigned int attr )
{
    struct named_pipe_device *device = (struct named_pipe_device*)obj;
    struct object *found;

    assert( obj->ops == &named_pipe_device_ops );
    assert( device->pipes );

    if ((found = find_object( device->pipes, name, attr | OBJ_CASE_INSENSITIVE )))
        name->len = 0;

    return found;
}

static struct object *named_pipe_device_open_file( struct object *obj, unsigned int access,
                                                   unsigned int sharing, unsigned int options )
{
    return grab_object( obj );
}

static void named_pipe_device_destroy( struct object *obj )
{
    struct named_pipe_device *device = (struct named_pipe_device*)obj;
    assert( obj->ops == &named_pipe_device_ops );
    if (device->fd) release_object( device->fd );
    free( device->pipes );
}

static enum server_fd_type named_pipe_device_get_fd_type( struct fd *fd )
{
    return FD_TYPE_DEVICE;
}

void create_named_pipe_device( struct directory *root, const struct unicode_str *name )
{
    struct named_pipe_device *dev;

    if ((dev = create_named_object_dir( root, name, 0, &named_pipe_device_ops )) &&
        get_error() != STATUS_OBJECT_NAME_EXISTS)
    {
        dev->pipes = NULL;
        if (!(dev->fd = alloc_pseudo_fd( &named_pipe_device_fd_ops, &dev->obj, 0 )) ||
            !(dev->pipes = create_namespace( 7 )))
        {
            release_object( dev );
            dev = NULL;
        }
    }
    if (dev) make_object_static( &dev->obj );
}

static int pipe_data_remaining( struct pipe_end *end )
{
    struct pollfd pfd;
    int fd;

    assert( end->fd );
    fd = get_unix_fd( end->fd );
    if (fd < 0)
        return 0;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (0 > poll( &pfd, 1, 0 ))
        return 0;

    return pfd.revents&POLLIN;
}

static void check_flushed( void *arg )
{
    struct pipe_end *end = (struct pipe_end*) arg;

    assert( end->event_empty );
    if (pipe_data_remaining( end ))
    {
        end->flush_poll = add_timeout_user( -TICKS_PER_SEC / 10, check_flushed, end );
    }
    else
    {
        notify_empty( end );
        end->flush_poll = NULL;
        set_event( end->event_empty );
        release_object( end->event_empty );
        end->event_empty = NULL;
    }
}

static void pipe_end_flush( struct fd *fd, struct event **event )
{
    struct pipe_end *other_end, *end = get_fd_user( fd );

    if (!end) return;
    assert( end->inst );
    if (end->inst->state != ps_connected) return;

    other_end = is_server_end( end )? end->inst->client : end->inst->server;
    assert( other_end );

    /* FIXME: if multiple threads flush the same pipe,
              maybe should create a list of processes to notify */
    if (other_end->flush_poll) return;

    if (pipe_data_remaining( other_end ))
    {
        /* this kind of sux -
           there's no unix way to be alerted when a pipe becomes empty */
        other_end->event_empty = create_event( NULL, NULL, 0, 0, 0, NULL );
        if (!other_end->event_empty) return;
        other_end->flush_poll = add_timeout_user( -TICKS_PER_SEC / 10, check_flushed, other_end );
        *event = other_end->event_empty;
    }
}

static inline int is_overlapped( unsigned int options )
{
    return !(options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT));
}

static enum server_fd_type pipe_instance_get_fd_type( struct fd *fd )
{
    return FD_TYPE_PIPE;
}

static enum server_fd_type pipe_end_get_fd_type( struct fd *fd )
{
    return FD_TYPE_PIPE;
}

static obj_handle_t alloc_wait_event( struct process *process )
{
    obj_handle_t handle = 0;
    struct event *event = create_event( NULL, NULL, 0, 1, 0, NULL );

    if (event)
    {
        handle = alloc_handle( process, event, EVENT_ALL_ACCESS, 0 );
        release_object( event );
    }
    return handle;
}

static obj_handle_t pipe_instance_ioctl( struct fd *fd, ioctl_code_t code, const async_data_t *async_data,
                                       int blocking, const void *data, data_size_t size )
{
    struct pipe_instance *inst = get_fd_user( fd );
    struct async *async;
    obj_handle_t wait_handle = 0;

    switch(code)
    {
    case FSCTL_PIPE_LISTEN:
        switch(inst->state)
        {
        case ps_idle_server:
        case ps_wait_connect:
        case ps_disconnected_server:
            if (blocking)
            {
                async_data_t new_data = *async_data;
                if (!(wait_handle = alloc_wait_event( current->process ))) break;
                new_data.event = wait_handle;
                if (!(async = fd_queue_async( inst->ioctl_fd, &new_data, ASYNC_TYPE_WAIT )))
                {
                    close_handle( current->process, wait_handle );
                    break;
                }
            }
            else async = fd_queue_async( inst->ioctl_fd, async_data, ASYNC_TYPE_WAIT );

            if (async)
            {
                assert( inst->pipe );
                set_instance_state( inst, ps_wait_open );
                if (inst->pipe->waiters) async_wake_up( inst->pipe->waiters, STATUS_SUCCESS );
                release_object( async );
                set_error( STATUS_PENDING );
                return wait_handle;
            }
            break;
        case ps_connected:
            assert( 0 );
            break;
        case ps_disconnected_client:
            set_error( STATUS_NO_DATA_DETECTED );
            break;
        case ps_wait_open:
            set_error( STATUS_INVALID_HANDLE );
            break;
        }
        return 0;

    case FSCTL_PIPE_DISCONNECT:
        switch(inst->state)
        {
        case ps_connected:
            assert( 0 );
            break;
        case ps_idle_server:
        case ps_wait_open:
            set_error( STATUS_PIPE_LISTENING );
            break;
        case ps_wait_connect:
        case ps_disconnected_server:
        case ps_disconnected_client:
            set_error( STATUS_PIPE_DISCONNECTED );
            break;
        }
        return 0;

    default:
        return default_fd_ioctl( fd, code, async_data, blocking, data, size );
    }
}

static obj_handle_t pipe_end_ioctl( struct fd *fd, ioctl_code_t code, const async_data_t *async_data,
                                    int blocking, const void *data, data_size_t size )
{
    struct pipe_end *end = get_fd_user( fd );

    assert( end->inst );

    switch(code)
    {
    case FSCTL_PIPE_DISCONNECT:
        switch(end->inst->state)
        {
        case ps_connected:
            assert( end->inst->server );
            assert( end->inst->server->fd );
            assert( end->inst->client );
            assert( end->inst->client->fd );

            notify_empty( end->inst->server );
            notify_empty( end->inst->client );

            /* all waiting data lost */
            do_disconnect( end->inst );
            if (is_server_end( end ))
            {
                end->inst->client->inst = NULL;
                end->inst->client = NULL;
                set_instance_state( end->inst, ps_disconnected_server );
                release_object( end->inst );
            }
            else
            {
                /* FIXME: is this allowed?? */
                struct pipe_instance *inst = end->inst;
                end->inst->client = NULL;
                end->inst = NULL;
                set_instance_state( inst, ps_disconnected_server );
                release_object( inst );
            }
            break;
        case ps_disconnected_server:
            assert( !end->inst->client );
            do_disconnect( end->inst );
            break;
        case ps_disconnected_client:
            /* FIXME: is this allowed?? */
            assert( !end->inst->server );
            do_disconnect( end->inst );
            break;
        case ps_idle_server:
        case ps_wait_open:
        case ps_wait_connect:
            assert( 0 );
            break;
        }
        return 0;

    case FSCTL_PIPE_LISTEN:
        set_error( STATUS_PIPE_CONNECTED );
        return 0;

    default:
        return default_fd_ioctl( fd, code, async_data, blocking, data, size );
    }
}

static struct named_pipe *create_named_pipe( struct directory *root, const struct unicode_str *name,
                                             unsigned int attr )
{
    struct object *obj;
    struct named_pipe *pipe = NULL;
    struct unicode_str new_name;

    if (!name || !name->len) return alloc_object( &named_pipe_ops );

    if (!(obj = find_object_dir( root, name, attr, &new_name )))
    {
        set_error( STATUS_OBJECT_NAME_INVALID );
        return NULL;
    }
    if (!new_name.len)
    {
        if (attr & OBJ_OPENIF && obj->ops == &named_pipe_ops)
            set_error( STATUS_OBJECT_NAME_EXISTS );
        else
        {
            release_object( obj );
            obj = NULL;
            if (attr & OBJ_OPENIF)
                set_error( STATUS_OBJECT_TYPE_MISMATCH );
            else
                set_error( STATUS_OBJECT_NAME_COLLISION );
        }
        return (struct named_pipe *)obj;
    }

    if (obj->ops != &named_pipe_device_ops)
        set_error( STATUS_OBJECT_NAME_INVALID );
    else
    {
        struct named_pipe_device *dev = (struct named_pipe_device *)obj;
        if ((pipe = create_object( dev->pipes, &named_pipe_ops, &new_name, NULL )))
            clear_error();
    }

    release_object( obj );
    return pipe;
}

static struct pipe_end *get_pipe_end_obj( struct process *process,
                                obj_handle_t handle, unsigned int access )
{
    struct pipe_end *end;
    end = (struct pipe_end *)get_handle_obj( process, handle, 0, &pipe_end_ops );
    if (end && is_server_end( end ))
    {
        release_object( end );
        end = (struct pipe_end *)get_handle_obj( process, handle, access, &pipe_end_ops );
    }
    return end;
}

static struct pipe_end *create_pipe_end( struct pipe_instance *inst, unsigned int options, unsigned int flags )
{
    struct pipe_end *end;

    end = alloc_object( &pipe_end_ops );
    if (!end)
        return NULL;

    end->fd = NULL;
    end->inst = inst;
    end->options = options;
    end->flush_poll = NULL;
    end->event_empty = NULL;
    end->flags = flags;

    if (flags & NAMED_PIPE_SERVER_END)
        inst->server = end;
    else
        inst->client = end;

    grab_object( inst );

    return end;
}

static struct pipe_instance *create_pipe_instance( struct named_pipe *pipe, unsigned int options, unsigned int flags )
{
    struct pipe_instance *inst;

    inst = alloc_object( &pipe_instance_ops );
    if (!inst)
        return NULL;

    inst->pipe = pipe;
    inst->client = NULL;
    inst->server = create_pipe_end( inst, options, flags | NAMED_PIPE_SERVER_END );
    if (!inst->server)
    {
        release_object( inst );
        return NULL;
    }

    list_add_head( &pipe->instances, &inst->entry );
    grab_object( pipe );
    if (!(inst->ioctl_fd = alloc_pseudo_fd( &pipe_instance_fd_ops, &inst->obj, options )))
    {
        release_object( inst );
        return NULL;
    }
    set_instance_state( inst, ps_idle_server );
    return inst;
}

static struct pipe_instance *find_available_instance( struct named_pipe *pipe )
{
    struct pipe_instance *inst;

    /* look for pipe instances that are listening */
    LIST_FOR_EACH_ENTRY( inst, &pipe->instances, struct pipe_instance, entry )
    {
        if (inst->state == ps_wait_open)
            return (struct pipe_instance *)grab_object( inst );
    }

    /* fall back to pipe instances that are idle */
    LIST_FOR_EACH_ENTRY( inst, &pipe->instances, struct pipe_instance, entry )
    {
        if (inst->state == ps_idle_server)
            return (struct pipe_instance *)grab_object( inst );
    }

    return NULL;
}

static struct object *named_pipe_open_file( struct object *obj, unsigned int access,
                                            unsigned int sharing, unsigned int options )
{
    struct named_pipe *pipe = (struct named_pipe *)obj;
    struct pipe_instance *inst;
    struct pipe_end *client;
    unsigned int pipe_sharing;
    int fds[2];

    if (!(inst = find_available_instance( pipe )))
    {
        set_error( STATUS_PIPE_NOT_AVAILABLE );
        return NULL;
    }

    assert( inst->server );
    assert( !inst->server->fd );
    assert( !inst->client );

    pipe_sharing = inst->pipe->sharing;
    if (((access & GENERIC_READ) && !(pipe_sharing & FILE_SHARE_READ)) ||
        ((access & GENERIC_WRITE) && !(pipe_sharing & FILE_SHARE_WRITE)))
    {
        set_error( STATUS_ACCESS_DENIED );
        release_object( inst );
        return NULL;
    }

    if ((client = create_pipe_end( inst, options, inst->server->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE )))
    {
        if (!socketpair( PF_UNIX, SOCK_STREAM, 0, fds ))
        {
            assert( !inst->server->fd );

            /* for performance reasons, only set nonblocking mode when using
             * overlapped I/O. Otherwise, we will be doing too much busy
             * looping */
            if (is_overlapped( options )) fcntl( fds[1], F_SETFL, O_NONBLOCK );
            if (is_overlapped( inst->server->options )) fcntl( fds[0], F_SETFL, O_NONBLOCK );

            if (pipe->insize)
            {
                setsockopt( fds[0], SOL_SOCKET, SO_RCVBUF, &pipe->insize, sizeof(pipe->insize) );
                setsockopt( fds[1], SOL_SOCKET, SO_RCVBUF, &pipe->insize, sizeof(pipe->insize) );
            }
            if (pipe->outsize)
            {
                setsockopt( fds[0], SOL_SOCKET, SO_SNDBUF, &pipe->outsize, sizeof(pipe->outsize) );
                setsockopt( fds[1], SOL_SOCKET, SO_SNDBUF, &pipe->outsize, sizeof(pipe->outsize) );
            }

            client->fd = create_anonymous_fd( &pipe_end_fd_ops, fds[1], &client->obj, options );
            inst->server->fd = create_anonymous_fd( &pipe_end_fd_ops, fds[0],
                                                    &inst->server->obj, inst->server->options );

            if (client->fd && inst->server->fd)
            {
                allow_fd_caching( client->fd );
                allow_fd_caching( inst->server->fd );
                fd_copy_completion( inst->ioctl_fd, inst->server->fd );
                if (inst->state == ps_wait_open)
                    fd_async_wake_up( inst->ioctl_fd, ASYNC_TYPE_WAIT, STATUS_SUCCESS );
                set_instance_state( inst, ps_connected );
            }
            else
            {
                release_object( client );
                client = NULL;
            }
        }
        else
        {
            file_set_error();
            release_object( client );
            client = NULL;
        }
    }
    release_object( inst );
    return &client->obj;
}

static obj_handle_t named_pipe_device_ioctl( struct fd *fd, ioctl_code_t code, const async_data_t *async_data,
                                             int blocking, const void *data, data_size_t size )
{
    struct named_pipe_device *device = get_fd_user( fd );

    switch(code)
    {
    case FSCTL_PIPE_WAIT:
        {
            const FILE_PIPE_WAIT_FOR_BUFFER *buffer = data;
            obj_handle_t wait_handle = 0;
            struct named_pipe *pipe;
            struct pipe_instance *inst;
            struct unicode_str name;

            if (size < sizeof(*buffer) ||
                size < FIELD_OFFSET(FILE_PIPE_WAIT_FOR_BUFFER, Name[buffer->NameLength/sizeof(WCHAR)]))
            {
                set_error( STATUS_INVALID_PARAMETER );
                return 0;
            }
            name.str = buffer->Name;
            name.len = (buffer->NameLength / sizeof(WCHAR)) * sizeof(WCHAR);
            if (!(pipe = (struct named_pipe *)find_object( device->pipes, &name, OBJ_CASE_INSENSITIVE )))
            {
                set_error( STATUS_PIPE_NOT_AVAILABLE );
                return 0;
            }
            if (!(inst = find_available_instance( pipe )))
            {
                struct async *async;

                if (!pipe->waiters && !(pipe->waiters = create_async_queue( NULL ))) goto done;

                if (blocking)
                {
                    async_data_t new_data = *async_data;
                    if (!(wait_handle = alloc_wait_event( current->process ))) goto done;
                    new_data.event = wait_handle;
                    if (!(async = create_async( current, pipe->waiters, &new_data )))
                    {
                        close_handle( current->process, wait_handle );
                        wait_handle = 0;
                    }
                }
                else async = create_async( current, pipe->waiters, async_data );

                if (async)
                {
                    timeout_t when = buffer->TimeoutSpecified ? buffer->Timeout.QuadPart : pipe->timeout;
                    async_set_timeout( async, when, STATUS_IO_TIMEOUT );
                    release_object( async );
                    set_error( STATUS_PENDING );
                }
            }
            else release_object( inst );

        done:
            release_object( pipe );
            return wait_handle;
        }

    default:
        return default_fd_ioctl( fd, code, async_data, blocking, data, size );
    }
}


DECL_HANDLER(create_named_pipe)
{
    struct named_pipe *pipe;
    struct pipe_instance *inst;
    struct unicode_str name;
    struct directory *root = NULL;

    if (!req->sharing || (req->sharing & ~(FILE_SHARE_READ | FILE_SHARE_WRITE)) ||
        (!(req->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE) && (req->flags & NAMED_PIPE_MESSAGE_STREAM_READ)))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    reply->handle = 0;
    get_req_unicode_str( &name );
    if (req->rootdir && !(root = get_directory_obj( current->process, req->rootdir, 0 )))
        return;

    pipe = create_named_pipe( root, &name, req->attributes | OBJ_OPENIF );

    if (root) release_object( root );
    if (!pipe) return;

    if (get_error() != STATUS_OBJECT_NAME_EXISTS)
    {
        /* initialize it if it didn't already exist */
        pipe->numinstances = 0;
        pipe->waiters = NULL;
        list_init( &pipe->instances );
        pipe->insize = req->insize;
        pipe->outsize = req->outsize;
        pipe->maxinstances = req->maxinstances;
        pipe->timeout = req->timeout;
        pipe->sharing = req->sharing;
    }
    else
    {
        if (pipe->maxinstances <= pipe->numinstances)
        {
            set_error( STATUS_INSTANCE_NOT_AVAILABLE );
            release_object( pipe );
            return;
        }
        if (pipe->sharing != req->sharing)
        {
            set_error( STATUS_ACCESS_DENIED );
            release_object( pipe );
            return;
        }
        clear_error(); /* clear the name collision */
    }

    inst = create_pipe_instance( pipe, req->options, req->flags );
    if (inst)
    {
        reply->handle = alloc_handle( current->process, inst->server, req->access, req->attributes );
        release_object( inst->server );
        inst->pipe->numinstances++;
        release_object( inst );
    }

    release_object( pipe );
}

DECL_HANDLER(get_named_pipe_info)
{
    struct pipe_end *end;

    if ((end = get_pipe_end_obj( current->process, req->handle, FILE_READ_ATTRIBUTES )))
    {
        if (!end->inst)
        {
            set_error( STATUS_PIPE_DISCONNECTED );
            release_object( end );
            return;
        }
        assert( end->inst->pipe );
        reply->flags        = end->flags;
        reply->sharing      = end->inst->pipe->sharing;
        reply->maxinstances = end->inst->pipe->maxinstances;
        reply->instances    = end->inst->pipe->numinstances;
        reply->insize       = end->inst->pipe->insize;
        reply->outsize      = end->inst->pipe->outsize;

        release_object( end );
    }
}

DECL_HANDLER(set_named_pipe_info)
{
    struct pipe_end *end;
    NTSTATUS status = STATUS_SUCCESS;

    if ((end = get_pipe_end_obj( current->process, req->handle, FILE_WRITE_ATTRIBUTES )))
    {
        if (req->flags & NAMED_PIPE_MESSAGE_STREAM_READ)
        {
            if (!(end->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE))
                status = STATUS_INVALID_PARAMETER;
            else
                end->flags |= NAMED_PIPE_MESSAGE_STREAM_READ;
        }
        else
        {
            end->flags &= (~(unsigned int)NAMED_PIPE_MESSAGE_STREAM_READ);
        }

        if (status)
        {
            set_error(status);
        }
        else
        {
            if (req->flags & NAMED_PIPE_NONBLOCKING_MODE)
            {
                end->flags |= NAMED_PIPE_NONBLOCKING_MODE;
            }
            else
            {
                end->flags &= (~(unsigned int)NAMED_PIPE_NONBLOCKING_MODE);
            }
        }

        release_object( end );
    }
}
