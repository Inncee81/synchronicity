/*****************************************************************************
 * vlcpulse.c : PulseAudio support library for LibVLC plugins
 *****************************************************************************
 * Copyright (C) 2008 the VideoLAN team
 * Copyright (C) 2009-2011 Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define MODULE_STRING "vlcpulse"
#include <vlc_common.h>
#include <pulse/pulseaudio.h>

#include <vlc_pulse.h>
#include <assert.h>

#undef vlc_pa_error
void vlc_pa_error (vlc_object_t *obj, const char *msg, pa_context *ctx)
{
    msg_Err (obj, "%s: %s", msg, pa_strerror (pa_context_errno (ctx)));
}

static pa_threaded_mainloop *vlc_pa_mainloop;
static unsigned refs = 0;
static vlc_mutex_t lock = VLC_STATIC_MUTEX;

/**
 * Creates and references the VLC PulseAudio threaded main loop.
 * @return the mainloop or NULL on failure
 */
static pa_threaded_mainloop *vlc_pa_mainloop_init (void)
{
    pa_threaded_mainloop *mainloop;

    vlc_mutex_lock (&lock);
    if (refs == 0)
    {
        mainloop = pa_threaded_mainloop_new ();
        if (unlikely(mainloop == NULL))
            goto out;

        if (pa_threaded_mainloop_start (mainloop) < 0)
        {
            pa_threaded_mainloop_free (mainloop);
            goto out;
        }
        vlc_pa_mainloop = mainloop;
    }
    else
    {
        if (unlikely(refs < UINT_MAX))
        {
            mainloop = NULL;
            goto out;
        }
        mainloop = vlc_pa_mainloop;
    }

    assert (mainloop != NULL);
    refs++;
out:
    vlc_mutex_unlock (&lock);
    return mainloop;
}

/**
 * Releases a reference to the VLC PulseAudio main loop.
 */
static void vlc_pa_mainloop_deinit (pa_threaded_mainloop *mainloop)
{
    vlc_mutex_lock (&lock);
    assert (refs > 0);
    assert (mainloop == vlc_pa_mainloop);

    if (--refs > 0)
        mainloop = NULL;
    vlc_mutex_unlock (&lock);

    if (mainloop != NULL)
    {
        pa_threaded_mainloop_stop (mainloop);
        pa_threaded_mainloop_free (mainloop);
    }
}

/**
 * Acquires the main loop lock.
 */
void vlc_pa_lock (void)
{
    pa_threaded_mainloop_lock (vlc_pa_mainloop);
}

/**
 * Releases the main loop lock.
 */
void vlc_pa_unlock (void)
{
    pa_threaded_mainloop_unlock (vlc_pa_mainloop);
}

/**
 * Signals the main loop.
 */
void vlc_pa_signal (int do_wait)
{
    pa_threaded_mainloop_signal (vlc_pa_mainloop, do_wait);
}

/**
 * Waits for the main loop to be signaled.
 */
void vlc_pa_wait (void)
{
    pa_threaded_mainloop_wait (vlc_pa_mainloop);
}


static void context_state_cb (pa_context *ctx, void *userdata)
{
    pa_threaded_mainloop *mainloop = userdata;

    switch (pa_context_get_state(ctx))
    {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            pa_threaded_mainloop_signal(mainloop, 0);
        default:
            break;
    }
}

static bool context_wait (pa_threaded_mainloop *mainloop, pa_context *ctx)
{
    pa_context_state_t state;

    while ((state = pa_context_get_state (ctx)) != PA_CONTEXT_READY)
    {
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            return -1;
        pa_threaded_mainloop_wait (mainloop);
    }
    return 0;
}

/**
 * Initializes the PulseAudio main loop and connects to the PulseAudio server.
 * @return a PulseAudio context on success, or NULL on error
 */
pa_context *vlc_pa_connect (vlc_object_t *obj)
{
    pa_threaded_mainloop *mainloop = vlc_pa_mainloop_init ();
    if (unlikely(mainloop == NULL))
        return NULL;

    char *ua = var_InheritString (obj, "user-agent");
    pa_context *ctx;

    pa_threaded_mainloop_lock (mainloop);

    ctx = pa_context_new (pa_threaded_mainloop_get_api (mainloop), ua);
    free (ua);
    if (unlikely(ctx == NULL))
        goto fail;

    pa_context_set_state_callback (ctx, context_state_cb, mainloop);
    if (pa_context_connect (ctx, NULL, 0, NULL) < 0
     || context_wait (mainloop, ctx))
    {
        vlc_pa_error (obj, "PulseAudio server connection failure", ctx);
        pa_context_unref (ctx);
        goto fail;
    }

    pa_threaded_mainloop_unlock (mainloop);
    return ctx;

fail:
    pa_threaded_mainloop_unlock (mainloop);
    vlc_pa_mainloop_deinit (mainloop);
    return NULL;
}

/**
 * Closes a connection to PulseAudio.
 */
void vlc_pa_disconnect (vlc_object_t *obj, pa_context *ctx)
{
    pa_threaded_mainloop *mainloop = vlc_pa_mainloop;

    pa_threaded_mainloop_lock (mainloop);
    pa_context_unref (ctx);
    pa_threaded_mainloop_unlock (mainloop);

    vlc_pa_mainloop_deinit (mainloop);
    (void) obj;
}