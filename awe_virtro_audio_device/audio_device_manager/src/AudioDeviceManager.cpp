/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <thread>
#include <chrono>
#include <pthread.h>
#include <sys/mman.h>
#include <future>

#ifdef ENABLE_RT_THREAD_PCMMSG
#include "amss/compresmgr_client_api.h"
#endif

#include "AudioDeviceManager.h"

#define MAX_HAB_BUFFER_SIZE 4096
#define RT_THREAD_PCMMSG_DEFAULT_POLICY SCHED_FIFO
#define RT_THREAD_PCMMSG_DEFAULT_PRIORITY 45

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
#ifdef OFF_TARGET_UNIT_TEST
void AudioDeviceManager::EmptyFuncSockinfo(struct SocketInfo sock_info)
{
    return;
}

void AudioDeviceManager::EmptyFuncPCMMsgArgs(struct PCMMsgArgs args)
{
    return;
}
#endif

void AudioDeviceManager::OpenCommChannel(uint32_t mmid)
{
    int32_t ret = 0;

    std::string threadName = "awe_adm_hab_" + std::to_string(mmid);
    pthread_setname_np(pthread_self(), threadName.c_str());
    while (1)
    {
        struct SocketInfo sock_info = {0};
        std::function<void(struct SocketInfo)> thread_func;
        std::thread thread_obj;
        unsigned int thread_priority = 10;
        int policy;
        sched_param thread_params;

        AWE_LOGI("Calling HAB open for mmid %d", mmid);
        ret = ihab_->Open(&sock_info.vcid, mmid, -1, 0);

        if (ret)
        {
            AWE_LOGE("Socket Open failed for mmid %d", mmid);
            // After KPI optimization, we are not able to open the /dev/hab until certain point.
            std::this_thread::sleep_for(100ms);
#ifdef OFF_TARGET_UNIT_TEST
            return;
#else
            continue;
#endif
        }
        ret = ihab_->SocketQuery(sock_info.vcid, &sock_info, 0);

        if (ret)
        {
            AWE_LOGE("Socket query failed for mmid %d, assuming GVM 1", mmid);
            sock_info.vmid = 1;
        }

        sock_info.mmid = mmid;
        switch (mmid) {
            case MM_AUD_1:
                AWE_LOGI("Launching CtlQueueFunc, mmid: %d, vmid: %d", mmid, sock_info.vmid);
#ifdef OFF_TARGET_UNIT_TEST
                thread_func = std::bind(&AudioDeviceManager::EmptyFuncSockinfo, this, _1);
#else
                thread_func = std::bind(&AudioDeviceManager::CtlQueueFunc, this, _1);
#endif
                break;
            case MM_AUD_2:
                AWE_LOGI("Launching EventQueueFunc, mmid: %d, vmid: %d", mmid, sock_info.vmid);
#ifdef OFF_TARGET_UNIT_TEST
                thread_func = std::bind(&AudioDeviceManager::EmptyFuncSockinfo, this, _1);
#else
                thread_func = std::bind(&AudioDeviceManager::EventQueueFunc, this, _1);
#endif
                break;
            case MM_AUD_3:
            case MM_AUD_4:
                AWE_LOGI("Launching PCMQueueFunc, mmid: %d, vmid: %d", mmid, sock_info.vmid);
#ifdef OFF_TARGET_UNIT_TEST
                thread_func = std::bind(&AudioDeviceManager::EmptyFuncSockinfo, this, _1);
#else
                thread_func = std::bind(&AudioDeviceManager::PCMQueueFunc, this, _1);
#endif
                thread_priority = 0;
                break;
        }
        thread_obj = std::thread(thread_func, sock_info);
        pthread_getschedparam(thread_obj.native_handle(), &policy, &thread_params);
        thread_params.sched_priority = thread_priority;
        pthread_setschedparam(thread_obj.native_handle(), policy, &thread_params);
        thread_obj.detach();
#ifdef OFF_TARGET_UNIT_TEST
        return;
#endif
    }
}

