/*
 * Copyright (c) 2014, 2016-2017 The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_utils"
/* #define LOG_NDEBUG 0 */

#include <errno.h>
#include <cutils/properties.h>
#include <cutils/config_utils.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <cutils/str_parms.h>
#include <cutils/log.h>
#include <cutils/misc.h>

#include "audio_hw.h"
#include "platform.h"
#include "platform_api.h"
#include "audio_extn.h"
#include "sound/compress_params.h"


#define AUDIO_OUTPUT_POLICY_VENDOR_CONFIG_FILE "/vendor/etc/audio_output_policy.conf"
#define AUDIO_IO_POLICY_VENDOR_CONFIG_FILE "/vendor/etc/audio_io_policy.conf"

#define OUTPUTS_TAG "outputs"
#define INPUTS_TAG "inputs"

#define DYNAMIC_VALUE_TAG "dynamic"
#define FLAGS_TAG "flags"
#define PROFILES_TAG "profile"
#define FORMATS_TAG "formats"
#define SAMPLING_RATES_TAG "sampling_rates"
#define BIT_WIDTH_TAG "bit_width"
#define APP_TYPE_TAG "app_type"

#define STRING_TO_ENUM(string) { #string, string }
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct string_to_enum {
    const char *name;
    uint32_t value;
};

const struct string_to_enum s_flag_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_DIRECT),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_PRIMARY),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_FAST),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_DEEP_BUFFER),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_NON_BLOCKING),
#ifdef INCALL_MUSIC_ENABLED
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_INCALL_MUSIC),
#endif
#ifdef COMPRESS_VOIP_ENABLED
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_VOIP_RX),
#endif
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_NONE),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_FAST),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_HW_HOTWORD),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_RAW),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_SYNC),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_TIMESTAMP),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_COMPRESS),
};

const struct string_to_enum s_format_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_16_BIT),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_8_BIT),
    STRING_TO_ENUM(AUDIO_FORMAT_MP3),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC),
    STRING_TO_ENUM(AUDIO_FORMAT_VORBIS),
    STRING_TO_ENUM(AUDIO_FORMAT_AMR_NB),
    STRING_TO_ENUM(AUDIO_FORMAT_AMR_WB),
    STRING_TO_ENUM(AUDIO_FORMAT_AC3),
    STRING_TO_ENUM(AUDIO_FORMAT_E_AC3),
#ifdef FORMATS_ENABLED
    STRING_TO_ENUM(AUDIO_FORMAT_DTS),
    STRING_TO_ENUM(AUDIO_FORMAT_DTS_LBR),
    STRING_TO_ENUM(AUDIO_FORMAT_WMA),
    STRING_TO_ENUM(AUDIO_FORMAT_WMA_PRO),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADIF),
    STRING_TO_ENUM(AUDIO_FORMAT_AMR_WB_PLUS),
    STRING_TO_ENUM(AUDIO_FORMAT_EVRC),
    STRING_TO_ENUM(AUDIO_FORMAT_EVRCB),
    STRING_TO_ENUM(AUDIO_FORMAT_EVRCWB),
    STRING_TO_ENUM(AUDIO_FORMAT_QCELP),
    STRING_TO_ENUM(AUDIO_FORMAT_MP2),
    STRING_TO_ENUM(AUDIO_FORMAT_EVRCNW),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_16_BIT_OFFLOAD),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_24_BIT_OFFLOAD),
    STRING_TO_ENUM(AUDIO_FORMAT_FLAC),
    STRING_TO_ENUM(AUDIO_FORMAT_ALAC),
    STRING_TO_ENUM(AUDIO_FORMAT_APE),
#endif
};

static uint32_t string_to_enum(const struct string_to_enum *table, size_t size,
                               const char *name)
{
    size_t i;
    for (i = 0; i < size; i++) {
        if (strcmp(table[i].name, name) == 0) {
            ALOGV("%s found %s", __func__, table[i].name);
            return table[i].value;
        }
    }
    return 0;
}

