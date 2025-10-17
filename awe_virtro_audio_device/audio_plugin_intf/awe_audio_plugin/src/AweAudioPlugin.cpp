/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#if !defined (__QNX__)
#include <glib.h>
#endif
#include "AweAudioPlugin.h"
#include "awe_log_util.h"
#include "awe_mixer_control.h"

#if !defined (__QNX__)
#define strlcpy g_strlcpy
#endif
#define MAX_TLV_SIZE 4096
#define AWE_TLV_HEADER_SIZE (2 * sizeof(uint32_t))

const std::map<uint32_t, uint32_t> sampleRateMap = {
    {VIRTIO_SND_PCM_RATE_5512, 5512},
    {VIRTIO_SND_PCM_RATE_8000, 8000},
    {VIRTIO_SND_PCM_RATE_11025, 11025},
    {VIRTIO_SND_PCM_RATE_12000, 12000},
    {VIRTIO_SND_PCM_RATE_16000, 16000},
    {VIRTIO_SND_PCM_RATE_22050, 22050},
    {VIRTIO_SND_PCM_RATE_24000, 24000},
    {VIRTIO_SND_PCM_RATE_32000, 32000},
    {VIRTIO_SND_PCM_RATE_44100, 44100},
    {VIRTIO_SND_PCM_RATE_48000, 48000},
    {VIRTIO_SND_PCM_RATE_64000, 64000},
    {VIRTIO_SND_PCM_RATE_88200, 88200},
    {VIRTIO_SND_PCM_RATE_96000, 96000},
    {VIRTIO_SND_PCM_RATE_176400, 176400},
    {VIRTIO_SND_PCM_RATE_192000, 192000},
    {VIRTIO_SND_PCM_RATE_384000, 384000}
};

const std::map<uint32_t, uint32_t> formatMap = {
    {VIRTIO_SND_PCM_FMT_IMA_ADPCM, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_MU_LAW, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_A_LAW, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S8, AUDIO_PCM_FORMAT_S8},
    {VIRTIO_SND_PCM_FMT_U8, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S16, AUDIO_PCM_FORMAT_S16_LE},
    {VIRTIO_SND_PCM_FMT_U16, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S18_3, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U18_3, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S20_3, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U20_3, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S24_3, AUDIO_PCM_FORMAT_S24_3LE},
    {VIRTIO_SND_PCM_FMT_U24_3, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S20, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U20, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S24, AUDIO_PCM_FORMAT_S24_LE},
    {VIRTIO_SND_PCM_FMT_U24, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S32, AUDIO_PCM_FORMAT_S32_LE},
    {VIRTIO_SND_PCM_FMT_U32, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_FLOAT, AUDIO_PCM_FORMAT_FLOAT_LE},
    {VIRTIO_SND_PCM_FMT_FLOAT64, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_DSD_U8, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_DSD_U16, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_DSD_U32, AUDIO_PCM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME, AUDIO_PCM_FORMAT_INVALID}
};

STATIC_FUNC int WaitIndexBufferAvailable(AweStream *stream, uint8_t index, uint32_t event_id)
{
    if (stream == nullptr){
        AWE_LOGE("Invalide stream");
        return AWE_EBADPARAM;
    }
    if (stream->buf_pool.num_buffs <= index){
        AWE_LOGE("Invalide index input");
        return AWE_EBADPARAM;
    }
    std::unique_lock<std::mutex> buf_lock(stream->buf_pool.rw_mutex);
    do {
        if (!stream->buf_pool.buf_used[index]) {
            stream->buf_pool.buf_used[index] = 1;
            stream->buf_pool.free_count--;
            break;
        }

        stream->buf_pool.rw_cond.wait(buf_lock, [stream, event_id]{return stream->stream_event & event_id;});
        stream->stream_event &= ~event_id;
    } while (1);
    stream->stream_event &= ~event_id;

    //TIMEOUT case...
    return AWE_EOK;
}

STATIC_FUNC int MarkIndexBufferAvailable(AweStream *stream, uint8_t index, uint32_t event_id)
{
    if (stream == nullptr){
        AWE_LOGE("Invalide stream");
        return AWE_EBADPARAM;
    }
    if (stream->buf_pool.num_buffs <= index){
        AWE_LOGE("Invalide index input");
        return AWE_EBADPARAM;
    }
    std::unique_lock<std::mutex> buf_lock(stream->buf_pool.rw_mutex);
    if (stream->buf_pool.buf_used[index] != 0) {
        stream->buf_pool.buf_used[index] = 0;
        stream->buf_pool.free_count++;
    }
    stream->stream_event |= AWE_STREAM_EVENT_BUF_AVAIL;
    stream->buf_pool.rw_cond.notify_one();
    return AWE_EOK;
}

STATIC_FUNC int WaitStreamEOS(AweStream *stream, uint32_t event_id)
{
    if (stream == nullptr){
        AWE_LOGE("Invalide stream");
        return AWE_EBADPARAM;
    }
    std::unique_lock<std::mutex> stream_lock(stream->stream_mutex);
    stream->stream_cond.wait(stream_lock, [stream, event_id]{return stream->stream_event & event_id;});
    stream->stream_event &= ~event_id;
    return AWE_EOK;
}

//callback event handle need to conside the mutex
STATIC_FUNC void awe_pcm_event_cb(struct awemgr_event *event_params, void* client_data)
{
    uint8_t index = 0;
    if (event_params == nullptr || client_data == nullptr) {
        AWE_LOGE("Invalid input args");
        return;
    }

    AweStream* stream = (AweStream*)client_data;
    switch (event_params->eventType) {
    case AWE_EVENT_READ_DONE:
    case AWE_EVENT_WRITE_DONE: {
    /* TODO  support this mode later
        struct awe_event_rw_done_payload* payload =
            (struct awe_event_rw_done_payload*)(event_params->param);
        index = (payload->offset) / (stream->buf_pool.buf_size);
        MarkIndexBufferAvailable(stream, index, AWE_STREAM_EVENT_BUF_AVAIL);
    */
        break;
    }
    case AWE_EVENT_EARLY_EOS: {
        break;
    }
    case AWE_EVENT_EOS_RENDERED: {
        break;
    }
    default:
        break;
    }
}

STATIC_FUNC void AweEventCallbackFunc(const awemgr_event *ev, void *client_data)
{
    AweAudioPlugin *awe_plug = (AweAudioPlugin*)client_data;
    const char *event_name = ev->module.name;
    unsigned int instanceId = ev->module.instanceId;
    unsigned int objectId = ev->module.objectId;
    uint32_t eventType = ev->eventType;

    AWE_LOGD("%s: Got control from name %s", __func__, event_name);

    if ((objectId == AWE_SSR_EVENT_OBJECT_ID) && (instanceId == AWE_MAX_PROCESSOR_CORES) &&
        (!strcmp(event_name, AWE_SSR_EVENT_NAME))) {

        enum SSRState ssrState = SSR_STATE_UNKNOWN;
        switch(eventType) {
            case AWE_SSR_EVENT_CONTROLLER_DOWN:
            {
                ssrState = SSR_STATE_DOWN;
                break;
            }
            case AWE_SSR_EVENT_CONTROLLER_RECOVERY_COMPLETE:
            {
                ssrState = SSR_STATE_UP;
                break;
            }
            default:
            {
                AWE_LOGE("SSR callback called for unsupported event type %04x", eventType);
                return;
            }
        }
        if (awe_plug->adm_ssr_cb)
            awe_plug->adm_ssr_cb(ssrState);
        else
            AWE_LOGE("SSR callback not registered");
    }
    else {

        AweCtrlValue *AweCtrl = awe_plug->GetAweCtrlFromName(event_name);
        if (AweCtrl == nullptr) {
            AWE_LOGE("AweCtrl structure not found for event control %s", ev->module.name);
            return;
        }
        struct GVMEventCallbackParams *eventTLV = (struct GVMEventCallbackParams*)AweCtrl->elem_payload.value.tlv.tlv;
        eventTLV->source_module_id = objectId;
        eventTLV->event_id = eventType;
        eventTLV->param_size = ev->sizeInBytes;
        memcpy(&eventTLV->param, ev->payload, eventTLV->param_size);

        AWE_LOGD("AweEventCallbackFunc: callback called for module id %d event id %d param size %d", eventTLV->source_module_id, eventTLV->event_id, eventTLV->param_size);
        awe_plug->adm_event_cb(AweCtrl->elem_payload.control_id);
    }
}

