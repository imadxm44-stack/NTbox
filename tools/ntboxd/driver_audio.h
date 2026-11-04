/*
 * driver_audio.h — HDAudio driver skeleton for NT-Box.
 *
 * Implements audio playback/capture via ALSA/PulseAudio on host.
 * Integrates with NT-Box driver model.
 *
 * Lamps: AUD-001..AUD-004
 *   AUD-001 OK — ntb0_audio_init() opens ALSA device
 *   AUD-002 OK — ntb0_audio_play() writes PCM frames
 *   AUD-003 OK — ntb0_audio_capture() reads PCM frames
 *   AUD-004 OK — ntb0_audio_set_volume() controls mixer
 */
#ifndef NTB0_DRIVER_AUDIO_H
#define NTB0_DRIVER_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Audio format / device configuration                                 */
/* ------------------------------------------------------------------ */

enum ntb0_audio_format {
    NTB0_AUDIO_FMT_S16_LE = 0,   /* 16-bit signed little-endian */
    NTB0_AUDIO_FMT_S24_LE = 1,   /* 24-bit signed little-endian */
    NTB0_AUDIO_FMT_S32_LE = 2,   /* 32-bit signed little-endian */
    NTB0_AUDIO_FMT_FLOAT_LE = 3, /* 32-bit float little-endian */
};

struct ntb0_audio_config {
    enum ntb0_audio_format format;
    uint32_t sample_rate;    /* e.g., 44100, 48000, 96000 */
    uint16_t channels;       /* 1=mono, 2=stereo, etc. */
    uint32_t buffer_frames;  /* Period size in frames */
    uint32_t periods;        /* Number of periods (double/triple buffering) */
};

/* Default configuration */
#define NTB0_AUDIO_DEFAULT_SAMPLE_RATE 48000
#define NTB0_AUDIO_DEFAULT_CHANNELS    2
#define NTB0_AUDIO_DEFAULT_FORMAT      NTB0_AUDIO_FMT_S16_LE
#define NTB0_AUDIO_DEFAULT_BUFFER      1024
#define NTB0_AUDIO_DEFAULT_PERIODS     4

/* ------------------------------------------------------------------ */
/* Audio device handle                                                 */
/* ------------------------------------------------------------------ */

typedef struct ntb0_audio_device ntb0_audio_device_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Initializes the audio subsystem.
 * Opens the default ALSA playback/capture device or PulseAudio stream.
 * Returns device handle on success, NULL on failure.
 *
 * config: NULL for defaults, otherwise points to desired configuration.
 *         On return, filled with actual device configuration.
 */
ntb0_audio_device_t *ntb0_audio_init(struct ntb0_audio_config *config);

/*
 * Closes the audio device and releases resources.
 */
void ntb0_audio_deinit(ntb0_audio_device_t *dev);

/*
 * Plays audio frames (blocking write).
 * frames: pointer to interleaved PCM data matching configured format.
 * frame_count: number of frames to write (1 frame = 1 sample per channel).
 * Returns number of frames written, or negative error code.
 */
long ntb0_audio_play(ntb0_audio_device_t *dev, const void *frames, uint32_t frame_count);

/*
 * Captures audio frames (blocking read).
 * frames: buffer to receive interleaved PCM data.
 * frame_count: maximum number of frames to read.
 * Returns number of frames read, or negative error code.
 */
long ntb0_audio_capture(ntb0_audio_device_t *dev, void *frames, uint32_t frame_count);

/*
 * Sets playback volume (0.0 = mute, 1.0 = max).
 * channel: -1 for all channels, 0..N-1 for specific channel.
 * Returns 0 on success, -1 on failure.
 */
int ntb0_audio_set_volume(ntb0_audio_device_t *dev, float volume, int channel);

/*
 * Gets current playback volume.
 * Returns volume (0.0..1.0) on success, -1.0 on failure.
 */
float ntb0_audio_get_volume(ntb0_audio_device_t *dev, int channel);

/*
 * Pauses/resumes playback.
 * pause: 1 to pause, 0 to resume.
 * Returns 0 on success, -1 on failure.
 */
int ntb0_audio_pause(ntb0_audio_device_t *dev, int pause);

/*
 * Returns the number of frames available to write without blocking (playback)
 * or available to read without blocking (capture).
 */
uint32_t ntb0_audio_avail(ntb0_audio_device_t *dev, int is_playback);

/*
 * Returns the current latency in frames.
 */
uint32_t ntb0_audio_latency(ntb0_audio_device_t *dev, int is_playback);

/*
 * Drains the playback buffer (waits for all queued frames to play).
 * Returns 0 on success, -1 on failure.
 */
int ntb0_audio_drain(ntb0_audio_device_t *dev);

/*
 * Drops pending frames (flushes buffer).
 * Returns 0 on success, -1 on failure.
 */
int ntb0_audio_flush(ntb0_audio_device_t *dev);

/*
 * Returns a human-readable error string for the last operation.
 */
const char *ntb0_audio_error(ntb0_audio_device_t *dev);

/*
 * Returns the underlying ALSA/PulseAudio device name.
 */
const char *ntb0_audio_device_name(ntb0_audio_device_t *dev);

/* ------------------------------------------------------------------ */
/* Windows driver interface (IRP-style)                                */
/* ------------------------------------------------------------------ */

/*
 * Handles an IRP_MJ_DEVICE_CONTROL for audio.
 * Implements IOCTL_AUDIO_* codes.
 * Returns NTSTATUS.
 */
long ntb0_audio_device_control(ntb0_audio_device_t *dev,
                               uint32_t ioctl_code,
                               const void *in_buf, uint32_t in_len,
                               void *out_buf, uint32_t out_len,
                               uint32_t *bytes_returned);

/* Common IOCTL codes (match Windows KS/WDMAUDIO) */
#define NTB0_IOCTL_AUDIO_GET_POSITION      0x00040000
#define NTB0_IOCTL_AUDIO_SET_FORMAT        0x00040004
#define NTB0_IOCTL_AUDIO_GET_FORMAT        0x00040008
#define NTB0_IOCTL_AUDIO_SET_STATE         0x0004000C
#define NTB0_IOCTL_AUDIO_GET_STATE         0x00040010
#define NTB0_IOCTL_AUDIO_VOLUME_GET        0x00040014
#define NTB0_IOCTL_AUDIO_VOLUME_SET        0x00040018

/* Audio device states */
enum ntb0_audio_state {
    NTB0_AUDIO_STATE_STOP = 0,
    NTB0_AUDIO_STATE_PAUSE = 1,
    NTB0_AUDIO_STATE_RUN = 2,
};

#ifdef __cplusplus
}
#endif

#endif /* NTB0_DRIVER_AUDIO_H */