static audio_io_flags_t parse_flag_names(char *name)
{
    uint32_t flag = 0;
    audio_io_flags_t io_flags;
    char *last_r;
    char *flag_name = strtok_r(name, "|", &last_r);

    while (flag_name != NULL) {
        if (strlen(flag_name) != 0) {
            flag |= string_to_enum(s_flag_name_to_enum_table,
                               ARRAY_SIZE(s_flag_name_to_enum_table),
                               flag_name);
        }
        flag_name = strtok_r(NULL, "|", &last_r);
    }

    ALOGV("parse_flag_names: flag - %d", flag);
    io_flags.in_flags = (audio_input_flags_t)flag;
    io_flags.out_flags = (audio_output_flags_t)flag;
    return io_flags;
}

static void parse_format_names(char *name, struct streams_io_cfg *s_info)
{
    struct stream_format *sf_info = NULL;
    char *last_r;
    char *str = strtok_r(name, "|", &last_r);


    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG) == 0)
        return;

    list_init(&s_info->format_list);
    while (str != NULL) {
        audio_format_t format = (audio_format_t)string_to_enum(s_format_name_to_enum_table,
                                              ARRAY_SIZE(s_format_name_to_enum_table), str);
        ALOGV("%s: format - %d", __func__, format);
        if (format != 0) {
            sf_info = (struct stream_format *)calloc(1, sizeof(struct stream_format));
            if (sf_info == NULL)
                break; /* return whatever was parsed */

            sf_info->format = format;
            list_add_tail(&s_info->format_list, &sf_info->list);
        }
        str = strtok_r(NULL, "|", &last_r);
    }
}

static void parse_sample_rate_names(char *name, struct streams_io_cfg *s_info)
{
    struct stream_sample_rate *ss_info = NULL;
    uint32_t sample_rate = 48000;
    char *last_r;
    char *str = strtok_r(name, "|", &last_r);


    if (str != NULL && 0 == strcmp(str, DYNAMIC_VALUE_TAG))
        return;

    list_init(&s_info->sample_rate_list);
    while (str != NULL) {
        sample_rate = (uint32_t)strtol(str, (char **)NULL, 10);
        ALOGV("%s: sample_rate - %d", __func__, sample_rate);
        if (0 != sample_rate) {
            ss_info = (struct stream_sample_rate *)calloc(1, sizeof(struct stream_sample_rate));
            if (ss_info == NULL)
                break; /* return whatever was parsed */

            ss_info->sample_rate = sample_rate;
            list_add_tail(&s_info->sample_rate_list, &ss_info->list);
        }
        str = strtok_r(NULL, "|", &last_r);
    }
}

static int parse_bit_width_names(char *name)
{
    int bit_width = 16;
    char *last_r;
    char *str = strtok_r(name, "|", &last_r);


    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG))
        bit_width = (int)strtol(str, (char **)NULL, 10);

    ALOGV("%s: bit_width - %d", __func__, bit_width);
    return bit_width;
}

static int parse_app_type_names(void *platform, char *name)
{
    int app_type = platform_get_default_app_type(platform);
    char *last_r;
    char *str = strtok_r(name, "|", &last_r);


    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG))
        app_type = (int)strtol(str, (char **)NULL, 10);

    ALOGV("%s: app_type - %d", __func__, app_type);
    return app_type;
}

static void update_streams_cfg_list(cnode *root, void *platform,
                                    struct listnode *streams_cfg_list)
{
    cnode *node = root->first_child;
    struct streams_io_cfg *s_info;

    ALOGV("%s", __func__);
    s_info = (struct streams_io_cfg *)calloc(1, sizeof(struct streams_io_cfg));

    if (!s_info) {
        ALOGE("failed to allocate mem for s_info list element");
        return;
    }

    while (node) {
        if (strcmp(node->name, FLAGS_TAG) == 0) {
            s_info->flags = parse_flag_names((char *)node->value);
        } else if (strcmp(node->name, PROFILES_TAG) == 0) {
            strlcpy(s_info->profile, (char *)node->value, sizeof(s_info->profile));
        } else if (strcmp(node->name, FORMATS_TAG) == 0) {
            parse_format_names((char *)node->value, s_info);
        } else if (strcmp(node->name, SAMPLING_RATES_TAG) == 0) {
            s_info->app_type_cfg.sample_rate = CODEC_BACKEND_DEFAULT_SAMPLE_RATE;
            parse_sample_rate_names((char *)node->value, s_info);
        } else if (strcmp(node->name, BIT_WIDTH_TAG) == 0) {
            s_info->app_type_cfg.bit_width = parse_bit_width_names((char *)node->value);
        } else if (strcmp(node->name, APP_TYPE_TAG) == 0) {
            s_info->app_type_cfg.app_type = parse_app_type_names(platform, (char *)node->value);
        }
        node = node->next;
    }
    list_add_tail(streams_cfg_list, &s_info->list);
}