STATIC_FUNC SoundCardMixerCtl* find_mixer_ctl_by_name(vector<SoundCardMixerCtl>* controls, const std::string& name)
{
    if ((controls == nullptr) || (name.empty())) {
        AWE_LOGE("Invalid input parameter");
        return nullptr;
    }
    for (auto& ctl : *controls) {
        if (ctl.id.name == name)
            return &ctl; //Found, return a pointer to the element
    }
    return nullptr; // Not found, return nullptr
}

AweCtrlValue* AweAudioPlugin::GetAweCtrlFromName(const char *ctrl_name)
{
    if (ctrl_name == nullptr)
        return nullptr;

    auto it = kctrl_value_map_.find(ctrl_name);
    if (it == kctrl_value_map_.end()) {
        AWE_LOGE("Unable to find AweCtrl from name %s", ctrl_name);
        return nullptr;
    }
    return it->second;
}

//Function to set a named control
uint32_t AweAudioPlugin::AweSetNamedControl(AweCtrlValue *AweCtrl, const CtlElemPayload &ctl_elem_payload, awe_control_type_t type)
{
    uint32_t ret = AWE_EOK;
    uint32_t payload_sz = 0;
    awe_named_control_param_t * ctrl_param = NULL;

    AWE_LOGD("ctl access_control %d AweCtrl->elem_count %d!!", ctl_elem_payload.access_control, AweCtrl->elem_count);

    if (ctl_elem_payload.access_control == tlv_bytes) {
        AweCtrlVectorValue_t * plugin_ctl_data = (AweCtrlVectorValue_t *)&ctl_elem_payload.value.tlv.tlv[0];
        if (plugin_ctl_data->do_set == 0)
        {
            //do set =0, stored offset and count into AweCtrl.
            AweCtrlVectorValue_t * AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];
            AweCtrldata->count = plugin_ctl_data->count;
            AweCtrldata->offset = plugin_ctl_data->offset;

            AWE_LOGD("cache AweCtrldata->count %d offset %d, not set to dsp!!", AweCtrldata->count, AweCtrldata->offset);
            return ret;
        } else {
            //do set =1, use offset and count stored in AweCtrl.
            AweCtrlVectorValue_t * AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];
            if (AweCtrldata->count <= 0 || AweCtrldata->count > AweCtrl->elem_count) {
                AWE_LOGE("(count %d) mismatch, use AweCtrl->elem_count %d as default!!", AweCtrldata->count, AweCtrl->elem_count);
                AweCtrldata->count = AweCtrl->elem_count - AWE_TLV_HEADER_SIZE;//two int is append for offset,count and do_set.
            }
            if (AweCtrldata->count > MAX_TLV_SIZE || AweCtrldata->count <= 0) {
                AWE_LOGE("Unsuppoted TLV size %d", AweCtrldata->count);
                return AWE_EBADPARAM;
            }

            AWE_LOGD("AweCtrldata->count %d offset %d!!", AweCtrldata->count, AweCtrldata->offset);

            //count is byte lengths
            ctrl_param = (awe_named_control_param_t*)calloc(1, sizeof(awe_named_control_param_t) + AweCtrldata->count);
            if (ctrl_param == NULL) {
                AWE_LOGE("Fail to allocate control!!");
                return AWE_ENOMEMORY;
            }
            ctrl_param->offset = AweCtrldata->offset;
            ctrl_param->type = type;

            switch (type) {
                case AWE_CONTROL_TYPE_FLOAT: {
                    ctrl_param->count = AweCtrldata->count/sizeof(float);
                    payload_sz = AweCtrldata->count;
                    memcpy((void *)ctrl_param->value.floating, &plugin_ctl_data->value.floating, ctrl_param->count * sizeof(float));

                    for (int i = 0; i < ctrl_param->count; i++)
                        AWE_LOGD("ctrl_param->value.floating [%d]: value %f", i, ctrl_param->value.floating[i]);
                    break;
                }
                case AWE_CONTROL_TYPE_INT: {
                    ctrl_param->count = AweCtrldata->count/sizeof(uint32_t);
                    payload_sz = AweCtrldata->count;
                    memcpy((void *)ctrl_param->value.integer, &plugin_ctl_data->value.integer, ctrl_param->count * sizeof(uint32_t));

                    for (int i = 0; i < ctrl_param->count; i++)
                        AWE_LOGD("ctrl_param->value.integer [%d]: value %f", i, ctrl_param->value.integer[i]);
                    break;
                }
                default: {
                    AWE_LOGE("Unrecognized TLV awe_control_type %d", type);
                    ret = AWE_EBADPARAM;
                    goto exit;
                }
            }
        }
    } else {
        ctrl_param = (awe_named_control_param_t*)calloc(1, sizeof(awe_named_control_param_t) + AweCtrl->elem_count * sizeof(uint32_t));
        //here allocate size is bigger than expected for float type.
        if (ctrl_param == NULL) {
            AWE_LOGE("Fail to allocate control!!");
            return AWE_ENOMEMORY;
        }
        ctrl_param->offset = 0;
        ctrl_param->type = type;

        switch (type) {
            case AWE_CONTROL_TYPE_INT: {
                ctrl_param->count = AweCtrl->elem_count;
                payload_sz = ctrl_param->count * sizeof(int32_t);
                memcpy((void *)ctrl_param->value.integer, &AweCtrl->elem_payload.value.integer.value, ctrl_param->count * sizeof(int32_t));
                break;
            }
            case AWE_CONTROL_TYPE_FLOAT: {
                ctrl_param->count = (AweCtrl->elem_count)/sizeof(float);
                payload_sz = ctrl_param->count * sizeof(float);
                memcpy((void *)ctrl_param->value.floating, &AweCtrl->elem_payload.value.bytes.data, ctrl_param->count * sizeof(float));
                break;
            }
            default: {
                AWE_LOGE("Unrecognized awe_control_type %d", type);
                ret = AWE_EBADPARAM;
                goto exit;
            }
        }
    }

    AWE_LOGD("ctrl_param->offset %d, ctrl_param->count %d, payload_sz %d!!", ctrl_param->offset, ctrl_param->count, payload_sz);

    snprintf(reinterpret_cast<char*>(ctrl_param->name), AWE_MAX_CONTROL_NAME_LENGTH, "%s", AweCtrl->elem_payload.name.c_str());
    ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_SET_NAMED_CONTROL, ctrl_param, sizeof(awe_named_control_param_t) + payload_sz);
    if (ret)
        AWE_LOGE("AWE set control: %s failed with rc = %d", AweCtrl->elem_payload.name.c_str(), ret);

exit:
    if (ctrl_param != NULL)
        free(ctrl_param);
    ctrl_param = NULL;
    return ret;
}

