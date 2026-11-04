/*
 * driver_audio.c — HDAudio driver implementation for NT-Box.
 *
 * Uses ALSA (libasound2) on host for audio playback/capture.
 * Implements Windows-compatible audio device interface.
 *
 * Lamps: AUD-001..AUD-004
 *   AUD-001 OK — ntb0_audio_init() opens ALSA device
 *   AUD-002 OK — ntb0_audio_play() writes PCM frames
 *   AUD-003 OK — ntb0_audio_capture() reads PCM frames
 *   AUD-004 OK — ntb0_audio_set_volume() controls mixer
 */
#define _GNU_SOURCE
#include "driver_audio.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Internal device structure                                           */
/* ------------------------------------------------------------------ */

struct ntb0_audio_device {
    /* ALSA handles */
    snd_pcm_t *playback_handle;
    snd_pcm_t *capture_handle;

    /* Configuration */
    struct ntb0_audio_config config;
    int config_valid;

    /* Format mapping */
    snd_pcm_format_t alsa_format;
    size_t frame_bytes;  /* bytes per frame (all channels) */

    /* Volume control */
    snd_mixer_t *mixer;
    snd_mixer_elem_t *playback_vol_elem;
    long vol_min, vol_max;

    /* Error tracking */
    char last_error[256];

    /* Device names */
    char playback_name[128];
    char capture_name[128];

    /* State */
    enum ntb0_audio_state state;
};

/* ------------------------------------------------------------------ */
/* Format conversion                                                     */
/* ------------------------------------------------------------------ */

static snd_pcm_format_t ntb0_audio_alsa_format(enum ntb0_audio_format fmt)
{
    switch (fmt) {
    case NTB0_AUDIO_FMT_S16_LE: return SND_PCM_FORMAT_S16_LE;
    case NTB0_AUDIO_FMT_S24_LE: return SND_PCM_FORMAT_S24_LE;
    case NTB0_AUDIO_FMT_S32_LE: return SND_PCM_FORMAT_S32_LE;
    case NTB0_AUDIO_FMT_FLOAT_LE: return SND_PCM_FORMAT_FLOAT_LE;
    default: return SND_PCM_FORMAT_S16_LE;
    }
}

static size_t ntb0_audio_format_bytes(enum ntb0_audio_format fmt)
{
    switch (fmt) {
    case NTB0_AUDIO_FMT_S16_LE: return 2;
    case NTB0_AUDIO_FMT_S24_LE: return 3;
    case NTB0_AUDIO_FMT_S32_LE: return 4;
    case NTB0_AUDIO_FMT_FLOAT_LE: return 4;
    default: return 2;
    }
}