void AudioDeviceManager::CtlQueueFunc(struct SocketInfo sock_info)
{
    int32_t ret = 0;
    char *buff = nullptr;
    uint32_t buff_size;
    std::future<uint32_t> future;
    std::future<uint32_t> stop_future;
    std::future<uint32_t> release_future;
    struct virtio_snd_hdr *hdr;

    AWE_LOGI("Enter");

    struct PluginUtils utils;
    utils.sciu = isciu_;

    AWE_LOGI("SoundCard Built");

    std::pair<int32_t, std::vector<FrontendInfo>> fe_dais(sock_info.vmid, *fe_info_);
    all_fe_dais_.insert(fe_dais);

    std::pair<int32_t, std::vector<SoundCardMixerCtl>> mixer_ctls(sock_info.vmid, *sc_mixer_ctl_);
    all_mixer_ctls_.insert(mixer_ctls);

    utils.pcm_plugin = iaudio_plugin_;
    plugin_utils_.insert(std::pair<int32_t, struct PluginUtils>(sock_info.vmid, utils));
    AWE_LOGI("CSD2AP Built");

    buff = static_cast<char*>(calloc(1, MAX_HAB_BUFFER_SIZE));
    if (buff == nullptr){
        AWE_LOGE("CtlQueueFunc failed to allocate memory");
        return;
    }

    pthread_setname_np(pthread_self(), "awe_adm_ctlq");

    while (1)
    {
        buff_size = MAX_HAB_BUFFER_SIZE;
        ret = ihab_->Receive(sock_info.vcid, buff, &buff_size, -1, 0);
        if (buff_size > (uint32_t) MAX_HAB_BUFFER_SIZE)
        {
            AWE_LOGE("receive buffer is too small, allocated size: %d, received: %d", MAX_HAB_BUFFER_SIZE, buff_size);
        }
        else if (ret)
        {
            AWE_LOGE("receive failed, ret %d", ret);
            if (ret == -ENODEV) {
                for (auto &iter: pcm_stream_threads_[sock_info.vmid]) {
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
                        stop_future = std::async(std::launch::async, [&]() {
                            return plugin_utils_[sock_info.vmid].pcm_plugin->StreamStop(iter.first);
                        });

                        release_future = std::async(std::launch::async, [&]() {
                            return plugin_utils_[sock_info.vmid].pcm_plugin->StreamRelease(iter.first);
                        });

                        if (stop_future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
                            AWE_LOGE("StreamStop timeout, skip and continue");
                        } else {
                            ret = stop_future.get();
                        }

                        if (release_future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
                            AWE_LOGE("StreamRelease timeout, skip and continue");
                        } else {
                            ret = release_future.get();
                        }
                    }

                    delete iter.second;
                }

                for (auto &iter : dma_export_map_[sock_info.vmid]) {
                    uint32_t export_id = iter.second.export_id;
                    int heap_fd = iter.second.fd;
                    uint32_t vcid = iter.second.vcid;
                    ihab_->Unimport(vcid, export_id, (void*)(uintptr_t)heap_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                }

                for (auto &iter : push_pull_map_[sock_info.vmid]) {
                    uint32_t data_export_id = iter.second.data_export_id;
                    uint32_t pos_export_id = iter.second.pos_export_id;
                    uint32_t data_fd = iter.second.data_fd;
                    uint32_t pos_fd = iter.second.pos_fd;
                    ihab_->Unimport(sock_info.vcid, data_export_id, (void*)(uintptr_t)data_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                    ihab_->Unimport(sock_info.vcid, pos_export_id, (void*)(uintptr_t)pos_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                }

                ihab_->Close(sock_info.vcid);

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
                AWE_LOGE("ProcessCtlMsg hdr->code:%04x timeout, skip and continue", hdr->code);
                continue;
            }

            buff_size = future.get();
            AWE_LOGD("Sending response to virtio-snd, size: %d", buff_size);
            ihab_->Send(sock_info.vcid, buff, buff_size, 0);
        }
#ifdef OFF_TARGET_UNIT_TEST
        return;
#endif
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
#if !defined(__QNX__)
    unsigned int direction = sock_info.mmid == MM_AUD_3 ? SNDRV_PCM_STREAM_CAPTURE : SNDRV_PCM_STREAM_PLAYBACK;
#else
    unsigned int direction = sock_info.mmid == MM_AUD_3 ? VIRTIO_STREAM_CAPTURE : VIRTIO_STREAM_PLAYBACK;
#endif
    string fe_dai_name;

    std::string threadName = "awe_adm_pcmq_" + std::to_string(direction);
    pthread_setname_np(pthread_self(), threadName.c_str());

    while (1)
    {
        struct virtio_pcm_msg* pcm_msg = static_cast<struct virtio_pcm_msg*>(calloc(1, sizeof(struct virtio_pcm_msg)));
        if (pcm_msg == nullptr){
            AWE_LOGE("PCMQueueFunc failed to allocate memory");
            break;
        }

        buff_size = sizeof(struct virtio_pcm_msg);

        ret = ihab_->Receive(sock_info.vcid, (void*)pcm_msg, &buff_size, -1, 0);

        if (buff_size > (uint32_t)sizeof(struct virtio_pcm_msg))
        {
            // receive buffer is too small, alloc %d, actual %d
            AWE_LOGE("receive buffer is too small, allocated size: %d, received: %d", sizeof(struct virtio_pcm_msg), buff_size);
        }
        else if (ret)
        {
            // receive failed, ret %d, recv size %d
            AWE_LOGE("receive failed, ret %d", ret);
            if (ret == -ENODEV) {
                if (pcm_msg)
                    free(pcm_msg);
                AWE_LOGI("Check if CtlQueue is done cleaning up");
                while(pcm_stream_threads_[sock_info.vmid].size() > 0) {
                    // wait for all unimport operations to complete
                    std::this_thread::sleep_for(1ms);
                }

                ihab_->Close(sock_info.vcid);
                AWE_LOGD("pcm msg hab closed");
                break;
            }
        }
        else
        {
            stream_id = ((struct virtio_pcm_msg*)pcm_msg)->xfer.stream_id;
            fe_dai_name = all_fe_dais_[sock_info.vmid][stream_id].name;
            struct PCMMsgArgs args = {stream_id, sock_info, direction};
            if (pcm_stream_threads_[sock_info.vmid].find(fe_dai_name) == pcm_stream_threads_[sock_info.vmid].end()) {
#ifdef OFF_TARGET_UNIT_TEST
                std::function<void(PCMMsgArgs)> pcm_func = std::bind(&AudioDeviceManager::EmptyFuncPCMMsgArgs, this, _1);
#else
                std::function<void(PCMMsgArgs)> pcm_func = std::bind(&AudioDeviceManager::ProcessPCMMsg, this, _1);
#endif
              PCMStreamThread *stream_thread = new PCMStreamThread;
              pcm_stream_threads_[sock_info.vmid][fe_dai_name] = stream_thread;
              stream_thread->thread_obj = std::thread(pcm_func, args);
            }
            {
              std::lock_guard<std::mutex> stream_queue_lock(pcm_stream_threads_[sock_info.vmid][fe_dai_name]->stream_mutex);
              pcm_stream_threads_[sock_info.vmid][fe_dai_name]->msg_queue.push(pcm_msg);
            }
            if (pcm_stream_threads_[sock_info.vmid][fe_dai_name])
                pcm_stream_threads_[sock_info.vmid][fe_dai_name]->stream_cond.notify_one();
        }
#ifdef OFF_TARGET_UNIT_TEST
        return;
#endif
    }
}

void AudioDeviceManager::EventQueueFunc(struct SocketInfo sock_info)
{
    int32_t ret = 0;
    char *buff = nullptr;
    uint32_t buff_size = MAX_HAB_BUFFER_SIZE;
    std::function<void(enum SSRState)> ssr_cb_func = std::bind(&AudioDeviceManager::SSRCallbackFunc, this, sock_info, _1);
    std::function<void(uint32_t control_id)> adm_event_cb_func = std::bind(&AudioDeviceManager::EventCallbackFunc, this, sock_info, _1);

    buff = static_cast<char*>(calloc(1, MAX_HAB_BUFFER_SIZE));
    if (buff == nullptr){
        AWE_LOGE("EventQueueFunc failed to allocate memory");
        return;
    }

   // wait if ctlqueue has not created the plugins yet
    while (plugin_utils_.find(sock_info.vmid) == plugin_utils_.end()) {
        AWE_LOGI("Plugin not created yet, sleeping for 10ms");
        std::this_thread::sleep_for(10ms);
#ifdef OFF_TARGET_UNIT_TEST
        return;
#endif
    }

    // Register callback for event
    ret = plugin_utils_[sock_info.vmid].pcm_plugin->RegisterEventCallback(adm_event_cb_func);
    if (ret)
        AWE_LOGE("Failed to register event callback");

    // Register callback for SSR
    ret = plugin_utils_[sock_info.vmid].pcm_plugin->RegisterSSREvent(ssr_cb_func);
    if (ret)
        AWE_LOGE("Failed to register for SSR events");

    pthread_setname_np(pthread_self(), "awe_adm_eventq");

    while (1)
    {
      ret = ihab_->Receive(sock_info.vcid, buff, &buff_size, -1, 0);

      // ssr is unregistered when audio plugin is deleted by the control queue
      if (ret == -ENODEV) {
        ihab_->Close(sock_info.vcid);
        break;
      }
#ifdef OFF_TARGET_UNIT_TEST
      return;
#endif
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

    AWE_LOGI("SSR event received, ssr state %d", ssr_state);

    ssr_msg.hdr.code = VIRTIO_SND_EVT_SSR;
    ssr_msg.data = (uint32_t)ssr_state;
    ihab_->Send(sock_info.vcid, &ssr_msg, sizeof(struct virtio_snd_event), 0);

    AWE_LOGI("SSRCallbackFunc: Exit");
}

void AudioDeviceManager::EventCallbackFunc(struct SocketInfo sock_info, uint32_t control_id)
{
    struct virtio_snd_dc_event event_msg;

    AWE_LOGI("Mixer event received for control id %d", control_id);

    event_msg.hdr.code = VIRTIO_SND_EVT_DC_NOTIFY;
    event_msg.control_id = control_id;
#if !defined(__QNX__)
    event_msg.mask = SNDRV_CTL_EVENT_MASK_VALUE;
#else
    event_msg.mask = VIRTIO_EVENT_MASK_VALUE;
#endif
    ihab_->Send(sock_info.vcid, &event_msg, sizeof(struct virtio_snd_dc_event), 0);
    AWE_LOGI("EventCallbackFunc: Exit");
}

uint32_t AudioDeviceManager::ProcessCtlMsg(void *buff, uint32_t buff_size, struct SocketInfo sock_info)
{
    struct virtio_snd_hdr *hdr = (struct virtio_snd_hdr*) buff;
    unsigned int gvm_id = sock_info.vmid;

    if (buff == nullptr) {
        AWE_LOGE("ProcessCtlMsg Invalid Args");
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
        case VIRTIO_SND_R_SHMEM_MAP:
            ProcessShmemMap(*(struct virtio_snd_dc_write_msg*)buff, sock_info);
            buff_size = sizeof(struct virtio_snd_dc_write_msg);
        break;
        case VIRTIO_SND_R_SHMEM_UNMAP:
            ProcessShmemUnmap(*(struct virtio_snd_dc_write_msg*)buff, sock_info);
            buff_size = sizeof(struct virtio_snd_dc_write_msg);
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
        AWE_LOGE("ProcessPCMCount failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    num_pcm = plugin_utils_[gvm_id].sciu->GetNumPCMSubstreams();
    AWE_LOGI("Number of PCM is %d", num_pcm);

    buff.response.code = VIRTIO_SND_S_OK;
    buff.count = num_pcm;
}

void AudioDeviceManager::ProcessPCMInfo(struct virtio_snd_pcm_info_msg &buff, unsigned int gvm_id)
{
    std::vector<FrontendInfo> fe_info;

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end()) {
        AWE_LOGE("ProcessPCMInfo failed to find fe dais for gvm id: %u", gvm_id);
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
        AWE_LOGE("ProcessDCCount failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    num_ctls = plugin_utils_[gvm_id].sciu->GetNumMixerCtls();
    AWE_LOGI("Number of mixer ctls is %d", num_ctls);

    buff.response.code = VIRTIO_SND_S_OK;
    buff.count = num_ctls;
}

void AudioDeviceManager::ProcessDCInfo(struct virtio_snd_dc_info_msg &buff, unsigned int gvm_id)
{
    std::vector<SoundCardMixerCtl> all_ctls;
    uint16_t start_id;
    uint16_t count;

    AWE_LOGD("Enter");

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end()) {
        AWE_LOGE("ProcessDCInfo failed to find mixer ctls for gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    all_ctls = all_mixer_ctls_[gvm_id];
    start_id = buff.request.start_id;
    count = buff.request.count;

    buff.response.code = VIRTIO_SND_S_OK;

#if !defined(__QNX__)
    for (int i = start_id; i < start_id + count; i++) {
        buff.response_ext[i - start_id].hdr.hda_fn_nid = i;
        snprintf(reinterpret_cast<char*>(buff.response_ext[i - start_id].elem_info.id.name), SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", all_ctls[i].id.name.c_str());
        buff.response_ext[i - start_id].elem_info.id.iface = all_ctls[i].id.iface;
        buff.response_ext[i - start_id].elem_info.access = all_ctls[i].access;
        buff.response_ext[i - start_id].elem_info.type = all_ctls[i].type;
        buff.response_ext[i - start_id].elem_info.count = all_ctls[i].count;
        if (all_ctls[i].type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
            buff.response_ext[i - start_id].elem_info.value.integer.min = all_ctls[i].value.integer.min;
            buff.response_ext[i - start_id].elem_info.value.integer.max = all_ctls[i].value.integer.max;
            buff.response_ext[i - start_id].elem_info.value.integer.step = all_ctls[i].value.integer.step;
        }
        else if (all_ctls[i].type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
            buff.response_ext[i - start_id].elem_info.value.enumerated.items = all_ctls[i].value.enumerated.items;
        }
        else if (all_ctls[i].type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
            buff.response_ext[i - start_id].elem_info.value.integer64.min = all_ctls[i].value.integer64.min;
            buff.response_ext[i - start_id].elem_info.value.integer64.max = all_ctls[i].value.integer64.max;
            buff.response_ext[i - start_id].elem_info.value.integer64.step = all_ctls[i].value.integer64.step;
        }
    }
#else
     for (int i = start_id; i < start_id + count; i++) {
        struct virtio_ctl_elem_info * tmp =(struct virtio_ctl_elem_info *) &buff.response_ext[i - start_id].elem_info;
        buff.response_ext[i - start_id].hdr.hda_fn_nid = i;
        snprintf(reinterpret_cast<char*>(((char *)&(buff.response_ext[i - start_id].elem_info)) + VIRTIO_CTL_ELEM_ID_NAME_OFFSET), VIRTIO_CTL_ELEM_ID_NAME_MAXLEN, "%s", all_ctls[i].id.name.c_str());
        COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_ID_IFACE_OFFSET,      &all_ctls[i].id.iface,  0, sizeof(all_ctls[i].id.iface));
        COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_ACCESS_OFFSET,   &all_ctls[i].access,    0, sizeof(all_ctls[i].access));
        COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_TYPE_OFFSET,     &all_ctls[i].type,      0, sizeof(all_ctls[i].type));
        COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_COUNT_OFFSET,    &all_ctls[i].count,     0, sizeof(all_ctls[i].count));
        if (all_ctls[i].type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER) {
            COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER_MIN_OFFSET,    &all_ctls[i].value.integer.min,     0, sizeof(all_ctls[i].value.integer.min));
            COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER_MAX_OFFSET,    &all_ctls[i].value.integer.max,     0, sizeof(all_ctls[i].value.integer.max));
            COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER_STEP_OFFSET,   &all_ctls[i].value.integer.step,    0, sizeof(all_ctls[i].value.integer.step));
        }
        else if (all_ctls[i].type == VIRTIO_MIXER_CTL_TYPE_MASK_ENUMERATED) {
            COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_ITEMS_OFFSET, &all_ctls[i].value.enumerated.items, 0, sizeof(all_ctls[i].value.enumerated.items));
        }
        else if (all_ctls[i].type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER64) {
            COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER64_MIN_OFFSET,  &all_ctls[i].value.integer64.min,   0, sizeof(all_ctls[i].value.integer64.min));
            COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER64_MAX_OFFSET,  &all_ctls[i].value.integer64.max,   0, sizeof(all_ctls[i].value.integer64.max));
            COPY_VALUE_TO_BUF(&buff.response_ext[i - start_id].elem_info, VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER64_STEP_OFFSET, &all_ctls[i].value.integer64.step,  0, sizeof(all_ctls[i].value.integer64.step));
        }
    }