// Function to get a named control
uint32_t AweAudioPlugin::AweGetNamedControl(AweCtrlValue *AweCtrl, const CtlElemPayload &ctl_elem_payload, awe_control_type_t type)
{
    uint32_t ret = AWE_EOK;
    awe_named_control_param_t* ctrl_param = NULL;
    uint32_t payload_sz = 0;
    AweCtrlVectorValue_t * plugin_ctl_data = (AweCtrlVectorValue_t *)&ctl_elem_payload.value.tlv.tlv[0];
    AweCtrlVectorValue_t * AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];

    AWE_LOGD("ctl access_control %d AweCtrl->elem_count %d!!", ctl_elem_payload.access_control, AweCtrl->elem_count);

    if (ctl_elem_payload.access_control == tlv_bytes) {
        if (plugin_ctl_data->do_set != 0) {
            AWE_LOGE("Not expected do_set flag=%d for get", plugin_ctl_data->do_set);
            return AWE_EUNEXPECTED;
        }
        //do set = 0, use offset and count stored in AweCtrl.
        if (AweCtrldata->count <= 0 || AweCtrldata->count > AweCtrl->elem_count) {
            AWE_LOGE("Data length (count %d) is missing, use AweCtrl->elem_count as default length!!", AweCtrldata->count);
            AweCtrldata->offset = 0;
            AweCtrldata->count = AweCtrl->elem_count - AWE_TLV_HEADER_SIZE; //two int is append for offset,count and do_set.
        }
        if (AweCtrldata->count > MAX_TLV_SIZE || AweCtrldata->count <= 0) {
            AWE_LOGE("Unsuppoted TLV size %d", AweCtrldata->count);
            return AWE_EBADPARAM;
        }

        AWE_LOGD("AweCtrldata->count %d, offset %d!", AweCtrldata->count, AweCtrldata->offset);

        ctrl_param = (awe_named_control_param_t*)calloc(1, sizeof(awe_named_control_param_t) + AweCtrldata->count);
        if (ctrl_param == NULL) {
            AWE_LOGE("Fail to allocate control!!");
            return AWE_ENOMEMORY;
        }
        ctrl_param->offset = AweCtrldata->offset;
        ctrl_param->type = type;

        switch (type) {
            case AWE_CONTROL_TYPE_FLOAT: {
                ctrl_param->count = AweCtrldata->count/sizeof(float);
                payload_sz = ctrl_param->count*sizeof(float);
                break;
            }
            case AWE_CONTROL_TYPE_INT: {
                ctrl_param->count = AweCtrldata->count/sizeof(uint32_t);
                payload_sz = ctrl_param->count*sizeof(uint32_t);
                break;
            }
            default: {
                AWE_LOGE("Unrecognized TLV awe_control_type %d", type);
                ret = AWE_EBADPARAM;
                goto exit;
            }
        }
    } else {
        ctrl_param = (awe_named_control_param_t*)calloc(1, sizeof(awe_named_control_param_t) + AweCtrl->elem_count * sizeof(uint32_t));
        //here allocate size is bigger than expected for float type.
        if (ctrl_param == NULL) {
            AWE_LOGE("Fail to allocate control!!");
            return AWE_ENOMEMORY;
        }
        ctrl_param->offset = 0;
        ctrl_param->type = type;

        switch (type) {
            case AWE_CONTROL_TYPE_INT: {
                ctrl_param->count = AweCtrl->elem_count;
                payload_sz = ctrl_param->count*sizeof(uint32_t);
                break;
            }
            case AWE_CONTROL_TYPE_FLOAT: {
                ctrl_param->count = (AweCtrl->elem_count)/sizeof(float);
                payload_sz = ctrl_param->count*sizeof(float);
                break;
            }
            default: {
                AWE_LOGE("Unrecognized awe_control_type %d", type);
                ret = AWE_EBADPARAM;
                goto exit;
            }
        }
    }

    AWE_LOGD("ctrl_param->count %d, ctrl_param->offset %d payload_sz %d!", ctrl_param->count, ctrl_param->offset, payload_sz);

    snprintf(reinterpret_cast<char*>(ctrl_param->name), AWE_MAX_CONTROL_NAME_LENGTH, "%s", AweCtrl->elem_payload.name.c_str());

    ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_GET_NAMED_CONTROL, ctrl_param, sizeof(awe_named_control_param_t) + payload_sz);
    if (ret) {
        AWE_LOGE("AWE get control: %s failed with rc = %d", AweCtrl->elem_payload.name.c_str(), ret);
        goto exit;
    }

    if (ctl_elem_payload.access_control == tlv_bytes) {
        switch (type) {
            case AWE_CONTROL_TYPE_FLOAT: {
                memcpy(&AweCtrldata->value.floating, ctrl_param->value.floating, ctrl_param->count * sizeof(float));

                for(int i=0; i < ctrl_param->count; i++)
                    AWE_LOGD("ctrl_param float data[%d]: %f", i, ctrl_param->value.floating[i]);
                break;
            }
            case AWE_CONTROL_TYPE_INT: {
                memcpy(&AweCtrldata->value.integer, ctrl_param->value.integer, ctrl_param->count * sizeof(uint32_t));

                for(int i=0; i < ctrl_param->count; i++)
                    AWE_LOGD("ctrl_param integer data[%d]: %d", i, ctrl_param->value.integer[i]);
                break;
            }
            default: {
                AWE_LOGE("Unrecognized TLV awe_control_type %d", type);
                ret = AWE_EBADPARAM;
                goto exit;
            }
        }
    } else {
        switch (type) {
            case AWE_CONTROL_TYPE_INT: {
                memcpy(&AweCtrl->elem_payload.value.integer.value, ctrl_param->value.integer,
                    ctrl_param->count * sizeof(int32_t));
                break;
            }
            case AWE_CONTROL_TYPE_FLOAT: {
                memcpy(&AweCtrl->elem_payload.value.bytes.data, ctrl_param->value.floating,
                    ctrl_param->count * sizeof(float));
                break;
            }
            default: {
                AWE_LOGE("Unrecognized awe_control_type %d", type);
                ret = AWE_EBADPARAM;
                goto exit;
            }
        }
    }

exit:
    if (ctrl_param != NULL)
        free(ctrl_param);
    ctrl_param = NULL;
    return ret;
}

AweAudioPlugin::AweAudioPlugin(vector<FrontendInfo> *all_fe_dai,
        vector<SoundCardMixerCtl> *all_mixer_ctl)
        : IAudioPlugin(all_fe_dai),all_mixer_ctl_(all_mixer_ctl)
{
    awe_snd_card_handle_ = 0;
    if ((all_fe_dai != nullptr) || (all_mixer_ctl != nullptr))
        InitClassResource();
}

AweAudioPlugin::~AweAudioPlugin()
{
    ReleaseClassResource();
}

uint32_t AweAudioPlugin::DcWrite(const CtlElemPayload &ctl_elem_payload)
{
    uint32_t ret = AWE_EOK;
    // Find the control in the kctrl_value_map_
    auto it = kctrl_value_map_.find(ctl_elem_payload.name);
    if (it == kctrl_value_map_.end()) {
        AWE_LOGE("AWE control: %s not found", ctl_elem_payload.name.c_str());
        return AWE_EBADPARAM;
    }
    // (1) Handle the request from tinyalsa
    AweCtrlValue* AweCtrl = it->second;
    AweCtrl->elem_payload.control_id = ctl_elem_payload.control_id;
    AweCtrl->elem_payload.type = ctl_elem_payload.type;
    AweCtrl->elem_payload.access_control = ctl_elem_payload.access_control;
    AWE_LOGI("AWE control: %s DcWrite, control id = %d, elem_ctl_id = %d elem_count %d",
        ctl_elem_payload.name.c_str(), AweCtrl->elem_payload.control_id, AweCtrl->elem_ctrl_id, AweCtrl->elem_count);
#if !defined(__QNX__)
    //(2) Cache the control value
    if ((ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
        || (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)){
        memcpy(&AweCtrl->elem_payload.value.integer.value, &ctl_elem_payload.value.integer.value,
           AweCtrl->elem_count * sizeof(int32_t));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcWrite integer value [%d]: %ld", i, AweCtrl->elem_payload.value.integer.value[i]);
        }
    }
    else if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER64 ) {
        memcpy(&AweCtrl->elem_payload.value.integer64.value, &ctl_elem_payload.value.integer64.value,
           AweCtrl->elem_count * sizeof(int64_t));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcWrite integer64 value[%d]: %lld", i, AweCtrl->elem_payload.value.integer64.value[i]);
        }
    }
    else if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        memcpy(&AweCtrl->elem_payload.value.enumerated.item, &ctl_elem_payload.value.enumerated.item,
            AweCtrl->elem_count * sizeof(unsigned int));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcWrite enumerated item[%d]: %d", i, AweCtrl->elem_payload.value.enumerated.item[i]);
        }
    }
    else if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES) {
        if (ctl_elem_payload.access_control == bytes) {
            memcpy(&AweCtrl->elem_payload.value.bytes.data, &ctl_elem_payload.value.bytes.data,
                AweCtrl->elem_count*sizeof(unsigned char));
            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcWrite bytes data[%d]: %d", i, AweCtrl->elem_payload.value.bytes.data[i]);
        } else if( ctl_elem_payload.access_control == tlv_bytes) {
                //will cache the value in AweSetNamedControl
                AWE_LOGD("skip cache bytes data !!!");
        }
        else {
            AWE_LOGE("Unsupported Access type 0x%x for AWE control %s",
                ctl_elem_payload.access_control, ctl_elem_payload.name.c_str());
            ret = AWE_EBADPARAM;
        }
    }
