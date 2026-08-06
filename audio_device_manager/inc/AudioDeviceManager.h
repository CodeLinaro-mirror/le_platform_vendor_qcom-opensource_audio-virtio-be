/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef AUDIO_DEVICE_MANAGER_H_
#define AUDIO_DEVICE_MANAGER_H_

#include "audio_plugin_interface.h"
#include "ISoundCardInfoUtil.h"
#include "adm_service_type.h"
#include "hab_interface.h"
#include "virtio_snd.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <atomic>
#include "audio_log_util.h"

#define DEFAULT_QUEUE_BITMAP ((1 << 0) | (1 << 1) | (1 << 2) | (1 << 3))
#define MAX_PROCESSOR_CORES 4

// Global variable declaration
extern ADMServiceType g_adm_service_type;
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
    std::condition_variable stream_ctrl_cond;
    /* used for pcm_stream_threads_ only
     * -1 - running
     *  0 - stop and all msgs are handled
     * >0 - stop and indicate the count of msgs to be handled
     */
    int32_t in_flight_cnt;
    struct PCMMsgComplt pcm_msg_complt;
    std::atomic<bool> stop_flag{false};


    ~PCMStreamThread() {
        stop_flag.store(true);
        stream_cond.notify_all();
        if (thread_obj.joinable()) {
            thread_obj.join();
        }

        // Clear any remaining messages, thread is joined, no lock needed
        while (!msg_queue.empty()) {
            auto msg = msg_queue.front();
            msg_queue.pop();
            if (msg) {
                free(msg);
            }
        }
    }
};

struct PushPullInfo {
    uint32_t data_export_id;
    uint32_t pos_export_id;
    uint32_t data_fd;
    uint32_t pos_fd;
};

struct ShmemInfo {
    uint32_t data_export_id;
    uint32_t data_fd;
    uint32_t export_fd;
    uint32_t data_size;
    uint32_t addr_lsw[MAX_PROCESSOR_CORES];
    uint32_t addr_msw[MAX_PROCESSOR_CORES];
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
        void ProcessVersion(struct virtio_snd_version_msg &msg);
        void ProcessPCMCount(struct virtio_snd_count_msg &msg, unsigned int gvm_id);
        void ProcessPCMInfo(struct virtio_snd_pcm_info_msg &msg, unsigned int gvm_id);
        void ProcessDCCount(struct virtio_snd_count_msg &msg, unsigned int gvm_id);
        void ProcessDCInfo(struct virtio_snd_dc_info_msg &msg, unsigned int gvm_id);
        uint32_t ProcessDCEnumInfo(struct virtio_snd_dc_enum_info_msg &msg, unsigned int gvm_id);

        void ProcessPCMSetParams(struct virtio_snd_pcm_set_params_msg &msg, struct SocketInfo sock_info);
        void ProcessPCMPrepare(struct virtio_snd_pcm_prepare_msg &msg, struct SocketInfo sock_info);
        void ProcessPCMTrigger(struct virtio_snd_pcm_trigger_msg &msg, unsigned int gvm_id);
        void ProcessPCMMmap(struct virtio_snd_pcm_mmap_msg &msg, struct SocketInfo sock_info);
        void ProcessDCRead(struct virtio_snd_dc_read_msg &msg, unsigned int gvm_id);
        void ProcessDCWrite(struct virtio_snd_dc_write_msg &msg, unsigned int gvm_id);
        void ProcessDCTlvRead(struct virtio_snd_dc_tlv_read_msg &msg, unsigned int gvm_id);
        void ProcessDCTlvWrite(struct virtio_snd_dc_tlv_write_msg &msg, uint32_t buff_size, unsigned int gvm_id);
        void ProcessShmemMap(struct virtio_snd_dc_write_msg &msg, struct SocketInfo sock_info);
        void ProcessShmemUnmap(struct virtio_snd_dc_write_msg &msg, struct SocketInfo sock_info);
        void DropPcmMsg(int32_t vcid, struct virtio_pcm_msg *msg);
        void UpdatePcmDeviceState(int32_t vmid, int32_t fe_stream_id, int32_t hdr_code);
        enum VirtioPCMState GetPcmDeviceState(int32_t vmid, int32_t fe_stream_id);
#ifdef OFF_TARGET_UNIT_TEST
        void EmptyFuncSockinfo(struct SocketInfo sock_info);
        void EmptyFuncPCMMsgArgs(struct PCMMsgArgs args);
        void SSRCallbackFunc(struct SocketInfo sock_info, enum SSRState);
        void EventCallbackFunc(struct SocketInfo sock_info, uint32_t control_id);
#endif
        // Callback function to handle pcm msg complete event
        void PcmMsgCompleteCallbackFunc(string &fe_dai_name, uint32_t msg_offset);
        void ProcessPCMMsg(struct PCMMsgArgs args);
        //void ProcessPCMPendingMsg(struct PCMMsgArgs args);
        std::unordered_map<int32_t, struct PluginUtils> plugin_utils_;

        std::unordered_map<int32_t, std::vector<FrontendInfo>> all_fe_dais_;
        // Mixer controls for plugins
        std::unordered_map<int32_t, std::vector<SoundCardMixerCtl>> all_mixer_ctls_;
        std::unordered_map<int32_t, std::unordered_map<int32_t, struct DMAExport>> dma_export_map_;
        std::unordered_map<int32_t, std::unordered_map<int32_t, struct PushPullInfo>> push_pull_map_;
        std::unordered_map<int32_t, std::unordered_map<string, std::shared_ptr<PCMStreamThread>>> pcm_stream_threads_;
        //std::unordered_map<int32_t, std::unordered_map<string, std::shared_ptr<PCMStreamThread>>> pcm_pending_threads_;
        std::unordered_map<int32_t, std::unordered_map<uint32_t, uint32_t>> vcid_map_;

    private:
#ifndef OFF_TARGET_UNIT_TEST
        void SSRCallbackFunc(struct SocketInfo sock_info, enum SSRState);
        void EventCallbackFunc(struct SocketInfo sock_info, uint32_t control_id);
#endif
        unsigned int queue_bitmap_;

        IAudioPlugin *iaudio_plugin_;
        ISoundCardInfoUtil *isciu_;
        IHab *ihab_;

        std::vector<FrontendInfo> *fe_info_;
        std::vector<SoundCardMixerCtl> *sc_mixer_ctl_;
        std::vector<ShmemInfo> shmem;
        std::mutex dma_export_map_mutex; // protect dma_export_map_
        std::mutex pcm_stream_threads_mutex; // protect pcm_stream_threads_ & pcm_pending_threads_
        std::mutex vcid_map_mutex;
        std::mutex snd_card_init_mutex;
};

#endif  // AUDIO_DEVICE_MANAGER_H_
