/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <thread>
#include <chrono>
#include <pthread.h>
#include <sys/mman.h>

#ifdef ENABLE_RT_THREAD_PCMMSG
#include "amss/compresmgr_client_api.h"
#endif

#include "AudioDeviceManager.h"

#define RT_THREAD_PCMMSG_DEFAULT_POLICY SCHED_FIFO
#define RT_THREAD_PCMMSG_DEFAULT_PRIORITY 45
#define STOP_TIMEOUT_MS 1000

#define SLEEP_US(us) do { \
    struct timespec ts; \
    ts.tv_sec = (us) / 1000000; \
    ts.tv_nsec = ((us) % 1000000) * 1000; \
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);\
} while (0)

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
            it2.second->stop_flag.store(true);
            it2.second->stream_cond.notify_all();
            if (it2.second->thread_obj.joinable())
            {
                it2.second->thread_obj.join();
            }
        }
    }

    for (auto &it: pcm_pending_threads_)
    {
        for (auto &it2: it.second)
        {
            it2.second->stop_flag.store(true);
            it2.second->stream_cond.notify_all();
            if (it2.second->thread_obj.joinable())
            {
                it2.second->thread_obj.join();
            }
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
        {
            std::lock_guard<std::mutex> lk(vcid_map_mutex);
            vcid_map_[sock_info.vmid][mmid] = sock_info.vcid;
        }

        {
            std::lock_guard<std::mutex> lock(snd_card_init_mutex);
            // If vmid has already been initialized, do not insert again
            bool already_inited =
                (all_fe_dais_.find(sock_info.vmid) != all_fe_dais_.end()) ||
                (all_mixer_ctls_.find(sock_info.vmid) != all_mixer_ctls_.end()) ||
                (plugin_utils_.find(sock_info.vmid) != plugin_utils_.end());

            if (!already_inited) {
                AWE_LOGI("Building sound card for vmid %d", sock_info.vmid);

                struct PluginUtils utils;
                utils.sciu = isciu_;

                std::pair<int32_t, std::vector<FrontendInfo>>fe_dais(sock_info.vmid, *fe_info_);
                all_fe_dais_.insert(fe_dais);

                std::pair<int32_t, std::vector<SoundCardMixerCtl>>mixer_ctls(sock_info.vmid, *sc_mixer_ctl_);
                all_mixer_ctls_.insert(mixer_ctls);

                utils.pcm_plugin = iaudio_plugin_;
                plugin_utils_.insert(std::pair<int32_t, struct PluginUtils>(sock_info.vmid, utils));

                AWE_LOGI("SoundCard Built");
            } else {
                AWE_LOGI("Built sound card for vmid %d already done, skip re-insert", sock_info.vmid);
            }
        }

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

    AWE_LOGI("Enter");

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
                // stop pcm thread and pending thread
                {
                    std::unique_lock<std::mutex> pcm_threads_lock(pcm_stream_threads_mutex);
                    for (auto &iter: pcm_stream_threads_[sock_info.vmid]) {
                        auto pcm_thread_ptr = iter.second;
                        pcm_threads_lock.unlock();
                        pcm_thread_ptr->stop_flag.store(true);
                        pcm_thread_ptr->stream_cond.notify_all();
                        if (pcm_thread_ptr->thread_obj.joinable())
                            pcm_thread_ptr->thread_obj.join();

                        {
                            std::lock_guard<std::mutex> stream_lock(pcm_thread_ptr->stream_mutex);
                            while (!pcm_thread_ptr->msg_queue.empty()) {
                                auto msg = pcm_thread_ptr->msg_queue.front();
                                pcm_thread_ptr->msg_queue.pop();
                                if (msg) {
                                    free(msg); // free only since hab error
                                }
                            }
                        }
                        pcm_threads_lock.lock();
                    }

                    for (auto &iter: pcm_pending_threads_[sock_info.vmid]) {
                        auto pcm_pending_thread_ptr = iter.second;
                        pcm_threads_lock.unlock();
                        pcm_pending_thread_ptr->stop_flag.store(true);
                        pcm_pending_thread_ptr->stream_cond.notify_all();
                        if (pcm_pending_thread_ptr->thread_obj.joinable())
                            pcm_pending_thread_ptr->thread_obj.join();

                        {
                            std::lock_guard<std::mutex> stream_lock(pcm_pending_thread_ptr->stream_mutex);
                            while (!pcm_pending_thread_ptr->msg_queue.empty()) {
                                auto msg = pcm_pending_thread_ptr->msg_queue.front();
                                pcm_pending_thread_ptr->msg_queue.pop();
                                if (msg) {
                                    free(msg); // free only since hab error
                                }
                            }
                        }
                        pcm_threads_lock.lock();
                    }
                }

                // stop gvm session
                for (auto &iter: pcm_stream_threads_[sock_info.vmid]) {
                    if (plugin_utils_[sock_info.vmid].pcm_plugin) {
                        plugin_utils_[sock_info.vmid].pcm_plugin->StreamStop(iter.first);
                        plugin_utils_[sock_info.vmid].pcm_plugin->StreamRelease(iter.first);
                    }
                }

                // unimport DMA
                {
                    std::unique_lock<std::mutex> dma_map_lock(dma_export_map_mutex);
                    for (auto &iter : dma_export_map_[sock_info.vmid]) {
                        uint32_t export_id = iter.second.export_id;
                        uint64_t heap_fd = iter.second.fd;
                        uint32_t vcid = iter.second.vcid;
                        ihab_->Unimport(vcid, export_id, (void*)heap_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                    }
                }

                for (auto &iter : push_pull_map_[sock_info.vmid]) {
                    uint32_t data_export_id = iter.second.data_export_id;
                    uint32_t pos_export_id = iter.second.pos_export_id;
                    uint32_t data_fd = iter.second.data_fd;
                    uint32_t pos_fd = iter.second.pos_fd;
                    ihab_->Unimport(sock_info.vcid, data_export_id, (void*)(uintptr_t)data_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                    ihab_->Unimport(sock_info.vcid, pos_export_id, (void*)(uintptr_t)pos_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                }

                // close hab
                ihab_->Close(sock_info.vcid);

                // reset resource table
                dma_export_map_[sock_info.vmid].clear();
                pcm_stream_threads_[sock_info.vmid].clear();
                pcm_pending_threads_[sock_info.vmid].clear();
                push_pull_map_[sock_info.vmid].clear();
                break;
            }
        }
        else
        {
            struct virtio_snd_hdr *hdr = (struct virtio_snd_hdr*) buff;
            buff_size = ProcessCtlMsg(buff, buff_size, sock_info);
            AWE_LOGD("Sending response to virtio-snd, size: %d", buff_size);
            ret = ihab_->Send(sock_info.vcid, buff, buff_size, 0);
            if(ret)
            {
                AWE_LOGE("Fail to send command[0x%x] response, ret with %d", hdr->code, ret);
            }
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
            stream_id = pcm_msg->xfer.stream_id;
            fe_dai_name = all_fe_dais_[sock_info.vmid][stream_id].name;
            {
                std::lock_guard<std::mutex> pcm_thread_lock(pcm_stream_threads_mutex);
                if (pcm_stream_threads_[sock_info.vmid].find(fe_dai_name) != pcm_stream_threads_[sock_info.vmid].end()) {
                    {
                      std::lock_guard<std::mutex> stream_queue_lock(pcm_stream_threads_[sock_info.vmid][fe_dai_name]->stream_mutex);
                      pcm_stream_threads_[sock_info.vmid][fe_dai_name]->msg_queue.push(pcm_msg);
                    }
                    pcm_stream_threads_[sock_info.vmid][fe_dai_name]->stream_cond.notify_one();
                }
                else {
                    AWE_LOGE("Cannot find pcm thread for stream_id %d, dropping pcm msg", stream_id);
                    DropPcmMsg(sock_info.vcid, pcm_msg);
                }
            }
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

    if (plugin_utils_[sock_info.vmid].pcm_plugin == nullptr) {
        AWE_LOGE("EventQueueFunc failed to find sock_info.vmid: %u", sock_info.vmid);
        free(buff);
        buff = nullptr;
        return;
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

// Single CTL HAB QUEUE guarantees the CTL msg commands are serialized
void AudioDeviceManager::UpdatePcmDeviceState(int32_t vmid, int32_t fe_stream_id, int32_t hdr_code)
{
    struct FrontendInfo& fe_info = all_fe_dais_[vmid][fe_stream_id];

    switch (hdr_code) {
        case VIRTIO_SND_R_PCM_SET_PARAMS:
            fe_info.stream_state = VIRTIO_PCM_OPENED;
            break;
        case VIRTIO_SND_R_PCM_PREPARE:
            fe_info.stream_state = VIRTIO_PCM_PREPARED;
            break;
        case VIRTIO_SND_R_PCM_START:
            fe_info.stream_state = VIRTIO_PCM_STARTED;
            break;
        case VIRTIO_SND_R_PCM_STOP:
            fe_info.stream_state = VIRTIO_PCM_STOPPED;
            break;
        case VIRTIO_SND_R_PCM_RELEASE:
            fe_info.stream_state = VIRTIO_PCM_CLOSED;
            break;
        default:
            AWE_LOGE("Invalid Args");
            return;
    }

    AWE_LOGI("Update stream_id:%d, FE:%s state to: %d",
        fe_stream_id, fe_info.name.c_str(), (int32_t)fe_info.stream_state);
}

// DropPCMMSG is used to avoid processing PCM messages when stream is stopped or closed
void AudioDeviceManager::DropPcmMsg(int32_t vcid, struct virtio_pcm_msg *msg)
{
    if(!msg) {
        AWE_LOGE("msg is null");
        return;
    }
    msg->status.status = VIRTIO_SND_S_IO_ERR;
    msg->length += sizeof(msg->status);
    ihab_->Send(vcid, msg, sizeof(struct virtio_pcm_msg), 0);
    AWE_LOGI("drop pcm msg and send a dummy response ");
    free(msg);
}
enum VirtioPCMState AudioDeviceManager::GetPcmDeviceState(int32_t vmid, int32_t fe_stream_id)
{
    struct FrontendInfo& fe_info = all_fe_dais_[vmid][fe_stream_id];

    return fe_info.stream_state;
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
        case VIRTIO_SND_R_VERSION:
          ProcessVersion(*(struct virtio_snd_version_msg*)buff);
          buff_size = sizeof(struct virtio_snd_version_msg);
          break;
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
          ProcessPCMSetParams(*(struct virtio_snd_pcm_set_params_msg*)buff, sock_info);
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
          buff_size = sizeof(struct virtio_snd_dc_hdr) + sizeof(virtio_snd_hdr) + MAX_TLV_SIZE;
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

void AudioDeviceManager::ProcessVersion(struct virtio_snd_version_msg &buff)
{
    AWE_LOGI("Enter");
    buff.response.code = VIRTIO_SND_S_OK;
    buff.version = VSND_VERSION_2;
    AWE_LOGI("Backend version is %d", buff.version == VSND_VERSION_1 ? 1 : 2);
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

void AudioDeviceManager::ProcessPCMSetParams(struct virtio_snd_pcm_set_params_msg &buff, struct SocketInfo sock_info)
 {
    struct PCMSetParams pcm_params;
    struct FrontendInfo fe_info;
    int32_t ret = -EINVAL;
    uint64_t heap_fd = -1;
    struct DMAExport dma_area_map;
    uint32_t export_id;
    uint32_t dma_bytes;
    unsigned int gvm_id = sock_info.vmid;
    struct PCMMsgArgs args;
    int direction, pcm_mmid, pcm_vcid;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMSetParams failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_info = all_fe_dais_[gvm_id][buff.request.hdr.stream_id];
    direction = fe_info.direction;
#if !defined(__QNX__)
    pcm_mmid = (direction == SNDRV_PCM_STREAM_PLAYBACK ? MM_AUD_4 : MM_AUD_3);
#else
    pcm_mmid = (direction == VIRTIO_STREAM_PLAYBACK ?  MM_AUD_4 : MM_AUD_3);
#endif
    pcm_vcid = vcid_map_[gvm_id][pcm_mmid];
    export_id = buff.request.export_id;
    dma_bytes = buff.request.dma_bytes;

    if (!buff.request.is_mmap_noirq) {
        {
            std::lock_guard<std::mutex> lock(dma_export_map_mutex);
            if (dma_export_map_[gvm_id].find(fe_info.id) != dma_export_map_[gvm_id].end()) {
                AWE_LOGE("Notice!!! Warning!!!, fe:%s dma was not cleaned up in previous session", fe_info.name.c_str());
                ret = -1;
                goto response;
            }
        }

        ret = ihab_->Import(pcm_vcid, (void**)&heap_fd, dma_bytes,
                            export_id, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
        AWE_LOGD("Import vcid %x, export id %d, ret value %d", pcm_vcid, export_id, ret);
        if (ret) {
            AWE_LOGE("Hab Import failed, skipping setParam operation");
            goto response;
        }

        dma_area_map.export_id = export_id;
        dma_area_map.fd = heap_fd;
        dma_area_map.vcid = pcm_vcid;

        {
            std::lock_guard<std::mutex> lock(dma_export_map_mutex);
            dma_export_map_[gvm_id][fe_info.id] = dma_area_map;
        }
    }

    pcm_params.buffer_bytes = buff.request.buffer_bytes;
    pcm_params.period_bytes = buff.request.period_bytes;
    pcm_params.features = buff.request.features;
    pcm_params.format = buff.request.format;
    pcm_params.rate = buff.request.rate;
    pcm_params.channels = buff.request.channels;
    pcm_params.is_mmap_noirq = buff.request.is_mmap_noirq;
    pcm_params.buff_fd = heap_fd;

    ret = plugin_utils_[gvm_id].pcm_plugin->StreamSetParam(fe_info.name, pcm_params);
    if (ret) {
        AWE_LOGE("StreamSetParam failed for stream %s", fe_info.name.c_str());
        goto response;
    }

    UpdatePcmDeviceState(gvm_id, buff.request.hdr.stream_id, buff.request.hdr.hdr.code);

    args.stream_id = buff.request.hdr.stream_id;
    args.sock_info = {pcm_vcid, pcm_mmid, (int32_t)gvm_id};
    args.direction = direction;

    /* both queue need to be clean */
    {
        std::lock_guard<std::mutex> pcm_threads_lock(pcm_stream_threads_mutex);
        if (pcm_stream_threads_[sock_info.vmid].find(fe_info.name) != pcm_stream_threads_[sock_info.vmid].end() ||
            pcm_pending_threads_[sock_info.vmid].find(fe_info.name) != pcm_pending_threads_[sock_info.vmid].end()) {
            AWE_LOGE("Error !!! Pcm thread %s not cleaned up from previous session", fe_info.name.c_str());
            ret = -1;
            goto response;
        }
    }
    {
#ifdef OFF_TARGET_UNIT_TEST
        std::function<void(PCMMsgArgs)> pcm_func = std::bind(&AudioDeviceManager::EmptyFuncPCMMsgArgs, this, _1);
#else
        std::function<void(PCMMsgArgs)> pcm_pending_func = std::bind(&AudioDeviceManager::ProcessPCMPendingMsg, this, _1);
        std::function<void(PCMMsgArgs)> pcm_func = std::bind(&AudioDeviceManager::ProcessPCMMsg, this, _1);
#endif

        auto pending_thread = std::make_shared<PCMStreamThread>();
        auto stream_thread = std::make_shared<PCMStreamThread>();
        {
            std::lock_guard<std::mutex> pcm_threads_lock(pcm_stream_threads_mutex);
            pcm_pending_threads_[sock_info.vmid][fe_info.name] = pending_thread;
            pcm_stream_threads_[sock_info.vmid][fe_info.name] = stream_thread;
        }

        AWE_LOGD("Starting pcm thread for %s", fe_info.name.c_str());
        stream_thread->in_flight_cnt = -1; // -1 for running
        pending_thread->thread_obj = std::thread(pcm_pending_func, args);
        stream_thread->thread_obj = std::thread(pcm_func, args);
    }

response:
    if (!ret)
        buff.response.code = VIRTIO_SND_S_OK;
    else
        buff.response.code = VIRTIO_SND_S_IO_ERR;

    return;
 }

void AudioDeviceManager::ProcessPCMPrepare(struct virtio_snd_pcm_prepare_msg &buff, struct SocketInfo sock_info)
 {
    int32_t ret;
    struct FrontendInfo fe_info;
    struct virtio_pcm_msg *msg = nullptr;
    int32_t pcm_vcid;
    int pcm_mmid;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(sock_info.vmid) == all_fe_dais_.end() ||
        plugin_utils_[sock_info.vmid].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMPrepare failed to find gvm id: %d", sock_info.vmid);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_info = all_fe_dais_[sock_info.vmid][buff.request.stream_id];

    if (buff.request.hdr.code == VIRTIO_SND_R_PCM_PREPARE) {
        ret = plugin_utils_[sock_info.vmid].pcm_plugin->StreamPrepare(fe_info.name);
        if (!ret) {
            UpdatePcmDeviceState(sock_info.vmid, buff.request.stream_id, buff.request.hdr.code);

            std::lock_guard<std::mutex> pcm_threads_lock(pcm_stream_threads_mutex);
            if (pcm_stream_threads_[sock_info.vmid].find(fe_info.name) !=
                                    pcm_stream_threads_[sock_info.vmid].end()) {

                std::lock_guard<std::mutex> stream_lock(
                            pcm_stream_threads_[sock_info.vmid][fe_info.name]->stream_mutex);
                pcm_stream_threads_[sock_info.vmid][fe_info.name]->in_flight_cnt = -1; // reset to -1 for resume
            }
        }
    }
    else {
#if !defined(__QNX__)
        pcm_mmid = (fe_info.direction == SNDRV_PCM_STREAM_PLAYBACK ? MM_AUD_4 : MM_AUD_3);
#else
        pcm_mmid = (fe_info.direction == VIRTIO_STREAM_PLAYBACK ? MM_AUD_4 : MM_AUD_3);
#endif
        pcm_vcid = vcid_map_[sock_info.vmid][pcm_mmid];
        UpdatePcmDeviceState(sock_info.vmid, buff.request.stream_id, buff.request.hdr.code);

        {
            std::unique_lock<std::mutex> pcm_threads_lock(pcm_stream_threads_mutex);
            auto it = pcm_stream_threads_[sock_info.vmid].find(fe_info.name);
            if ( it != pcm_stream_threads_[sock_info.vmid].end()) {
                auto pcm_thread_ptr = it->second;
                pcm_threads_lock.unlock();

                pcm_thread_ptr->stop_flag.store(true);
                pcm_thread_ptr->stream_cond.notify_all();
                if (pcm_thread_ptr->thread_obj.joinable())
                    pcm_thread_ptr->thread_obj.join();

                {
                    std::lock_guard<std::mutex> stream_lock(pcm_thread_ptr->stream_mutex);
                    while (!pcm_thread_ptr->msg_queue.empty())
                    {
                        msg = pcm_thread_ptr->msg_queue.front();
                        pcm_thread_ptr->msg_queue.pop();
                        if (msg) {
                            AWE_LOGE("Reach msg drop for gvm %d dai %s !",
                                    sock_info.vmid, fe_info.name.c_str());
                            DropPcmMsg(pcm_vcid, msg);
                        }
                    }
                }

                pcm_threads_lock.lock();
                pcm_stream_threads_[sock_info.vmid].erase(fe_info.name);
            }

            auto it2 = pcm_pending_threads_[sock_info.vmid].find(fe_info.name);
            if ( it2 != pcm_pending_threads_[sock_info.vmid].end()) {
                auto pcm_pending_thread_ptr = it2->second;
                pcm_threads_lock.unlock();

                pcm_pending_thread_ptr->stop_flag.store(true);
                pcm_pending_thread_ptr->stream_cond.notify_all();
                if (pcm_pending_thread_ptr->thread_obj.joinable())
                    pcm_pending_thread_ptr->thread_obj.join();

                {
                    std::lock_guard<std::mutex> stream_lock(pcm_pending_thread_ptr->stream_mutex);
                    while (!pcm_pending_thread_ptr->msg_queue.empty())
                    {
                        msg = pcm_pending_thread_ptr->msg_queue.front();
                        pcm_pending_thread_ptr->msg_queue.pop();
                        if (msg) {
                            AWE_LOGE("Reach msg drop for gvm %d dai %s !",
                                    sock_info.vmid, fe_info.name.c_str());
                            DropPcmMsg(pcm_vcid, msg);
                        }
                    }
                }

                pcm_threads_lock.lock();
                pcm_pending_threads_[sock_info.vmid].erase(fe_info.name);
            }
        }

        ret = plugin_utils_[sock_info.vmid].pcm_plugin->StreamRelease(fe_info.name);

        {
            std::unique_lock<std::mutex> dma_map_lock(dma_export_map_mutex);
            if (dma_export_map_[sock_info.vmid].find(fe_info.id) != dma_export_map_[sock_info.vmid].end()) {
                uint32_t export_id = dma_export_map_[sock_info.vmid][fe_info.id].export_id;
                uint64_t heap_fd = dma_export_map_[sock_info.vmid][fe_info.id].fd;
                uint32_t vcid = dma_export_map_[sock_info.vmid][fe_info.id].vcid;
                dma_export_map_[sock_info.vmid].erase(fe_info.id);
                dma_map_lock.unlock();

                AWE_LOGD("Unimporting export id %d pcm id %d", export_id, fe_info.id);
                ret = ihab_->Unimport(vcid, export_id, (void*)heap_fd, HABMM_IMPORT_FLAGS_CACHED | HABMM_EXPIMP_FLAGS_FD);
                AWE_LOGD("Unimport export id %d returned %d", export_id, ret);
            }
        }

        if (push_pull_map_[sock_info.vmid].find(fe_info.id) != push_pull_map_[sock_info.vmid].end()) {
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

    return;
 }

void AudioDeviceManager::ProcessPCMTrigger(struct virtio_snd_pcm_trigger_msg &buff, unsigned int gvm_id)
 {
    struct FrontendInfo fe_info;
    int32_t ret = 0;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMTrigger failed to find gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    fe_info = all_fe_dais_[gvm_id][buff.request.stream_id];

    if (buff.request.hdr.code == VIRTIO_SND_R_PCM_START) {
        ret = plugin_utils_[gvm_id].pcm_plugin->StreamStart(fe_info.name);
        if (!ret) {
            std::unique_lock<std::mutex> lock(pcm_stream_threads_mutex);
            auto pcm_thread_ptr = pcm_stream_threads_[gvm_id][fe_info.name];
            lock.unlock();

            std::lock_guard<std::mutex> stream_lock(pcm_thread_ptr->stream_mutex);
            pcm_thread_ptr->in_flight_cnt = -1; // reset to -1 for resume
        }
    } else {
        std::unique_lock<std::mutex> stream_map_lock(pcm_stream_threads_mutex);
        auto it = pcm_stream_threads_[gvm_id].find(fe_info.name);
        if (it != pcm_stream_threads_[gvm_id].end()) {
            auto pcm_thread_ptr = it->second;
            auto pcm_pending_thread_ptr = pcm_pending_threads_[gvm_id][fe_info.name];
            stream_map_lock.unlock();
            /* flush msg queue first and push all msg into pending queue to avoid pcm_msg lost */
            {
                std::unique_lock<std::mutex> stream_lock(pcm_thread_ptr->stream_mutex);
                /* cache the valid pcm_msg numbers as in_flight_cnt
                 * pcm_msg comes after stop will be dropped in release
                 */
                pcm_thread_ptr->in_flight_cnt = pcm_thread_ptr->msg_queue.size();
                /* wait in_flight_cnt reduce to 0 */
                auto start = std::chrono::steady_clock::now();
                bool pred_ret = pcm_thread_ptr->stream_ctrl_cond.wait_for(
                                    stream_lock,
                                    std::chrono::milliseconds(STOP_TIMEOUT_MS),
                                    [this,gvm_id=gvm_id,fe_name=fe_info.name, pcm_thread_ptr]{
                                    return (pcm_thread_ptr->in_flight_cnt == 0);
                                    });
                auto end = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                if (elapsed >= std::chrono::milliseconds(STOP_TIMEOUT_MS))
                {
                    /* (1) pred_ret = 0 and elapsed >= STOP_TIMEOUT_MS: Need to debug why in_flight_cnt is not 0;
                    *  (2) pred_ret = 1 and elapsed >= STOP_TIMEOUT_MS: Need to check if notification was received,
                    * if notification was received, it's not a bug ignore it.
                    */
                    AWE_LOGE("%s STOP: in_flight_cnt is %d, elapsed %d ms, pred_ok %d",
                        fe_info.name.c_str(), pcm_thread_ptr->in_flight_cnt, elapsed, pred_ret);
                }
            }

            {
                std::unique_lock<std::mutex> pending_lock(pcm_pending_thread_ptr->stream_mutex);
                /* pending queue needs to be fully clean up */
                auto start = std::chrono::steady_clock::now();
                bool pred_ret = pcm_pending_thread_ptr->stream_ctrl_cond.wait_for(
                                    pending_lock,
                                    std::chrono::milliseconds(STOP_TIMEOUT_MS),
                                    [this,gvm_id=gvm_id,fe_name=fe_info.name, pcm_pending_thread_ptr]{
                                    return pcm_pending_thread_ptr->msg_queue.empty();
                                   });
                auto end = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                if (elapsed >= std::chrono::milliseconds(STOP_TIMEOUT_MS))
                {
                    /* (1) pred_ret = 0 and elapsed >= STOP_TIMEOUT_MS: Need to debug why msg_queue is not empty;
                    *  (2) pred_ret = 1 and elapsed >= STOP_TIMEOUT_MS: Need to check if notification was received,
                    * if notification was received, it's not a bug ignore it.
                    */
                    AWE_LOGE("%s STOP: msg_queue size is %d, elapsed %d ms, pred_ok %d",
                        fe_info.name.c_str(), pcm_pending_thread_ptr->msg_queue.size(), elapsed, pred_ret);
                }
            }

            ret = plugin_utils_[gvm_id].pcm_plugin->StreamStop(fe_info.name);
        }
    }

    if (!ret) {
        buff.response.code = VIRTIO_SND_S_OK;
        UpdatePcmDeviceState(gvm_id, buff.request.stream_id, buff.request.hdr.code);
        {
            std::unique_lock<std::mutex> lock(pcm_stream_threads_mutex);
            if (pcm_stream_threads_[gvm_id].find(fe_info.name) != pcm_stream_threads_[gvm_id].end())
                pcm_stream_threads_[gvm_id][fe_info.name]->stream_cond.notify_all();
            if (pcm_pending_threads_[gvm_id].find(fe_info.name) != pcm_pending_threads_[gvm_id].end())
                pcm_pending_threads_[gvm_id][fe_info.name]->stream_cond.notify_all();
        }
    }
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

    if (all_mixer_ctls_.find(gvm_id) == all_mixer_ctls_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessDCRead failed to find mixer ctls for gvm id: %u", gvm_id);
        buff.response.code = VIRTIO_SND_S_IO_ERR;
        return;
    }

    ctl_info = all_mixer_ctls_[gvm_id][buff.request.control_id];

    AWE_LOGI("Processing read for control %s", ctl_info.id.name.c_str());

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = ACCESS_READWRITE;

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
    elem_payload.access_control = ACCESS_READWRITE;
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

    AWE_LOGI( "Reading TLV value for control %s", ctl_info.id.name.c_str());

    memset(&buff.tlv[0], 0, MAX_TLV_SIZE);

    elem_payload.name = ctl_info.id.name;
    elem_payload.control_id = buff.request.control_id;
    elem_payload.type = ctl_info.type;
    elem_payload.access_control = ACCESS_TLV_READWRITE;
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
    elem_payload.access_control = ACCESS_TLV_READWRITE;

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
    enum VirtioPCMState stream_state;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMMsg failed to find gvm id: %u", gvm_id);
        return;
    }

    fe_info = all_fe_dais_[gvm_id][args.stream_id];

    {
        std::lock_guard<std::mutex> lock(pcm_stream_threads_mutex);
        if (pcm_stream_threads_[gvm_id].find(fe_info.name) == pcm_stream_threads_[gvm_id].end()) {
            AWE_LOGE("ProcessPCMMsg failed to find fe dai name %s", fe_info.name.c_str());
            return;
        }
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
        std::unique_lock<std::mutex> streams_map_lock(pcm_stream_threads_mutex);
        auto it = pcm_stream_threads_[gvm_id].find(fe_info.name);
        if (it == pcm_stream_threads_[gvm_id].end() || !it->second) {
            AWE_LOGE("pcm_stream_threads_[%d][%s] is empty, fe thread is already deleted", gvm_id, fe_info.name.c_str());
            return;
        }
        auto pcm_thread_ptr = it->second;
        auto pit = pcm_pending_threads_[gvm_id].find(fe_info.name);
        if (pit == pcm_pending_threads_[gvm_id].end() || !pit->second ) {
            AWE_LOGE("pcm_pending_threads_[%d][%s] is empty, fe thread is already deleted", gvm_id, fe_info.name.c_str());
            return;
        }
        auto pcm_pending_thread_ptr = pit->second;
        // Pre-wait shutdown check
        if (pcm_thread_ptr->stop_flag.load()) {
            AWE_LOGI("Stop flag set for stream[%d][%s], exiting thread", gvm_id, fe_info.name.c_str());
            return;
        }

        streams_map_lock.unlock();
        {
            std::unique_lock<std::mutex> stream_lock(pcm_thread_ptr->stream_mutex);

            /* wait until ANYONE of below
             * 1. msg_queue is NOT empty && in_flight_cnt is NOT 0
             * 2. device state is stopped || closed
             */
            pcm_thread_ptr->stream_cond.wait(stream_lock,
                        [this,gvm_id=gvm_id,fe_name=fe_info.name,stream_id=args.stream_id, pcm_thread_ptr] {
                        return pcm_thread_ptr->stop_flag.load() ||
                                (((!pcm_thread_ptr->msg_queue.empty()) &&
                                 (pcm_thread_ptr->in_flight_cnt != 0)) ||
                                (GetPcmDeviceState(gvm_id, stream_id) == VIRTIO_PCM_STOPPED) ||
                                (GetPcmDeviceState(gvm_id, stream_id) == VIRTIO_PCM_CLOSED));
                        });
            // Post-wait shutdown check
            if (pcm_thread_ptr->stop_flag.load()) {
                AWE_LOGI("Stop flag detected during wait for stream[%d][%s], exiting thread", gvm_id, fe_info.name.c_str());
                return;
            }
            stream_state = GetPcmDeviceState(gvm_id, args.stream_id);
            if (stream_state == VIRTIO_PCM_STOPPED ||
                stream_state == VIRTIO_PCM_CLOSED) {
                AWE_LOGE("exiting stream thread for stream %d", args.stream_id);
                return;
            }

            msg = pcm_thread_ptr->msg_queue.front();
            if (msg == nullptr) {
                AWE_LOGE("msg is null, ignore msg");
                pcm_thread_ptr->msg_queue.pop();
                continue;
            }

#ifdef OFF_TARGET_UNIT_TEST
            return;
#endif
        }

        {
            std::lock_guard<std::mutex> lock(dma_export_map_mutex);
            if (dma_export_map_[gvm_id].find(fe_info.id) == dma_export_map_[gvm_id].end())
            {
                AWE_LOGE("Notice!!! Warning!!! dma_export_map_ is not ready for fe %s",
                    fe_info.name.c_str());
                ret = -1;
                goto pending_handle;
            }
            heap_fd = dma_export_map_[gvm_id][fe_info.id].fd;
        }

        pcm_buf_msg.offset = msg->desc.offset;
        pcm_buf_msg.size = msg->length;
        pcm_buf_msg.fe_dai_name = fe_info.name;
        pcm_buf_msg.buff_fd = (uint64_t)heap_fd;
        pcm_buf_msg.dma_bytes = msg->desc.dma_bytes;

        if ((GetPcmDeviceState(gvm_id, args.stream_id) == VIRTIO_PCM_STARTED) ||
            (GetPcmDeviceState(gvm_id, args.stream_id) == VIRTIO_PCM_PREPARED)) {
#if !defined(__QNX__)
            if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
                ret = plugin_utils_[gvm_id].pcm_plugin->StreamWrite(pcm_buf_msg);
            } else {
                ret = plugin_utils_[gvm_id].pcm_plugin->StreamRead(pcm_buf_msg);
            }
#else
            if (direction == VIRTIO_STREAM_PLAYBACK) {
                ret = plugin_utils_[gvm_id].pcm_plugin->StreamWrite(pcm_buf_msg);
            } else {
                ret = plugin_utils_[gvm_id].pcm_plugin->StreamRead(pcm_buf_msg);
            }
#endif
        } else {
            AWE_LOGE("Skip Write/Read msg since PCM is not in started or prepared state: fe_dai_name=%s",
                    pcm_buf_msg.fe_dai_name.c_str());
            ret = -1;
        }

pending_handle:
        if (ret) {
            /* error when RW index */
            DropPcmMsg(sock_info.vcid, msg);
        } else {
            /* logically the corresponding pending thread is alive
             * DO NOT hold global lock pcm_stream_threads_mutex for better performance
             */
            {
                std::lock_guard<std::mutex> pending_lock(pcm_pending_thread_ptr->stream_mutex);
                pcm_pending_thread_ptr->msg_queue.push(msg);
            }
            pcm_pending_thread_ptr->stream_cond.notify_one();
        }

        {
            std::lock_guard<std::mutex> empty_lock(pcm_thread_ptr->stream_mutex);
            pcm_thread_ptr->msg_queue.pop();
            /* it's under stopping when in_flight_cnt >= 0
             * no pcm_msg to be handled only remain in msg queue when in_flight_cnt == 0
             */
            if (pcm_thread_ptr->in_flight_cnt > 0) {
                pcm_thread_ptr->in_flight_cnt--;
            }
            if (pcm_thread_ptr->in_flight_cnt == 0) {
                pcm_thread_ptr->stream_ctrl_cond.notify_one();
            }
        }
    }
}

void AudioDeviceManager::ProcessPCMPendingMsg(struct PCMMsgArgs args)
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
    enum VirtioPCMState stream_state;

    AWE_LOGD("Enter");

    if (all_fe_dais_.find(gvm_id) == all_fe_dais_.end() ||
        plugin_utils_[gvm_id].pcm_plugin == nullptr) {
        AWE_LOGE("ProcessPCMMsg failed to find gvm id: %u", gvm_id);
        return;
    }

    fe_info = all_fe_dais_[gvm_id][args.stream_id];

    {
        std::lock_guard<std::mutex> lock(pcm_stream_threads_mutex);
        if (pcm_pending_threads_[gvm_id].find(fe_info.name) ==
                        pcm_pending_threads_[gvm_id].end()) {
            AWE_LOGE("ProcessPCMMsg failed to find fe dai name %s", fe_info.name.c_str());
            return;
        }
    }

    pthread_setname_np(pthread_self(), "awe_adm_pendmsg");

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

    while(1)
    {
        std::unique_lock<std::mutex> streams_map_lock(pcm_stream_threads_mutex);
        auto it = pcm_pending_threads_[gvm_id].find(fe_info.name);
        if (it == pcm_pending_threads_[gvm_id].end() || !it->second ) {
            AWE_LOGE("pcm_pending_threads_[%d][%s] is empty, fe thread is already deleted", gvm_id, fe_info.name.c_str());
            return;
        }
        auto pcm_pending_thread_ptr = it->second;
        // Pre-wait shutdown check
        if (pcm_pending_thread_ptr->stop_flag.load()) {
            AWE_LOGI("Stop flag set for stream[%d][%s], exiting thread", gvm_id, fe_info.name.c_str());
            return;
        }
        streams_map_lock.unlock();
        {
            std::unique_lock<std::mutex> pending_lock(pcm_pending_thread_ptr->stream_mutex);

            pcm_pending_thread_ptr->stream_cond.wait(pending_lock,
                        [this,gvm_id=gvm_id,fe_name=fe_info.name,stream_id=args.stream_id, pcm_pending_thread_ptr] {
                        return pcm_pending_thread_ptr->stop_flag.load() ||
                                ((!pcm_pending_thread_ptr->msg_queue.empty()) ||
                                (GetPcmDeviceState(gvm_id, stream_id) == VIRTIO_PCM_STOPPED) ||
                                (GetPcmDeviceState(gvm_id, stream_id) == VIRTIO_PCM_CLOSED));
                        });
            // Post-wait shutdown check
            if (pcm_pending_thread_ptr->stop_flag.load()) {
                AWE_LOGI("Stop flag detected during wait for stream[%d][%s], exiting thread", gvm_id, fe_info.name.c_str());
                return;
            }
            stream_state = GetPcmDeviceState(gvm_id, args.stream_id);
            if (stream_state == VIRTIO_PCM_STOPPED ||
                stream_state == VIRTIO_PCM_CLOSED) {
                AWE_LOGE("exiting pending thread for stream %d", args.stream_id);
                return;
            }

            msg = pcm_pending_thread_ptr->msg_queue.front();
            if (msg == nullptr) {
                AWE_LOGE("msg is null, ignore msg");
                pcm_pending_thread_ptr->msg_queue.pop();
                continue;
            }

#ifdef OFF_TARGET_UNIT_TEST
            return;
#endif
        }

        {
            std::lock_guard<std::mutex> lock(dma_export_map_mutex);
            if (dma_export_map_[gvm_id].find(fe_info.id) == dma_export_map_[gvm_id].end())
            {
                AWE_LOGE("Notice!!! Warning!!! dma_export_map_ is not ready for fe %s", fe_info.name.c_str());
                ret = -1;
                goto skip;
            }
            heap_fd = dma_export_map_[gvm_id][fe_info.id].fd;
        }

        pcm_buf_msg.offset = msg->desc.offset;
        pcm_buf_msg.size = msg->length;
        pcm_buf_msg.fe_dai_name = fe_info.name;
        pcm_buf_msg.buff_fd = (uint64_t)heap_fd;
        pcm_buf_msg.dma_bytes = msg->desc.dma_bytes;

        if ((GetPcmDeviceState(gvm_id, args.stream_id) == VIRTIO_PCM_STARTED) ||
            (GetPcmDeviceState(gvm_id, args.stream_id) == VIRTIO_PCM_PREPARED)) {
#if !defined(__QNX__)
            if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
                ret = plugin_utils_[gvm_id].pcm_plugin->StreamWriteDone(pcm_buf_msg);
            }
#else
            if (direction == VIRTIO_STREAM_PLAYBACK) {
                ret = plugin_utils_[gvm_id].pcm_plugin->StreamWriteDone(pcm_buf_msg);
            }
#endif
        } else {
            AWE_LOGE("Skip Write/Read msg since PCM is not in started or prepared state: fe_dai_name=%s",
                        pcm_buf_msg.fe_dai_name.c_str());
            ret = -1;
        }

skip:
        if (!ret)
            msg->status.status = VIRTIO_SND_S_OK;
        else
            msg->status.status = VIRTIO_SND_S_IO_ERR;

        msg->length += sizeof(msg->status);

        AWE_LOGD("Hab responding to virtio sound: ret %d", (uint32_t)ret);
        ihab_->Send(sock_info.vcid, msg, sizeof(struct virtio_pcm_msg), 0);
        free(msg);

        {
            std::lock_guard<std::mutex> empty_lock(pcm_pending_thread_ptr->stream_mutex);
            pcm_pending_thread_ptr->msg_queue.pop();
            if (pcm_pending_thread_ptr->msg_queue.empty()) {
                pcm_pending_thread_ptr->stream_ctrl_cond.notify_one();
            }
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
    elem_payload.access_control = ACCESS_READWRITE;
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
    elem_payload.access_control = ACCESS_READWRITE;
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