#else
    //(2) Cache the control value
    if ((ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER)
        || (ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_BOOLEAN)){
        memcpy(&AweCtrl->elem_payload.value.integer.value, &ctl_elem_payload.value.integer.value,
           AweCtrl->elem_count * sizeof(int32_t));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcWrite integer value [%d]: %ld", i, AweCtrl->elem_payload.value.integer.value[i]);
        }
    }
    else if (ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER64 ) {
        memcpy(&AweCtrl->elem_payload.value.integer64.value, &ctl_elem_payload.value.integer64.value,
           AweCtrl->elem_count * sizeof(int64_t));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcWrite integer64 value[%d]: %lld", i, AweCtrl->elem_payload.value.integer64.value[i]);
        }
    }
    else if (ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_ENUMERATED) {
        memcpy(&AweCtrl->elem_payload.value.enumerated.item, &ctl_elem_payload.value.enumerated.item,
            AweCtrl->elem_count * sizeof(unsigned int));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcWrite enumerated item[%d]: %d", i, AweCtrl->elem_payload.value.enumerated.item[i]);
        }
    }
    else if (ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_BYTES) {
        if (ctl_elem_payload.access_control == bytes) {
            memcpy(&AweCtrl->elem_payload.value.bytes.data, &ctl_elem_payload.value.bytes.data,
                AweCtrl->elem_count*sizeof(unsigned char));
            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcWrite bytes data[%d]: %d", i, AweCtrl->elem_payload.value.bytes.data[i]);
        } else if( ctl_elem_payload.access_control == tlv_bytes) {
                //will cache the value in AweSetNamedControl
                AWE_LOGD("skip cache bytes data !!!");
        }
        else {
            AWE_LOGE("Unsupported Access type 0x%x for AWE control %s",
                ctl_elem_payload.access_control, ctl_elem_payload.name.c_str());
            ret = AWE_EBADPARAM;
        }
    }
#endif
    //(3) Send set control value to DSP
    SoundCardMixerCtl* mixCtl = find_mixer_ctl_by_name(all_mixer_ctl_, ctl_elem_payload.name);
    if (mixCtl == nullptr) {
        AWE_LOGE("No element with name : %s ", ctl_elem_payload.name.c_str());
        return AWE_EBADPARAM;
    }
    switch (mixCtl->ctrl_mode) {
        case NAMED_CONTROL: {
            AWE_LOGI("Set named control command: %s ", ctl_elem_payload.name.c_str());
            ret = AweSetNamedControl(AweCtrl, ctl_elem_payload, mixCtl->awe_ctl_type);
            break;
        }
        case STATIC_CONTROL_NODSP: {
            AWE_LOGD("Set static control without DSP command: %s ", ctl_elem_payload.name.c_str());
            if (!strncmp(ctl_elem_payload.name.c_str(), "AWE getMmapFD", strlen("AWE getMmapFD"))) {
                AWE_LOGD("Cache mmap fd for requested pcm device in AweCtrl structure");
                int fe_id = ctl_elem_payload.value.integer.value[0];
                for (auto& fe_iter : fe_dai_awe_map_) {
                    AweStream *stream = fe_iter.second;
                    if (stream->device_id == fe_id) {
                        AweCtrl->elem_payload.value.integer.value[0] = stream->gvm_mmap_fd;
                        break;
                    }
                }
            } else if (!strncmp(ctl_elem_payload.name.c_str(), "ShmemMap", strlen("ShmemMap"))) {
                AWE_LOGD("AWE MapSMMU");
                ret = AweShmemMapSMMU(AweCtrl, mixCtl->awe_ctl_type);
            } else if (!strncmp(ctl_elem_payload.name.c_str(), "ShmemUnmap", strlen("ShmemUnmap"))) {
                AWE_LOGD("AWE UnmapSMMU");
                ret = AweShmemUnMapSMMU(AweCtrl, mixCtl->awe_ctl_type);
            }
            break;
        }
        case STATIC_CONTROL_DSP: {
            AWE_LOGD("Set static control with DSP command: %s ", ctl_elem_payload.name.c_str());
            break;
        }
        case EVENT_CONTROL: {
            int isRegister = ctl_elem_payload.value.tlv.tlv[0];
            struct awe_event_reg_cfg event_sub = {0};

            AWE_LOGD("Event type mixer control command %s, value %d", ctl_elem_payload.name.c_str(), isRegister);

            strlcpy(event_sub.event_name, ctl_elem_payload.name.c_str(), AWE_MAX_EVENT_NAME_LENGTH);
            event_sub.is_register = isRegister;
            AWE_LOGD("Subcribing for events for event control : %s", event_sub.event_name);

            ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_SUBSCRIBE_EVENTS, &event_sub,
                            sizeof(struct awe_event_reg_cfg));
            if (ret != AWE_EOK && ret != AWE_EALREADY)
                AWE_LOGE("Event subscription for %s failed %d", ctl_elem_payload.name.c_str(), ret);

            break;
        }
        default: {
            AWE_LOGE("Unrecognized control mode %d", mixCtl->ctrl_mode);
            return AWE_EBADPARAM;
        }
    }
    return ret;
}

