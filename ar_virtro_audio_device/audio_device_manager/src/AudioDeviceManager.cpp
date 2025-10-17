/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file AudioDeviceManager.cpp
 * @brief Audio Device Manager - Manages commuication with
 * virtio-snd and dispatches commands to
 * SoundCardInfoUtility and the BE plugins
 */

#include "AudioDeviceManager.h"
#include <thread>
#include <chrono>
#include <pthread.h>
#include <future>

#define MAX_HAB_BUFFER_SIZE 4096

using namespace std::placeholders;

AudioDeviceManager::AudioDeviceManager(
        IAudioPlugin *iaudio_plugin,
        ISoundCardInfoUtil *isciu,
        IHab *ihab,
        std::vector<FrontendInfo> *fe_info,
        std::vector<SoundCardMixerCtl> *sc_mixer_ctl,
        const unsigned int queue_bitmap) : iaudio_plugin_(iaudio_plugin),
                                           isciu_(isciu),
                                           ihab_(ihab),
                                           fe_info_(fe_info),
                                           sc_mixer_ctl_(sc_mixer_ctl),
                                           queue_bitmap_(queue_bitmap)
{
    auto adm_pcm_msg_cb_func = std::bind(&AudioDeviceManager::PcmMsgCompleteCallbackFunc,\
            this, _1, _2);
    iaudio_plugin_->RegisterPcmMsgCallbackFunc(adm_pcm_msg_cb_func);
}

void AudioDeviceManager::launch()
{
    for (int i = MM_AUD_1; i <= MM_AUD_4; i++)
    {
    // check if bit is set for the queue
        if (queue_bitmap_ & (1 << (i - MM_AUD_1)))
            std::thread(&AudioDeviceManager::OpenCommChannel, this, i).detach();
    }
}

AudioDeviceManager::~AudioDeviceManager()
{
    for (auto &it : plugin_utils_)
    {
        delete it.second.sciu;
        delete it.second.pcm_plugin;
    }

    // explicitly delete these raw pointers since these
    // are allocated on heap before ADM instantiation
    delete ihab_;
    delete fe_info_;
    delete sc_mixer_ctl_;

    for (auto &it: pcm_stream_threads_)
    {
        for (auto &it2: it.second)
        {
            {
                std::lock_guard<std::mutex> stream_queue_lock(it2.second->stream_mutex);
                it2.second->msg_queue.push(nullptr);
            }

            it2.second->stream_cond.notify_one();
            if (it2.second->thread_obj.joinable())
                it2.second->thread_obj.join();

            delete it2.second;
        }
    }
}

void AudioDeviceManager::PcmMsgCompleteCallbackFunc(string &fe_dai_name, uint32_t msg_offset)
{
    int32_t vcid;
    struct virtio_pcm_msg *msg = nullptr;
    uint32_t gvm_id = 2; // hard code gvm id for now

    std::lock_guard<std::mutex> msg_lock(msg_mutex);

    if (pcm_stream_threads_.find(gvm_id) == pcm_stream_threads_.end() ||
       pcm_stream_threads_[gvm_id].find(fe_dai_name) == pcm_stream_threads_[gvm_id].end() ||
       pcm_stream_threads_[gvm_id][fe_dai_name]->pcm_msg_complt.pcm_msg_map.find(msg_offset) == \
       pcm_stream_threads_[gvm_id][fe_dai_name]->pcm_msg_complt.pcm_msg_map.end() ||
       pcm_stream_threads_[gvm_id][fe_dai_name]->pcm_msg_complt.pcm_msg_map.at(msg_offset) == nullptr)
    {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "PcmMsgCompleteCallbackFunc "
        "invalid args for gvm id:%d, fe dai name:%s, msg_offset:%u",
        gvm_id, fe_dai_name.c_str(), msg_offset);
        return;
    }

    msg = pcm_stream_threads_[gvm_id][fe_dai_name]->pcm_msg_complt.pcm_msg_map[msg_offset];
    vcid = pcm_stream_threads_[gvm_id][fe_dai_name]->pcm_msg_complt.vcid;
    msg->status.status = VIRTIO_SND_S_OK;
    msg->length += sizeof(msg->status);
    ihab_->Send(vcid, msg, sizeof(struct virtio_pcm_msg), 0);
    free(msg);
    pcm_stream_threads_[gvm_id][fe_dai_name]->pcm_msg_complt.pcm_msg_map.at(msg_offset) = nullptr;
}

