/*****************************************************************************
 * pulse.c : PulseAudio input plugin for vlc
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

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_demux.h>
#include <vlc_plugin.h>
#include <pulse/pulseaudio.h>
#include <vlc_pulse.h>

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);

vlc_module_begin ()
    set_shortname (N_("PulseAudio"))
    set_description (N_("PulseAudio input"))
    set_capability ("access_demux", 0)
    set_category (CAT_INPUT)
    set_subcategory (SUBCAT_INPUT_ACCESS)
    add_shortcut ("pulse", "pulseaudio", "pa")
    set_callbacks (Open, Close)
vlc_module_end ()

struct demux_sys_t
{
    pa_stream *stream; /**< PulseAudio playback stream object */
    pa_context *context; /**< PulseAudio connection context */

    es_out_id_t *es;
    bool discontinuity; /**< The next block will not follow the last one */
    unsigned framesize; /**< Byte size of a sample */
    mtime_t caching; /**< Caching value */
};

/* Stream helpers */
static void stream_state_cb(pa_stream *s, void *userdata)
{
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY:
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            vlc_pa_signal(0);
        default:
            break;
    }
    (void) userdata;
}

static void stream_moved_cb(pa_stream *s, void *userdata)
{
    demux_t *demux = userdata;
    uint32_t idx = pa_stream_get_device_index(s);

    msg_Dbg(demux, "connected to source %"PRIu32": %s", idx,
                  pa_stream_get_device_name(s));
}

static void stream_overflow_cb(pa_stream *s, void *userdata)
{
    demux_t *demux = userdata;

    msg_Err(demux, "overflow");
    (void) s;
}

static void stream_started_cb(pa_stream *s, void *userdata)
{
    demux_t *demux = userdata;

    msg_Dbg(demux, "started");
    (void) s;
}

static void stream_suspended_cb(pa_stream *s, void *userdata)
{
    demux_t *demux = userdata;

    msg_Dbg(demux, "suspended");
    (void) s;
}

static void stream_underflow_cb(pa_stream *s, void *userdata)
{
    demux_t *demux = userdata;

    msg_Dbg(demux, "underflow");
    (void) s;
}

static int stream_wait(pa_stream *stream)
{
    pa_stream_state_t state;

    while ((state = pa_stream_get_state(stream)) != PA_STREAM_READY) {
        if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
            return -1;
        vlc_pa_wait();
    }
    return 0;
}

static void stream_read_cb(pa_stream *s, size_t length, void *userdata)
{
    demux_t *demux = userdata;
    demux_sys_t *sys = demux->p_sys;
    const void *ptr;
    unsigned samples = length / sys->framesize;

    if (pa_stream_peek(s, &ptr, &length) < 0) {
        vlc_pa_error(demux, "cannot peek stream", sys->context);
        return;
    }

    mtime_t pts = mdate();
    pa_usec_t latency;
    int negative;

    if (pa_stream_get_latency(s, &latency, &negative) < 0) {
        vlc_pa_error(demux, "cannot determine latency", sys->context);
        return;
    }
    if (negative)
        pts += latency;
    else
        pts -= latency;

    es_out_Control(demux->out, ES_OUT_SET_PCR, pts);
    if (unlikely(sys->es == NULL))
        goto race;

    block_t *block = block_Alloc(length);
    if (likely(block != NULL)) {
        vlc_memcpy(block->p_buffer, ptr, length);
        block->i_nb_samples = samples;
        block->i_dts = block->i_pts = pts;
        if (sys->discontinuity) {
            block->i_flags |= BLOCK_FLAG_DISCONTINUITY;
            sys->discontinuity = false;
        }

        es_out_Send(demux->out, sys->es, block);
    } else
        sys->discontinuity = true;
race:
    pa_stream_drop(s);
}