int AweAudioPlugin::DcRead(struct CtlElemPayload &ctl_elem_payload)
{
    uint32_t ret = AWE_EOK;
    // Find the control
    auto it = kctrl_value_map_.find(ctl_elem_payload.name);
    if (it == kctrl_value_map_.end()) {
        AWE_LOGE("AWE control: %s not found", ctl_elem_payload.name.c_str());
        return AWE_EBADPARAM;
    }
    // (1) Handle the request from tinyalsa
    AweCtrlValue* AweCtrl = it->second;
    AweCtrl->elem_payload.control_id = ctl_elem_payload.control_id;
    AweCtrl->elem_payload.type = ctl_elem_payload.type;
    AweCtrl->elem_payload.access_control = ctl_elem_payload.access_control;
    AWE_LOGI("AWE control: %s DcRead, control id = %d, elem_ctl_id = %d",
        ctl_elem_payload.name.c_str(), AweCtrl->elem_payload.control_id, AweCtrl->elem_ctrl_id);

    //(2) Get control value command from DSP
    SoundCardMixerCtl* mixCtl = find_mixer_ctl_by_name(all_mixer_ctl_, ctl_elem_payload.name);
    if (mixCtl == nullptr) {
        AWE_LOGE("No element with name : %s ", ctl_elem_payload.name.c_str());
        return AWE_EBADPARAM;
    }
    switch (mixCtl->ctrl_mode) {
        case NAMED_CONTROL: {
            AWE_LOGD("Get named control command: %s ", ctl_elem_payload.name.c_str());
            ret = AweGetNamedControl(AweCtrl, ctl_elem_payload, mixCtl->awe_ctl_type);
            break;
        }
        case STATIC_CONTROL_NODSP: {
            AWE_LOGD("Get static control without DSP command: %s ", ctl_elem_payload.name.c_str());
            break;
        }
        case STATIC_CONTROL_DSP: {
            AWE_LOGD("Get static control with DSP command: %s ", ctl_elem_payload.name.c_str());
            if (!strncmp(ctl_elem_payload.name.c_str(), "AWE coreErrorInfo", strlen("AWE coreErrorInfo"))) {
                ret = AweGetCoreErrorInfo(AweCtrl);
            }
            break;
        }
        case EVENT_CONTROL: {
            AWE_LOGD("Get event control command: %s", ctl_elem_payload.name.c_str());
            break;
        }
        default: {
            AWE_LOGE("Unrecognized control mode %d", mixCtl->ctrl_mode);
            return AWE_EBADPARAM;
        }
    }

    //(3) Return the control value to tinyalsa
#if !defined(__QNX__)
    if ((ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
        || (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)){
        memcpy(&ctl_elem_payload.value.integer.value, &AweCtrl->elem_payload.value.integer.value,
            AweCtrl->elem_count * sizeof(int32_t));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcRead integer value [%d]: %d", i, ctl_elem_payload.value.integer.value[i]);
        }
    }
    else if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER64 ) {
        memcpy(&ctl_elem_payload.value.integer64.value, &AweCtrl->elem_payload.value.integer64.value,
            AweCtrl->elem_count * sizeof(int64_t));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcRead integer64 value[%d]: %lld", i, ctl_elem_payload.value.integer64.value[i]);
        }
    }
    else if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        memcpy(&ctl_elem_payload.value.enumerated.item, &AweCtrl->elem_payload.value.enumerated.item,
            AweCtrl->elem_count * sizeof(unsigned int));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcRead enumerated item[%d]: %d", i,  ctl_elem_payload.value.enumerated.item[i]);
        }
    }
    else if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES) {
        if (ctl_elem_payload.access_control == bytes) {
            memcpy(&ctl_elem_payload.value.bytes.data, &AweCtrl->elem_payload.value.bytes.data,
                AweCtrl->elem_count * sizeof(unsigned char));
            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcRead bytes data[%d]: 0x%x", i, ctl_elem_payload.value.bytes.data[i]);
        }
        else if(ctl_elem_payload.access_control == tlv_bytes) {
            AweCtrlVectorValue_t * AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];
            memcpy(ctl_elem_payload.value.tlv.tlv, AweCtrl->elem_payload.value.tlv.tlv, AweCtrl->elem_count);

            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcRead tlv bytes data[%d]: 0x%x", i, AweCtrl->elem_payload.value.tlv.tlv[i]);

            //clear offset, count and do_set in AweCtrl
            AweCtrldata->offset = 0;
            AweCtrldata->count = 0;
            AweCtrldata->do_set = 0;
        }
        else {
            AWE_LOGE("Unsupported Access type 0x%x for AWE control %s",
                ctl_elem_payload.access_control, ctl_elem_payload.name.c_str());
            ret = AWE_EBADPARAM;
        }
    }
#else
    if ((ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER)
        || (ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_BOOLEAN)){
        memcpy(&ctl_elem_payload.value.integer.value, &AweCtrl->elem_payload.value.integer.value,
            AweCtrl->elem_count * sizeof(int32_t));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcRead integer value [%d]: %d", i, ctl_elem_payload.value.integer.value[i]);
        }
    }
    else if (ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER64 ) {
        memcpy(&ctl_elem_payload.value.integer64.value, &AweCtrl->elem_payload.value.integer64.value,
            AweCtrl->elem_count * sizeof(int64_t));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcRead integer64 value[%d]: %lld", i, ctl_elem_payload.value.integer64.value[i]);
        }
    }
    else if (ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_ENUMERATED) {
        memcpy(&ctl_elem_payload.value.enumerated.item, &AweCtrl->elem_payload.value.enumerated.item,
            AweCtrl->elem_count * sizeof(unsigned int));
        for(int i=0; i<AweCtrl->elem_count; i++) {
            AWE_LOGD("DcRead enumerated item[%d]: %d", i,  ctl_elem_payload.value.enumerated.item[i]);
        }
    }
    else if (ctl_elem_payload.type == VIRTIO_MIXER_CTL_TYPE_MASK_BYTES) {
        if (ctl_elem_payload.access_control == bytes) {
            memcpy(&ctl_elem_payload.value.bytes.data, &AweCtrl->elem_payload.value.bytes.data,
                AweCtrl->elem_count * sizeof(unsigned char));
            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcRead bytes data[%d]: 0x%x", i, ctl_elem_payload.value.bytes.data[i]);
        }
        else if(ctl_elem_payload.access_control == tlv_bytes) {
            AweCtrlVectorValue_t * AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];
            memcpy(ctl_elem_payload.value.tlv.tlv, AweCtrl->elem_payload.value.tlv.tlv, AweCtrl->elem_count);

            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcRead tlv bytes data[%d]: 0x%x", i, AweCtrl->elem_payload.value.tlv.tlv[i]);

            //clear offset, count and do_set in AweCtrl
            AweCtrldata->offset = 0;
            AweCtrldata->count = 0;
            AweCtrldata->do_set = 0;
        }
        else {
            AWE_LOGE("Unsupported Access type 0x%x for AWE control %s",
                ctl_elem_payload.access_control, ctl_elem_payload.name.c_str());
            ret = AWE_EBADPARAM;
        }
    }
#endif
    return ret;
}

uint32_t AweAudioPlugin::StreamSetParam(string fe_dai_name,struct PCMSetParams &set_param_payload)
{
    uint32_t ret = AWE_EOK;
    uint32_t handle;

    // Find the stream
    auto it = fe_dai_awe_map_.find(fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    if ((stream->handle != 0) || (stream->state != PCM_STATE_CLOSED)) {
        AWE_LOGI("The AWE Stream %s has been opened", fe_dai_name.c_str());
    } else {
        // Attempt to open the stream
        struct awe_stream_open_config open_cfg;
        open_cfg.device_id = stream->device_id;
        //open_cfg.op_code = (stream->direction == INPUT) ? 0x00 : 0x01;
        AWE_LOGI("AWE stream: %s StreamSetParam, awe_open.", fe_dai_name.c_str());
        open_cfg.data_mode = AWE_DATA_SHMEM;
        ret = awe_open(AWE_MODULE_TYPE_STREAM, &open_cfg, sizeof(open_cfg), &handle);
        if (ret) {
            AWE_LOGE("awe_open failed ret = %d", ret);
            return ret;
        }
        stream->handle = handle;
        stream->state = PCM_STATE_OPEN;
        stream->data_mode = open_cfg.data_mode;
        AWE_LOGI( "awe_open successful for stream : %s", fe_dai_name.c_str());
    }

    // Update AweStream's format and samplerate
    if (sampleRateMap.find(set_param_payload.rate) != sampleRateMap.end()) {
        stream->format.rate = sampleRateMap.at(set_param_payload.rate);
        AWE_LOGI("sample rate is : %d", stream->format.rate);
    } else {
        AWE_LOGE("sample rate: %d from virtio is not supportd in AWE", set_param_payload.rate);
        return AWE_EBADPARAM;
    }
    if (formatMap.find(set_param_payload.format) != formatMap.end()) {
        stream->format.format = formatMap.at(set_param_payload.format);
        AWE_LOGI("format is : 0x%x", stream->format.format);
    } else {
        AWE_LOGE("format: 0x%x from virtio is not supportd in AWE", set_param_payload.format);
        return AWE_EBADPARAM;
    }

    stream->format.channels  = set_param_payload.channels;
    AWE_LOGI("AWE stream: %s StreamSetParam, set media config.", fe_dai_name.c_str());
    ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_MEDIA_CONFIG, &stream->format, sizeof(struct awe_media_config));
    if (ret != AWE_EOK) {
        AWE_LOGE("Failed to set media config for stream : %s", fe_dai_name.c_str());
        return ret;
    }

    // only cache period_size, period_cnt;
    stream->buffer_config.period_size  =  set_param_payload.period_bytes;
    stream->buffer_config.period_cnt   =  set_param_payload.buffer_bytes / set_param_payload.period_bytes;

    //populate the buffer pool with the hardware parameters
    stream->buf_pool.free_count = stream->buffer_config.period_cnt;
    stream->buf_pool.num_buffs = stream->buffer_config.period_cnt;
    stream->buf_pool.buf_size = stream->buffer_config.period_size;
    stream->buf_pool.buf_used = std::vector<uint8_t>(stream->buf_pool.num_buffs, 0);

    stream->state = PCM_STATE_SETUP;
    return AWE_EOK;
}

