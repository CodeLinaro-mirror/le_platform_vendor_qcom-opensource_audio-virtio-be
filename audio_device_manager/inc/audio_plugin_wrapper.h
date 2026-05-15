/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef AUDIO_PLUGIN_WRAPPER_H_
#define AUDIO_PLUGIN_WRAPPER_H_

#include "audio_plugin_interface.h"
#include "adm_service_type.h"
#include "SoundCardInfoTypes.h"
#include <vector>
#include <string>

/* AR plugin factory: second arg is std::vector<MixerCtlInfo>* */
typedef IAudioPlugin* (*create_ar_plugin_fn_t)(std::vector<FrontendInfo>*,
                                               std::vector<MixerCtlInfo>*);
/* AWE plugin factory: second arg is std::vector<SoundCardMixerCtl>* */
typedef IAudioPlugin* (*create_awe_plugin_fn_t)(std::vector<FrontendInfo>*,
                                                std::vector<SoundCardMixerCtl>*);
typedef void          (*destroy_plugin_fn_t)(IAudioPlugin*);

/**
 * @class AudioPluginWrapper
 * @brief RAII handle that dlopen-loads the appropriate audio plugin .so
 *        at runtime based on ADMServiceType (AR or AWE).
 *
 * Use the static factory methods CreateAR() / CreateAWE() instead of
 * constructing directly.  This prevents accidentally pairing the wrong
 * ADMServiceType with the wrong mixer-ctl argument type, which would
 * previously produce only a runtime log error rather than a compile-time
 * or link-time diagnostic.
 */
class AudioPluginWrapper {
public:
    /** Factory: create an AR (AGM) plugin wrapper. */
    static AudioPluginWrapper* CreateAR(std::vector<FrontendInfo>* all_fe_dai,
                                        std::vector<MixerCtlInfo>* all_mixer_ctl_info);

    /** Factory: create an AWE plugin wrapper. */
    static AudioPluginWrapper* CreateAWE(std::vector<FrontendInfo>* all_fe_dai,
                                         std::vector<SoundCardMixerCtl>* all_mixer_ctl);

    ~AudioPluginWrapper();

    AudioPluginWrapper(const AudioPluginWrapper&) = delete;
    AudioPluginWrapper& operator=(const AudioPluginWrapper&) = delete;

    /** Returns the inner plugin pointer. Check IsLoaded() before use. */
    IAudioPlugin* Get() const { return plugin_; }

    /** Returns true if the .so was loaded and the inner plugin is valid. */
    bool IsLoaded() const { return plugin_ != nullptr; }

private:
    /* Private default constructor — callers must use the factory methods. */
    AudioPluginWrapper() = default;

    void*               so_handle_  = nullptr;
    IAudioPlugin*       plugin_     = nullptr;
    destroy_plugin_fn_t destroy_fn_ = nullptr;

    bool LoadAR(std::vector<FrontendInfo>* all_fe_dai,
                std::vector<MixerCtlInfo>* all_mixer_ctl_info);
    bool LoadAWE(std::vector<FrontendInfo>* all_fe_dai,
                 std::vector<SoundCardMixerCtl>* all_mixer_ctl);
};

#endif  // AUDIO_PLUGIN_WRAPPER_H_
