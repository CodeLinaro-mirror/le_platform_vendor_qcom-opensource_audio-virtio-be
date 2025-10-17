/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file AudioDeviceManager.h
 * @brief Audio Device Manager - Manages commuication with
 * virtio-snd and dispatches commands to
 * SoundCardInfoUtility and the BE plugins
 */

#ifndef AUDIO_DEVICE_MANAGER_H_
#define AUDIO_DEVICE_MANAGER_H_

#include "ISoundCardInfoUtil.h"
#include "audio_plugin_interface.h"
#include "hab_interface.h"
#include "virtio_snd.h"
#include "audio_log_util.h"

#include <vector>
#include <unordered_map>
#include <functional>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>


#define DEFAULT_QUEUE_BITMAP ((1 << 0) | (1 << 1) | (1 << 2) | (1 << 3))

struct PluginUtils {
    ISoundCardInfoUtil *sciu;
    IAudioPlugin *pcm_plugin;
};

struct PCMMsgArgs {
    uint32_t stream_id;
    struct SocketInfo sock_info;
    unsigned int direction;
};

struct PCMMsgComplt {
    int32_t vcid;
    unordered_map<uint32_t, struct virtio_pcm_msg*> pcm_msg_map;
};

struct PCMStreamThread {
    std::thread thread_obj;
    std::queue<struct virtio_pcm_msg*> msg_queue;
    std::mutex stream_mutex;
    std::condition_variable stream_cond;
    struct PCMMsgComplt pcm_msg_complt;
};


struct PushPullInfo {
    uint32_t data_export_id;
    uint32_t pos_export_id;
    uint32_t data_fd;
    uint32_t pos_fd;
};

class AudioDeviceManager {

    public:
        AudioDeviceManager(IAudioPlugin *iaudio_plugin,
                           ISoundCardInfoUtil *isciu,
                           IHab *ihab,
                           std::vector<FrontendInfo> *fe_info,
                           std::vector<SoundCardMixerCtl> *sc_mixer_ctl,
                           const unsigned int queue_bitmap = DEFAULT_QUEUE_BITMAP);
        ~AudioDeviceManager();

        void launch();

    protected:
        void OpenCommChannel(uint32_t mmid);

        void CtlQueueFunc(struct SocketInfo sock_info);
        void EventQueueFunc(struct SocketInfo sock_info);
        void PCMQueueFunc(struct SocketInfo sock_info);

        uint32_t ProcessCtlMsg(void *buff, uint32_t buff_size, struct SocketInfo sock_info);
        void ProcessPCMCount(struct virtio_snd_count_msg &msg, unsigned int gvm_id);
        void ProcessPCMInfo(struct virtio_snd_pcm_info_msg &msg, unsigned int gvm_id);
        void ProcessDCCount(struct virtio_snd_count_msg &msg, unsigned int gvm_id);
        void ProcessDCInfo(struct virtio_snd_dc_info_msg &msg, unsigned int gvm_id);
        uint32_t ProcessDCEnumInfo(struct virtio_snd_dc_enum_info_msg &msg, unsigned int gvm_id);

        void ProcessPCMSetParams(struct virtio_snd_pcm_set_params_msg &msg, unsigned int gvm_id);
        void ProcessPCMPrepare(struct virtio_snd_pcm_prepare_msg &msg, struct SocketInfo sock_info);
        void ProcessPCMTrigger(struct virtio_snd_pcm_trigger_msg &msg, unsigned int gvm_id);
        void ProcessPCMMmap(struct virtio_snd_pcm_mmap_msg &msg, struct SocketInfo sock_info);
        void ProcessDCRead(struct virtio_snd_dc_read_msg &msg, unsigned int gvm_id);
        void ProcessDCWrite(struct virtio_snd_dc_write_msg &msg, unsigned int gvm_id);
        void ProcessDCTlvRead(struct virtio_snd_dc_tlv_read_msg &msg, unsigned int gvm_id);
        void ProcessDCTlvWrite(struct virtio_snd_dc_tlv_write_msg &msg, uint32_t buff_size, unsigned int gvm_id);

        void ProcessPCMMsg(struct PCMMsgArgs args);

        std::unordered_map<int32_t, struct PluginUtils> plugin_utils_;

        std::unordered_map<int32_t, std::vector<FrontendInfo>> all_fe_dais_;

        // Mixer controls for plugins
        std::unordered_map<int32_t, std::vector<SoundCardMixerCtl>> all_mixer_ctls_;
        std::unordered_map<int32_t, std::unordered_map<int32_t, struct DMAExport>> dma_export_map_;
        std::unordered_map<int32_t, std::unordered_map<int32_t, struct PushPullInfo>> push_pull_map_;
        std::unordered_map<int32_t, std::unordered_map<string, struct PCMStreamThread*>> pcm_stream_threads_;

        // Callback function to handle pcm msg complete event
        void PcmMsgCompleteCallbackFunc(string &fe_dai_name, uint32_t msg_offset);

    private:
        void SSRCallbackFunc(struct SocketInfo sock_info, enum SSRState);
        unsigned int queue_bitmap_;
        std::mutex msg_mutex;
        std::mutex pcm_stream_threads_mutex;

        IAudioPlugin *iaudio_plugin_;
        ISoundCardInfoUtil *isciu_;
        IHab *ihab_;

        std::vector<FrontendInfo> *fe_info_;
        std::vector<SoundCardMixerCtl> *sc_mixer_ctl_;
};
#endif // AUDIO_DEVICE_MANAGER_H_