uint32_t AweAudioPlugin::StreamMmap(string fe_dai_name, PCMMmapParams &msg)
{
    uint32_t ret = AWE_EOK;
    // Find the stream
    auto it = fe_dai_awe_map_.find(fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    // Check if handle and state are valid
    if (stream->handle == 0){
        AWE_LOGE("Invalid handle for AWE Stream %s", fe_dai_name.c_str());
        return AWE_EHANDLE;
    }
    if (stream->data_mode != AWE_DATA_SHMEM) {
        AWE_LOGE("Invalid data_mode for AWE Stream %s", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }

    stream->is_push_poll = true;
    stream->buffer_config.transfer_buffer_fd = msg.data_fd;
    stream->buffer_config.position_buffer_fd = msg.pos_fd;
    stream->gvm_mmap_fd = msg.data_gvm_fd;
    AWE_LOGI("AWE stream: %s StreamMmap, set buffer config.", fe_dai_name.c_str());
    ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_BUFFER_CONFIG, &stream->buffer_config, sizeof(awe_buffer_config));
    if (ret != AWE_EOK) {
        AWE_LOGE("Failed to set buffer config for stream : %s", fe_dai_name.c_str());
        return ret;
    }
    return AWE_EOK;
}

// This function is just for playback and capture.
uint32_t AweAudioPlugin::StreamPrepare(string fe_dai_name)
{
    uint32_t ret = AWE_EOK;
    struct awe_reg_callback cb_config = {0};

    // Find the stream
    auto it = fe_dai_awe_map_.find(fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    if ((stream->handle == 0) || (stream->state != PCM_STATE_SETUP)){
        AWE_LOGE("Invalid handle for stream or state for AWE Stream %s", fe_dai_name.c_str());
        return AWE_EHANDLE;
    }
    AWE_LOGI("AWE stream: %s StreamPrepare start.", fe_dai_name.c_str());
    /* TODO  support this mode later
    if (stream->data_mode == AWE_DATA_SHMEM) {
        cb_config.event_cb = awe_pcm_event_cb;
        cb_config.client_data = static_cast<void*>(stream);

        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_REGISTER_CALLBACK,
            &cb_config, sizeof(struct awe_reg_callback));
        if(ret != AWE_EOK){
            AWE_LOGE("Register event callback failure for stream AWE Stream %s", fe_dai_name.c_str());
            return ret;
        }
    } */

    stream->state = PCM_STATE_PREPARED;
    return AWE_EOK;
}

uint32_t AweAudioPlugin::StreamRelease(string fe_dai_name)
{
    uint32_t ret = AWE_EOK;
    // Find the stream
    auto it = fe_dai_awe_map_.find(fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    if ((stream->handle == 0) || (stream->state == PCM_STATE_CLOSED)) {
        AWE_LOGE("AWE Stream %s already closed", fe_dai_name.c_str());
        return AWE_EOK;
    }

    AWE_LOGI("AWE stream: %s StreamRelease awe_close.", fe_dai_name.c_str());
    ret = awe_close(AWE_MODULE_TYPE_STREAM, stream->handle);
    if (ret != AWE_EOK) {
        // log and return error but still cleanup other resources
        AWE_LOGE("Failed to close AWE stream : %s", fe_dai_name.c_str());
    }
    stream->handle = 0;
    stream->state = PCM_STATE_CLOSED;
    memset(&stream->format, 0, sizeof(awe_media_config));
    memset(&stream->threshold, 0, sizeof(awe_threshold_config));
    memset(&stream->buffer_config, 0, sizeof(awe_buffer_config));
    stream->buf_pool.buf_used.clear();
    stream->buf_pool.free_count = 0;
    stream->buf_pool.num_buffs = 0;
    stream->buf_pool.buf_size = 0;
    stream->is_push_poll = false;
    stream->gvm_mmap_fd = 0;
    return ret;
}

uint32_t AweAudioPlugin::StreamStart(string fe_dai_name)
{
    uint32_t ret = AWE_EOK;
    // Find the stream
    auto it = fe_dai_awe_map_.find(fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    // Check if handle and state are valid
    if ((stream->handle == 0) && (stream->state != PCM_STATE_PREPARED)){
        AWE_LOGE("Invalid handle for stream or state for AWE Stream %s", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }

    if (stream->sess_mode == kSessModeNoHost) {
        AWE_LOGI("AWE stream: %s StreamStart for kSessModeNoHost.", fe_dai_name.c_str());
        stream->buffer_config.transfer_buffer_fd = 0;
        stream->buffer_config.position_buffer_fd = 0;
        AWE_LOGI("AWE stream: %s StreamStart set buff config for kSessModeNoHost.", fe_dai_name.c_str());
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_BUFFER_CONFIG, &stream->buffer_config, sizeof(awe_buffer_config));
        if (ret != AWE_EOK) {
            AWE_LOGE("StreamStart set buff config failed for AWE stream: %s", fe_dai_name.c_str());
            return ret;
        }
        struct awe_threshold_config threshold_config;
        threshold_config.start_level = 0;
        threshold_config.stop_level = stream->buffer_config.period_size * stream->buffer_config.period_cnt;
        AWE_LOGI("AWE stream: %s StreamStart set threshold for kSessModeNoHost: start level %d, stop level %d",fe_dai_name.c_str(), threshold_config.start_level, threshold_config.stop_level);
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_THRESHOLD, &threshold_config, sizeof(struct awe_threshold_config));
        if (ret != AWE_EOK) {
            AWE_LOGE("StreamStart set threshold failed for stream : %s", fe_dai_name.c_str());
            return ret;
        }
    }

    if ((stream->is_push_poll) || (stream->sess_mode == kSessModeNoHost)) {
        AWE_LOGI("AWE stream: %s StreamStart for push pull or kSessModeNoHost sess_mode.", fe_dai_name.c_str());
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_START, nullptr, 0);
        if (ret != AWE_EOK) {
            AWE_LOGE("Failed to start AWE stream : %s", fe_dai_name.c_str());
            return ret;
        }
        stream->state = PCM_STATE_RUNNING;
    }

    return AWE_EOK;
}

uint32_t AweAudioPlugin::StreamStop(string fe_dai_name)
{
    uint32_t ret = AWE_EOK;
    // Find the stream
    auto it = fe_dai_awe_map_.find(fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    // Check if handle and state are valid
    if (stream->handle == 0){
        AWE_LOGE("Invalid handle for AWE Stream %s", fe_dai_name.c_str());
        return AWE_EHANDLE;
    }

    AWE_LOGI("AWE stream: %s StreamStop.", fe_dai_name.c_str());
    ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_STOP, nullptr, 0);
    if (ret != AWE_EOK && ret != AWE_ESUBSYSRESET) {
        AWE_LOGE("Failed to stop AWE stream : %s", fe_dai_name.c_str());
        return ret;
    }
    stream->state = PCM_STATE_PREPARED;
    return AWE_EOK;
}

uint32_t AweAudioPlugin::StreamWrite(virtio_snd_pcm_buf_msg &msg)
{
    uint32_t ret = AWE_EOK;
    struct awe_rw_buffer rw_buff = {0};
    // Find the stream
    auto it = fe_dai_awe_map_.find(msg.fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", msg.fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    // Check if handle and input params are valid
    if (stream->handle == 0){
        AWE_LOGE("Invalid handle for AWE Stream %s", msg.fe_dai_name.c_str());
        return AWE_EHANDLE;
    }
    if ((msg.buff_fd <= 0) || (msg.size == 0) || (msg.dma_bytes == 0)){
        AWE_LOGE("Invalid message payload for AWE stream: %s, buff_fd = %d, size=%d, dma_bytes=%d",
            msg.fe_dai_name.c_str(), msg.buff_fd, msg.size, msg.dma_bytes);
        return AWE_EBADPARAM;
    }
    if (stream->state == PCM_STATE_PREPARED) {
        stream->buffer_config.transfer_buffer_fd = msg.buff_fd;
        //AWE controller will allocate memroy in the regular read/write mode.
        stream->buffer_config.position_buffer_fd = 0;
        AWE_LOGI("AWE stream: %s StreamWrite, set buffer config in the first write.", msg.fe_dai_name.c_str());
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_BUFFER_CONFIG, &stream->buffer_config, sizeof(awe_buffer_config));
        if (ret != AWE_EOK) {
            AWE_LOGE("Failed to set buffer config for stream : %s", msg.fe_dai_name.c_str());
            return ret;
        }

        struct awe_threshold_config threshold_config = {0, 1024};

        AWE_LOGI("start level %d, stop level %d", threshold_config.start_level, threshold_config.stop_level);
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_THRESHOLD, &threshold_config, sizeof(struct awe_threshold_config));
        if (ret != AWE_EOK) {
            AWE_LOGE("AWE_IOCTL_STREAM_SET_THRESHOLD_CONFIG failed, ret= %d!", ret);
            return ret;
        }
        AWE_LOGI("AWE stream: %s StreamWrite, start stream in the first write.", msg.fe_dai_name.c_str());
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_START, nullptr, 0);
        if (ret != AWE_EOK) {
            AWE_LOGE("Failed to start AWE stream : %s", msg.fe_dai_name.c_str());
            return ret;
        }
        stream->state = PCM_STATE_RUNNING;
    }

    if (stream->state == PCM_STATE_RUNNING){
        // Set the awe_rw_buffer structure
        rw_buff.buf_vaddr = 0;
        rw_buff.offset = msg.offset;
        rw_buff.buf_size = msg.size;
        // Initial value, DSP will update this value
        rw_buff.consumed_size = 0;

        //uint8_t index = rw_buff.offset / (stream->buf_pool.buf_size);

        ret = awe_write(stream->handle, &rw_buff, sizeof(struct awe_rw_buffer));
        if (ret != AWE_EOK) {
            AWE_LOGE("AWE stream : %s write failed", msg.fe_dai_name.c_str());
            return ret;
        }
        stream->state = PCM_STATE_RUNNING;
    }
    return AWE_EOK;
}

