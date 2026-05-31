/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef SOUND_CARD_INFO_WRAPPER_H_
#define SOUND_CARD_INFO_WRAPPER_H_

#include "ISoundCardInfoUtil.h"
#include "adm_service_type.h"
#include <vector>

typedef ISoundCardInfoUtil* (*create_sciu_fn_t)(const int gvm_id);
typedef void                (*destroy_sciu_fn_t)(ISoundCardInfoUtil*);

/* AR-only extras */
typedef std::vector<MixerCtlInfo>* (*create_mixer_ctl_map_fn_t)(
        ISoundCardInfoUtil*, std::vector<SoundCardMixerCtl>*);
typedef void (*destroy_mixer_ctl_map_fn_t)(std::vector<MixerCtlInfo>*);

/**
 * @class SoundCardInfoWrapper
 * @brief RAII handle that dlopen-loads the appropriate sound card info .so
 *        at runtime based on ADMServiceType (AR or AWE).
 *
 * Use the static factory methods CreateAR() / CreateAWE() instead of
 * constructing directly. Both return nullptr on failure, making the error
 * explicit at the call site and consistent with AudioPluginWrapper.
 */
class SoundCardInfoWrapper {
public:
    /** Factory: create an AR (AGM) sound card info wrapper. */
    static SoundCardInfoWrapper* CreateAR(const int gvm_id);

    /** Factory: create an AWE sound card info wrapper. */
    static SoundCardInfoWrapper* CreateAWE(const int gvm_id);

    ~SoundCardInfoWrapper();

    SoundCardInfoWrapper(const SoundCardInfoWrapper&) = delete;
    SoundCardInfoWrapper& operator=(const SoundCardInfoWrapper&) = delete;

    /** Returns the inner sciu pointer. Always valid when obtained via factory. */
    ISoundCardInfoUtil* Get() const { return sciu_; }

    /** Returns true if the .so was loaded and the inner sciu is valid. */
    bool IsLoaded() const { return sciu_ != nullptr; }

    /**
     * Build the MixerCtlInfo map (AR only). Ownership stays inside the wrapper;
     * use GetMixerCtlMap() to access the result. Returns false on failure.
     */
    bool BuildMixerCtlMap(std::vector<SoundCardMixerCtl>* mixer_ctls);

    /**
     * Returns the MixerCtlInfo map previously built by BuildMixerCtlMap().
     * The wrapper retains ownership — callers must NOT delete this pointer.
     * Returns nullptr if BuildMixerCtlMap() has not been called or failed.
     */
    std::vector<MixerCtlInfo>* GetMixerCtlMap() const { return mixer_ctl_map_; }

private:
    /* Private default constructor — callers must use the factory methods. */
    SoundCardInfoWrapper() = default;

    void*               so_handle_  = nullptr;
    ISoundCardInfoUtil* sciu_       = nullptr;
    destroy_sciu_fn_t   destroy_fn_ = nullptr;

    /* AR-only function pointers */
    create_mixer_ctl_map_fn_t  create_mixer_ctl_map_fn_  = nullptr;
    destroy_mixer_ctl_map_fn_t destroy_mixer_ctl_map_fn_ = nullptr;

    /* Owns the MixerCtlInfo map built by BuildMixerCtlMap().
     * Automatically freed in the destructor. */
    std::vector<MixerCtlInfo>* mixer_ctl_map_ = nullptr;

    /* Private: only the destructor and CreateMixerCtlMap() should free the map. */
    void DestroyMixerCtlMap(std::vector<MixerCtlInfo>* map);

    bool LoadAR(const int gvm_id);
    bool LoadAWE(const int gvm_id);
};

#endif  // SOUND_CARD_INFO_WRAPPER_H_
