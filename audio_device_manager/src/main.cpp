/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "AweAudioPlugin.h"
#include "AweSoundCardInfoUtil.h"
#include "AGMAudioPlugin.h"
#include "ArSoundCardInfoUtil.h"
#include "AudioDeviceManager.h"
#include <iostream>

ADMServiceType g_adm_service_type = ADM_TYPE_AR;
extern "C" {
    static AudioDeviceManager *adm = nullptr;

    int adm_init(const char* service_type)
    {
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Launching AudioDeviceManager!");
        if (service_type != nullptr) {
            if (strcmp(service_type, "awe") == 0) {
                g_adm_service_type = ADM_TYPE_AWE;
                AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "ADM Service Type set to AWE");
            } else if (strcmp(service_type, "ar") == 0) {
                g_adm_service_type = ADM_TYPE_AR;
                AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "ADM Service Type set to AR");
            } else {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Unknown service_type: %s", service_type);
                return -1;
            }
        } else {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "service_type is NULL");
            return -1;
        }
        ISoundCardInfoUtil *sciu = nullptr;
        // Create SoundCardInfoUtil based on service type
        if (g_adm_service_type == ADM_TYPE_AWE) {
            sciu = new AweSoundCardInfoUtil();
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Created AweSoundCardInfoUtil");
        } else if (g_adm_service_type == ADM_TYPE_AR) {
            sciu= new SoundCardInfoUtil();
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Created ArSoundCardInfoUtil");
        }

        HabImpl *hab = new HabImpl();

        if (sciu->BuildSoundCard()) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to build sound card!");
            return -1;
        }

        std::vector<FrontendInfo> *fe_info = new std::vector<FrontendInfo>(sciu->GetPCMSubstreams());
        if (fe_info == nullptr) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to get PCM substreams!");
            return -1;
        }

        std::vector<SoundCardMixerCtl> *sc_mixer_ctl = new std::vector<SoundCardMixerCtl>(sciu->GetMixerCtls());
        if (sc_mixer_ctl == nullptr) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to get mixer controls!");
            return -1;
        }

     // Create AudioPlugin and AudioDeviceManager based on service type
        if (g_adm_service_type == ADM_TYPE_AWE) {
            AweAudioPlugin *awe_audio_plugin = new AweAudioPlugin(fe_info, sc_mixer_ctl);
            if (awe_audio_plugin == nullptr) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to create AweAudioPlugin!");
                return -1;
            }
            adm = new AudioDeviceManager(awe_audio_plugin, sciu, hab, fe_info, sc_mixer_ctl);
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Created AudioDeviceManager with AweAudioPlugin");
        } else if (g_adm_service_type == ADM_TYPE_AR) {
            SoundCardInfoUtil *ar_sciu = dynamic_cast<SoundCardInfoUtil*>(sciu);
            std::vector<MixerCtlInfo> *all_mixer_ctl_info = new std::vector<MixerCtlInfo>(ar_sciu->CreateMixerCtlMap(*sc_mixer_ctl));
            if (all_mixer_ctl_info == nullptr || all_mixer_ctl_info->empty()) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to create mixer control map");
                return -1;
            }
            AGMAudioPlugin *agm_audio_plugin = new AGMAudioPlugin(fe_info, all_mixer_ctl_info);
            if (agm_audio_plugin == nullptr) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to create AGMAudioPlugin!");
                return -1;
            }
            adm = new AudioDeviceManager(agm_audio_plugin, sciu, hab, fe_info, sc_mixer_ctl);
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Created AudioDeviceManager with AGMAudioPlugin");
        }

        if (adm == nullptr) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to create AudioDeviceManager!");
            return -1;
        }

        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Created adm instance!");

        adm->launch();
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "AudioDeviceManager threads launched");

        return 0;
    }

    int adm_deinit()
    {
        if (adm) {
          std::cout << "deleting adm instance" << std::endl;
          delete adm;
        }
        g_adm_service_type = ADM_TYPE_UNINITIALIZED;
        return 0;
    }

    // Getter function to access the service type from other modules
    ADMServiceType adm_get_service_type()
    {
        return g_adm_service_type;
    }
}
