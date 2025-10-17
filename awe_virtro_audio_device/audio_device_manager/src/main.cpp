/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */


#include "AudioDeviceManager.h"
#include <iostream>


extern "C" {
    int adm_init(void)
    {
        AWE_LOGI("AWE adm_init start");
        AWE_LOGI("Launching AudioDeviceManager!");
        AweSoundCardInfoUtil *sciu = new AweSoundCardInfoUtil();

        HabImpl *hab = new HabImpl();

        if (sciu->BuildSoundCard()) {
            AWE_LOGE("Failed to build sound card!");
            return -1;
        }

        std::vector<FrontendInfo> *fe_info = new std::vector<FrontendInfo>(sciu->GetPCMSubstreams());
        if (fe_info == nullptr) {
            AWE_LOGE("Failed to get PCM substreams!");
            return -1;
        }

        std::vector<SoundCardMixerCtl> *sc_mixer_ctl = new std::vector<SoundCardMixerCtl>(sciu->GetMixerCtls());
        if (sc_mixer_ctl == nullptr) {
            AWE_LOGE("Failed to get mixer controls!");
            return -1;
        }

        AweAudioPlugin *awe_audio_plugin = new AweAudioPlugin(fe_info, sc_mixer_ctl);

        static AudioDeviceManager *adm = nullptr;

        adm = new AudioDeviceManager(awe_audio_plugin, sciu, hab, fe_info, sc_mixer_ctl);

        AWE_LOGI("Created adm instance!");

        adm->launch();

        AWE_LOGI("AudioDeviceManager threads launched!");

        return 0;
    }
}
