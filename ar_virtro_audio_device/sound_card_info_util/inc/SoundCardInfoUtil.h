/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "DigitalAudioIntfBuilder.h"
#include "MixerCtlBuilder.h"
#include "XMLParser.h"
#include "SoundCardInfoTypes.h"
#include "ISoundCardInfoUtil.h"
#include <vector>

#ifndef SOUND_CARD_INFO_UTIL_H_
#define SOUND_CARD_INFO_UTIL_H_

class SoundCardInfoUtil : public ISoundCardInfoUtil {

public:
    SoundCardInfoUtil(const int gvm_id = 0);
    ~SoundCardInfoUtil();

    uint32_t BuildSoundCard() override;
    int GetNumPCMSubstreams() override;
    int GetNumMixerCtls() override;
    std::vector<FrontendInfo> GetPCMSubstreams() override;
    std::vector<SoundCardMixerCtl> GetMixerCtls() override;
    uint32_t GetEnumsForControl(const int control_id, std::vector<VirtioSndDCEnum> &ctl_enums) override;
    std::vector<MixerCtlInfo> CreateMixerCtlMap(std::vector<SoundCardMixerCtl> mixer_ctls);

protected:
    int gvm_id_ = 0;
    int num_pcm_frontends_;
    int num_mixer_ctls_;
    DAICounts       dai_counts_;
    FrontendVectors all_frontends_;
    SortedDCEnums   all_frontend_enums_;
    BackendVectors  all_backends_;
    SortedDCEnums   all_backend_enums_;
    MixerCtlExtns   all_ctl_extns_;
    std::vector<SoundCardMixerCtl>  all_mixer_ctls_;
    std::vector<DAIGroup>           mixer_ctl_enum_lookup_;
    void CountDAIs();

};
#endif // SOUND_CARD_INFO_UTIL_H_