static void load_cfg_list(cnode *root, void *platform,
                          struct listnode *streams_output_cfg_list,
                          struct listnode *streams_input_cfg_list)
{
    cnode *node = NULL;

    node = config_find(root, OUTPUTS_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("%s: loading output %s", __func__, node->name);
            update_streams_cfg_list(node, platform, streams_output_cfg_list);
            node = node->next;
        }
    } else {
        ALOGI("%s: could not load output, node is NULL", __func__);
    }

    node = config_find(root, INPUTS_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("%s: loading input %s", __func__, node->name);
            update_streams_cfg_list(node, platform, streams_input_cfg_list);
            node = node->next;
        }
    } else {
        ALOGI("%s: could not load input, node is NULL", __func__);
    }
}

static void send_app_type_cfg(void *platform, struct mixer *mixer,
                              struct listnode *streams_output_cfg_list,
                              struct listnode *streams_input_cfg_list)
{
    size_t app_type_cfg[MAX_LENGTH_MIXER_CONTROL_IN_INT] = {0};
    int length = 0, i, num_app_types = 0;
    struct listnode *node;
    bool update;
    struct mixer_ctl *ctl = NULL;
    const char *mixer_ctl_name = "App Type Config";
    struct streams_io_cfg *s_info = NULL;

    if (!mixer) {
        ALOGE("%s: mixer is null",__func__);
        return;
    }
    ctl = mixer_get_ctl_by_name(mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",__func__, mixer_ctl_name);
        return;
    }
    app_type_cfg[length++] = num_app_types;

    if (list_empty(streams_output_cfg_list)) {
        app_type_cfg[length++] = platform_get_default_app_type_v2(platform, PCM_PLAYBACK);
        app_type_cfg[length++] = 48000;
        app_type_cfg[length++] = 16;
        num_app_types += 1;
    }
    if (list_empty(streams_input_cfg_list)) {
        app_type_cfg[length++] = platform_get_default_app_type_v2(platform, PCM_CAPTURE);
        app_type_cfg[length++] = 48000;
        app_type_cfg[length++] = 16;
        num_app_types += 1;
    }

    list_for_each(node, streams_output_cfg_list) {
        s_info = node_to_item(node, struct streams_io_cfg, list);
        update = true;
        for (i=0; i<length; i=i+3) {
            if (app_type_cfg[i+1] == 0)
                break;
            else if (app_type_cfg[i+1] == (size_t)s_info->app_type_cfg.app_type) {
                if (app_type_cfg[i+2] < (size_t)s_info->app_type_cfg.sample_rate)
                    app_type_cfg[i+2] = s_info->app_type_cfg.sample_rate;
                if (app_type_cfg[i+3] < s_info->app_type_cfg.bit_width)
                    app_type_cfg[i+3] = s_info->app_type_cfg.bit_width;
                update = false;
                break;
            }
        }
        if (update && ((length + 3) <= MAX_LENGTH_MIXER_CONTROL_IN_INT)) {
            num_app_types += 1;
            app_type_cfg[length++] = s_info->app_type_cfg.app_type;
            app_type_cfg[length++] = s_info->app_type_cfg.sample_rate;
            app_type_cfg[length++] = s_info->app_type_cfg.bit_width;
        }
    }
    list_for_each(node, streams_input_cfg_list) {
        s_info = node_to_item(node, struct streams_io_cfg, list);
        update = true;
        for (i=0; i<length; i=i+3) {
            if (app_type_cfg[i+1] == 0)
                break;
            else if (app_type_cfg[i+1] == (size_t)s_info->app_type_cfg.app_type) {
                if (app_type_cfg[i+2] < (size_t)s_info->app_type_cfg.sample_rate)
                    app_type_cfg[i+2] = s_info->app_type_cfg.sample_rate;
                if (app_type_cfg[i+3] < s_info->app_type_cfg.bit_width)
                    app_type_cfg[i+3] = s_info->app_type_cfg.bit_width;
                update = false;
                break;
            }
        }
        if (update && ((length + 3) <= MAX_LENGTH_MIXER_CONTROL_IN_INT)) {
            num_app_types += 1;
            app_type_cfg[length++] = s_info->app_type_cfg.app_type;
            app_type_cfg[length++] = s_info->app_type_cfg.sample_rate;
            app_type_cfg[length++] = s_info->app_type_cfg.bit_width;
        }
    }
    ALOGV("%s: num_app_types: %d", __func__, num_app_types);
    if (num_app_types) {
        app_type_cfg[0] = num_app_types;
        mixer_ctl_set_array(ctl, app_type_cfg, length);
    }
}