#endif
}


uint32_t AudioDeviceManager::ProcessDCEnumInfo(struct virtio_snd_dc_enum_info_msg &buff, unsigned int gvm_id)
 {
    std::vector<VirtioSndDCEnum> dc_enums;
    int rc = 0;
    uint32_t size = 0;

    if (plugin_utils_[gvm_id].sciu == nullptr) {
        AWE_LOGE("ProcessDCEnumInfo failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return size;
    }

    rc = plugin_utils_[gvm_id].sciu->GetEnumsForControl(buff.request.control_id, dc_enums);
    if (rc != 0) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "failed to get enums for mixer control");
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return size;
    }

    size = sizeof(struct virtio_snd_hdr) + sizeof(struct virtio_snd_dc_hdr);
    buff.response.code = VIRTIO_SND_S_OK;

    for (int i = 0; i < dc_enums.size(); i++) {
        snprintf(reinterpret_cast<char*>(buff.response_ext[i].name), VIRTIO_SND_DC_NAME_MAXLEN, "%s", reinterpret_cast<const char*>(dc_enums[i].dai_name));
        size += sizeof(struct virtio_snd_dc_enum_value);
    }

    return size;
 }

void AudioDeviceManager::ProcessPCMSetParams(struct virtio_snd_pcm_set_params_msg &buff, unsigned int gvm_id)
 {
    uint32_t ret;
    struct PCMSetParams pcm_params;
    struct FrontendInfo fe_info;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMSetParams failed to find gvm id: %u", gvm_id);
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

    ret = plugin_utils_[gvm_id].pcm_plugin->StreamSetParam(fe_info.name, pcm_params);

    if (!ret)
        buff.response.code = VIRTIO_SND_S_OK;
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;
 }

