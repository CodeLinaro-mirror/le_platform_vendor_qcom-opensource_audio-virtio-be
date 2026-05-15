/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "sound_card_info_wrapper.h"
#include "audio_log_util.h"
#include <dlfcn.h>

static const char kArScIuSoName[]  = "libArSoundCardInfoUtil.so";
static const char kAweScIuSoName[] = "libAweSoundCardInfoUtil.so";

/* --------------------------------------------------------------------------
 * Factory methods
 * -------------------------------------------------------------------------- */

SoundCardInfoWrapper* SoundCardInfoWrapper::CreateAR(const int gvm_id)
{
    SoundCardInfoWrapper* w = new SoundCardInfoWrapper();
    if (!w->LoadAR(gvm_id)) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper::CreateAR: failed to load AR sciu");
        delete w;
        return nullptr;
    }
    return w;
}

SoundCardInfoWrapper* SoundCardInfoWrapper::CreateAWE(const int gvm_id)
{
    SoundCardInfoWrapper* w = new SoundCardInfoWrapper();
    if (!w->LoadAWE(gvm_id)) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper::CreateAWE: failed to load AWE sciu");
        delete w;
        return nullptr;
    }
    return w;
}

/* --------------------------------------------------------------------------
 * Destructor
 * -------------------------------------------------------------------------- */

SoundCardInfoWrapper::~SoundCardInfoWrapper()
{
    // Free the MixerCtlInfo map before destroying sciu_ (it was allocated inside the .so)
    if (mixer_ctl_map_) {
        DestroyMixerCtlMap(mixer_ctl_map_);
        mixer_ctl_map_ = nullptr;
    }
    if (sciu_ && destroy_fn_) {
        destroy_fn_(sciu_);
        sciu_ = nullptr;
    }
    if (so_handle_) {
        dlclose(so_handle_);
        so_handle_ = nullptr;
    }
}

/* --------------------------------------------------------------------------
 * Private: load AR (libArSoundCardInfoUtil.so)
 * -------------------------------------------------------------------------- */

bool SoundCardInfoWrapper::LoadAR(const int gvm_id)
{
    so_handle_ = dlopen(kArScIuSoName, RTLD_NOW | RTLD_LOCAL);
    if (!so_handle_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: dlopen(%s) failed: %s",
                       kArScIuSoName, dl_err ? dl_err : "(null)");
        return false;
    }

    auto create_fn = reinterpret_cast<create_sciu_fn_t>(
                         dlsym(so_handle_, "createArSoundCardInfoUtil"));
    if (!create_fn) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: dlsym(createArSoundCardInfoUtil) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_); so_handle_ = nullptr;
        return false;
    }

    destroy_fn_ = reinterpret_cast<destroy_sciu_fn_t>(
                      dlsym(so_handle_, "destroyArSoundCardInfoUtil"));
    if (!destroy_fn_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: dlsym(destroyArSoundCardInfoUtil) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_); so_handle_ = nullptr;
        return false;
    }

    create_mixer_ctl_map_fn_ = reinterpret_cast<create_mixer_ctl_map_fn_t>(
                                   dlsym(so_handle_, "createArMixerCtlMap"));
    if (!create_mixer_ctl_map_fn_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: dlsym(createArMixerCtlMap) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_); so_handle_ = nullptr;
        return false;
    }

    destroy_mixer_ctl_map_fn_ = reinterpret_cast<destroy_mixer_ctl_map_fn_t>(
                                    dlsym(so_handle_, "destroyArMixerCtlMap"));
    if (!destroy_mixer_ctl_map_fn_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: dlsym(destroyArMixerCtlMap) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_); so_handle_ = nullptr;
        return false;
    }

    sciu_ = create_fn(gvm_id);
    if (!sciu_) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: createArSoundCardInfoUtil returned nullptr");
        dlclose(so_handle_); so_handle_ = nullptr;
        destroy_fn_ = nullptr;
        create_mixer_ctl_map_fn_  = nullptr;
        destroy_mixer_ctl_map_fn_ = nullptr;
        return false;
    }

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                   "SoundCardInfoWrapper: %s loaded successfully, sciu=%p",
                   kArScIuSoName, sciu_);
    return true;
}

/* --------------------------------------------------------------------------
 * Private: load AWE (libAweSoundCardInfoUtil.so)
 * -------------------------------------------------------------------------- */

bool SoundCardInfoWrapper::LoadAWE(const int gvm_id)
{
    so_handle_ = dlopen(kAweScIuSoName, RTLD_NOW | RTLD_LOCAL);
    if (!so_handle_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: dlopen(%s) failed: %s",
                       kAweScIuSoName, dl_err ? dl_err : "(null)");
        return false;
    }

    auto create_fn = reinterpret_cast<create_sciu_fn_t>(
                         dlsym(so_handle_, "createAweSoundCardInfoUtil"));
    if (!create_fn) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: dlsym(createAweSoundCardInfoUtil) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_); so_handle_ = nullptr;
        return false;
    }

    destroy_fn_ = reinterpret_cast<destroy_sciu_fn_t>(
                      dlsym(so_handle_, "destroyAweSoundCardInfoUtil"));
    if (!destroy_fn_) {
        const char* dl_err = dlerror();
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: dlsym(destroyAweSoundCardInfoUtil) failed: %s",
                       dl_err ? dl_err : "(null)");
        dlclose(so_handle_); so_handle_ = nullptr;
        return false;
    }

    sciu_ = create_fn(gvm_id);
    if (!sciu_) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: createAweSoundCardInfoUtil returned nullptr");
        dlclose(so_handle_); so_handle_ = nullptr;
        destroy_fn_ = nullptr;
        return false;
    }

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                   "SoundCardInfoWrapper: %s loaded successfully, sciu=%p",
                   kAweScIuSoName, sciu_);
    return true;
}

/* --------------------------------------------------------------------------
 * AR-only: BuildMixerCtlMap / DestroyMixerCtlMap (private)
 * -------------------------------------------------------------------------- */

bool SoundCardInfoWrapper::BuildMixerCtlMap(
        std::vector<SoundCardMixerCtl>* mixer_ctls)
{
    if (!create_mixer_ctl_map_fn_) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: BuildMixerCtlMap called but not available "
                       "(AWE path or not loaded)");
        return false;
    }
    if (!sciu_ || !mixer_ctls) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: BuildMixerCtlMap: null sciu_ or mixer_ctls");
        return false;
    }
    /* Guard against double-allocation: destroy any previously created map
     * before overwriting the pointer, so it is not leaked. */
    if (mixer_ctl_map_) {
        DestroyMixerCtlMap(mixer_ctl_map_);
        mixer_ctl_map_ = nullptr;
    }
    mixer_ctl_map_ = create_mixer_ctl_map_fn_(sciu_, mixer_ctls);
    if (!mixer_ctl_map_) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "SoundCardInfoWrapper: createArMixerCtlMap returned nullptr");
        return false;
    }
    return true;
}

void SoundCardInfoWrapper::DestroyMixerCtlMap(std::vector<MixerCtlInfo>* map)
{
    if (destroy_mixer_ctl_map_fn_ && map) {
        destroy_mixer_ctl_map_fn_(map);
    }
}