int AweAudioPlugin::StreamRead(virtio_snd_pcm_buf_msg &msg)
{
    uint32_t ret = AWE_EOK;
    struct awe_rw_buffer rw_buff = {0};
    // Find the stream
    auto it = fe_dai_awe_map_.find(msg.fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", msg.fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    // Check if handle and input params are valid
    if (stream->handle == 0){
        AWE_LOGE("Invalid handle for AWE Stream %s", msg.fe_dai_name.c_str());
        return AWE_EHANDLE;
    }
    if ((msg.buff_fd <= 0) || (msg.size == 0) || (msg.dma_bytes == 0)){
        AWE_LOGE("Invalid message payload for AWE stream: %s, buff_fd = %d, size=%d, dma_bytes=%d",
            msg.fe_dai_name.c_str(), msg.buff_fd, msg.size, msg.dma_bytes);
        return AWE_EBADPARAM;
    }
    if (stream->state == PCM_STATE_PREPARED) {
        stream->buffer_config.transfer_buffer_fd = msg.buff_fd;
        //AWE controller will allocate memroy in the regular read/write mode.
        stream->buffer_config.position_buffer_fd = 0;
        AWE_LOGI("AWE stream: %s StreamWrite, set buffer config in the first read.", msg.fe_dai_name.c_str());
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_BUFFER_CONFIG, &stream->buffer_config, sizeof(awe_buffer_config));
        if (ret != AWE_EOK) {
            AWE_LOGE("Failed to set buffer config for stream : %s", msg.fe_dai_name.c_str());
            return ret;
        }

        struct awe_threshold_config threshold_config = {0};

        AWE_LOGD("start level %d, stop level %d", threshold_config.start_level, threshold_config.stop_level);
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_THRESHOLD, &threshold_config, sizeof(struct awe_threshold_config));
        if (ret != AWE_EOK) {
            AWE_LOGE("AWE_IOCTL_STREAM_SET_THRESHOLD_CONFIG failed, ret= %d!", ret);
            return ret;
        }

        AWE_LOGI("AWE stream: %s StreamWrite, start stream in the first read.", msg.fe_dai_name.c_str());
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_START, nullptr, 0);
        if (ret != AWE_EOK) {
            AWE_LOGE("Failed to start AWE stream : %s", msg.fe_dai_name.c_str());
            return ret;
        }
        stream->state = PCM_STATE_RUNNING;
    }
    if (stream->state == PCM_STATE_RUNNING){
        // Set the awe_rw_buffer structure
        rw_buff.buf_vaddr = 0;
        rw_buff.offset = msg.offset;
        rw_buff.buf_size = msg.size;
        rw_buff.consumed_size = 0;

        //uint8_t index = rw_buff.offset / (stream->buf_pool.buf_size);
        ret = awe_read(stream->handle, &rw_buff, sizeof(struct awe_rw_buffer));
        if (ret != AWE_EOK) {
            AWE_LOGE("AWE stream : %s read failed", msg.fe_dai_name.c_str());
            return ret;
        }
        stream->state = PCM_STATE_RUNNING;
    }
    return AWE_EOK;
}

int AweAudioPlugin::RegisterSSREvent(std::function<void(enum SSRState)> ssr_cb_func)
{
    uint32_t ret = AWE_EOK;
    struct awe_event_reg_cfg event_sub = {0};

    strlcpy(event_sub.event_name, AWE_SSR_EVENT_NAME, AWE_MAX_EVENT_NAME_LENGTH);
    event_sub.is_register = 1;

    ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_SUBSCRIBE_EVENTS, &event_sub,
                    sizeof(struct awe_event_reg_cfg));

    if (ret != AWE_EOK && ret != AWE_EALREADY)
        AWE_LOGE("SSR event subscription failed %d", ret);
    else
        adm_ssr_cb = ssr_cb_func;

    return ret;
}

int AweAudioPlugin::RegisterEventCallback(std::function<void(uint32_t control_id)> event_cb_func)
{
    struct awe_reg_callback cb_config = {0};
    uint32_t ret = AWE_EOK;

    cb_config.event_cb = AweEventCallbackFunc;
    cb_config.client_data = (void*)this;

    ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_REGISTER_CALLBACK, &cb_config, sizeof(struct awe_reg_callback));
    if (ret) {
        AWE_LOGE("AWE Register Event Callback failed %d", ret);
        return ret;
    }

    adm_event_cb = event_cb_func;
    return ret;
}