void audio_extn_utils_update_streams_cfg_lists(void *platform,
                                    struct mixer *mixer,
                                    struct listnode *streams_output_cfg_list,
                                    struct listnode *streams_input_cfg_list)
{
    cnode *root;
    char *data = NULL;

    ALOGV("%s", __func__);
    list_init(streams_output_cfg_list);
    list_init(streams_input_cfg_list);

    root = config_node("", "");
    if (root == NULL) {
        ALOGE("cfg_list, NULL config root");
        return;
    }

    data = (char *)load_file(AUDIO_IO_POLICY_VENDOR_CONFIG_FILE, NULL);
    if (data == NULL) {
        ALOGD("%s: failed to open io config file(%s), trying older config file",
              __func__, AUDIO_IO_POLICY_VENDOR_CONFIG_FILE);
        data = (char *)load_file(AUDIO_OUTPUT_POLICY_VENDOR_CONFIG_FILE, NULL);
        if (data == NULL) {
            send_app_type_cfg(platform, mixer,
                              streams_output_cfg_list,
                              streams_input_cfg_list);
            ALOGE("%s: could not load io policy config!", __func__);
            return;
        }
    }

    config_load(root, data);
    load_cfg_list(root, platform, streams_output_cfg_list,
                                  streams_input_cfg_list);

    send_app_type_cfg(platform, mixer, streams_output_cfg_list,
                                       streams_input_cfg_list);

    config_free(root);
    free(data);
}

static void audio_extn_utils_dump_streams_cfg_list(
                                    struct listnode *streams_cfg_list)
{
    struct listnode *node_i, *node_j;
    struct streams_io_cfg *s_info;
    struct stream_format *sf_info;
    struct stream_sample_rate *ss_info;

    list_for_each(node_i, streams_cfg_list) {
        s_info = node_to_item(node_i, struct streams_io_cfg, list);
        ALOGV("%s: flags-%d, sample_rate-%d, bit_width-%d, app_type-%d",
               __func__, s_info->flags.out_flags, s_info->app_type_cfg.sample_rate,
               s_info->app_type_cfg.bit_width, s_info->app_type_cfg.app_type);
        list_for_each(node_j, &s_info->format_list) {
            sf_info = node_to_item(node_j, struct stream_format, list);
            ALOGV("format-%x", sf_info->format);
        }
        list_for_each(node_j, &s_info->sample_rate_list) {
            ss_info = node_to_item(node_j, struct stream_sample_rate, list);
            ALOGV("sample rate-%d", ss_info->sample_rate);
        }
    }
}

void audio_extn_utils_dump_streams_cfg_lists(
                                    struct listnode *streams_output_cfg_list,
                                    struct listnode *streams_input_cfg_list)
{
    ALOGV("%s", __func__);
    audio_extn_utils_dump_streams_cfg_list(streams_output_cfg_list);
    audio_extn_utils_dump_streams_cfg_list(streams_input_cfg_list);
}

static void audio_extn_utils_release_streams_cfg_list(
                                    struct listnode *streams_cfg_list)
{
    struct listnode *node_i, *node_j;
    struct streams_io_cfg *s_info;

    ALOGV("%s", __func__);

    while (!list_empty(streams_cfg_list)) {
        node_i = list_head(streams_cfg_list);
        s_info = node_to_item(node_i, struct streams_io_cfg, list);
        while (!list_empty(&s_info->format_list)) {
            node_j = list_head(&s_info->format_list);
            list_remove(node_j);
            free(node_to_item(node_j, struct stream_format, list));
        }
        while (!list_empty(&s_info->sample_rate_list)) {
            node_j = list_head(&s_info->sample_rate_list);
            list_remove(node_j);
            free(node_to_item(node_j, struct stream_sample_rate, list));
        }
        list_remove(node_i);
        free(node_to_item(node_i, struct streams_io_cfg, list));
    }
}