static void ntb0_audio_set_error(ntb0_audio_device_t *dev, const char *fmt, ...)
{
    if (!dev) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dev->last_error, sizeof(dev->last_error), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* ALSA parameter setup                                                  */
/* ------------------------------------------------------------------ */

static int ntb0_audio_setup_params(ntb0_audio_device_t *dev, snd_pcm_t *handle,
                                    int is_playback)
{
    snd_pcm_hw_params_t *params;
    snd_pcm_sw_params_t *swparams;
    int err;
    uint32_t buffer_size, period_size;

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_sw_params_alloca(&swparams);

    /* Hardware parameters */
    if ((err = snd_pcm_hw_params_any(handle, params)) < 0) {
        ntb0_audio_set_error(dev, "hw_params_any: %s", snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_hw_params_set_access(handle, params,
            SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        ntb0_audio_set_error(dev, "set_access: %s", snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_hw_params_set_format(handle, params, dev->alsa_format)) < 0) {
        ntb0_audio_set_error(dev, "set_format: %s", snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_hw_params_set_channels(handle, params, dev->config.channels)) < 0) {
        ntb0_audio_set_error(dev, "set_channels: %s", snd_strerror(err));
        return -1;
    }

    uint32_t rate = dev->config.sample_rate;
    if ((err = snd_pcm_hw_params_set_rate_near(handle, params, &rate, 0)) < 0) {
        ntb0_audio_set_error(dev, "set_rate: %s", snd_strerror(err));
        return -1;
    }
    dev->config.sample_rate = rate;

    period_size = dev->config.buffer_frames;
    if ((err = snd_pcm_hw_params_set_period_size_near(handle, params,
            &period_size, 0)) < 0) {
        ntb0_audio_set_error(dev, "set_period_size: %s", snd_strerror(err));
        return -1;
    }
    dev->config.buffer_frames = period_size;

    buffer_size = period_size * dev->config.periods;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(handle, params,
            &buffer_size)) < 0) {
        ntb0_audio_set_error(dev, "set_buffer_size: %s", snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_hw_params(handle, params)) < 0) {
        ntb0_audio_set_error(dev, "hw_params: %s", snd_strerror(err));
        return -1;
    }

    /* Software parameters */
    if ((err = snd_pcm_sw_params_current(handle, swparams)) < 0) {
        ntb0_audio_set_error(dev, "sw_params_current: %s", snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_sw_params_set_start_threshold(handle, swparams,
            is_playback ? (buffer_size - period_size) : 1)) < 0) {
        ntb0_audio_set_error(dev, "set_start_threshold: %s", snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size)) < 0) {
        ntb0_audio_set_error(dev, "set_avail_min: %s", snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_sw_params(handle, swparams)) < 0) {
        ntb0_audio_set_error(dev, "sw_params: %s", snd_strerror(err));
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Mixer / volume control                                                */
/* ------------------------------------------------------------------ */

static int ntb0_audio_init_mixer(ntb0_audio_device_t *dev)
{
    int err;
    snd_mixer_selem_id_t *sid;
    const char *card = "default";

    if ((err = snd_mixer_open(&dev->mixer, 0)) < 0) {
        return -1;
    }

    if ((err = snd_mixer_attach(dev->mixer, card)) < 0) {
        snd_mixer_close(dev->mixer);
        dev->mixer = NULL;
        return -1;
    }

    if ((err = snd_mixer_selem_register(dev->mixer, NULL, NULL)) < 0) {
        snd_mixer_close(dev->mixer);
        dev->mixer = NULL;
        return -1;
    }

    if ((err = snd_mixer_load(dev->mixer)) < 0) {
        snd_mixer_close(dev->mixer);
        dev->mixer = NULL;
        return -1;
    }

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");

    dev->playback_vol_elem = snd_mixer_find_selem(dev->mixer, sid);
    if (!dev->playback_vol_elem) {
        /* Try "PCM" as fallback */
        snd_mixer_selem_id_set_name(sid, "PCM");
        dev->playback_vol_elem = snd_mixer_find_selem(dev->mixer, sid);
    }

    if (dev->playback_vol_elem) {
        snd_mixer_selem_get_playback_volume_range(dev->playback_vol_elem,
                                                   &dev->vol_min, &dev->vol_max);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                            */
/* ------------------------------------------------------------------ */

ntb0_audio_device_t *ntb0_audio_init(struct ntb0_audio_config *config)
{
    ntb0_audio_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return NULL;
    }

    /* Set defaults */
    if (config) {
        dev->config = *config;
    } else {
        dev->config.format = NTB0_AUDIO_DEFAULT_FORMAT;
        dev->config.sample_rate = NTB0_AUDIO_DEFAULT_SAMPLE_RATE;
        dev->config.channels = NTB0_AUDIO_DEFAULT_CHANNELS;
        dev->config.buffer_frames = NTB0_AUDIO_DEFAULT_BUFFER;
        dev->config.periods = NTB0_AUDIO_DEFAULT_PERIODS;
    }

    dev->alsa_format = ntb0_audio_alsa_format(dev->config.format);
    dev->frame_bytes = ntb0_audio_format_bytes(dev->config.format) * dev->config.channels;
    dev->state = NTB0_AUDIO_STATE_STOP;

    /* Open playback device */
    const char *playback_dev = "default";
    int err = snd_pcm_open(&dev->playback_handle, playback_dev,
                           SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        ntb0_audio_set_error(dev, "snd_pcm_open playback: %s", snd_strerror(err));
        goto fail;
    }
    snprintf(dev->playback_name, sizeof(dev->playback_name), "%s", playback_dev);

    if (ntb0_audio_setup_params(dev, dev->playback_handle, 1) < 0) {
        goto fail;
    }

    /* Open capture device */
    const char *capture_dev = "default";
    err = snd_pcm_open(&dev->capture_handle, capture_dev,
                       SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        /* Capture is optional */
        dev->capture_handle = NULL;
    } else {
        snprintf(dev->capture_name, sizeof(dev->capture_name), "%s", capture_dev);
        if (ntb0_audio_setup_params(dev, dev->capture_handle, 0) < 0) {
            snd_pcm_close(dev->capture_handle);
            dev->capture_handle = NULL;
        }
    }

    /* Prepare playback */
    if ((err = snd_pcm_prepare(dev->playback_handle)) < 0) {
        ntb0_audio_set_error(dev, "prepare playback: %s", snd_strerror(err));
        goto fail;
    }

    /* Initialize mixer */
    ntb0_audio_init_mixer(dev);

    dev->config_valid = 1;

    /* Return actual config */
    if (config) {
        *config = dev->config;
    }

    return dev;

fail:
    ntb0_audio_deinit(dev);
    return NULL;
}

void ntb0_audio_deinit(ntb0_audio_device_t *dev)
{
    if (!dev) return;

    if (dev->playback_handle) {
        snd_pcm_drain(dev->playback_handle);
        snd_pcm_close(dev->playback_handle);
    }
    if (dev->capture_handle) {
        snd_pcm_drop(dev->capture_handle);
        snd_pcm_close(dev->capture_handle);
    }
    if (dev->mixer) {
        snd_mixer_close(dev->mixer);
    }
    free(dev);
}

long ntb0_audio_play(ntb0_audio_device_t *dev, const void *frames, uint32_t frame_count)
{
    if (!dev || !dev->config_valid || !frames || frame_count == 0) {
        return -EINVAL;
    }

    if (dev->state == NTB0_AUDIO_STATE_STOP) {
        int err = snd_pcm_prepare(dev->playback_handle);
        if (err < 0) {
            ntb0_audio_set_error(dev, "prepare: %s", snd_strerror(err));
            return err;
        }
        dev->state = NTB0_AUDIO_STATE_RUN;
    }

    snd_pcm_sframes_t written = snd_pcm_writei(dev->playback_handle, frames, frame_count);
    if (written < 0) {
        if (written == -EPIPE) {
            /* Underrun */
            snd_pcm_prepare(dev->playback_handle);
            written = snd_pcm_writei(dev->playback_handle, frames, frame_count);
        }
        if (written < 0) {
            ntb0_audio_set_error(dev, "writei: %s", snd_strerror((int)written));
            return written;
        }
    }

    return written;
}

long ntb0_audio_capture(ntb0_audio_device_t *dev, void *frames, uint32_t frame_count)
{
    if (!dev || !dev->config_valid || !frames || frame_count == 0) {
        return -EINVAL;
    }

    if (!dev->capture_handle) {
        ntb0_audio_set_error(dev, "capture device not available");
        return -ENODEV;
    }

    if (dev->state == NTB0_AUDIO_STATE_STOP) {
        int err = snd_pcm_prepare(dev->capture_handle);
        if (err < 0) {
            ntb0_audio_set_error(dev, "prepare capture: %s", snd_strerror(err));
            return err;
        }
        dev->state = NTB0_AUDIO_STATE_RUN;
    }

    snd_pcm_sframes_t read = snd_pcm_readi(dev->capture_handle, frames, frame_count);
    if (read < 0) {
        if (read == -EPIPE) {
            /* Overrun */
            snd_pcm_prepare(dev->capture_handle);
            read = snd_pcm_readi(dev->capture_handle, frames, frame_count);
        }
        if (read < 0) {
            ntb0_audio_set_error(dev, "readi: %s", snd_strerror((int)read));
            return read;
        }
    }

    return read;
}

int ntb0_audio_set_volume(ntb0_audio_device_t *dev, float volume, int channel)
{
    if (!dev || !dev->playback_vol_elem) {
        return -1;
    }

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    long range = dev->vol_max - dev->vol_min;
    long vol = dev->vol_min + (long)(volume * range);

    int err;
    if (channel < 0) {
        /* All channels */
        err = snd_mixer_selem_set_playback_volume_all(dev->playback_vol_elem, vol);
    } else {
        err = snd_mixer_selem_set_playback_volume(dev->playback_vol_elem, channel, vol);
    }

    return err < 0 ? -1 : 0;
}

float ntb0_audio_get_volume(ntb0_audio_device_t *dev, int channel)
{
    if (!dev || !dev->playback_vol_elem) {
        return -1.0f;
    }

    long vol;
    int err = (channel < 0) ?
        snd_mixer_selem_get_playback_volume(dev->playback_vol_elem, 0, &vol) :
        snd_mixer_selem_get_playback_volume(dev->playback_vol_elem, channel, &vol);

    if (err < 0) {
        return -1.0f;
    }

    long range = dev->vol_max - dev->vol_min;
    if (range == 0) return 0.0f;
    return (float)(vol - dev->vol_min) / (float)range;
}

int ntb0_audio_pause(ntb0_audio_device_t *dev, int pause)
{
    if (!dev || !dev->playback_handle) {
        return -1;
    }

    int err = snd_pcm_pause(dev->playback_handle, pause);
    if (err < 0) {
        ntb0_audio_set_error(dev, "pause: %s", snd_strerror(err));
        return -1;
    }

    dev->state = pause ? NTB0_AUDIO_STATE_PAUSE : NTB0_AUDIO_STATE_RUN;
    return 0;
}

uint32_t ntb0_audio_avail(ntb0_audio_device_t *dev, int is_playback)
{
    if (!dev) return 0;

    snd_pcm_t *handle = is_playback ? dev->playback_handle : dev->capture_handle;
    if (!handle) return 0;

    snd_pcm_sframes_t avail = snd_pcm_avail_update(handle);
    return avail > 0 ? (uint32_t)avail : 0;
}

uint32_t ntb0_audio_latency(ntb0_audio_device_t *dev, int is_playback)
{
    if (!dev) return 0;

    snd_pcm_t *handle = is_playback ? dev->playback_handle : dev->capture_handle;
    if (!handle) return 0;

    snd_pcm_sframes_t delay;
    int err = snd_pcm_delay(handle, &delay);
    if (err < 0) return 0;
    return delay > 0 ? (uint32_t)delay : 0;
}

int ntb0_audio_drain(ntb0_audio_device_t *dev)
{
    if (!dev || !dev->playback_handle) return -1;

    int err = snd_pcm_drain(dev->playback_handle);
    if (err < 0) {
        ntb0_audio_set_error(dev, "drain: %s", snd_strerror(err));
        return -1;
    }
    dev->state = NTB0_AUDIO_STATE_STOP;
    return 0;
}

int ntb0_audio_flush(ntb0_audio_device_t *dev)
{
    if (!dev || !dev->playback_handle) return -1;

    int err = snd_pcm_drop(dev->playback_handle);
    if (err < 0) {
        ntb0_audio_set_error(dev, "drop: %s", snd_strerror(err));
        return -1;
    }
    dev->state = NTB0_AUDIO_STATE_STOP;
    return 0;
}

const char *ntb0_audio_error(ntb0_audio_device_t *dev)
{
    return dev ? dev->last_error : "invalid device";
}

const char *ntb0_audio_device_name(ntb0_audio_device_t *dev)
{
    return dev ? dev->playback_name : "";
}

/* ------------------------------------------------------------------ */
/* Windows driver IRP interface                                          */
/* ------------------------------------------------------------------ */

long ntb0_audio_device_control(ntb0_audio_device_t *dev,
                               uint32_t ioctl_code,
                               const void *in_buf, uint32_t in_len,
                               void *out_buf, uint32_t out_len,
                               uint32_t *bytes_returned)
{
    if (!dev || !bytes_returned) {
        return -EINVAL;
    }

    *bytes_returned = 0;

    switch (ioctl_code) {
    case NTB0_IOCTL_AUDIO_GET_POSITION: {
        if (out_len < sizeof(uint64_t)) return -EINVAL;
        uint64_t pos = ntb0_audio_latency(dev, 1);
        memcpy(out_buf, &pos, sizeof(pos));
        *bytes_returned = sizeof(pos);
        return 0;
    }
    case NTB0_IOCTL_AUDIO_SET_FORMAT: {
        if (in_len < sizeof(struct ntb0_audio_config)) return -EINVAL;
        struct ntb0_audio_config *cfg = (struct ntb0_audio_config *)in_buf;
        /* Would need to reopen device with new format */
        ntb0_audio_set_error(dev, "format change not supported at runtime");
        return -ENOTSUP;
    }
    case NTB0_IOCTL_AUDIO_GET_FORMAT: {
        if (out_len < sizeof(struct ntb0_audio_config)) return -EINVAL;
        memcpy(out_buf, &dev->config, sizeof(dev->config));
        *bytes_returned = sizeof(dev->config);
        return 0;
    }
    case NTB0_IOCTL_AUDIO_SET_STATE: {
        if (in_len < sizeof(uint32_t)) return -EINVAL;
        uint32_t state = *(const uint32_t *)in_buf;
        if (state == NTB0_AUDIO_STATE_RUN) {
            return ntb0_audio_pause(dev, 0);
        } else if (state == NTB0_AUDIO_STATE_PAUSE) {
            return ntb0_audio_pause(dev, 1);
        } else if (state == NTB0_AUDIO_STATE_STOP) {
            return ntb0_audio_flush(dev);
        }
        return -EINVAL;
    }
    case NTB0_IOCTL_AUDIO_GET_STATE: {
        if (out_len < sizeof(uint32_t)) return -EINVAL;
        *(uint32_t *)out_buf = dev->state;
        *bytes_returned = sizeof(uint32_t);
        return 0;
    }
    case NTB0_IOCTL_AUDIO_VOLUME_SET: {
        if (in_len < sizeof(float)) return -EINVAL;
        float vol = *(const float *)in_buf;
        return ntb0_audio_set_volume(dev, vol, -1);
    }
    case NTB0_IOCTL_AUDIO_VOLUME_GET: {
        if (out_len < sizeof(float)) return -EINVAL;
        float vol = ntb0_audio_get_volume(dev, -1);
        if (vol < 0) return -1;
        *(float *)out_buf = vol;
        *bytes_returned = sizeof(float);
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/* Need va_list for error formatting */
#include <stdarg.h>