uint32_t AweAudioPlugin::InitClassResource()
{
    awe_snd_card_open_config_t open_cfg = {AWE_VM_ID_INVALID, 0};
    uint32_t ret = AWE_EOK;
#if !defined(__QNX__)
    unsigned int tlv_mask = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
                SNDRV_CTL_ELEM_ACCESS_TLV_WRITE |
                SNDRV_CTL_ELEM_ACCESS_TLV_COMMAND;
#else
    unsigned int tlv_mask = VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_READ |
                VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_WRITE |
                VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_COMMAND;
#endif
    if (all_fe_dai_ == nullptr) {
        AWE_LOGE("No pcm device was found");
        ret = AWE_EBADPARAM;
        goto fail;
    }

    for (const auto& fe_info : *all_fe_dai_) {
        AweStream* newStream = new AweStream();
        newStream->fe_dai_name = fe_info.name;
        newStream->direction = fe_info.direction;
        newStream->sess_mode = fe_info.sess_mode;
        newStream->device_id = fe_info.id;
        newStream->state = PCM_STATE_CLOSED;
        // fe_dai_awe_map_[fe_info.name] = stream;
        pair<string, AweStream*> streamPair(fe_info.name, newStream);
        AWE_LOGI("Insert the %s to fe_dai_awe_map_.",newStream->fe_dai_name.c_str());
        fe_dai_awe_map_.insert(streamPair);
    }

    if (all_mixer_ctl_ == nullptr) {
        AWE_LOGE("No kcontrol was found");
        ret = AWE_EBADPARAM;
        goto fail;
    }

    for (const auto& ele_info : *all_mixer_ctl_) {
        AweCtrlValue* newCtrlValue = new AweCtrlValue();
        memset(newCtrlValue, 0, sizeof(AweCtrlValue));
        newCtrlValue->elem_payload.name = ele_info.id.name;
        newCtrlValue->elem_payload.type = ele_info.type;
        if (ele_info.access & tlv_mask) {
            newCtrlValue->elem_payload.access_control = tlv_bytes;
            newCtrlValue->elem_payload.value.tlv.tlv = new uint8_t[MAX_TLV_SIZE];
            memset(newCtrlValue->elem_payload.value.tlv.tlv, 0, MAX_TLV_SIZE);
            AWE_LOGI("ctrl is tlv %s.",newCtrlValue->elem_payload.name.c_str());
        }
        else {
            newCtrlValue->elem_payload.access_control = bytes;
        }
        newCtrlValue->elem_ctrl_id = ele_info.id.ctl_id;
        newCtrlValue->elem_count = ele_info.count;
        pair<string, AweCtrlValue*> kctlPair(ele_info.id.name, newCtrlValue);
        kctrl_value_map_.insert(kctlPair);
        AWE_LOGI("Insert the %s to kctrl_value_map_.",newCtrlValue->elem_payload.name.c_str());
    }
    open_cfg.vmid = AWE_VM_ID_1;
    ret = awe_open(AWE_MODULE_TYPE_SND_CARD, (void *)&open_cfg, sizeof(open_cfg), &awe_snd_card_handle_);
    if (ret) {
        AWE_LOGE("Sound card open failed, ret = %d", ret);
        goto fail;
    }
    return AWE_EOK;

fail:
    for (auto& iter : fe_dai_awe_map_) {
        delete iter.second;
        iter.second = nullptr;
    }
    fe_dai_awe_map_.clear();

    for (auto& iter : kctrl_value_map_) {
        if (iter.second->elem_payload.access_control == tlv_bytes && iter.second->elem_payload.value.tlv.tlv != nullptr)
            delete[] iter.second->elem_payload.value.tlv.tlv;
        delete iter.second;
        iter.second = nullptr;
    }
    kctrl_value_map_.clear();
    return ret;
}

void AweAudioPlugin::ReleaseClassResource()
{
    for (auto& iter : fe_dai_awe_map_) {
        delete iter.second;
        iter.second = nullptr;
    }
    fe_dai_awe_map_.clear();

    for (auto& iter : kctrl_value_map_) {
        if (iter.second->elem_payload.access_control == tlv_bytes && iter.second->elem_payload.value.tlv.tlv != nullptr)
            delete[] iter.second->elem_payload.value.tlv.tlv;
        delete iter.second;
        iter.second = nullptr;
    }
    kctrl_value_map_.clear();
}

// map new allocated memory 
int32_t AweAudioPlugin::AweShmemMapSMMU(AweCtrlValue *AweCtrl, awe_control_type_t type)
{
    int32_t rc;
    AweShmemMapInfo map_info = {0};
    awe_shmem_map_t shmem_map = {0};
    // expected from payload
    map_info.data_export_id = AweCtrl->elem_payload.value.integer.value[0];
    map_info.data_fd = AweCtrl->elem_payload.value.integer.value[1];
    map_info.export_fd = AweCtrl->elem_payload.value.integer.value[2];
    map_info.data_size = AweCtrl->elem_payload.value.integer.value[3];
    shmem_map.size = map_info.data_size;
    shmem_map.hlos_cache = AWE_SHMEM_CACHED;//TODO: check this
    shmem_map.pid = getpid();
    shmem_map.fd = map_info.export_fd;

    rc = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_MAP_SHMEM, &shmem_map, sizeof(shmem_map));
    if (rc){
        AWE_LOGE("AWE set control: %s failed with rc = %d", AweCtrl->elem_payload.name.c_str(), rc);
    }
    if (!rc){
        memcpy(&AweCtrl->elem_payload.value.integer.value[4], &shmem_map.mem_addr_lsw[0], 2*sizeof(shmem_map.mem_addr_lsw));//both lsw and msw
        memcpy(&map_info.mem_addr_lsw[0], &shmem_map.mem_addr_lsw[0], 2*sizeof(shmem_map.mem_addr_lsw));//both lsw and msw
        shmem_map_info.push_back(map_info);
    }

    return rc;
}

int32_t AweAudioPlugin::AweShmemUnMapSMMU(AweCtrlValue *AweCtrl, awe_control_type_t type)
{
    // expected from payload
    uint32_t data_export_id = AweCtrl->elem_payload.value.integer.value[0];
    awe_shmem_unmap_t shmem_unmap = {0};
    int32_t rc = AWE_EOK;
    // find in shmem_map_info based on data_export_fd and unmap/erase
    AWE_LOGD("AweShmemUnMapSMMU: export id %d", data_export_id);
    for (auto it = shmem_map_info.begin(); it != shmem_map_info.end(); it++){
        if (it->data_export_id == data_export_id){
            AWE_LOGD("AweShmemUnMapSMMU: match export id %d", data_export_id);
            shmem_unmap.mem_addr_lsw = it->mem_addr_lsw[AWE_DSP0];
            shmem_unmap.mem_addr_msw = it->mem_addr_msw[AWE_DSP0];
            rc = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_UNMAP_SHMEM, &shmem_unmap, sizeof(shmem_unmap));
            shmem_map_info.erase(it);
            // invalidate payload in AweCtrl
            memset(&AweCtrl->elem_payload.value.integer.value[0], 0, 12*(sizeof(int)));//must match struct ShmemInfo!
            if (rc) {
                AWE_LOGE("awe_shmem_unmap failed %d\n", rc);
                rc = AWE_EFAILED;
            } 
            return rc;
        }
    }
    AWE_LOGD("AweShmemUnMapSMMU: export id %d not found", data_export_id);
    rc = AWE_EBADPARAM;
    return rc;
}

uint32_t AweAudioPlugin::AweGetCoreErrorInfo(AweCtrlValue *AweCtrl)
{
    uint32_t rc = AWE_EOK;
    awe_core_error_info_t *ctrl_param = (awe_core_error_info_t *)&AweCtrl->elem_payload.value.bytes.data;
    if (!ctrl_param){
        return AWE_EBADPARAM;
    }
    memset(ctrl_param->description, 0 ,sizeof(ctrl_param->description));
    rc = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_GET_AWE_CORE_ERROR_INFO, ctrl_param, sizeof(awe_core_error_info_t));

    return rc;

}