void AudioDeviceManager::ProcessPCMPrepare(struct virtio_snd_pcm_prepare_msg &buff, struct SocketInfo sock_info)
 {
    uint32_t ret;
    struct FrontendInfo fe_info;
    struct virtio_pcm_msg *msg = nullptr;
    int32_t vcid;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(sock_info.vmid) == all_fe_dais_.end() ||
        plugin_utils_[sock_info.vmid].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMPrepare failed to find gvm id: %d", sock_info.vmid);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_info = all_fe_dais_[sock_info.vmid][buff.request.stream_id];

    if (buff.request.hdr.code == VIRTIO_SND_R_PCM_PREPARE)
        ret = plugin_utils_[sock_info.vmid].pcm_plugin->StreamPrepare(fe_info.name);

    else {
        if (pcm_stream_threads_[sock_info.vmid].find(fe_info.name) == pcm_stream_threads_[sock_info.vmid].end()) {
            AWE_LOGE("ProcessPCMPrepare failed to find fe dai name: %s", fe_info.name.c_str());
        } else {
            auto& by_name = pcm_stream_threads_[sock_info.vmid];
            auto it = by_name.find(fe_info.name);
            auto* stream = it->second;
            // drain existing messages with IO_ERR
            while (!stream->msg_queue.empty()) {
                msg = stream->msg_queue.front();
                stream->msg_queue.pop();
                msg->status.status = VIRTIO_SND_S_IO_ERR;
                msg->length += sizeof(msg->status);
                ihab_->Send(sock_info.vcid, msg, sizeof(struct virtio_pcm_msg), 0);
                free(msg);
            }
            // Terminate the worker cleanly
            std::lock_guard<std::mutex> lk(stream->stream_mutex);
            stream->msg_queue.push(nullptr);
            stream->stream_cond.notify_one();
            if (stream->thread_obj.joinable()) {
                stream->thread_obj.join();
            }
            delete stream;
            by_name.erase(it);
        }

        ret = plugin_utils_[sock_info.vmid].pcm_plugin->StreamRelease(fe_info.name);

        if (dma_export_map_[sock_info.vmid].find(fe_info.id) != dma_export_map_[sock_info.vmid].end()) {
          uint32_t export_id = dma_export_map_[sock_info.vmid][fe_info.id].export_id;
          uint64_t heap_fd = dma_export_map_[sock_info.vmid][fe_info.id].fd;
          uint32_t vcid = dma_export_map_[sock_info.vmid][fe_info.id].vcid;
          ihab_->Unimport(vcid, export_id, (void*)heap_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
          dma_export_map_[sock_info.vmid].erase(fe_info.id);
        }
        else if (push_pull_map_[sock_info.vmid].find(fe_info.id) != push_pull_map_[sock_info.vmid].end()) {
          uint32_t data_export_id = push_pull_map_[sock_info.vmid][fe_info.id].data_export_id;
          uint32_t pos_export_id = push_pull_map_[sock_info.vmid][fe_info.id].pos_export_id;
          uint32_t data_fd = push_pull_map_[sock_info.vmid][fe_info.id].data_fd;
          uint32_t pos_fd = push_pull_map_[sock_info.vmid][fe_info.id].pos_fd;

          ihab_->Unimport(sock_info.vcid, data_export_id, (void*)(uintptr_t)data_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
          ihab_->Unimport(sock_info.vcid, pos_export_id, (void*)(uintptr_t)pos_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
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

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMTrigger failed to find gvm id: %u", gvm_id);
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
                                   [this,gvm_id=gvm_id,fe_name=fe_info.name]{
                                   return pcm_stream_threads_[gvm_id][fe_name]->msg_queue.empty();
                                   });
            }

            pcm_stream_threads_[gvm_id][fe_info.name]->msg_queue.push(nullptr);
            pcm_stream_threads_[gvm_id][fe_info.name]->stream_cond.notify_all();
            if (pcm_stream_threads_[gvm_id][fe_info.name]->thread_obj.joinable())
                pcm_stream_threads_[gvm_id][fe_info.name]->thread_obj.join();
        }
        ret = plugin_utils_[gvm_id].pcm_plugin->StreamStop(fe_info.name);
    }

    if (!ret)
        buff.response.code = VIRTIO_SND_S_OK;
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;

    AWE_LOGI("Exit");
 }

void AudioDeviceManager::ProcessPCMMmap(struct virtio_snd_pcm_mmap_msg &buff, struct SocketInfo sock_info)
 {
    uint32_t ret;
    uint32_t fe_id;
    string fe_dai_name;
    struct PushPullInfo push_pull_info;
    struct PCMMmapParams mmap_params;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(sock_info.vmid) == all_fe_dais_.end() ||
        plugin_utils_[sock_info.vmid].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMMmap failed to find gvm id: %d", sock_info.vmid);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_id = all_fe_dais_[sock_info.vmid][buff.request.hdr.stream_id].id;
    fe_dai_name = all_fe_dais_[sock_info.vmid][buff.request.hdr.stream_id].name;

    if (push_pull_map_[sock_info.vmid].find(fe_id) != push_pull_map_[sock_info.vmid].end()) {
        AWE_LOGI("Push Pull fds already imported");
        buff.response.code = VIRTIO_SND_S_OK;
        return;
    }

    ret = ihab_->Import(sock_info.vcid, (void**)&mmap_params.data_fd,
                      buff.request.data_size, buff.request.data_export_id, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
    if(ret) {
          AWE_LOGE("Hab Import failed for data buffer, skipping MMAP operation");
          goto response;
        }

    ret = ihab_->Import(sock_info.vcid, (void**)&mmap_params.pos_fd,
                      buff.request.pos_size, buff.request.pos_export_id, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
    if(ret) {
          AWE_LOGE("Hab Import failed for position buffer, skipping MMAP operation");
          goto response;
        }

    mmap_params.data_gvm_fd = buff.request.data_fd;
    mmap_params.pos_gvm_fd = buff.request.pos_fd;
    mmap_params.data_size = buff.request.data_size;
    mmap_params.pos_size = buff.request.pos_size;
    ret = plugin_utils_[sock_info.vmid].pcm_plugin->StreamMmap(fe_dai_name, mmap_params);
    if (ret) {
        AWE_LOGE("Push-Pull configuration failed");
        goto response;
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
    uint32_t ret;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end()) {
        AWE_LOGE("ProcessDCRead failed to find mixer ctls for gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];

    AWE_LOGI("Processing read for control %s", ctl_info.id.name.c_str());

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = bytes;

    buff.response.code = VIRTIO_SND_S_OK;

    ret = plugin_utils_[gvm_id].pcm_plugin->DcRead(elem_payload);
    if (ret) {
        AWE_LOGE("Mixer control read for control %s failed", ctl_info.id.name.c_str());
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }
#if !defined(__QNX__)
    if((elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
        || (elem_payload.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)){
        for (int i = 0; i < ctl_info.count; i++)
            buff.response_ext.value.integer.value[i] = (long) elem_payload.value.integer.value[i];
    }
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        memcpy(&buff.response_ext.value.enumerated.item, elem_payload.value.enumerated.item,
            sizeof(elem_payload.value.enumerated.item));
    }
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES)
        memcpy(&buff.response_ext.value.bytes.data, elem_payload.value.bytes.data,
            sizeof(elem_payload.value.bytes.data));
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;
    buff.response_ext.id.numid = buff.request.control_id + 1;
    snprintf(reinterpret_cast<char*>(buff.response_ext.id.name), SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", ctl_info.id.name.c_str());
#else
   if((elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER)
        || (elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_BOOLEAN)){
        for (int i = 0; i < ctl_info.count; i++)
            COPY_VALUE_TO_BUF(&buff.response_ext, VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET + i * sizeof(long), &elem_payload.value.integer.value, i * sizeof(uint32_t), sizeof(uint32_t));
    }
    else if (elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_ENUMERATED) {
        COPY_VALUE_TO_BUF(&buff.response_ext, VIRTIO_CTL_ELEM_VALUE_ENUMERATED_ITEM_OFFSET, &elem_payload.value.enumerated.item, 0, VIRTIO_CTL_ELEM_VALUE_ENUMERATED_ITEM_SIZE);
    }
    else if (elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_BYTES)
        COPY_VALUE_TO_BUF(&buff.response_ext, VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_OFFSET, &elem_payload.value.bytes.data, 0, VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_SIZE);
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;
    uint32_t num_id = buff.request.control_id + 1;
    COPY_VALUE_TO_BUF(&buff.response_ext, VIRTIO_CTL_ELEM_ID_NUMID_OFFSET, &num_id, 0, sizeof(uint32_t));
    snprintf(reinterpret_cast<char*>(((char *)&(buff.response_ext)) + VIRTIO_CTL_ELEM_ID_NAME_OFFSET), VIRTIO_CTL_ELEM_ID_NAME_MAXLEN, "%s", ctl_info.id.name.c_str());
#endif
 }

void AudioDeviceManager::ProcessDCWrite(struct virtio_snd_dc_write_msg &buff, unsigned int gvm_id)
 {
    uint32_t ret;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessDCWrite failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];

    AWE_LOGI("Processing write for control %s", ctl_info.id.name.c_str());

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = bytes;
#if !defined(__QNX__)

    if((elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
        || (elem_payload.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)) {
        for (int i = 0; i < ctl_info.count; i++)
            elem_payload.value.integer.value[i] = (int32_t)buff.request_ext.value.integer.value[i];
    }
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
        for (int i = 0; i < ctl_info.count; i++)
            elem_payload.value.integer64.value[i] = (int64_t)buff.request_ext.value.integer64.value[i];
    }
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        memcpy(&elem_payload.value.enumerated.item, &buff.request_ext.value.enumerated.item,
               sizeof(elem_payload.value.enumerated.item));
    }
    else if (elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES) {
        memcpy(&elem_payload.value.bytes.data, &buff.request_ext.value.bytes.data,
               sizeof(elem_payload.value.bytes.data));
    }
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;
#else
    if((elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER)
        || (elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_BOOLEAN)) {
        for (int i = 0; i < ctl_info.count; i++)
            COPY_VALUE_TO_BUF(&elem_payload.value.integer.value, i * sizeof(int32_t), &buff.request_ext, VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET + i * sizeof(long), sizeof(int32_t));
    }
    else if (elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER64) {
        for (int i = 0; i < ctl_info.count; i++)
            COPY_VALUE_TO_BUF(&elem_payload.value.integer64.value, i * sizeof(int64_t), &buff.request_ext, VIRTIO_CTL_ELEM_VALUE_INTEGER64_VALUE_OFFSET + i * sizeof(long long), sizeof(int64_t));
    }
    else if (elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_ENUMERATED) {
        COPY_VALUE_TO_BUF(&elem_payload.value.enumerated.item, 0, &buff.request_ext, VIRTIO_CTL_ELEM_VALUE_ENUMERATED_ITEM_OFFSET, VIRTIO_CTL_ELEM_VALUE_ENUMERATED_ITEM_SIZE);
    }
    else if (elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_BYTES) {
        COPY_VALUE_TO_BUF(&elem_payload.value.bytes.data, 0, &buff.request_ext, VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_OFFSET, VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_SIZE);
    }
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;
#endif

    ret = plugin_utils_[gvm_id].pcm_plugin->DcWrite(elem_payload);
    if (ret == AWE_ECORE){
        AWE_LOGE("Mixer control write for control %s failed with core error", ctl_info.id.name.c_str());
        buff.response.code = VIRTIO_SND_S_IO_CORE_ERR;
        return;
    } else if (ret) {
        AWE_LOGE("Mixer control write for control %s failed", ctl_info.id.name.c_str());
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    buff.response.code = VIRTIO_SND_S_OK;
    return;
 }

void AudioDeviceManager::ProcessDCTlvRead(struct virtio_snd_dc_tlv_read_msg &buff, unsigned int gvm_id)
 {
    uint32_t ret;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessDCTlvRead failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];

    AWE_LOGI( "Reading TLV value for control %s", ctl_info.id.name);

    memset(&buff.tlv[0], 0, 1024);

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = tlv_bytes;
    elem_payload.value.tlv.tlv = (uint8_t*)&buff.tlv[0];

    ret = plugin_utils_[gvm_id].pcm_plugin->DcRead(elem_payload);
    if (ret == AWE_ECORE){
        AWE_LOGE("DcRead failed for TLV payload with core error!");
        buff.response.code = VIRTIO_SND_S_IO_CORE_ERR;
        return;
    }
    else if (ret) {
        AWE_LOGE("DcRead failed for TLV payload");
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    buff.response.code = VIRTIO_SND_S_OK;
 }

 void AudioDeviceManager::ProcessDCTlvWrite(struct virtio_snd_dc_tlv_write_msg &buff, uint32_t buff_size, unsigned int gvm_id)
 {
    uint32_t ret;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;
    struct virtio_snd_hdr response;
    unsigned int tlv_size = 0;
    uint8_t *tlv_to_write;

    AWE_LOGD("Enter");
    if (buff_size < sizeof(struct virtio_snd_dc_hdr) ||
        all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessDCTlvWrite failed to find gvm id: %u", gvm_id);
        response.code = VIRTIO_SND_S_IO_ERR;
        goto response;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];
    tlv_size = buff_size - sizeof(struct virtio_snd_dc_hdr);

    tlv_to_write = static_cast<uint8_t*>(calloc(1, tlv_size*sizeof(uint8_t)));
    if (tlv_to_write == nullptr) {
        AWE_LOGE("failed to allocate for tlv to write");
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
        AWE_LOGD("TLV value [%d]: 0x%x", i, tlv_to_write[i]);

    ret = plugin_utils_[gvm_id].pcm_plugin->DcWrite(elem_payload);
    if (!ret) {
        response.code = VIRTIO_SND_S_OK;
    }
    else if (ret == AWE_ECORE){
        AWE_LOGE("Mixer control TLV write for control %s failed with core error", ctl_info.id.name.c_str());
        response.code = VIRTIO_SND_S_IO_CORE_ERR;
    }
    else {
        AWE_LOGE("Mixer control TLV write for control %s failed", ctl_info.id.name.c_str());
        response.code = VIRTIO_SND_S_IO_ERR;
    }

    free(tlv_to_write);

response:
    memcpy((unsigned char*)&buff + sizeof(struct virtio_snd_dc_hdr) + tlv_size, &response, sizeof(response));

 }

 void AudioDeviceManager::ProcessPCMMsg(struct PCMMsgArgs args)
 {
    uint64_t heap_fd = 0;
    struct virtio_snd_pcm_buf_msg pcm_buf_msg;
    struct DMAExport dma_area_map;
    unsigned int direction = args.direction;
    struct SocketInfo sock_info = args.sock_info;
    struct virtio_pcm_msg *msg;
    unsigned int gvm_id = sock_info.vmid;
    struct FrontendInfo fe_info;
    uint32_t ret = 0;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMMsg failed to find gvm id: %u", gvm_id);
        return;
    }

    fe_info = all_fe_dais_[gvm_id][args.stream_id];

    if (pcm_stream_threads_[gvm_id].find(fe_info.name) == pcm_stream_threads_[gvm_id].end()) {
        AWE_LOGE("ProcessPCMMsg failed to find fe dai name %s", fe_info.name.c_str());
        return;
    }

    pthread_setname_np(pthread_self(), "awe_adm_pcmmsg");

#ifdef ENABLE_RT_THREAD_PCMMSG

    pthread_setname_np(pthread_self(), COMPRESMGR_CPU_THREAD_GRP_NAME);

    struct sched_param pcmmsg_sched = {0};
    CPUConfigReq_t cpu_config_req = {COMPRESMGR_CPU_COMPONENT_NAME, COMPRESMGR_CPU_THREAD_GRP_NAME};
    CPUConfigResp_t cpu_config_resp = {0, 0, 0};
    CompResmgrRet_e ret_get_cpu_config = CompResmgrGetCPUConfig(&cpu_config_req, &cpu_config_resp);
    if (ret_get_cpu_config != COMPRESMGR_RET_SUCCESS) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "CompResmgrGetCPUConfig failed, using default RT config");
        cpu_config_resp.schedPolicy = RT_THREAD_PCMMSG_DEFAULT_POLICY;
        cpu_config_resp.priority = RT_THREAD_PCMMSG_DEFAULT_PRIORITY;
    }
    pcmmsg_sched.sched_priority = cpu_config_resp.priority;

    ret = pthread_setschedparam(pthread_self(), cpu_config_resp.schedPolicy, &pcmmsg_sched);
    if (ret)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "pthread_setschedparam failed, fall back to TS thread");
    else
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Set RT thread %s with policy %d and priority %d succeeded",
                       COMPRESMGR_CPU_THREAD_GRP_NAME, cpu_config_resp.schedPolicy, pcmmsg_sched.sched_priority);

#endif  // ENABLE_RT_THREAD_PCMMSG

    while(1) {
    {
      std::unique_lock<std::mutex> stream_lock(pcm_stream_threads_[gvm_id][fe_info.name]->stream_mutex);
      pcm_stream_threads_[gvm_id][fe_info.name]->stream_cond.wait(stream_lock,
                         [this,gvm_id=gvm_id,fe_name=fe_info.name] {
                         return !pcm_stream_threads_[gvm_id][fe_name]->msg_queue.empty();
                         });

      msg = pcm_stream_threads_[gvm_id][fe_info.name]->msg_queue.front();
      pcm_stream_threads_[gvm_id][fe_info.name]->msg_queue.pop();

      // NULL msg sent by destructor to terminate thread. Cleanup and exit
      if (msg == NULL) {
        AWE_LOGE("msg is NULL");
        return;
      }
#ifdef OFF_TARGET_UNIT_TEST
      return;
#endif
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
          AWE_LOGE("Hab Import failed, skipping PCM operation");
          goto response;
        }

        dma_area_map.export_id = msg->desc.export_id;
        dma_area_map.fd = heap_fd;
        dma_area_map.vcid = sock_info.vcid;
        dma_export_map_[gvm_id][fe_info.id] = dma_area_map;
    }

    pcm_buf_msg.offset = msg->desc.offset;
    pcm_buf_msg.size = msg->length;
    pcm_buf_msg.fe_dai_name = fe_info.name;
    pcm_buf_msg.buff_fd = (uint64_t)heap_fd;
    pcm_buf_msg.dma_bytes = msg->desc.dma_bytes;
#if !defined(__QNX__)
    if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
        ret = plugin_utils_[gvm_id].pcm_plugin->StreamWrite(pcm_buf_msg);
    }
    else {
        ret = plugin_utils_[gvm_id].pcm_plugin->StreamRead(pcm_buf_msg);

    }