void audio_extn_utils_release_streams_cfg_lists(
                                    struct listnode *streams_output_cfg_list,
                                    struct listnode *streams_input_cfg_list)
{
    ALOGV("%s", __func__);
    audio_extn_utils_release_streams_cfg_list(streams_output_cfg_list);
    audio_extn_utils_release_streams_cfg_list(streams_input_cfg_list);
}

static bool set_app_type_cfg(struct streams_io_cfg *s_info,
                    struct stream_app_type_cfg *app_type_cfg,
                    uint32_t sample_rate, uint32_t bit_width)
 {
    struct listnode *node_i;
    struct stream_sample_rate *ss_info;
    list_for_each(node_i, &s_info->sample_rate_list) {
        ss_info = node_to_item(node_i, struct stream_sample_rate, list);
        if ((sample_rate <= ss_info->sample_rate) &&
            (bit_width == s_info->app_type_cfg.bit_width)) {

            app_type_cfg->app_type = s_info->app_type_cfg.app_type;
            app_type_cfg->sample_rate = ss_info->sample_rate;
            app_type_cfg->bit_width = s_info->app_type_cfg.bit_width;
            ALOGV("%s app_type_cfg->app_type %d, app_type_cfg->sample_rate %d, app_type_cfg->bit_width %d",
                   __func__, app_type_cfg->app_type, app_type_cfg->sample_rate, app_type_cfg->bit_width);
            return true;
        }
    }
    /*
     * Reiterate through the list assuming dafault sample rate.
     * Handles scenario where input sample rate is higher
     * than all sample rates in list for the input bit width.
     */
    sample_rate = CODEC_BACKEND_DEFAULT_SAMPLE_RATE;

    list_for_each(node_i, &s_info->sample_rate_list) {
        ss_info = node_to_item(node_i, struct stream_sample_rate, list);
        if ((sample_rate <= ss_info->sample_rate) &&
            (bit_width == s_info->app_type_cfg.bit_width)) {
            app_type_cfg->app_type = s_info->app_type_cfg.app_type;
            app_type_cfg->sample_rate = sample_rate;
            app_type_cfg->bit_width = s_info->app_type_cfg.bit_width;
            ALOGV("%s Assuming sample rate. app_type_cfg->app_type %d, app_type_cfg->sample_rate %d, app_type_cfg->bit_width %d",
                   __func__, app_type_cfg->app_type, app_type_cfg->sample_rate, app_type_cfg->bit_width);
            return true;
        }
    }
    return false;
}

void audio_extn_utils_update_stream_input_app_type_cfg(void *platform,
                                  struct listnode *streams_input_cfg_list,
                                  audio_devices_t devices __unused,
                                  audio_input_flags_t flags,
                                  audio_format_t format,
                                  uint32_t sample_rate,
                                  uint32_t bit_width,
                                  char* profile,
                                  struct stream_app_type_cfg *app_type_cfg)
{
    struct listnode *node_i, *node_j;
    struct streams_io_cfg *s_info;
    struct stream_format *sf_info;

    ALOGV("%s: flags: 0x%x, format: 0x%x sample_rate %d, profile %s",
           __func__, flags, format, sample_rate, profile);

    list_for_each(node_i, streams_input_cfg_list) {
        s_info = node_to_item(node_i, struct streams_io_cfg, list);
        /* Along with flags do profile matching if set at either end.*/
        if (s_info->flags.in_flags == flags &&
            ((profile[0] == '\0' && s_info->profile[0] == '\0') ||
             strncmp(s_info->profile, profile, sizeof(s_info->profile)) == 0)) {
            list_for_each(node_j, &s_info->format_list) {
                sf_info = node_to_item(node_j, struct stream_format, list);
                if (sf_info->format == format) {
                    if (set_app_type_cfg(s_info, app_type_cfg, sample_rate, bit_width))
                        return;
                }
            }
        }
    }
    ALOGW("%s: App type could not be selected. Falling back to default", __func__);
    app_type_cfg->app_type = platform_get_default_app_type_v2(platform, PCM_CAPTURE);
    app_type_cfg->sample_rate = CODEC_BACKEND_DEFAULT_SAMPLE_RATE;
    app_type_cfg->bit_width = 16;
}

