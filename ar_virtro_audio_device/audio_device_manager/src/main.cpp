/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "SoundCardInfoUtil.h"
#include "AudioDeviceManager.h"
#include <iostream>
#include "csd2_audio_plugin.h"
#include "hab_interface.h"

#ifdef LOGGER_USE_SLOG2
void __attribute__((constructor)) __ctor_slog2_init(void);

/**This is our default buffer*/
static slog2_buffer_t amss_default_slog2_buffer;

const static slog2_buffer_set_config_t buffer_cfg = {
    .num_buffers = 1, //Can be a maximum of  MAX_SLOG2_BUFFERS (4)
    .buffer_set_name = "csd2_audio_plugin",
    .verbosity_level = QCLOG_INFO,
    .buffer_config = {
        {.buffer_name = "default", .num_pages = 4}, //logger_log uses this
    }
};

void __ctor_slog2_init(void)
{
    if (-1 == slog2_register(&buffer_cfg, &amss_default_slog2_buffer, 0))
    {
        //can't use logger_log here, use slog directly
        slogf(_SLOG_SETCODE(QCLOG_AMSS_MM_APPS_CAD + 1, 1),
              _SLOG_ERROR, "Couldn't register slog2 buffer from audio_chime");
        //default to slog for future logger_log() calls
        amss_default_slog2_buffer = NULL;
    }
    else
    {
        /* Despite setting the default buffer to amss_default_slog2_buffer,
         * logger_log will continue to retrieve the default buffer as its
         * possible for libs compiled with logger_log() to run in
         * processes that don't have a default buffer
         * For such cases logger_log() will invoke legacy slog.*/
        slog2_set_default_buffer(amss_default_slog2_buffer);
    }
}
#endif

extern "C" {
static AudioDeviceManager *adm = nullptr;
  int adm_init(void)
  {
    struct PluginUtils utils;
    std::cout << "Launching AudioDeviceManager!" << std::endl;

    SoundCardInfoUtil *sciu = new SoundCardInfoUtil();
    HabImpl *hab = new HabImpl();

    if (sciu->BuildSoundCard()) {
        std::cout << "failed to build sound card" << std::endl;
        return -1;
    }

    std::vector<FrontendInfo> *fe_info = new std::vector<FrontendInfo>(sciu->GetPCMSubstreams());
    if (fe_info == nullptr || fe_info->empty()) {
        std::cout << "failed to get PCM substreams" << std::endl;
        return -1;
    }

    std::vector<SoundCardMixerCtl> *sc_mixer_ctl = new std::vector<SoundCardMixerCtl>(sciu->GetMixerCtls());
    if (sc_mixer_ctl == nullptr || sc_mixer_ctl->empty()) {
        std::cout << "failed to get mixer controls" << std::endl;
        return -1;
    }

    std::vector<MixerCtlInfo> *all_mixer_ctl_info = new std::vector<MixerCtlInfo>(sciu->CreateMixerCtlMap(*sc_mixer_ctl));
    if (all_mixer_ctl_info == nullptr || all_mixer_ctl_info->empty()) {
        std::cout << "failed to create mixer control map" << std::endl;
        return -1;
    }

    CSD2AudioPlugin *csd2_audio_plugin = new CSD2AudioPlugin(fe_info, all_mixer_ctl_info);

    adm = new AudioDeviceManager(csd2_audio_plugin, sciu,
            hab, fe_info, sc_mixer_ctl);

    std::cout << "Created adm instance" << std::endl;

    adm->launch();

    std::cout << "AudioDeviceManager threads launched" << std::endl;

    return 0;
 }

  int adm_deinit()
  {
    if (adm) {
      std::cout << "deleting adm instance" << std::endl;
      delete adm;
    }
    return 0;
  }
}