void AudioDeviceManager::OpenCommChannel(uint32_t mmid)
{
    int32_t ret = 0;

    while (1)
    {
        struct SocketInfo sock_info = {0};
        std::function<void(struct SocketInfo)> thread_func;
        std::thread thread_obj;
        unsigned int thread_priority = 10;
        int policy;
        sched_param thread_params;

        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Calling HAB open for mmid %d", mmid);
        ret = ihab_->Open(&sock_info.vcid, mmid, -1, 0);

        if (ret)
        {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Socket Open failed for mmid %d", mmid);
            continue;
        }
        ret = ihab_->SocketQuery(sock_info.vcid, &sock_info, 0);

        if (ret)
        {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Socket query failed for mmid %d, assuming GVM 1", mmid);
            sock_info.vmid = 1;
        }

        sock_info.mmid = mmid;
        switch (mmid) {
            case MM_AUD_1:
                AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Launching CtlQueueFunc, mmid: %d, vmid: %d", mmid, sock_info.vmid);
                thread_func = std::bind(&AudioDeviceManager::CtlQueueFunc, this, _1);
                break;
            case MM_AUD_2:
                AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Launching EventQueueFunc, mmid: %d, vmid: %d", mmid, sock_info.vmid);
                thread_func = std::bind(&AudioDeviceManager::EventQueueFunc, this, _1);
                break;
            case MM_AUD_3:
            case MM_AUD_4:
                AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Launching PCMQueueFunc, mmid: %d, vmid: %d", mmid, sock_info.vmid);
                thread_func = std::bind(&AudioDeviceManager::PCMQueueFunc, this, _1);
                thread_priority = 0;
            break;
        }

        thread_obj = std::thread(thread_func, sock_info);
        pthread_getschedparam(thread_obj.native_handle(), &policy, &thread_params);
        thread_params.sched_priority = thread_priority;
        pthread_setschedparam(thread_obj.native_handle(), policy, &thread_params);
        thread_obj.detach();
    }
}

void AudioDeviceManager::CtlQueueFunc(struct SocketInfo sock_info)
{
    int32_t ret = 0;
    char *buff = nullptr;
    uint32_t buff_size;
    std::future<uint32_t> future;
    struct virtio_snd_hdr *hdr;

    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Enter");

    struct PluginUtils utils;
    utils.sciu = isciu_;

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "SoundCard Built");

    std::pair<int32_t, std::vector<FrontendInfo>> fe_dais(sock_info.vmid, *fe_info_);
    all_fe_dais_.insert(fe_dais);

    std::pair<int32_t, std::vector<SoundCardMixerCtl>> mixer_ctls(sock_info.vmid, *sc_mixer_ctl_);
    all_mixer_ctls_.insert(mixer_ctls);

    utils.pcm_plugin = iaudio_plugin_;
    plugin_utils_.insert(std::pair<int32_t, struct PluginUtils>(sock_info.vmid, utils));
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "CSD2AP Built");

    buff = static_cast<char*>(calloc(1, MAX_HAB_BUFFER_SIZE));
    if (buff == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "CtlQueueFunc failed to allocate memory");
        return;
    }

    while (1)
    {
        buff_size = MAX_HAB_BUFFER_SIZE;
        ret = ihab_->Receive(sock_info.vcid, buff, &buff_size, -1, 0);
        if (buff_size > (uint32_t) MAX_HAB_BUFFER_SIZE)
        {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "receive buffer is too small, allocated size: %d, received: %d", MAX_HAB_BUFFER_SIZE, buff_size);
        }
        else if (ret)
        {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "receive failed, ret %d", ret);
            if (ret == -ENODEV) {
                for(auto &iter : dma_export_map_[sock_info.vmid]) {
                    uint32_t export_id = iter.second.export_id;
                    int heap_fd = iter.second.fd;
                    uint32_t vcid = iter.second.vcid;
                    ihab_->Unimport(vcid, export_id, (void*)heap_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                    ihab_->Close(vcid);
                }

                for(auto &iter : push_pull_map_[sock_info.vmid]) {
                    uint32_t data_export_id = iter.second.data_export_id;
                    uint32_t pos_export_id = iter.second.pos_export_id;
                    uint32_t data_fd = iter.second.data_fd;
                    uint32_t pos_fd = iter.second.pos_fd;
                    ihab_->Unimport(sock_info.vcid, data_export_id, (void*)data_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                    ihab_->Unimport(sock_info.vcid, pos_export_id, (void*)pos_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                }

                ihab_->Close(sock_info.vcid);

                for (auto &iter : pcm_stream_threads_[sock_info.vmid]) {
                    {
                        std::unique_lock<std::mutex> stream_lock(iter.second->stream_mutex);
                        iter.second->stream_cond.wait(stream_lock, [stream=iter.second] {
                                                      return stream->msg_queue.empty();
                                                      });
                    }

                    iter.second->msg_queue.push(nullptr);
                    iter.second->stream_cond.notify_all();
                    if (iter.second->thread_obj.joinable())
                        iter.second->thread_obj.join();

                    if (plugin_utils_[sock_info.vmid].pcm_plugin) {
                        plugin_utils_[sock_info.vmid].pcm_plugin->StreamStop(iter.first);
                        plugin_utils_[sock_info.vmid].pcm_plugin->StreamRelease(iter.first);
                    }

                    delete iter.second;
                }

                dma_export_map_[sock_info.vmid].clear();
                push_pull_map_[sock_info.vmid].clear();
                pcm_stream_threads_.erase(sock_info.vmid);
                break;
            }
        }
        else
        {
            hdr = (struct virtio_snd_hdr*)buff;
            future = std::async(std::launch::async, [&]() {
                return ProcessCtlMsg(buff, buff_size, sock_info);
            });

            if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessCtlMsg hdr->code:%04x timeout, skip and continue", hdr->code);
                continue;
            } else {
                buff_size = future.get();
            }
            AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Sending response to virtio-snd, size: %d", buff_size);
            ihab_->Send(sock_info.vcid, buff, buff_size, 0);
        }
    }

    if (buff != nullptr)
    {
        free(buff);
        buff = nullptr;
    }
}