#else
    if (direction == VIRTIO_STREAM_PLAYBACK) {
        ret = plugin_utils_[gvm_id].pcm_plugin->StreamWrite(pcm_buf_msg);
    }
    else {
        ret = plugin_utils_[gvm_id].pcm_plugin->StreamRead(pcm_buf_msg);

    }
#endif
response:
    if (!ret)
        msg->status.status = VIRTIO_SND_S_OK;
    else
        msg->status.status = VIRTIO_SND_S_IO_ERR;

    msg->length += sizeof(msg->status);

    AWE_LOGD("Hab responding to virtio sound: ret %d", (uint32_t)ret);
    ihab_->Send(sock_info.vcid, msg, sizeof(struct virtio_pcm_msg), 0);
    free(msg);

    {
      std::lock_guard<std::mutex> empty_lock(pcm_stream_threads_[gvm_id][fe_info.name]->stream_mutex);
      if (pcm_stream_threads_[gvm_id][fe_info.name]->msg_queue.empty())
        pcm_stream_threads_[gvm_id][fe_info.name]->stream_cond.notify_one();
    }
  }
}

void AudioDeviceManager::ProcessShmemMap(struct virtio_snd_dc_write_msg &buff, struct SocketInfo sock_info)
{
    uint32_t ret=0;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;
    int *fd = NULL;
    buff.response.code = VIRTIO_SND_S_OK;

    if (all_mixer_ctls_.find(sock_info.vmid) == all_mixer_ctls_.end() ||
        plugin_utils_[sock_info.vmid].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessDCWrite failed to find gvm id: %u", sock_info.vmid);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[sock_info.vmid][buff.request.control_id];

    AWE_LOGI("Processing ProcessShmemMap for control %s", ctl_info.id.name.c_str());

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = bytes;
#if !defined(__QNX__)
    memcpy(&elem_payload.value.bytes.data, &buff.request_ext.value.bytes.data,
        sizeof(elem_payload.value.bytes.data));
#else
    COPY_VALUE_TO_BUF(&elem_payload.value.bytes.data, 0, &buff.request_ext, VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_OFFSET, sizeof(elem_payload.value.bytes.data));
#endif

    struct ShmemInfo *shmem_info = reinterpret_cast<struct ShmemInfo *>(&elem_payload.value.bytes.data);
    uint32_t *puint = reinterpret_cast<uint32_t *>(&elem_payload.value.bytes.data);
    AWE_LOGI("Processing ProcessShmemMap for control %s, sz %d, export_id %08x", ctl_info.id.name.c_str(), shmem_info->data_size, shmem_info->data_export_id);

    //call import id
    ret = ihab_->Import(sock_info.vcid, (void**)&fd,
    shmem_info->data_size, shmem_info->data_export_id, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);//TODO: make uncached
    if(ret) {
        AWE_LOGE("Hab Import failed for data buffer, skipping shmemAlloc");
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }
    shmem_info->export_fd = (int)((int64_t)fd&0x00000000ffffffff);//TODO: check if should be 64 bits
    AWE_LOGI("Processing ProcessShmemMap for control %s, fd after import: %lX,  lsw fd %08x", ctl_info.id.name.c_str(), fd, shmem_info->export_fd);
    ret = plugin_utils_[sock_info.vmid].pcm_plugin->DcWrite(elem_payload);
    if (ret) {
        AWE_LOGE("Mixer ProcessShmemMap for control %s failed", ctl_info.id.name.c_str());
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        ret = ihab_->Unimport(sock_info.vcid, shmem_info->data_export_id, (void*)fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
        if (ret)
            AWE_LOGE("Hab Unimport failed for data buffer while shmemmap");
        return;
    }
    // store for cleanup
    shmem.push_back(*shmem_info);
    return;
}

void AudioDeviceManager::ProcessShmemUnmap(struct virtio_snd_dc_write_msg &buff, struct SocketInfo sock_info)
{
#if !defined(__QNX__)
    uint32_t ret;
    struct SoundCardMixerCtl ctl_info;
    struct CtlElemPayload elem_payload;
    uintptr_t fd;
    buff.response.code = VIRTIO_SND_S_OK;

    if (all_mixer_ctls_.find(sock_info.vmid) == all_mixer_ctls_.end() ||
        plugin_utils_[sock_info.vmid].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessDCWrite failed to find gvm id: %u", sock_info.vmid);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[sock_info.vmid][buff.request.control_id];

    AWE_LOGI("Processing ProcessShmemUnmap for control %s", ctl_info.id.name.c_str());

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = bytes;
#if !defined(__QNX__)
    memcpy(&elem_payload.value.bytes.data, &buff.request_ext.value.bytes.data,
        sizeof(elem_payload.value.bytes.data));
#else
    COPY_VALUE_TO_BUF(&elem_payload.value.bytes.data, 0, &buff.request_ext, VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_OFFSET, sizeof(elem_payload.value.bytes.data));
#endif
    struct ShmemInfo *shmem_info = reinterpret_cast<struct ShmemInfo *>(&elem_payload.value.integer.value[0]);

    ret = plugin_utils_[sock_info.vmid].pcm_plugin->DcWrite(elem_payload);
    if (ret) {
        AWE_LOGE("Mixer ProcessShmemUnmap for control %s failed", ctl_info.id.name.c_str());
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }
    for (auto it = shmem.begin(); it != shmem.end(); it++){
        if (it->data_export_id == shmem_info->data_export_id){
            fd = (uintptr_t)it->export_fd;
            ret = ihab_->Unimport(sock_info.vcid, it->data_export_id, (void*)fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
            shmem.erase(it);
            if(ret) {
                AWE_LOGE("Hab Unimport failed for data buffer while shmemUnmap");
                buff.response.code = VIRTIO_SND_S_IO_ERR;
            }
            return;
        }
    }
    AWE_LOGE("Unable to find the data export id 0x%08x", shmem_info->data_export_id);
    buff.response.code = VIRTIO_SND_S_IO_ERR;
#endif
    return;
}