static int Control(demux_t *demux, int query, va_list ap)
{
    demux_sys_t *sys = demux->p_sys;

    switch (query)
    {
        case DEMUX_GET_TIME:
        {
            pa_usec_t us;

            if (pa_stream_get_time(sys->stream, &us) < 0)
                return VLC_EGENERIC;
            *(va_arg(ap, int64_t *)) = us;
            break;
        }

        //case DEMUX_SET_NEXT_DEMUX_TIME: TODO
        //case DEMUX_GET_META TODO

        case DEMUX_GET_PTS_DELAY:
            *(va_arg(ap, int64_t *)) = sys->caching;
            break;

        case DEMUX_HAS_UNSUPPORTED_META:
        case DEMUX_CAN_RECORD:
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_CONTROL_PACE:
        case DEMUX_CAN_CONTROL_RATE:
        case DEMUX_CAN_SEEK:
            *(va_arg(ap, bool *)) = false;
            break;

        default:
            return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static int Open(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;

    pa_context *ctx = vlc_pa_connect(obj);
    if (ctx == NULL)
        return VLC_EGENERIC;

    demux_sys_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL)) {
        vlc_pa_disconnect (obj, ctx);
        return VLC_ENOMEM;
    }
    sys->stream = NULL;
    sys->context = ctx;
    sys->es = NULL;
    sys->discontinuity = false;
    sys->caching = INT64_C(1000) * var_InheritInteger(obj, "live-caching");
    demux->p_sys = sys;

    /* Stream parameters */
    struct pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16NE;
    ss.rate = 48000;
    ss.channels = 2;
    assert(pa_sample_spec_valid(&ss));

    struct pa_channel_map map;
    map.channels = 2;
    map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
    map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
    assert(pa_channel_map_valid(&map));

    const pa_stream_flags_t flags = PA_STREAM_INTERPOLATE_TIMING
                                  | PA_STREAM_AUTO_TIMING_UPDATE
                                  /*| PA_STREAM_FIX_FORMAT
                                  | PA_STREAM_FIX_RATE
                                  | PA_STREAM_FIX_CHANNELS*/;
    const struct pa_buffer_attr attr = {
        .maxlength = -1,
        .fragsize = pa_usec_to_bytes(sys->caching, &ss) / 2,
    };

    es_format_t fmt;

    /* Create record stream */
    pa_stream *s;

    vlc_pa_lock();
    s = pa_stream_new(ctx, "audio stream", &ss, &map);
    if (s == NULL)
        goto error;

    sys->stream = s;
    pa_stream_set_state_callback(s, stream_state_cb, NULL);
    pa_stream_set_read_callback(s, stream_read_cb, demux);
    pa_stream_set_moved_callback(s, stream_moved_cb, demux);
    pa_stream_set_overflow_callback(s, stream_overflow_cb, demux);
    pa_stream_set_started_callback(s, stream_started_cb, demux);
    pa_stream_set_suspended_callback(s, stream_suspended_cb, demux);
    pa_stream_set_underflow_callback(s, stream_underflow_cb, demux);

    if (pa_stream_connect_record(s, NULL, &attr, flags) < 0
     || stream_wait(s)) {
        vlc_pa_error(obj, "cannot connect record stream", ctx);
        goto error;
    }

    /* The ES should be initialized before stream_read_cb(), but how? */
    es_format_Init(&fmt, AUDIO_ES, VLC_CODEC_S16N);
    fmt.audio.i_physical_channels = fmt.audio.i_original_channels =
        AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT;
    fmt.audio.i_channels = ss.channels;
    fmt.audio.i_rate = ss.rate;
    fmt.audio.i_bitspersample = 16;
    fmt.audio.i_blockalign = 2 * ss.channels;
    fmt.i_bitrate = ss.channels * ss.rate * fmt.audio.i_bitspersample;
    sys->framesize = fmt.audio.i_blockalign;
    sys->es = es_out_Add (demux->out, &fmt);

    const struct pa_buffer_attr *pba = pa_stream_get_buffer_attr(s);
    msg_Dbg(obj, "using buffer metrics: maxlength=%"PRIu32", fragsize=%"PRIu32,
            pba->maxlength, pba->fragsize);
    vlc_pa_unlock();

    demux->pf_demux = NULL;
    demux->pf_control = Control;
    return VLC_SUCCESS;

error:
    vlc_pa_unlock();
    Close(obj);
    return VLC_EGENERIC;
}

static void Close (vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    demux_sys_t *sys = demux->p_sys;
    pa_context *ctx = sys->context;
    pa_stream *s = sys->stream;

    if (likely(s != NULL)) {
        vlc_pa_lock();
        pa_stream_disconnect(s);
        pa_stream_set_state_callback(s, NULL, NULL);
        pa_stream_set_read_callback(s, NULL, NULL);
        pa_stream_set_moved_callback(s, NULL, NULL);
        pa_stream_set_overflow_callback(s, NULL, NULL);
        pa_stream_set_started_callback(s, NULL, NULL);
        pa_stream_set_suspended_callback(s, NULL, NULL);
        pa_stream_set_underflow_callback(s, NULL, NULL);
        pa_stream_unref(s);
        vlc_pa_unlock();
    }

    vlc_pa_disconnect(obj, ctx);
    free(sys);
}