void AudioDeviceManager::PCMQueueFunc(struct SocketInfo sock_info)
{
    int32_t ret = 0;
    uint32_t buff_size;
    uint32_t stream_id;
    unsigned int direction = sock_info.mmid == MM_AUD_3 ? SNDRV_PCM_STREAM_CAPTURE : SNDRV_PCM_STREAM_PLAYBACK;
    string fe_dai_name;

    while (1)
    {
        struct virtio_pcm_msg* pcm_msg = static_cast<struct virtio_pcm_msg*>(calloc(1, sizeof(struct virtio_pcm_msg)));
        if (pcm_msg == nullptr){
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "PCMQueueFunc failed to allocate memory");
            break;
        }

        buff_size = sizeof(struct virtio_pcm_msg);

        ret = ihab_->Receive(sock_info.vcid, (void*)pcm_msg, &buff_size, -1, 0);
        if (buff_size > (uint32_t)sizeof(struct virtio_pcm_msg))
        {
            // receive buffer is too small, alloc %d, actual %d
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "receive buffer is too small, allocated size: %d, received: %d", sizeof(struct virtio_pcm_msg), buff_size);
        }
        else if (ret)
        {
            // receive failed, ret %d, recv size %d
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "receive failed, ret %d", ret);
            if (ret == -ENODEV) {
                ihab_->Close(sock_info.vcid);
                break;
            }
        }
        else
        {
            stream_id = ((struct virtio_pcm_msg*)pcm_msg)->xfer.stream_id;
            fe_dai_name = all_fe_dais_[sock_info.vmid][stream_id].name;
            struct PCMMsgArgs args = {stream_id, sock_info, direction};
            std::lock_guard<std::mutex> stream_threads_lock(pcm_stream_threads_mutex);
            if (pcm_stream_threads_[sock_info.vmid].find(fe_dai_name) == \
                pcm_stream_threads_[sock_info.vmid].end()) {
                std::function<void(PCMMsgArgs)> pcm_func = std::bind(&AudioDeviceManager::ProcessPCMMsg, this, _1);
                PCMStreamThread *stream_thread = new PCMStreamThread;
                pcm_stream_threads_[sock_info.vmid][fe_dai_name] = stream_thread;
                pcm_stream_threads_[sock_info.vmid][fe_dai_name]->pcm_msg_complt.vcid \
                = sock_info.vcid;
                stream_thread->thread_obj = std::thread(pcm_func, args);
            }

            {
                std::lock_guard<std::mutex> stream_queue_lock(pcm_stream_threads_[sock_info.vmid][fe_dai_name]->stream_mutex);
                pcm_stream_threads_[sock_info.vmid][fe_dai_name]->msg_queue.push(pcm_msg);
            }

            pcm_stream_threads_[sock_info.vmid][fe_dai_name]->stream_cond.notify_one();
        }
    }
}

void AudioDeviceManager::EventQueueFunc(struct SocketInfo sock_info)
{
    int32_t ret = 0;
    char *buff = nullptr;
    uint32_t buff_size = MAX_HAB_BUFFER_SIZE;
    std::function<void(enum SSRState)> ssr_cb_func = std::bind(&AudioDeviceManager::SSRCallbackFunc, this, sock_info, _1);

    buff = static_cast<char*>(calloc(1, MAX_HAB_BUFFER_SIZE));
    if (buff == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "EventQueueFunc failed to allocate memory");
        return;
    }

   // wait if ctlqueue has not created the plugins yet
    while (plugin_utils_.find(sock_info.vmid) == plugin_utils_.end()) {
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Plugin not created yet, sleeping for 10ms");
        std::this_thread::sleep_for(10ms);
    }

    // Register callback for SSR
    ret = plugin_utils_[sock_info.vmid].pcm_plugin->RegisterSSREvent(ssr_cb_func);
    if (ret)
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Failed to register for SSR events");

    while (1)
    {
      ret = ihab_->Receive(sock_info.vcid, buff, &buff_size, -1, 0);

      // ssr is unregistered when audio plugin is deleted by the control queue
      if (ret == -ENODEV) {
        ihab_->Close(sock_info.vcid);
        break;
      }
    }

    if (buff != nullptr)
    {
        free(buff);
        buff = nullptr;
    }
}

void AudioDeviceManager::SSRCallbackFunc(struct SocketInfo sock_info, enum SSRState ssr_state)
{
    struct virtio_snd_event ssr_msg;

    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "SSR event received, ssr state %d", ssr_state);

    ssr_msg.hdr.code = VIRTIO_SND_EVT_SSR;
    ssr_msg.data = (uint32_t)ssr_state;
    ihab_->Send(sock_info.vcid, &ssr_msg, sizeof(struct virtio_snd_event), 0);

    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "SSRCallbackFunc: Exit");
}

