/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "audio_plugin_wrapper.h"
#include "sound_card_info_wrapper.h"
#include "AudioDeviceManager.h"
#include <memory>

ADMServiceType g_adm_service_type = ADM_TYPE_AR;
extern "C" {
    static AudioDeviceManager *adm = nullptr;

    static std::unique_ptr<AudioPluginWrapper>    g_plugin_wrapper;
    static std::unique_ptr<SoundCardInfoWrapper>  g_sciu_wrapper;

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

        // Create SoundCardInfoWrapper — dlopen AR or AWE .so based on service type
        if (g_adm_service_type == ADM_TYPE_AR) {
            g_sciu_wrapper.reset(SoundCardInfoWrapper::CreateAR(ADM_HOST_GVM_ID));
        } else {
            g_sciu_wrapper.reset(SoundCardInfoWrapper::CreateAWE(ADM_HOST_GVM_ID));
        }
        if (!g_sciu_wrapper) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                           "Failed to create SoundCardInfoWrapper (dlopen failed)!");
            return -1;
        }
        ISoundCardInfoUtil *sciu = g_sciu_wrapper->Get();
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                       "Created SoundCardInfoWrapper for service_type=%d", g_adm_service_type);

        // Use unique_ptr so that all failure paths below automatically free these objects.
        std::unique_ptr<HabImpl> hab(new HabImpl());

        if (sciu->BuildSoundCard()) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to build sound card!");
            return -1;
        }

        std::unique_ptr<std::vector<FrontendInfo>> fe_info(
                new std::vector<FrontendInfo>(sciu->GetPCMSubstreams()));
        if (fe_info->empty()) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "No PCM frontends found!");
            return -1;
        }

        std::unique_ptr<std::vector<SoundCardMixerCtl>> sc_mixer_ctl(
                new std::vector<SoundCardMixerCtl>(sciu->GetMixerCtls()));

        // Create AudioPlugin and AudioDeviceManager based on service type
        if (g_adm_service_type == ADM_TYPE_AWE) {
            g_plugin_wrapper.reset(AudioPluginWrapper::CreateAWE(fe_info.get(),
                                                                  sc_mixer_ctl.get()));
            if (!g_plugin_wrapper) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                               "Failed to create AudioPluginWrapper for AWE (dlopen failed)!");
                return -1;
            }
            adm = new AudioDeviceManager(g_plugin_wrapper->Get(), sciu, hab.release(),
                                         fe_info.release(), sc_mixer_ctl.release());
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Created AudioDeviceManager with AweAudioPlugin");
        } else if (g_adm_service_type == ADM_TYPE_AR) {
            if (!g_sciu_wrapper->BuildMixerCtlMap(sc_mixer_ctl.get()) ||
                g_sciu_wrapper->GetMixerCtlMap()->empty()) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to create mixer control map");
                return -1;
            }
            g_plugin_wrapper.reset(AudioPluginWrapper::CreateAR(fe_info.get(),
                                                                 g_sciu_wrapper->GetMixerCtlMap()));
            if (!g_plugin_wrapper) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                               "Failed to create AudioPluginWrapper for AR (dlopen failed)!");
                return -1;
            }
            adm = new AudioDeviceManager(g_plugin_wrapper->Get(), sciu, hab.release(),
                                         fe_info.release(), sc_mixer_ctl.release());
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
          AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "deleting adm instance");
          delete adm;
          adm = nullptr;
        }
        // Destroy wrappers after adm
        g_plugin_wrapper.reset();
        g_sciu_wrapper.reset();
        g_adm_service_type = ADM_TYPE_UNINITIALIZED;
        return 0;
    }

    // Getter function to access the service type from other modules
    ADMServiceType adm_get_service_type()
    {
        return g_adm_service_type;
    }
}
