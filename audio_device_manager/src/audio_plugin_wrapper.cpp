/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "audio_plugin_wrapper.h"
#include "audio_log_util.h"
#include <dlfcn.h>

static const char kArPluginSoName[]  = "libArAudioPluginIntf.so";
static const char kAwePluginSoName[] = "libAweAudioPluginIntf.so";


/* AR factory */
AudioPluginWrapper* AudioPluginWrapper::CreateAR(std::vector<FrontendInfo>* all_fe_dai,
                                                  std::vector<MixerCtlInfo>* all_mixer_ctl_info)
{
    AudioPluginWrapper* w = new AudioPluginWrapper();
    if (!w->LoadAR(all_fe_dai, all_mixer_ctl_info)) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper::CreateAR: failed to load AR plugin");
        delete w;
        return nullptr;
    }
    return w;
}

/* AWE factory */
AudioPluginWrapper* AudioPluginWrapper::CreateAWE(std::vector<FrontendInfo>* all_fe_dai,
                                                   std::vector<SoundCardMixerCtl>* all_mixer_ctl)
{
    AudioPluginWrapper* w = new AudioPluginWrapper();
    if (!w->LoadAWE(all_fe_dai, all_mixer_ctl)) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper::CreateAWE: failed to load AWE plugin");
        delete w;
        return nullptr;
    }
    return w;
}

AudioPluginWrapper::~AudioPluginWrapper()
{
    if (plugin_ && destroy_fn_) {
        destroy_fn_(plugin_);
        plugin_ = nullptr;
    }
    if (so_handle_) {
        dlclose(so_handle_);
        so_handle_ = nullptr;
    }
}

/* --------------------------------------------------------------------------
 * Private: load AR plugin (libArAudioPluginIntf.so)
 * -------------------------------------------------------------------------- */

bool AudioPluginWrapper::LoadAR(std::vector<FrontendInfo>* all_fe_dai,
                                std::vector<MixerCtlInfo>* all_mixer_ctl_info)
{
    so_handle_ = dlopen(kArPluginSoName, RTLD_NOW | RTLD_LOCAL);
    if (!so_handle_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper: dlopen(%s) failed: %s",
                       kArPluginSoName, dl_err ? dl_err : "(null)");
        return false;
    }

    auto create_fn = reinterpret_cast<create_ar_plugin_fn_t>(
                         dlsym(so_handle_, "createAGMAudioPlugin"));
    if (!create_fn) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper: dlsym(createAGMAudioPlugin) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_);
        so_handle_ = nullptr;
        return false;
    }

    destroy_fn_ = reinterpret_cast<destroy_plugin_fn_t>(
                      dlsym(so_handle_, "destroyAGMAudioPlugin"));
    if (!destroy_fn_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper: dlsym(destroyAGMAudioPlugin) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_);
        so_handle_ = nullptr;
        return false;
    }

    plugin_ = create_fn(all_fe_dai, all_mixer_ctl_info);
    if (!plugin_) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper: createAGMAudioPlugin returned nullptr");
        dlclose(so_handle_);
        so_handle_ = nullptr;
        destroy_fn_ = nullptr;
        return false;
    }

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                   "AudioPluginWrapper: %s loaded successfully, plugin=%p",
                   kArPluginSoName, plugin_);
    return true;
}

/* --------------------------------------------------------------------------
 * Private: load AWE plugin (libAweAudioPluginIntf.so)
 * -------------------------------------------------------------------------- */

bool AudioPluginWrapper::LoadAWE(std::vector<FrontendInfo>* all_fe_dai,
                                 std::vector<SoundCardMixerCtl>* all_mixer_ctl)
{
    so_handle_ = dlopen(kAwePluginSoName, RTLD_NOW | RTLD_LOCAL);
    if (!so_handle_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper: dlopen(%s) failed: %s",
                       kAwePluginSoName, dl_err ? dl_err : "(null)");
        return false;
    }

    auto create_fn = reinterpret_cast<create_awe_plugin_fn_t>(
                         dlsym(so_handle_, "createAweAudioPlugin"));
    if (!create_fn) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper: dlsym(createAweAudioPlugin) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_);
        so_handle_ = nullptr;
        return false;
    }

    destroy_fn_ = reinterpret_cast<destroy_plugin_fn_t>(
                      dlsym(so_handle_, "destroyAweAudioPlugin"));
    if (!destroy_fn_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper: dlsym(destroyAweAudioPlugin) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_);
        so_handle_ = nullptr;
        return false;
    }

    plugin_ = create_fn(all_fe_dai, all_mixer_ctl);
    if (!plugin_) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "AudioPluginWrapper: createAweAudioPlugin returned nullptr");
        dlclose(so_handle_);
        so_handle_ = nullptr;
        destroy_fn_ = nullptr;
        return false;
    }

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                   "AudioPluginWrapper: %s loaded successfully, plugin=%p",
                   kAwePluginSoName, plugin_);
    return true;
}