uint32_t AudioDeviceManager::ProcessCtlMsg(void *buff, uint32_t buff_size, struct SocketInfo sock_info)
{
    struct virtio_snd_hdr *hdr = (struct virtio_snd_hdr*) buff;
    unsigned int gvm_id = sock_info.vmid;

    if (buff == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessCtlMsg Invalid Args");
        return buff_size;
    }

    switch (hdr->code) {
        case VIRTIO_SND_R_PCM_COUNT:
          ProcessPCMCount(*(struct virtio_snd_count_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_count_msg);
          break;

        case VIRTIO_SND_R_PCM_INFO:
          ProcessPCMInfo(*(struct virtio_snd_pcm_info_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_query_info) + sizeof(struct virtio_snd_hdr)
                      + sizeof(struct virtio_snd_pcm_info) * all_fe_dais_[gvm_id].size();
          break;

        case VIRTIO_SND_R_DC_COUNT:
          ProcessDCCount(*(struct virtio_snd_count_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_count_msg);
          break;

        case VIRTIO_SND_R_DC_INFO:
          ProcessDCInfo(*(struct virtio_snd_dc_info_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_query_info) + sizeof(struct virtio_snd_hdr);
          buff_size += ((struct virtio_snd_dc_info_msg*)buff)->request.count *
                       sizeof(struct virtio_snd_dc_info);
          break;

        case VIRTIO_SND_R_DC_ENUM_INFO:
          buff_size = ProcessDCEnumInfo(*(struct virtio_snd_dc_enum_info_msg*)buff, gvm_id);
          break;

        case VIRTIO_SND_R_PCM_SET_PARAMS:
          ProcessPCMSetParams(*(struct virtio_snd_pcm_set_params_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_pcm_set_params_msg);
          break;

        case VIRTIO_SND_R_PCM_PREPARE:
        case VIRTIO_SND_R_PCM_RELEASE:
          ProcessPCMPrepare(*(struct virtio_snd_pcm_prepare_msg*)buff, sock_info);
          buff_size = sizeof(struct virtio_snd_pcm_prepare_msg);
          break;

        case VIRTIO_SND_R_PCM_START:
        case VIRTIO_SND_R_PCM_STOP:
          ProcessPCMTrigger(*(struct virtio_snd_pcm_trigger_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_pcm_trigger_msg);
          break;

        case VIRTIO_SND_R_PCM_MMAP:
          ProcessPCMMmap(*(struct virtio_snd_pcm_mmap_msg*)buff, sock_info);
          buff_size = sizeof(struct virtio_snd_pcm_mmap_msg);
          break;

        case VIRTIO_SND_R_DC_READ:
          ProcessDCRead(*(struct virtio_snd_dc_read_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_dc_read_msg);
          break;

        case VIRTIO_SND_R_DC_WRITE:
          ProcessDCWrite(*(struct virtio_snd_dc_write_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_dc_write_msg);
          break;

        case VIRTIO_SND_R_DC_TLV_READ:
          ProcessDCTlvRead(*(struct virtio_snd_dc_tlv_read_msg*)buff, gvm_id);
          buff_size = sizeof(struct virtio_snd_dc_hdr) + sizeof(virtio_snd_hdr) + 1024;
          break;

        case VIRTIO_SND_R_DC_TLV_WRITE:
          ProcessDCTlvWrite(*(struct virtio_snd_dc_tlv_write_msg*)buff, buff_size, gvm_id);
          buff_size += sizeof(struct virtio_snd_hdr);
          break;

        default:
          break;
    }

    return buff_size;
}

void AudioDeviceManager::ProcessPCMCount(struct virtio_snd_count_msg &buff, unsigned int gvm_id)
{
    int num_pcm = 0;

    if (plugin_utils_[gvm_id].sciu == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMCount failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    num_pcm = plugin_utils_[gvm_id].sciu->GetNumPCMSubstreams();
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Number of PCM is %d", num_pcm);

    buff.response.code = VIRTIO_SND_S_OK;
    buff.count = num_pcm;
}

void AudioDeviceManager::ProcessPCMInfo(struct virtio_snd_pcm_info_msg &buff, unsigned int gvm_id)
{
    std::vector<FrontendInfo> fe_info;

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMInfo failed to find fe dais for gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_info = all_fe_dais_[gvm_id];
    buff.response.code = VIRTIO_SND_S_OK;

    for (int i = 0; i < fe_info.size(); i++) {
        buff.response_ext[i].hdr.hda_fn_nid = fe_info[i].id;
        buff.response_ext[i].features = fe_info[i].features;
        buff.response_ext[i].formats = fe_info[i].formats;
        buff.response_ext[i].rates = fe_info[i].rates;
        buff.response_ext[i].channels_min = fe_info[i].channels_min;
        buff.response_ext[i].channels_max = fe_info[i].channels_max;
        buff.response_ext[i].direction = fe_info[i].direction;
    }
}

void AudioDeviceManager::ProcessDCCount(struct virtio_snd_count_msg &buff, unsigned int gvm_id)
{
    int num_ctls;

    if (plugin_utils_[gvm_id].sciu == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessDCCount failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    num_ctls = plugin_utils_[gvm_id].sciu->GetNumMixerCtls();
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Number of mixer ctls is %d", num_ctls);

    buff.response.code = VIRTIO_SND_S_OK;
    buff.count = num_ctls;
}

void AudioDeviceManager::ProcessDCInfo(struct virtio_snd_dc_info_msg &buff, unsigned int gvm_id)
{
    std::vector<SoundCardMixerCtl> all_ctls;
    uint16_t start_id;
    uint16_t count;

    AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Enter");

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessDCInfo failed to find mixer ctls for gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    all_ctls = all_mixer_ctls_[gvm_id];
    start_id = buff.request.start_id;
    count = buff.request.count;

    buff.response.code = VIRTIO_SND_S_OK;

    for (int i = start_id; i < start_id + count; i++) {
        buff.response_ext[i - start_id].hdr.hda_fn_nid = i;
        snprintf(reinterpret_cast<char*>(buff.response_ext[i - start_id].elem_info.id.name), SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", all_ctls[i].id.name.c_str());
        buff.response_ext[i - start_id].elem_info.id.iface = all_ctls[i].id.iface;
        buff.response_ext[i - start_id].elem_info.access = all_ctls[i].access;
        buff.response_ext[i - start_id].elem_info.type = all_ctls[i].type;
        if (all_ctls[i].type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
            buff.response_ext[i - start_id].elem_info.count = 4;
            buff.response_ext[i - start_id].elem_info.value.integer.min = 0;
            buff.response_ext[i - start_id].elem_info.value.integer.max = 384000;
            buff.response_ext[i - start_id].elem_info.value.integer.step = 1;
        }
        else if (all_ctls[i].type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
            buff.response_ext[i - start_id].elem_info.count = 1;
            buff.response_ext[i - start_id].elem_info.value.enumerated.items = all_ctls[i].value.enumerated.items + 1;
        }
        else
            buff.response_ext[i - start_id].elem_info.count = 1024;
    }
}


uint32_t AudioDeviceManager::ProcessDCEnumInfo(struct virtio_snd_dc_enum_info_msg &buff, unsigned int gvm_id)
 {
    std::vector<VirtioSndDCEnum> dc_enums;
    int rc = SCIU_EOK;
    uint32_t size = 0;

    if (plugin_utils_[gvm_id].sciu == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessDCEnumInfo failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return size;
    }

    rc = plugin_utils_[gvm_id].sciu->GetEnumsForControl(buff.request.control_id, dc_enums);
    if (rc != SCIU_EOK) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "failed to get enums for mixer control");
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return size;
    }

    size = sizeof(struct virtio_snd_hdr) + sizeof(struct virtio_snd_dc_hdr);
    buff.response.code = VIRTIO_SND_S_OK;

    snprintf(reinterpret_cast<char*>(buff.response_ext[0].name), VIRTIO_SND_DC_NAME_MAXLEN, "%s", "ZERO");

    size += sizeof(struct virtio_snd_dc_enum_value);

    for (int i = 0; i < dc_enums.size(); i++) {
        snprintf(reinterpret_cast<char*>(buff.response_ext[i+1].name), VIRTIO_SND_DC_NAME_MAXLEN, "%s", reinterpret_cast<const char*>(dc_enums[i].dai_name));
        size += sizeof(struct virtio_snd_dc_enum_value);
    }

    return size;
 }

void AudioDeviceManager::ProcessPCMSetParams(struct virtio_snd_pcm_set_params_msg &buff, unsigned int gvm_id)
 {
    uint32_t ret = 0;
    struct PCMSetParams pcm_params;
    struct FrontendInfo fe_info;

    AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMSetParams failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_info = all_fe_dais_[gvm_id][buff.request.hdr.stream_id];
    pcm_params.buffer_bytes = buff.request.buffer_bytes;
    pcm_params.period_bytes = buff.request.period_bytes;
    pcm_params.features = buff.request.features;
    pcm_params.format = buff.request.format;
    pcm_params.rate = buff.request.rate;
    pcm_params.channels = buff.request.channels;
    pcm_params.is_mmap_noirq = buff.request.is_mmap_noirq;


     if (fe_info.direction == SNDRV_PCM_STREAM_PLAYBACK)
        ret = plugin_utils_[gvm_id].pcm_plugin->StreamSetParam(fe_info.name, pcm_params);
    else
        ret = plugin_utils_[gvm_id].pcm_plugin->SetParamForCapture(fe_info.name, pcm_params);

    if (!ret)
        buff.response.code = VIRTIO_SND_S_OK;
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;
 }

void AudioDeviceManager::ProcessPCMPrepare(struct virtio_snd_pcm_prepare_msg &buff, struct SocketInfo sock_info)
 {
    uint32_t ret = 0;
    struct FrontendInfo fe_info;
    struct virtio_pcm_msg *msg = nullptr;
    int32_t vcid;

    AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Enter");

    if (all_fe_dais_.find(sock_info.vmid) == all_fe_dais_.end() ||
        plugin_utils_[sock_info.vmid].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMPrepare failed to find gvm id: %d", sock_info.vmid);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_info = all_fe_dais_[sock_info.vmid][buff.request.stream_id];

    if (buff.request.hdr.code == VIRTIO_SND_R_PCM_PREPARE)
        ret = plugin_utils_[sock_info.vmid].pcm_plugin->StreamPrepare(fe_info.name);

    else {
        if (pcm_stream_threads_[sock_info.vmid].find(fe_info.name) == pcm_stream_threads_[sock_info.vmid].end()) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMPrepare failed to "
            "find fe dai name: %s", fe_info.name.c_str());
        } else {
            std::lock_guard<std::mutex> pcm_stream_threads_lock(pcm_stream_threads_mutex);
            while (!pcm_stream_threads_[sock_info.vmid][fe_info.name]->msg_queue.empty())
            {
                msg = pcm_stream_threads_[sock_info.vmid][fe_info.name]->msg_queue.front();
                pcm_stream_threads_[sock_info.vmid][fe_info.name]->msg_queue.pop();
                //send a fake msg
                msg->status.status = VIRTIO_SND_S_IO_ERR;
                msg->length += sizeof(msg->status);
                vcid = pcm_stream_threads_[sock_info.vmid][fe_info.name]->pcm_msg_complt.vcid;
                ihab_->Send(vcid, msg, sizeof(struct virtio_pcm_msg), 0);
                AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "send a fake msg ");
                free(msg);
            }
            delete pcm_stream_threads_[sock_info.vmid][fe_info.name];
            pcm_stream_threads_[sock_info.vmid].erase(fe_info.name);
        }

        ret = plugin_utils_[sock_info.vmid].pcm_plugin->StreamRelease(fe_info.name);

        if (dma_export_map_[sock_info.vmid].find(fe_info.id) != dma_export_map_[sock_info.vmid].end()) {
          uint32_t export_id = dma_export_map_[sock_info.vmid][fe_info.id].export_id;
          int heap_fd = dma_export_map_[sock_info.vmid][fe_info.id].fd;
          uint32_t vcid = dma_export_map_[sock_info.vmid][fe_info.id].vcid;
          ihab_->Unimport(vcid, export_id, (void*)heap_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
          dma_export_map_[sock_info.vmid].erase(fe_info.id);
        }
        else if (push_pull_map_[sock_info.vmid].find(fe_info.id) != push_pull_map_[sock_info.vmid].end()) {
          uint32_t data_export_id = push_pull_map_[sock_info.vmid][fe_info.id].data_export_id;
          uint32_t pos_export_id = push_pull_map_[sock_info.vmid][fe_info.id].pos_export_id;
          uint32_t data_fd = push_pull_map_[sock_info.vmid][fe_info.id].data_fd;
          uint32_t pos_fd = push_pull_map_[sock_info.vmid][fe_info.id].pos_fd;

          ihab_->Unimport(sock_info.vcid, data_export_id, (void*)data_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
          ihab_->Unimport(sock_info.vcid, pos_export_id, (void*)pos_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
          push_pull_map_[sock_info.vmid].erase(fe_info.id);
        }
    }
    if (!ret)
        buff.response.code = VIRTIO_SND_S_OK;
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;
 }

void AudioDeviceManager::ProcessPCMTrigger(struct virtio_snd_pcm_trigger_msg &buff, unsigned int gvm_id)
 {
    uint32_t ret = 0;
    struct FrontendInfo fe_info;

    AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMTrigger failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_info = all_fe_dais_[gvm_id][buff.request.stream_id];

    if (buff.request.hdr.code == VIRTIO_SND_R_PCM_START)
        ret = plugin_utils_[gvm_id].pcm_plugin->StreamStart(fe_info.name);
    else {
        if (pcm_stream_threads_[gvm_id].find(fe_info.name) != pcm_stream_threads_[gvm_id].end()) {
            {
                std::unique_lock<std::mutex> stream_lock(pcm_stream_threads_[gvm_id][fe_info.name]->stream_mutex);
                pcm_stream_threads_[gvm_id][fe_info.name]->stream_cond.wait(stream_lock,
                                [this,gvm_id=gvm_id,fe_dai_name=fe_info.name]{
                                return pcm_stream_threads_[gvm_id][fe_dai_name]->msg_queue.empty();
                                });
            }

            pcm_stream_threads_[gvm_id][fe_info.name]->msg_queue.push(NULL);
            pcm_stream_threads_[gvm_id][fe_info.name]->stream_cond.notify_all();
            if (pcm_stream_threads_[gvm_id][fe_info.name]->thread_obj.joinable())
                pcm_stream_threads_[gvm_id][fe_info.name]->thread_obj.join();

            ret = plugin_utils_[gvm_id].pcm_plugin->StreamStop(fe_info.name);
        }
    }

    if (!ret)
        buff.response.code = VIRTIO_SND_S_OK;
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;

 }

void AudioDeviceManager::ProcessPCMMmap(struct virtio_snd_pcm_mmap_msg &buff, struct SocketInfo sock_info)
 {
    uint32_t ret = 0;
    uint32_t fe_id;
    string fe_dai_name;
    struct PushPullInfo push_pull_info;
    struct PCMMmapParams mmap_params;

    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Enter");

    if (all_fe_dais_.find(sock_info.vmid) == all_fe_dais_.end() ||
        plugin_utils_[sock_info.vmid].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMMmap failed to find gvm id: %d", sock_info.vmid);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_id = all_fe_dais_[sock_info.vmid][buff.request.hdr.stream_id].id;
    fe_dai_name = all_fe_dais_[sock_info.vmid][buff.request.hdr.stream_id].name;

    if (push_pull_map_[sock_info.vmid].find(fe_id) != push_pull_map_[sock_info.vmid].end()) {
        AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Push Pull fds already imported");
        return;
    }

    ret = ihab_->Import(sock_info.vcid, (void**)&mmap_params.data_fd,
                      buff.request.data_size, buff.request.data_export_id, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
    if(ret) {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Hab Import failed for data buffer, skipping MMAP operation");
          goto response;
        }

    ret = ihab_->Import(sock_info.vcid, (void**)&mmap_params.pos_fd,
                      buff.request.pos_size, buff.request.pos_export_id, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
    if(ret) {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Hab Import failed for position buffer, skipping MMAP operation");
          goto response;
        }

    mmap_params.data_gvm_fd = buff.request.data_fd;
    mmap_params.pos_gvm_fd = buff.request.pos_fd;
    mmap_params.data_size = buff.request.data_size;
    mmap_params.pos_size = buff.request.pos_size;
    ret = plugin_utils_[sock_info.vmid].pcm_plugin->StreamMmap(fe_dai_name, mmap_params);
    if (ret) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Push-Pull configuration failed");
        goto response;
    }

    if (pcm_stream_threads_[sock_info.vmid].find(fe_dai_name) == pcm_stream_threads_[sock_info.vmid].end()) {
        struct PCMMsgArgs args = {buff.request.hdr.stream_id, sock_info, 0};
        std::function<void(PCMMsgArgs)> pcm_func = std::bind(&AudioDeviceManager::ProcessPCMMsg, this, _1);
        PCMStreamThread *stream_thread = new PCMStreamThread;
        pcm_stream_threads_[sock_info.vmid][fe_dai_name] = stream_thread;
        stream_thread->thread_obj = std::thread(pcm_func, args);
    }

    // cache export ids for use when unimporting memory
    push_pull_info.data_export_id = buff.request.data_export_id;
    push_pull_info.pos_export_id = buff.request.pos_export_id;
    push_pull_info.data_fd = mmap_params.data_fd;
    push_pull_info.pos_fd = mmap_params.pos_fd;
    push_pull_map_[sock_info.vmid][fe_id] = push_pull_info;

response:
    if (!ret)
        buff.response.code = VIRTIO_SND_S_OK;
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;
 }

void AudioDeviceManager::ProcessDCRead(struct virtio_snd_dc_read_msg &buff, unsigned int gvm_id)
 {
    uint32_t ret = 0;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessDCRead failed to find mixer ctls for gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];

    AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Processing read for control %s", ctl_info.id.name.c_str());

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;

    ret = plugin_utils_[gvm_id].pcm_plugin->DcRead(elem_payload);
    if (ret) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "DcRead failed");
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    buff.response.code = VIRTIO_SND_S_OK;
    if(elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
        memset(&buff.response_ext.value.integer.value, 0, sizeof(elem_payload.value));
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
        buff.response_ext.value.enumerated.item[0] = 0;
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES) {
        memcpy(buff.response_ext.value.bytes.data, elem_payload.value.bytes.data, sizeof(elem_payload.value.bytes.data));
    }
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;


    buff.response_ext.id.numid = buff.request.control_id + 1;
    snprintf(reinterpret_cast<char*>(buff.response_ext.id.name), ctl_info.id.name.length(), "%s", ctl_info.id.name.c_str());
 }

void AudioDeviceManager::ProcessDCWrite(struct virtio_snd_dc_write_msg &buff, unsigned int gvm_id)
 {
    uint32_t ret = 0;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessDCWrite failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];

    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Processing write for control %s", ctl_info.id.name.c_str());

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;

    if(elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
        elem_payload.value.integer.value[0] = buff.request_ext.value.integer.value[0];
        elem_payload.value.integer.value[1] = buff.request_ext.value.integer.value[1];
        elem_payload.value.integer.value[2] = buff.request_ext.value.integer.value[2];
        elem_payload.value.integer.value[3] = buff.request_ext.value.integer.value[3];
    }
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        elem_payload.value.enumerated.item[0] = buff.request_ext.value.enumerated.item[0];
    }
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES)
        memcpy(&elem_payload.value.bytes.data, &buff.request_ext.value.bytes.data,
               sizeof(elem_payload.value.bytes.data));

    ret = plugin_utils_[gvm_id].pcm_plugin->DcWrite(elem_payload);
    if (ret) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Mixer control write for control %s failed", ctl_info.id.name.c_str());
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    buff.response.code = VIRTIO_SND_S_OK;
 }

void AudioDeviceManager::ProcessDCTlvRead(struct virtio_snd_dc_tlv_read_msg &buff, unsigned int gvm_id)
 {
    uint32_t ret = 0;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessDCTlvRead failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];

    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,  "Reading TLV value for control %s", ctl_info.id.name.c_str());

    memset(&buff.tlv[0], 0, 1024);

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = tlv_bytes;
    elem_payload.value.tlv.tlv = (uint8_t*)&buff.tlv[0];

    ret = plugin_utils_[gvm_id].pcm_plugin->DcRead(elem_payload);
    if (ret) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "DcRead failed for TLV payload");
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    buff.response.code = VIRTIO_SND_S_OK;
 }

 void AudioDeviceManager::ProcessDCTlvWrite(struct virtio_snd_dc_tlv_write_msg &buff, uint32_t buff_size, unsigned int gvm_id)
 {
    uint32_t ret = 0;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;
    struct virtio_snd_hdr response;
    unsigned int tlv_size = 0;
    uint8_t *tlv_to_write;

    AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Enter");
    if (buff_size < sizeof(struct virtio_snd_dc_hdr) ||
        all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessDCTlvWrite failed to find gvm id: %u", gvm_id);
        response.code = VIRTIO_SND_S_IO_ERR;
        goto response;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];
    tlv_size = buff_size - sizeof(struct virtio_snd_dc_hdr);

    tlv_to_write = static_cast<uint8_t*>(calloc(1, tlv_size*sizeof(uint8_t)));
    if (tlv_to_write == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "failed to allocate for tlv to write");
        response.code = VIRTIO_SND_S_IO_ERR;
        goto response;
    }

    memset(tlv_to_write, 0, tlv_size);

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = tlv_bytes;

    memcpy(tlv_to_write, (unsigned char*)&buff + sizeof(struct virtio_snd_dc_hdr), tlv_size);

    elem_payload.value.tlv.tlv = tlv_to_write;

    for (int i = 0; i < tlv_size; i++)
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "TLV value [%d]: 0x%x", i, tlv_to_write[i]);

    ret = plugin_utils_[gvm_id].pcm_plugin->DcWrite(elem_payload);
    if (!ret) {
        response.code = VIRTIO_SND_S_OK;
    }
    else {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Mixer control TLV write for control %s failed", ctl_info.id.name.c_str());
        response.code = VIRTIO_SND_S_IO_ERR;
    }

    free(tlv_to_write);

response:
    memcpy((unsigned char*)&buff + sizeof(struct virtio_snd_dc_hdr) + tlv_size, &response, sizeof(response));

 }

 void AudioDeviceManager::ProcessPCMMsg(struct PCMMsgArgs args)
 {
    int heap_fd = -1;
    struct virtio_snd_pcm_buf_msg pcm_buf_msg;
    struct DMAExport dma_area_map;
    unsigned int direction = args.direction;
    struct SocketInfo sock_info = args.sock_info;
    struct virtio_pcm_msg *msg = nullptr;
    unsigned int gvm_id = sock_info.vmid;
    struct FrontendInfo fe_info;
    uint32_t ret = 0;

    AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMMsg failed to "
        "find gvm id: %u", gvm_id);
        return;
    }

    fe_info = all_fe_dais_[gvm_id][args.stream_id];

    if (pcm_stream_threads_[gvm_id].find(fe_info.name) == pcm_stream_threads_[gvm_id].end()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ProcessPCMMsg failed to "
        "find fe dai name: %s", fe_info.name.c_str());
        return;
    }

    while(1) {
    {
      std::unique_lock<std::mutex> stream_lock(pcm_stream_threads_[gvm_id][fe_info.name]->stream_mutex);
      pcm_stream_threads_[gvm_id][fe_info.name]->stream_cond.wait(stream_lock,
                         [this,gvm_id=gvm_id,fe_dai_name=fe_info.name] {
                         return !pcm_stream_threads_[gvm_id][fe_dai_name]->msg_queue.empty();
                         });

      msg = pcm_stream_threads_[gvm_id][fe_info.name]->msg_queue.front();
      pcm_stream_threads_[gvm_id][fe_info.name]->msg_queue.pop();

      // NULL msg sent by destructor to terminate thread. Cleanup and exit
      if (msg == NULL) {
        AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "msg is NULL");
        return;
      }
    }

    if (dma_export_map_[gvm_id].find(fe_info.id) != dma_export_map_[gvm_id].end())
    {
        heap_fd = dma_export_map_[gvm_id][fe_info.id].fd;
    }
    else
    {
        ret = ihab_->Import(sock_info.vcid, (void**)&heap_fd, msg->desc.dma_bytes,
                         msg->desc.export_id, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
        if(ret) {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Hab Import failed, skipping PCM operation");
          msg->status.status = VIRTIO_SND_S_IO_ERR;
          msg->length += sizeof(msg->status);
          ihab_->Send(sock_info.vcid, msg, sizeof(struct virtio_pcm_msg), 0);
          free(msg);
          goto notify;
        }

        dma_area_map.export_id = msg->desc.export_id;
        dma_area_map.fd = heap_fd;
        dma_area_map.vcid = sock_info.vcid;
        dma_export_map_[gvm_id][fe_info.id] = dma_area_map;
    }

    pcm_buf_msg.offset = msg->desc.offset;
    pcm_buf_msg.size = msg->length;
    pcm_buf_msg.fe_dai_name = fe_info.name;
    pcm_buf_msg.buff_fd = (int64_t)heap_fd;
    pcm_buf_msg.dma_bytes = msg->desc.dma_bytes;

    {
        std::lock_guard<std::mutex> pcm_msg_lock(msg_mutex);

        pcm_stream_threads_[gvm_id][fe_info.name]->pcm_msg_complt.pcm_msg_map[msg->desc.offset] = msg;
        if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
            ret = plugin_utils_[gvm_id].pcm_plugin->StreamWrite(pcm_buf_msg);
        }
        else {
            ret = plugin_utils_[gvm_id].pcm_plugin->StreamRead(pcm_buf_msg);
        }

        if (ret && msg != nullptr)
        {
            AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Hab responding "
            "to virtio sound: ret %d", (uint32_t)ret);
            msg->status.status = VIRTIO_SND_S_IO_ERR;
            msg->length += sizeof(msg->status);
            ihab_->Send(sock_info.vcid, msg, sizeof(struct virtio_pcm_msg), 0);
            free(msg);
            pcm_stream_threads_[gvm_id][fe_info.name]->pcm_msg_complt.pcm_msg_map.at(msg->desc.offset) = nullptr;
        }
    }

notify:
    {
      std::lock_guard<std::mutex> empty_lock(pcm_stream_threads_[gvm_id][fe_info.name]->stream_mutex);
      if (pcm_stream_threads_[gvm_id][fe_info.name]->msg_queue.empty())
        pcm_stream_threads_[gvm_id][fe_info.name]->stream_cond.notify_one();
    }
  }
}