void audio_extn_utils_update_stream_output_app_type_cfg(void *platform,
                                  struct listnode *streams_output_cfg_list,
                                  audio_devices_t devices,
                                  audio_output_flags_t flags,
                                  audio_format_t format,
                                  uint32_t sample_rate,
                                  uint32_t bit_width,
                                  audio_channel_mask_t channel_mask __unused,
                                  char *profile,
                                  struct stream_app_type_cfg *app_type_cfg)
{
    struct listnode *node_i, *node_j;
    struct streams_io_cfg *s_info;
    struct stream_format *sf_info;

    if ((24 == bit_width) &&
        (devices & AUDIO_DEVICE_OUT_SPEAKER)) {
        int32_t bw = platform_get_snd_device_bit_width(SND_DEVICE_OUT_SPEAKER);
        if (-ENOSYS != bw)
            bit_width = (uint32_t)bw;
        sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
        ALOGI("%s Allowing 24-bit playback on speaker ONLY at default sampling rate", __func__);
    }

    ALOGV("%s: flags: %x, format: %x sample_rate %d",
           __func__, flags, format, sample_rate);
    list_for_each(node_i, streams_output_cfg_list) {
        s_info = node_to_item(node_i, struct streams_io_cfg, list);
        /* Along with flags do profile matching if set at either end.*/
        if (s_info->flags.out_flags == flags &&
            ((profile[0] == '\0' && s_info->profile[0] == '\0') ||
             strncmp(s_info->profile, profile, sizeof(s_info->profile)) == 0)) {
            list_for_each(node_j, &s_info->format_list) {
                sf_info = node_to_item(node_j, struct stream_format, list);
                if (sf_info->format == format) {
                    if (set_app_type_cfg(s_info, app_type_cfg, sample_rate, bit_width))
                        return;
                }
            }
        }
    }
    list_for_each(node_i, streams_output_cfg_list) {
        s_info = node_to_item(node_i, struct streams_io_cfg, list);
        if (s_info->flags.out_flags == AUDIO_OUTPUT_FLAG_PRIMARY) {
            ALOGV("Compatible output profile not found.");
            app_type_cfg->app_type = s_info->app_type_cfg.app_type;
            app_type_cfg->sample_rate = s_info->app_type_cfg.sample_rate;
            app_type_cfg->bit_width = s_info->app_type_cfg.bit_width;
            ALOGV("%s Default to primary output: App type: %d sample_rate %d",
                  __func__, s_info->app_type_cfg.app_type, app_type_cfg->sample_rate);
            return;
        }
    }
    ALOGW("%s: App type could not be selected. Falling back to default", __func__);
    app_type_cfg->app_type = platform_get_default_app_type(platform);
    app_type_cfg->sample_rate = CODEC_BACKEND_DEFAULT_SAMPLE_RATE;
    app_type_cfg->bit_width = 16;
}

void audio_extn_utils_update_stream_app_type_cfg_for_usecase(
                                    struct audio_device *adev,
                                    struct audio_usecase *usecase)
{
    ALOGV("%s", __func__);

    switch(usecase->type) {
    case PCM_PLAYBACK:
        audio_extn_utils_update_stream_output_app_type_cfg(adev->platform,
                                                &adev->streams_output_cfg_list,
                                                usecase->stream.out->devices,
                                                usecase->stream.out->flags,
                                                usecase->stream.out->format,
                                                usecase->stream.out->sample_rate,
                                                usecase->stream.out->bit_width,
                                                usecase->stream.out->channel_mask,
                                                usecase->stream.out->profile,
                                                &usecase->stream.out->app_type_cfg);
        ALOGV("%s Selected apptype: %d", __func__, usecase->stream.out->app_type_cfg.app_type);
        break;
    case PCM_CAPTURE:
        audio_extn_utils_update_stream_input_app_type_cfg(adev->platform,
                                                &adev->streams_input_cfg_list,
                                                usecase->stream.in->device,
                                                usecase->stream.in->flags,
                                                usecase->stream.in->format,
                                                usecase->stream.in->sample_rate,
                                                usecase->stream.in->bit_width,
                                                usecase->stream.in->profile,
                                                &usecase->stream.in->app_type_cfg);
        ALOGV("%s Selected apptype: %d", __func__, usecase->stream.in->app_type_cfg.app_type);
        break;
    default:
        ALOGE("%s: app type cfg not supported for usecase type (%d)",
            __func__, usecase->type);
    }
}

