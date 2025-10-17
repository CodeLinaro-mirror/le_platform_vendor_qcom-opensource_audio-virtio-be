/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#ifndef I_SOUND_CARD_INFO_UTIL_H_
#define I_SOUND_CARD_INFO_UTIL_H_

#include "SoundCardInfoTypes.h"

class ISoundCardInfoUtil {

    public:
        virtual ~ISoundCardInfoUtil() = default;

        virtual uint32_t BuildSoundCard() = 0;
        virtual int GetNumPCMSubstreams() = 0;
        virtual int GetNumMixerCtls() = 0;
        virtual std::vector<FrontendInfo> GetPCMSubstreams() = 0;
        virtual std::vector<SoundCardMixerCtl> GetMixerCtls() = 0;
        virtual uint32_t GetEnumsForControl(const int control_id, std::vector<VirtioSndDCEnum> &ctl_enums) = 0;
};

#endif // I_SOUND_CARD_INFO_UTIL_H_