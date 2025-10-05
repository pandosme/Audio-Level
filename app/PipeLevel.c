#include "PipeLevel.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <math.h>
#include <stdio.h>

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

static unsigned int s_interval_sec = 5;
static PipeLevel_PeakCallback s_callback = NULL;
static void *s_cb_userdata = NULL;
static pthread_t s_pw_thread;
static int s_run = 0;

struct impl {
    struct pw_main_loop* loop;
    struct pw_core* core;
    struct pw_context* context;
    struct pw_registry* registry;
    struct spa_hook registry_listener;
    struct spa_list streams;
    struct spa_source* timer_source;
};

struct stream_data {
    struct spa_list link;
    struct pw_stream* stream;
    struct spa_hook stream_listener;
    uint32_t target_id;
    char target_name[64];
    struct spa_audio_info info;
    float peaks[PIPELEVEL_PEAK_CHANNELS];
};

static struct impl *s_impl = NULL;
static pthread_mutex_t s_level_mutex = PTHREAD_MUTEX_INITIALIZER;
static float s_current_peaks[PIPELEVEL_PEAK_CHANNELS] = {0.0f};
static unsigned int s_current_channels = 0;

// Ambient long-term state
static float s_ambient_dBFS = -96.0f;

static void on_param_changed(void* data, uint32_t id, const struct spa_pod* param) {
    struct stream_data* stream_data = data;
    LOG_TRACE("[PipeLevel] on_param_changed for node %s\n", stream_data->target_name);
    if (param == NULL || id != SPA_PARAM_Format) {
        LOG_TRACE("[PipeLevel] param is NULL or not SPA_PARAM_Format\n");
        return;
    }
    if(spa_format_audio_raw_parse(param, &stream_data->info.info.raw) < 0) {
        LOG_TRACE("[PipeLevel] failed to parse audio format for node %s\n", stream_data->target_name);
        return;
    }
    LOG("[PipeLevel] Node %s: capturing %d ch, %d Hz\n", stream_data->target_name,
        stream_data->info.info.raw.channels, stream_data->info.info.raw.rate);
}

static void on_state_changed(void* data,
                             enum pw_stream_state old,
                             enum pw_stream_state state,
                             const char* error) {
    struct stream_data* stream_data = data;
    LOG_TRACE("[PipeLevel] Stream state for %s: %d -> %d\n", stream_data->target_name, old, state);
    if (state == PW_STREAM_STATE_ERROR) {
        LOG_WARN("[PipeLevel] Stream from %s error: %s\n", stream_data->target_name, error);
    }
}

static void on_process(void* data) {
    struct stream_data* stream_data = data;
    struct pw_buffer* b = pw_stream_dequeue_buffer(stream_data->stream);
    if (b == NULL) {
        LOG_TRACE("[PipeLevel] pw_stream_dequeue_buffer returned NULL\n");
        return;
    }
    struct spa_buffer* buf = b->buffer;

    unsigned int ch = stream_data->info.info.raw.channels;
    if (ch > PIPELEVEL_PEAK_CHANNELS) ch = PIPELEVEL_PEAK_CHANNELS;

    for (unsigned int c = 0; c < ch; c++) {
        float peak = 0.0f;
        const float* samples = buf->datas[c].data;
        if (!samples) {
            LOG_TRACE("[PipeLevel] samples NULL for channel %u\n", c);
            continue;
        }
        uint32_t n_samples = buf->datas[c].chunk->size / sizeof(float);
        for (unsigned int i = 0; i < n_samples; i++) {
            float v = fabsf(samples[i]);
            if (v > peak) peak = v;
        }
        stream_data->peaks[c] = peak;
    }

    // Store latest for timer
    pthread_mutex_lock(&s_level_mutex);
    s_current_channels = ch;
    for (unsigned int c = 0; c < ch; c++) s_current_peaks[c] = stream_data->peaks[c];
    pthread_mutex_unlock(&s_level_mutex);

    pw_stream_queue_buffer(stream_data->stream, b);
    LOG_TRACE("[PipeLevel] on_process completed, peaks: ch0:%.5f ch1:%.5f\n", s_current_peaks[0], s_current_peaks[1]);
}

