/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "SoundCardInfoTypes.h"

class DigitalAudioIntfBuilder {
public:
    template <typename VectorT>
    static uint32_t SortDAIByDir(VectorT &all_dai, SortedDCEnums &dai_enums);
protected:
    static VirtioSndDCEnum SetEnumValFromDAIName(std::string dai_name_str);
    static void AddEnumVal(std::string name, std::vector<VirtioSndDCEnum> &enum_list);
};