int audio_extn_utils_send_app_type_cfg(struct audio_device *adev,
                                       struct audio_usecase *usecase)
{
    char mixer_ctl_name[MAX_LENGTH_MIXER_CONTROL_IN_INT];
    size_t app_type_cfg[MAX_LENGTH_MIXER_CONTROL_IN_INT] = {0};
    int len = 0, rc;
    struct mixer_ctl *ctl;
    int pcm_device_id, acdb_dev_id = 0, snd_device = usecase->out_snd_device;
    int32_t sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;

    ALOGV("%s", __func__);

    if ((usecase->id != USECASE_AUDIO_PLAYBACK_DEEP_BUFFER) &&
        (usecase->id != USECASE_AUDIO_PLAYBACK_LOW_LATENCY) &&
        (usecase->id != USECASE_AUDIO_PLAYBACK_MULTI_CH) &&
        (usecase->id != USECASE_AUDIO_PLAYBACK_OFFLOAD) &&
        (usecase->type != PCM_CAPTURE)) {
        ALOGV("%s: a rx/tx/loopback path where app type cfg is not required %d", __func__, usecase->id);
        rc = 0;
        goto exit_send_app_type_cfg;
    }

    if (usecase->type == PCM_PLAYBACK) {
        snd_device = usecase->out_snd_device;
        pcm_device_id = platform_get_pcm_device_id(usecase->id, PCM_PLAYBACK);
        snprintf(mixer_ctl_name, sizeof(mixer_ctl_name),
            "Audio Stream %d App Type Cfg", pcm_device_id);
        acdb_dev_id = platform_get_snd_device_acdb_id(usecase->out_snd_device);
    } else if (usecase->type == PCM_CAPTURE) {
        snd_device = usecase->in_snd_device;
        pcm_device_id = platform_get_pcm_device_id(usecase->id, PCM_CAPTURE);
        snprintf(mixer_ctl_name, sizeof(mixer_ctl_name),
            "Audio Stream Capture %d App Type Cfg", pcm_device_id);
        acdb_dev_id = platform_get_snd_device_acdb_id(usecase->in_snd_device);
    }
    if (acdb_dev_id <= 0) {
        ALOGE("%s: Couldn't get the acdb dev id", __func__);
        rc = -EINVAL;
        goto exit_send_app_type_cfg;
    }

    ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s", __func__,
              mixer_ctl_name);
        rc = -EINVAL;
        goto exit_send_app_type_cfg;
    }
    snd_device = (snd_device == SND_DEVICE_OUT_SPEAKER) ?
                 audio_extn_get_spkr_prot_snd_device(snd_device) : snd_device;
    acdb_dev_id = platform_get_snd_device_acdb_id(snd_device);
    if (acdb_dev_id < 0) {
        ALOGE("%s: Couldn't get the acdb dev id", __func__);
        rc = -EINVAL;
        goto exit_send_app_type_cfg;
    }

    if ((usecase->type == PCM_PLAYBACK) && (usecase->stream.out != NULL)) {
        if (usecase->stream.out->devices & AUDIO_DEVICE_OUT_SPEAKER) {
            usecase->stream.out->app_type_cfg.sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
        }

        sample_rate = usecase->stream.out->app_type_cfg.sample_rate;

    }
    if (usecase->type == PCM_PLAYBACK) {
        if ((24 == usecase->stream.out->bit_width) &&
                (usecase->stream.out->devices & AUDIO_DEVICE_OUT_SPEAKER)) {
            sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
        } else {
            sample_rate = usecase->stream.out->app_type_cfg.sample_rate;
        }
        app_type_cfg[len++] = usecase->stream.out->app_type_cfg.app_type;
        app_type_cfg[len++] = acdb_dev_id;
        app_type_cfg[len++] = sample_rate;

         ALOGI("%s PLAYBACK app_type %d, acdb_dev_id %d, sample_rate %d",
             __func__, usecase->stream.out->app_type_cfg.app_type, acdb_dev_id, sample_rate);

    } else if ((usecase->type == PCM_CAPTURE) && (usecase->stream.in != NULL)) {
        app_type_cfg[len++] = usecase->stream.in->app_type_cfg.app_type;
        app_type_cfg[len++] = acdb_dev_id;
        app_type_cfg[len++] = usecase->stream.in->app_type_cfg.sample_rate;
        ALOGI("%s CAPTURE app_type %d, acdb_dev_id %d, sample_rate %d",
           __func__, usecase->stream.in->app_type_cfg.app_type, acdb_dev_id,
           usecase->stream.in->app_type_cfg.sample_rate);
    } else {
        app_type_cfg[len++] = platform_get_default_app_type_v2(adev->platform, usecase->type);
        app_type_cfg[len++] = acdb_dev_id;
        app_type_cfg[len++] = sample_rate;
        ALOGI("%s default app_type %d, acdb_dev_id %d, sample_rate %d",
              __func__, platform_get_default_app_type_v2(adev->platform, usecase->type),
              acdb_dev_id, sample_rate);
    }

    mixer_ctl_set_array(ctl, app_type_cfg, len);
    ALOGI("%s: %s  app_type %d, acdb_dev_id %d, sample_rate %d",
            __func__, usecase->type == PCM_CAPTURE? "CAPTURE":"PLAYBACK",
                                  (int)app_type_cfg[0],(int)app_type_cfg[1],(int)app_type_cfg[2]);
    rc = 0;