// Timer every s_interval_sec: report peak and update/report ambient
static void on_timeout(void* data, uint64_t expirations) {
    (void)expirations;
    float peaks[PIPELEVEL_PEAK_CHANNELS] = {0};
    unsigned int n_ch = 0;
    float dBFS = -96.0f;

    pthread_mutex_lock(&s_level_mutex);
    n_ch = s_current_channels;
    for (unsigned int c = 0; c < n_ch; c++)
        peaks[c] = s_current_peaks[c];
    pthread_mutex_unlock(&s_level_mutex);

    // Use only ch0 for ambient (mono or main channel)
    if (peaks[0] > 0.000001f)
        dBFS = 20.0f * log10f(peaks[0]);

    // Fast ambient convergence at startup, then switch to slow/robust
    static int s_ambient_bootstrap_count = 0;
    static const int s_ambient_bootstrap_limit = 6;            // e.g. first 6 intervals
    static const float s_ambient_alpha_bootstrap = 0.4f;       // Fast adaptation (0.3 - 0.5 is typical)
    static const float s_ambient_alpha_normal = 0.01f;         // Slow, robust

    float this_alpha = (s_ambient_bootstrap_count < s_ambient_bootstrap_limit)
                        ? s_ambient_alpha_bootstrap
                        : s_ambient_alpha_normal;

    if (dBFS > -95.0f)
        s_ambient_dBFS = (1.0f - this_alpha) * s_ambient_dBFS + this_alpha * dBFS;

    if (s_ambient_bootstrap_count < s_ambient_bootstrap_limit)
        s_ambient_bootstrap_count++;

    LOG_TRACE("[PipeLevel] on_timeout: peaks: ch0:%.5f ch1:%.5f, ambient: %.2f dBFS (alpha=%.2f)\n", 
            peaks[0], peaks[1], s_ambient_dBFS, this_alpha);

    if (s_callback)
        s_callback(n_ch, peaks, s_ambient_dBFS, s_cb_userdata);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process       = on_process,
    .state_changed = on_state_changed,
};

static void registry_event_global(void* data, uint32_t id, uint32_t perm,
                                 const char* type, uint32_t version, const struct spa_dict* props) {
    struct impl *impl = (struct impl *)data;

    if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
        const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        LOG_TRACE("[PipeLevel] Found node: %s type:%s id:%u\n", name ? name : "(null)", media_class ? media_class : "(null)", id);
        if (!media_class || !strstr(media_class, "Audio/Source")) return;
        if (!name || strcmp(name, "AudioDevice0Input0") != 0) return;

        LOG_TRACE("[PipeLevel] Using node: %s (%s)\n", name, media_class);
        struct pw_properties* stream_props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_TARGET_OBJECT, name,
            NULL
        );
        if (!stream_props) {
            LOG_TRACE("[PipeLevel] Failed to alloc stream_props\n");
            return;
        }

        struct stream_data *stream_data = calloc(1, sizeof(struct stream_data));
        if (!stream_data) {
            LOG_TRACE("[PipeLevel] Failed to calloc stream_data\n");
            return;
        }
        stream_data->target_id = id;
        strncpy(stream_data->target_name, name, sizeof(stream_data->target_name) - 1);

        stream_data->stream = pw_stream_new(impl->core, "AudioLevel", stream_props);
        if (!stream_data->stream) {
            LOG_TRACE("[PipeLevel] pw_stream_new failed for %s\n", name);
            free(stream_data);
            return;
        }

        pw_stream_add_listener(stream_data->stream, &stream_data->stream_listener,
                               &stream_events, stream_data);

        uint8_t buf[256];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const struct spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32));

        int res = pw_stream_connect(stream_data->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                   PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                                   params, 1);
        if (res < 0) {
            LOG_TRACE("[PipeLevel] Stream connect error: %d\n", res);
        }
        spa_list_append(&impl->streams, &stream_data->link);
    }
}

static void *pw_mainloop_thread(void *arg) {
    LOG_TRACE("[PipeLevel] >>> pw_mainloop_thread start\n");

    struct impl *impl = calloc(1, sizeof(struct impl));
    if (!impl) {
        LOG_TRACE("[PipeLevel] Failed to calloc impl\n");
        return NULL;
    }
    s_impl = impl;

    pw_init(NULL, NULL);
    LOG_TRACE("[PipeLevel] pw_init complete\n");

    impl->loop = pw_main_loop_new(NULL);
    if (!impl->loop) {
        LOG_WARN("[PipeLevel] pw_main_loop_new failed\n");
        goto fail;
    }
    LOG_TRACE("[PipeLevel] pw_main_loop_new success\n");

    impl->context = pw_context_new(pw_main_loop_get_loop(impl->loop), NULL, 0);
    if (!impl->context) {
        LOG_WARN("[PipeLevel] pw_context_new failed\n");
        goto fail;
    }
    LOG_TRACE("[PipeLevel] pw_context_new success\n");

    impl->core = pw_context_connect(impl->context, NULL, 0);
    if (!impl->core) {
        LOG_WARN("[PipeLevel] pw_context_connect failed\n");
        goto fail;
    }
    LOG_TRACE("[PipeLevel] pw_context_connect success\n");

    impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
    if (!impl->registry) {
        LOG_WARN("[PipeLevel] pw_core_get_registry failed\n");
        goto fail;
    }
    LOG_TRACE("[PipeLevel] pw_core_get_registry success\n");

    spa_list_init(&impl->streams);

    static struct pw_registry_events registry_events = {
        PW_VERSION_REGISTRY_EVENTS,
        .global = registry_event_global,
    };
    pw_registry_add_listener(impl->registry, &impl->registry_listener, &registry_events, impl);

    impl->timer_source = pw_loop_add_timer(pw_main_loop_get_loop(impl->loop), on_timeout, impl);
    struct timespec ts = { .tv_sec  = s_interval_sec, .tv_nsec = 0 };
    int res = pw_loop_update_timer(pw_main_loop_get_loop(impl->loop), impl->timer_source, NULL, &ts, false);
    if (res < 0) {
        LOG_TRACE("[PipeLevel] pw_loop_update_timer failed: %d\n", res);
    }

    s_run = 1;
    LOG_TRACE("[PipeLevel] pw_main_loop_run starting\n");
    pw_main_loop_run(impl->loop);
    LOG_TRACE("[PipeLevel] pw_main_loop_run returned\n");

    s_run = 0;
    if (impl->timer_source) pw_loop_destroy_source(pw_main_loop_get_loop(impl->loop), impl->timer_source);
    spa_hook_remove(&impl->registry_listener);
    if (impl->registry) pw_proxy_destroy((struct pw_proxy*)impl->registry);
    struct stream_data *stream_data;
    spa_list_consume(stream_data, &impl->streams, link) {
        LOG_TRACE("[PipeLevel] Destroying stream for node %s\n", stream_data->target_name);
        spa_hook_remove(&stream_data->stream_listener);
        pw_stream_destroy(stream_data->stream);
        spa_list_remove(&stream_data->link);
        free(stream_data);
    }
    if (impl->core) pw_core_disconnect(impl->core);
    if (impl->context) pw_context_destroy(impl->context);
    if (impl->loop) pw_main_loop_destroy(impl->loop);

    pw_deinit();
    free(impl);
    s_impl = NULL;
    LOG_TRACE("[PipeLevel] <<< pw_mainloop_thread end\n");
    return NULL;

fail:
    LOG_WARN("[PipeLevel] pw_mainloop_thread failed during setup\n");
    if (impl) free(impl);
    s_impl = NULL;
    return NULL;
}

void PipeLevel_set_interval(unsigned int sec) {
    s_interval_sec = (sec ? sec : 2);

    // If the implementation/mainloop/timer is running, update it on the fly
    if (s_impl && s_impl->loop && s_impl->timer_source) {
        struct timespec ts = { .tv_sec = s_interval_sec, .tv_nsec = 0 };
        // Must only call from within (or safely lock) the mainloop thread!
        pw_loop_update_timer(
            pw_main_loop_get_loop(s_impl->loop),
            s_impl->timer_source,
            NULL, &ts, false
        );
        LOG("Timer interval updated to %u seconds\n", s_interval_sec);
    } else {
       LOG_WARN("Unable to set timer interval updated to %u seconds\n", sec);
	}
}

int PipeLevel_init(PipeLevel_PeakCallback cb, void *userdata) {
    s_callback = cb;
    s_cb_userdata = userdata;
    LOG_TRACE("[PipeLevel] PipeLevel_init\n");
    if (pthread_create(&s_pw_thread, NULL, pw_mainloop_thread, NULL) != 0) {
        LOG_WARN("[PipeLevel] pthread_create failed\n");
        return -1;
    }
    return 0;
}

void PipeLevel_deinit(void) {
    LOG_TRACE("[PipeLevel] PipeLevel_deinit\n");
    if (!s_impl) { LOG_TRACE("[PipeLevel] deinit: s_impl is NULL\n"); return; }
    if (s_run) {
        LOG_TRACE("[PipeLevel] pw_main_loop_quit\n");
        pw_main_loop_quit(s_impl->loop);
        pthread_join(s_pw_thread, NULL);
    }
    s_callback = NULL;
    s_cb_userdata = NULL;
    s_impl = NULL;
}