exit_send_app_type_cfg:
    return rc;
}

int read_line_from_file(const char *path, char *buf, size_t count)
{
    char * fgets_ret;
    FILE * fd;
    int rv;

    fd = fopen(path, "r");
    if (fd == NULL)
        return -1;

    fgets_ret = fgets(buf, (int)count, fd);
    if (NULL != fgets_ret) {
        rv = (int)strlen(buf);
    } else {
        rv = ferror(fd);
    }
    fclose(fd);

   return rv;
}

int get_snd_codec_id(audio_format_t format)
{
    int id = 0;

    switch (format & AUDIO_FORMAT_MAIN_MASK) {
    case AUDIO_FORMAT_MP3:
        id = SND_AUDIOCODEC_MP3;
        break;
    case AUDIO_FORMAT_AAC:
        id = SND_AUDIOCODEC_AAC;
        break;
    case AUDIO_FORMAT_AAC_ADTS:
        id = SND_AUDIOCODEC_AAC;
        break;
    case AUDIO_FORMAT_PCM_OFFLOAD:
    case AUDIO_FORMAT_PCM:
        id = SND_AUDIOCODEC_PCM;
        break;
    case AUDIO_FORMAT_FLAC:
        id = SND_AUDIOCODEC_FLAC;
        break;
    case AUDIO_FORMAT_ALAC:
        id = SND_AUDIOCODEC_ALAC;
        break;
    case AUDIO_FORMAT_APE:
        id = SND_AUDIOCODEC_APE;
        break;
    case AUDIO_FORMAT_VORBIS:
        id = SND_AUDIOCODEC_VORBIS;
        break;
    case AUDIO_FORMAT_WMA:
        id = SND_AUDIOCODEC_WMA;
        break;
    case AUDIO_FORMAT_WMA_PRO:
        id = SND_AUDIOCODEC_WMA_PRO;
        break;
    default:
        ALOGE("%s: Unsupported audio format :%x", __func__, format);
    }

    return id;
}

void audio_extn_utils_send_audio_calibration(struct audio_device *adev,
                                             struct audio_usecase *usecase)
{
    int type = usecase->type;

    if (type == PCM_PLAYBACK) {
        struct stream_out *out = usecase->stream.out;
        int snd_device = usecase->out_snd_device;
        snd_device = (snd_device == SND_DEVICE_OUT_SPEAKER) ?
                     audio_extn_get_spkr_prot_snd_device(snd_device) : snd_device;
        platform_send_audio_calibration(adev->platform, usecase,
                                        out->app_type_cfg.app_type,
                                        out->app_type_cfg.sample_rate);
    } else if (type == PCM_CAPTURE) {
        platform_send_audio_calibration(adev->platform, usecase,
                         usecase->stream.in->app_type_cfg.app_type,
                         usecase->stream.in->app_type_cfg.sample_rate);
    } else {
        /* when app type is default. the sample rate is not used to send cal */
        platform_send_audio_calibration(adev->platform, usecase,
                         platform_get_default_app_type_v2(adev->platform, usecase->type),
                         48000);
    }
}

