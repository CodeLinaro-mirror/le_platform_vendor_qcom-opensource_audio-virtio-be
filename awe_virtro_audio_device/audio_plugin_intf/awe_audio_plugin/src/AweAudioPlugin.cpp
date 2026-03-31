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
#include "awe_format_util.h"

#if !defined (__QNX__)
#define strlcpy g_strlcpy
#endif

#define SLEEP_US(us) do { \
    struct timespec ts; \
    ts.tv_sec = (us) / 1000000; \
    ts.tv_nsec = ((us) % 1000000) * 1000; \
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);\
} while (0)

// defines for raw control(DSPC tunning command)
#define MAX_POLL_CNT (5)
#define PFID_SET_VALUE_SET_CALL 60
#define PFID_SET_VALUES_SET_CALL 61
#define PFID_GET_CALL_FETCH_VALUE 62
#define PFID_GET_CALL_FETCH_VALUES 63
#define MAX_OEM_TUNNEL_PAYLOAD_SIZE 16344
#define AWE_RAW_CMD_HEADER_SZ_1 6
#define AWE_RAW_CMD_HEADER_SZ_2 5
#define AWE_RAW_CMD_GET_CORE_ID(handle)  ((0 << 4) | ((handle & 0x780) >> 7))
#define AWE_RAW_CMD_COMPUTE_HEADER(txSize, forCore, pfid) ((txSize << 16) | (forCore << 8) | pfid)
#define AWE_RAW_CMD_GET_HANDLE(handle) (handle & 0x3ffff07f)
#define AWE_RAW_CMD_IS_ARRAY(handle) (handle & 0x80000000)
#define IS_OEM_TUNNEL_SET(x) (x & 0x1)

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

/**
 * @brief Set or update the value of an AWE named control.
 *
 * Handles a single ALSA mixer control "set" request for an AWE named control.
 * The function parses the incoming CtlElemPayload (bytes or TLV bytes),
 * constructs an awe_named_control_param_t based on the access mode
 * (full BYTES write vs TLV vector slice with offset/count/do_set),
 * sends it to DSP via the appropriate AWE ioctl, and updates the AweCtrl cache.
 *
 * Typical call path: DcWrite() -> AweSetNamedControl().
 *
 * @param[in,out] AweCtrl
 *     Pointer to the internal control descriptor/cache object for this mixer
 *     control. On success, AweCtrl->elem_payload (and any cached TLV header
 *     such as offset/count/do_set) is updated to reflect the value written
 *     to DSP.
 *
 * @param[in] ctl_elem_payload
 *     ALSA-style control payload carrying the value to be written.
 *     - For ACCESS_READWRITE (plain BYTES), value.bytes.data[] is treated
 *       as a full control payload and written from offset = 0 for
 *       elem_count.
 *     - For ACCESS_TLV_READWRITE (TLV BYTES), value.tlv.tlv[] is interpreted
 *       as an AweCtrlVectorValue_t, which provides offset, count, do_set and
 *       the data slice to be written.
 *
 * @param[in] type
 *     AWE control data type (for example, AWE_CONTROL_TYPE_FLOAT or
 *     AWE_CONTROL_TYPE_INT), used to choose the correct value format.
 *
 * @return
 *     0 on success, non-zero error code on failure.
 */
uint32_t AweAudioPlugin::AweSetNamedControl(AweCtrlValue *AweCtrl, const CtlElemPayload &ctl_elem_payload, awe_control_type_t type)
{
    uint32_t ret = AWE_EOK;
    uint32_t payload_sz = 0;
    awe_named_control_param_t *ctrl_param = NULL;

    // NOTE on elem_count semantics:
    //
    //  - For normal controls (ACCESS_READWRITE):
    //      * INTEGER / BOOLEAN / ENUMERATED / INTEGER64:
    //          elem_count = number of elements (int32_t / int64_t / unsigned int, etc.)
    //      * BYTES:
    //          elem_count = number of bytes (= number of uint8_t elements).
    //      * FLOAT:
    //          Currently, elem_count is also interpreted as the number of bytes
    //          (i.e. sizeof(float) * number_of_floats). This is a known non-uniform behavior
    //          and may be unified in the future so that elem_count represents
    //          the number of float elements instead of raw bytes.
    //
    //  - For TLV controls (ACCESS_TLV_READWRITE):
    //      elem_count = total TLV size in BYTES (including TLV header).

    AWE_LOGD("access_control 0x%X, AweCtrl->elem_count %d (%s)",
             ctl_elem_payload.access_control,
             AweCtrl->elem_count,
             (ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) ? "bytes (TLV total)" : "elements");

    if (ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) {
        // ACCESS_TLV_READWRITE
        AweCtrlVectorValue_t *plugin_ctl_data = (AweCtrlVectorValue_t *)&ctl_elem_payload.value.tlv.tlv[0];

        if (plugin_ctl_data->do_set == 0) {
            // do_set = 0, stored offset and count into AweCtrl
            AweCtrlVectorValue_t *AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];
            AweCtrldata->count = plugin_ctl_data->count;   // in elements
            AweCtrldata->offset = plugin_ctl_data->offset;
            AweCtrldata->do_set = 0;
            AWE_LOGD("cache AweCtrldata->count %d (in elements) offset %d, not set to dsp", AweCtrldata->count, AweCtrldata->offset);
            return ret;
        } else {
            // do_set = 1, use offset and count sent in payload
            if (plugin_ctl_data->count <= 0 || plugin_ctl_data->count > AweCtrl->elem_count / sizeof(uint32_t)) {
                AWE_LOGE("(count %d) mismatch, use AweCtrl->elem_count %d as default", plugin_ctl_data->count, AweCtrl->elem_count);
                plugin_ctl_data->count = (AweCtrl->elem_count - AWE_TLV_HEADER_SIZE) / sizeof(uint32_t);
            }
            if (plugin_ctl_data->count > MAX_TLV_SIZE || plugin_ctl_data->count <= 0) {
                AWE_LOGE("Unsuppoted TLV size %d", plugin_ctl_data->count);
                return AWE_EBADPARAM;
            }

            AWE_LOGD("plugin_ctl_data->count %d (in elements), offset %d", plugin_ctl_data->count, plugin_ctl_data->offset);

            ctrl_param = (awe_named_control_param_t*)calloc(1, sizeof(awe_named_control_param_t) + plugin_ctl_data->count * sizeof(uint32_t));
            if (ctrl_param == NULL) {
                AWE_LOGE("Fail to allocate control");
                return AWE_ENOMEMORY;
            }

            ctrl_param->offset = plugin_ctl_data->offset;
            ctrl_param->type = type;
            ctrl_param->count = plugin_ctl_data->count;

            switch (type) {
                case AWE_CONTROL_TYPE_FLOAT: {
                    payload_sz = ctrl_param->count * sizeof(float);
                    memcpy((void *)ctrl_param->value.floating, &plugin_ctl_data->value.floating, payload_sz);

                    for (int i = 0; i < ctrl_param->count; i++)
                        AWE_LOGD("ctrl_param->value.floating [%d]: value %f", i, ctrl_param->value.floating[i]);

                    break;
                }
                case AWE_CONTROL_TYPE_INT: {
                    payload_sz = ctrl_param->count * sizeof(uint32_t);
                    memcpy((void *)ctrl_param->value.integer, &plugin_ctl_data->value.integer, payload_sz);

                    for (int i = 0; i < ctrl_param->count; i++)
                        AWE_LOGD("ctrl_param->value.integer [%d]: value %d", i, ctrl_param->value.integer[i]);

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
        // ACCESS_READWRITE
        ctrl_param = (awe_named_control_param_t*)calloc(1, sizeof(awe_named_control_param_t) + AweCtrl->elem_count * sizeof(uint32_t));

        // here allocate size is bigger than expected for float type.
        if (ctrl_param == NULL) {
            AWE_LOGE("Fail to allocate control");
            return AWE_ENOMEMORY;
        }

        ctrl_param->offset = 0;
        ctrl_param->type = type;

        switch (type) {
            case AWE_CONTROL_TYPE_INT: {
                ctrl_param->count = AweCtrl->elem_count;
                payload_sz = ctrl_param->count * sizeof(int32_t);
                memcpy((void *)ctrl_param->value.integer, &AweCtrl->elem_payload.value.integer.value, payload_sz);
                break;
            }
            case AWE_CONTROL_TYPE_FLOAT: {
                // TODO: Unify the elem_count as words for ACCESS_READWRITE AWE_CONTROL_TYPE_FLOAT
                ctrl_param->count = AweCtrl->elem_count / sizeof(float);
                payload_sz = AweCtrl->elem_count;
                memcpy((void *)ctrl_param->value.floating, &AweCtrl->elem_payload.value.bytes.data, payload_sz);
                break;
            }
            default: {
                AWE_LOGE("Unrecognized awe_control_type %d", type);
                ret = AWE_EBADPARAM;
                goto exit;
            }
        }
    } // ACCESS_TLV_READWRITE vs ACCESS_READWRITE

    AWE_LOGD("ctrl_param->count %d (elements), ctrl_param->offset %d, payload_sz %d (bytes)",
             ctrl_param->count, ctrl_param->offset, payload_sz);

    snprintf(reinterpret_cast<char*>(ctrl_param->name), AWE_MAX_CONTROL_NAME_LENGTH, "%s", AweCtrl->elem_payload.name.c_str());

    ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_SET_NAMED_CONTROL, ctrl_param, sizeof(awe_named_control_param_t) + payload_sz);
    if (ret)
        AWE_LOGE("AWE set control: %s failed with rc = %d", AweCtrl->elem_payload.name.c_str(), ret);

exit:
    if (ctrl_param != NULL) {
        free(ctrl_param);
        ctrl_param = NULL;
    }

    return ret;
}

/**
 * @brief Get the current value of an AWE named control.
 *
 * Handles a single ALSA mixer control "get" request for an AWE named control.
 * The function interprets the incoming CtlElemPayload (bytes / TLV bytes),
 * issues the corresponding AWE ioctl to read the control value from DSP
 * (or uses cached data when applicable), and updates the AweCtrl cache.
 *
 * Typical call path: DcRead() -> AweGetNamedControl().
 *
 * @param[in,out] AweCtrl
 *     Pointer to the internal control descriptor/cache object for this mixer
 *     control. On success, AweCtrl->elem_payload and related fields are updated
 *     with the latest value read from DSP.
 *
 * @param[in] ctl_elem_payload
 *     ALSA-style control descriptor describing which named control to read
 *     (name/numid/type/access_control/elem_count, etc.). For TLV controls,
 *     ctl_elem_payload.value.tlv.tlv[] carries the TLV buffer interpreted as
 *     AweCtrlVectorValue_t (offset/count, etc.).
 *
 * @param[in] type
 *     AWE control data type (for example, AWE_CONTROL_TYPE_FLOAT or
 *     AWE_CONTROL_TYPE_INT), used to interpret the returned data.
 *
 * @return
 *     0 on success, non-zero error code on failure.
 */
uint32_t AweAudioPlugin::AweGetNamedControl(AweCtrlValue *AweCtrl, const CtlElemPayload &ctl_elem_payload, awe_control_type_t type)
{
    uint32_t ret = AWE_EOK;
    awe_named_control_param_t *ctrl_param = NULL;
    uint32_t payload_sz = 0;
    AweCtrlVectorValue_t *plugin_ctl_data = (AweCtrlVectorValue_t *)&ctl_elem_payload.value.tlv.tlv[0];
    AweCtrlVectorValue_t *AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];

    // NOTE on elem_count semantics:
    //
    //  - For normal controls (ACCESS_READWRITE):
    //      * INTEGER / BOOLEAN / ENUMERATED / INTEGER64:
    //          elem_count = number of elements (int32_t / int64_t / unsigned int, etc.)
    //      * BYTES:
    //          elem_count = number of bytes (= number of uint8_t elements).
    //      * FLOAT:
    //          Currently, elem_count is also interpreted as the number of bytes
    //          (i.e. sizeof(float) * number_of_floats). This is a known non-uniform behavior
    //          and may be unified in the future so that elem_count represents
    //          the number of float elements instead of raw bytes.
    //
    //  - For TLV controls (ACCESS_TLV_READWRITE):
    //      elem_count = total TLV size in BYTES (including TLV header).

    AWE_LOGD("ctl access_control %d AweCtrl->elem_count %d (%s)",
             ctl_elem_payload.access_control,
             AweCtrl->elem_count,
             (ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) ? "bytes (TLV total)" : "elements");

    if (ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) {
        // ACCESS_TLV_READWRITE
        if (plugin_ctl_data->do_set != 0) {
            AWE_LOGE("Not expected do_set flag=%d for get", plugin_ctl_data->do_set);
            return AWE_EUNEXPECTED;
        }

        // do_set = 0, use offset and count stored in AweCtrl.
        if (AweCtrldata->count <= 0 || AweCtrldata->count > AweCtrl->elem_count / sizeof(uint32_t)) {
            AWE_LOGE("Data length (count %d) is missing, use AweCtrl->elem_count as default length", AweCtrldata->count);
            AweCtrldata->offset = 0;
            AweCtrldata->count = (AweCtrl->elem_count - AWE_TLV_HEADER_SIZE) / sizeof(uint32_t);
        }
        if (AweCtrldata->count > MAX_TLV_SIZE || AweCtrldata->count <= 0) {
            AWE_LOGE("Unsuppoted TLV size %d", AweCtrldata->count);
            return AWE_EBADPARAM;
        }

        AWE_LOGD("AweCtrldata->count %d (in elements), offset %d", AweCtrldata->count, AweCtrldata->offset);

        ctrl_param = (awe_named_control_param_t*)calloc(1, sizeof(awe_named_control_param_t) + AweCtrldata->count * sizeof(uint32_t));
        if (ctrl_param == NULL) {
            AWE_LOGE("Fail to allocate control!!");
            return AWE_ENOMEMORY;
        }

        ctrl_param->offset = AweCtrldata->offset;
        ctrl_param->type = type;
        ctrl_param->count = AweCtrldata->count;

        switch (type) {
            case AWE_CONTROL_TYPE_FLOAT: {
                payload_sz = ctrl_param->count * sizeof(float);
                break;
            }
            case AWE_CONTROL_TYPE_INT: {
                payload_sz = ctrl_param->count * sizeof(uint32_t);
                break;
            }
            default: {
                AWE_LOGE("Unrecognized TLV awe_control_type %d", type);
                ret = AWE_EBADPARAM;
                goto exit;
            }
        }
    } else {
        // ACCESS_READWRITE
        ctrl_param = (awe_named_control_param_t*)calloc(1, sizeof(awe_named_control_param_t) + AweCtrl->elem_count * sizeof(uint32_t));

        if (ctrl_param == NULL) {
            AWE_LOGE("Fail to allocate control!!");
            return AWE_ENOMEMORY;
        }

        ctrl_param->offset = 0;
        ctrl_param->type = type;

        switch (type) {
            case AWE_CONTROL_TYPE_INT: {
                ctrl_param->count = AweCtrl->elem_count;
                payload_sz = ctrl_param->count * sizeof(uint32_t);
                break;
            }
            case AWE_CONTROL_TYPE_FLOAT: {
                // TODO: Unify the elem_count as words for ACCESS_READWRITE AWE_CONTROL_TYPE_FLOAT
                ctrl_param->count = AweCtrl->elem_count / sizeof(float);
                payload_sz = AweCtrl->elem_count;
                break;
            }
            default: {
                AWE_LOGE("Unrecognized awe_control_type %d", type);
                ret = AWE_EBADPARAM;
                goto exit;
            }
        }
    } // ACCESS_TLV_READWRITE vs ACCESS_READWRITE

    AWE_LOGD("ctrl_param->count %d (in elements), ctrl_param->offset %d, payload_sz %d (in bytes)", ctrl_param->count, ctrl_param->offset, payload_sz);

    snprintf(reinterpret_cast<char*>(ctrl_param->name), AWE_MAX_CONTROL_NAME_LENGTH, "%s", AweCtrl->elem_payload.name.c_str());

    ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_GET_NAMED_CONTROL, ctrl_param, sizeof(awe_named_control_param_t) + payload_sz);
    if (ret) {
        AWE_LOGE("AWE get control: %s failed with rc = %d", AweCtrl->elem_payload.name.c_str(), ret);
        goto exit;
    }

    if (ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) {
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
                memcpy(&AweCtrl->elem_payload.value.integer.value, ctrl_param->value.integer, ctrl_param->count * sizeof(int32_t));
                break;
            }
            case AWE_CONTROL_TYPE_FLOAT: {
                memcpy(&AweCtrl->elem_payload.value.bytes.data, ctrl_param->value.floating, ctrl_param->count * sizeof(float));
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
    if (ctrl_param != NULL) {
        free(ctrl_param);
        ctrl_param = NULL;
    }

    return ret;
}

// raw tunning payload calculation based on DSPC tunning protocol
// http://download.dspconcepts.com/Release_D-8.C.2.3/Documentation/AWECore_Docs/AWECore_API_Doc/a00061.html
// reference raw payload built for awe_mixer_test -p 6 -r 1
// handle 8753200e, offset 00000000, mask 00008000, size 40
// 0010003d
// 0753200e
// 00000000
// 00004000
// 0000000a
// 00000001
// 00000002
// 00000003
// 00000004
// 00000005
// 00000006
// 00000007
// 00000008
// 00000009
// 0000000a
// 07436032
// reference raw payload built for awe_mixer_test -p 6 -r 0
// handle 8753000e, offset 00000000, mask 00008000, size 4
// 0007003d
// 0753000e
// 00000000
// 00004000
// 00000001
// 00000001
// 07544033
void AweAudioPlugin::AweBuildRawCtlCommand
    (uint32_t **buf, uint32_t set_flag, AweRawCtrlVectorValue_t * param, uint32_t * command_buf_len)
{
    uint32_t header = 0; //hex header
    uint32_t txSize = 0; //size in words
    uint32_t forCore = AWE_RAW_CMD_GET_CORE_ID(param->handle);

    if (txSize > AWE_IOCTL_SND_CARD_RAW_CTL_TRANSACT_BUF_LEN_MAX){
        AWE_LOGE("Memory requested %d > command buf %d. Need to send in chunks!\n", txSize,
            AWE_IOCTL_SND_CARD_RAW_CTL_TRANSACT_BUF_LEN_MAX);
        *buf = NULL;
        *command_buf_len = 0;
        return;
    }

    uint32_t *command_buf = (uint32_t *) calloc(1,
        sizeof(uint32_t)*AWE_IOCTL_SND_CARD_RAW_CTL_TRANSACT_BUF_LEN_MAX);
    if (command_buf == NULL) {
        AWE_LOGD("Memory alloc failed for command buf\n");
        *buf = NULL;
        *command_buf_len = 0;
        return;
    }

    awe_transact_tx_buf_t * raw_ctl = (awe_transact_tx_buf_t * )command_buf;
    raw_ctl->instance_id = (uint8_t) forCore;
    raw_ctl->payload[0] = AWE_RAW_CMD_GET_HANDLE(param->handle);
    AWE_LOGI("handle %08x, offset %08x, mask %08x, size %d", param->handle, param->offset, param->flag,
        param->param_payload_size);
    if (AWE_RAW_CMD_IS_ARRAY(param->handle)){
        if (set_flag) {
            txSize = AWE_RAW_CMD_HEADER_SZ_1 + param->param_payload_size/sizeof(uint32_t); // words
            header = AWE_RAW_CMD_COMPUTE_HEADER(txSize, forCore, PFID_SET_VALUES_SET_CALL);
            raw_ctl->opcode = (uint8_t)PFID_SET_VALUES_SET_CALL;
            memcpy((void *)&command_buf[5], &param->payload, param->param_payload_size);
        }
        else {
            txSize = AWE_RAW_CMD_HEADER_SZ_1;
            header = AWE_RAW_CMD_COMPUTE_HEADER(txSize, forCore, PFID_SET_VALUES_SET_CALL);
            raw_ctl->opcode =  (uint8_t)PFID_GET_CALL_FETCH_VALUES;
        }
        raw_ctl->payload[1] = (uint32_t)param->offset;
        raw_ctl->payload[2] = (uint32_t)(param->flag >> 1);
        raw_ctl->payload[3] = (uint32_t)param->param_payload_size/sizeof(uint32_t);
        *command_buf_len = AWE_RAW_CMD_HEADER_SZ_1 * sizeof(uint32_t) + param->param_payload_size;
        AWE_LOGI("payload %d, payload %d, payload %d, size %d", raw_ctl->payload[0] , raw_ctl->payload[1],
            raw_ctl->payload[2] , param->param_payload_size);
    } else {
        if (set_flag) {
            //payload for the PFID_SetValueSetCall
            txSize = AWE_RAW_CMD_HEADER_SZ_1;
            raw_ctl->opcode = (uint8_t)PFID_SET_VALUE_SET_CALL;
            uint32_t *pint = (uint32_t *)&param->payload[0];
            raw_ctl->payload[1] = *pint;
            raw_ctl->payload[2] = (uint32_t)(param->flag >> 1);
            raw_ctl->payload[3] = (uint32_t)param->offset;
            AWE_LOGD("word set:%08x",raw_ctl->payload[1]);
        } else {
            //PFID_GetCallFetchValue
            txSize = AWE_RAW_CMD_HEADER_SZ_2;
            raw_ctl->opcode = (uint8_t)PFID_GET_CALL_FETCH_VALUE;
            raw_ctl->payload[1] = (uint32_t)(param->flag >> 1);
            raw_ctl->payload[2] = (uint32_t)param->offset;
        }
        *command_buf_len = txSize * sizeof(uint32_t);
    }
    raw_ctl->length = (uint16_t) txSize;
    AweParamCtlComputeCrc(command_buf);
    *buf = command_buf;
    return;
}

void AweAudioPlugin::AweParamCtlComputeCrc(uint32_t *pMessage)
{
    uint32_t txLen = pMessage[0] >> 16;
    uint32_t csLen = txLen - 1u;
    uint32_t dwCS = 0;
    uint32_t i;
    for (i = 0u; i < csLen; ++i) {
        dwCS ^= pMessage[i];
     }
    pMessage[csLen] = dwCS;
}

//Function to set a param control
uint32_t AweAudioPlugin::AweSetRawParam
    (AweCtrlValue *AweCtrl, const CtlElemPayload &ctl_elem_payload, AweRawParamType type)
{
    uint32_t ret = AWE_EOK;
#ifndef __QNX__
    uint32_t command_buf_len = 0;
    AweRawCtrlVectorValue_t * plugin_ctl_data =
        (AweRawCtrlVectorValue_t *)&ctl_elem_payload.value.tlv.tlv[0];
    AweRawCtrlVectorValue_t * AweCtrldata =
        (AweRawCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];
    switch (type) {
        case RAW_PARAM_SET: {
            if (IS_OEM_TUNNEL_SET(plugin_ctl_data->flag)){
                // OEM_TUNNEL SET
                if (plugin_ctl_data->param_payload_size > MAX_OEM_TUNNEL_PAYLOAD_SIZE) {
                    AWE_LOGE("Payload size %d exceeds maximum allowed %d\n",
                        plugin_ctl_data->param_payload_size, MAX_OEM_TUNNEL_PAYLOAD_SIZE);
                    return AWE_EBADPARAM;
                }

                command_buf_len = plugin_ctl_data->param_payload_size +
                    sizeof(awe_oem_tunnel_msg_t);
                awe_oem_tunnel_msg_t *command_buf =
                    (awe_oem_tunnel_msg_t *) calloc(1, command_buf_len);
                if (command_buf == NULL) {
                    AWE_LOGE("%s: calloc failed.\n", __func__);
                    return AWE_ENOMEMORY;
                }

                command_buf->cmd_id = SET;
                command_buf->buff_sz = plugin_ctl_data->param_payload_size +
                    sizeof(oem_tunnel_in_buf_t);
                AWE_LOGI("Coping input buf of size %d for oem_tunnel command of size %d \n",
                    command_buf->buff_sz, command_buf_len);
                memcpy(&command_buf->buffer.in_buff, plugin_ctl_data, command_buf->buff_sz);

                ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_PROCESS_OEM_TUNNEL_MSG,
                    command_buf, command_buf_len);
                AWE_LOGI("AWE_IOCTL_SND_CARD_OEM_TUNNEL ret err %d\n",ret);
                AWE_LOGI("Error in OEM_TUNNEL output buf ret err %d\n",
                    command_buf->buffer.out_buff.error);

                ret |= command_buf->buffer.out_buff.error;
                free(command_buf);
                if (ret) {
                    return ret;
                }
            } else {
                uint32_t *command_buf = NULL;
                // prepare payload for raw control
                AweBuildRawCtlCommand(&command_buf, 1, plugin_ctl_data, &command_buf_len);
                if (command_buf == NULL) {
                    AWE_LOGE("%Command buffer memory alloc failed!\n");
                    return AWE_ENOMEMORY;
                }
                ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_RAW_CTL_TRANSACT, command_buf,
                    command_buf_len);
                free(command_buf);
            }
            break;
        }
        case RAW_PARAM_GET: {
            // cache control data
            AweCtrldata->param_payload_size = plugin_ctl_data->param_payload_size;
            AweCtrldata->offset = plugin_ctl_data->offset;
            AweCtrldata->instance_id = plugin_ctl_data->instance_id;
            AweCtrldata->handle = plugin_ctl_data->handle;
            AweCtrldata->flag = plugin_ctl_data->flag;
            break;
        }
        default: {
            AWE_LOGE("Unrecognized type %d", type);
            return AWE_EBADPARAM;
        }
    }
#endif
    AWE_LOGD("exit ret %d!", ret);
    return ret;
}

// Function to get a param control
uint32_t AweAudioPlugin::AweGetRawParam
    (AweCtrlValue *AweCtrl, const CtlElemPayload &ctl_elem_payload, AweRawParamType type)
{
    uint32_t ret = AWE_EOK;
#ifndef __QNX__
    uint32_t command_buf_len = 0;
    AweRawCtrlVectorValue_t * ctrl_param = NULL;
    AweRawCtrlVectorValue_t * plugin_ctl_data =
        (AweRawCtrlVectorValue_t *)&ctl_elem_payload.value.tlv.tlv[0];
    AweRawCtrlVectorValue_t * AweCtrldata =
        (AweRawCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];

    AWE_LOGD("enter %d AweCtrl->elem_count %d!!", ctl_elem_payload.access_control,
        AweCtrl->elem_count);

    switch (type) {
        case RAW_PARAM_SET: {
            AWE_LOGD("%s for setParam is not implemented", __func__);
            return ret;
        }
        case RAW_PARAM_GET: {
            if (IS_OEM_TUNNEL_SET(AweCtrldata->flag)){
                // OEM_TUNNEL GET
                command_buf_len = AweCtrldata->param_payload_size +
                    sizeof(awe_oem_tunnel_msg_t);
                awe_oem_tunnel_msg_t *command_buf =
                    (awe_oem_tunnel_msg_t *) calloc(1, command_buf_len);
                if (command_buf == NULL) {
                    AWE_LOGE("%s: calloc failed.\n", __func__);
                    return AWE_ENOMEMORY;
                }

                command_buf->cmd_id = GET;
                command_buf->buff_sz = AweCtrldata->param_payload_size +
                    sizeof(oem_tunnel_out_buf_t);
                AWE_LOGI("Coping input buf of size %d for oem_tunnel command of size %d\n",
                    command_buf->buff_sz, command_buf_len);
                memcpy(&command_buf->buffer.in_buff, AweCtrldata, command_buf->buff_sz);

                ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_PROCESS_OEM_TUNNEL_MSG,
                    command_buf, command_buf_len);
                AWE_LOGI("AWE_IOCTL_SND_CARD_OEM_TUNNEL ret err %d\n",ret);
                AWE_LOGI("Error in OEM_TUNNEL output buf ret err %d\n",
                    command_buf->buffer.out_buff.error);

                ret |= command_buf->buffer.out_buff.error;
                uint32_t ret_payload_sz =
                    command_buf->buffer.out_buff.payload_size <= AweCtrldata->param_payload_size ?
                    command_buf->buffer.out_buff.payload_size: AweCtrldata->param_payload_size;
                memcpy((void *)&plugin_ctl_data->payload, command_buf->buffer.out_buff.payload,
                    ret_payload_sz);
                free(command_buf);
            } else {
                if (AweCtrldata->param_payload_size){
                    uint32_t *command_buf = NULL;
                    // prepare payload for raw control
                    AweBuildRawCtlCommand(&command_buf, 0, AweCtrldata, &command_buf_len);
                    if (!command_buf){
                        return AWE_ENOMEMORY;
                    }

                    ret = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_RAW_CTL_TRANSACT,
                        command_buf, command_buf_len);
                    if (!ret){
                        uint32_t resplen = (command_buf[0]>>16);
                        AWE_LOGI("Returned %d words, payload size %d,param_confif size %d",
                            resplen, AweCtrldata->param_payload_size,sizeof(AweRawCtrlVectorValue_t));
                        memcpy((void *)&plugin_ctl_data->payload, &command_buf[2],
                            AweCtrldata->param_payload_size);
                        AWE_LOGI("first word:%d",*(uint32_t*)&plugin_ctl_data->payload[0]);
                    }
                    else
                        AWE_LOGE("Error %d on AWE_IOCTL_SND_CARD_RAW_CTL_TRANSACT", ret);
                    free(command_buf);
                }
            }
            // copy back to tlv(paylod from command_buf)
            plugin_ctl_data->offset = AweCtrldata->offset;
            plugin_ctl_data->param_payload_size = AweCtrldata->param_payload_size;
            plugin_ctl_data->instance_id = AweCtrldata->instance_id;
            plugin_ctl_data->handle = AweCtrldata->handle;
            plugin_ctl_data->flag = AweCtrldata->flag;
            AweCtrldata->param_payload_size = 0;
            AweCtrldata->offset = 0;
            AweCtrldata->instance_id = 0;
            AweCtrldata->handle = 0;
            AweCtrldata->flag = 0;
            break;
        }
        default: {
            AWE_LOGE("Unrecognized type %d", type);
            return AWE_EBADPARAM;
        }
    }
#endif
    AWE_LOGD(" exit ret %d!", ret);
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
        if (ctl_elem_payload.access_control == ACCESS_READWRITE) {
            memcpy(&AweCtrl->elem_payload.value.bytes.data, &ctl_elem_payload.value.bytes.data,
                AweCtrl->elem_count*sizeof(unsigned char));
            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcWrite bytes data[%d]: %d", i, AweCtrl->elem_payload.value.bytes.data[i]);
        } else if( ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) {
                //will cache the value in AweSetNamedControl
                AWE_LOGD("skip cache bytes data !!!");
        }
        else {
            AWE_LOGE("Unsupported Access type 0x%x for AWE control %s",
                ctl_elem_payload.access_control, ctl_elem_payload.name.c_str());
            ret = AWE_EBADPARAM;
        }
    }
#else // __QNX__
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
        if (ctl_elem_payload.access_control == ACCESS_READWRITE) {
            memcpy(&AweCtrl->elem_payload.value.bytes.data, &ctl_elem_payload.value.bytes.data,
                AweCtrl->elem_count*sizeof(unsigned char));
            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcWrite bytes data[%d]: %d", i, AweCtrl->elem_payload.value.bytes.data[i]);
        } else if( ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) {
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
            if (!strncmp(ctl_elem_payload.name.c_str(), "AWE setParam", strlen("AWE setParam"))) {
                AWE_LOGD("Set static control with AWE setParam command");
                ret = AweSetRawParam(AweCtrl, ctl_elem_payload, RAW_PARAM_SET);
            } else if (!strncmp(ctl_elem_payload.name.c_str(), "AWE getParam", strlen("AWE getParam"))) {
                AWE_LOGD("Set static control with AWE getParam command");
                ret = AweSetRawParam(AweCtrl, ctl_elem_payload, RAW_PARAM_GET);
            }
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
            } else if (!strncmp(ctl_elem_payload.name.c_str(), "AWE setParam", strlen("AWE setParam"))) {
                ret = AweGetRawParam(AweCtrl, ctl_elem_payload, RAW_PARAM_SET);//not suported
            } else if (!strncmp(ctl_elem_payload.name.c_str(), "AWE getParam", strlen("AWE getParam"))) {
                AWE_LOGD("Cache mmap fd for requested pcm device in AweCtrl structure");
                ret = AweGetRawParam(AweCtrl, ctl_elem_payload, RAW_PARAM_GET);
            } else if (!strncmp(ctl_elem_payload.name.c_str(), "AWE getTimestamp", strlen("AWE getTimestamp"))) {
                // Retrieve the cached fe_id
                int fe_id = *(int *)(&AweCtrl->elem_payload.value.bytes.data);
                // Find the stream by fe_id
                AweStream *stream = nullptr;
                for (auto& fe_iter : fe_dai_awe_map_) {
                    AweStream *s = fe_iter.second;
                    if (s->device_id == fe_id) {
                        stream = s;
                        break;
                    }
                }

                if (stream == nullptr) {
                    AWE_LOGE("No stream found for cached fe_id %d", fe_id);
                    ret = AWE_EBADPARAM;
                    break;
                }

                int64_t timestamp = 0;
                ret = StreamGetTimestamp(stream->fe_dai_name, &timestamp);
                AWE_LOGI("StreamTimestamp got %lld, fe_id %d", timestamp, fe_id);
                if (ret == AWE_EOK) {
                    memcpy(&AweCtrl->elem_payload.value.bytes.data, &timestamp, sizeof(timestamp));
                    AWE_LOGI("Timestamp sent %lld", *(long*)&AweCtrl->elem_payload.value.bytes.data);
                } else {
                    AWE_LOGE("Failed to get timestamp for stream %s (fe_id %d)",
                            stream->fe_dai_name.c_str(), fe_id);
                    ret = AWE_EFAILED;
                }
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
        if (ctl_elem_payload.access_control == ACCESS_READWRITE) {
            memcpy(&ctl_elem_payload.value.bytes.data, &AweCtrl->elem_payload.value.bytes.data,
                AweCtrl->elem_count * sizeof(unsigned char));
            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGI("DcRead bytes data[%d]: 0x%x", i, ctl_elem_payload.value.bytes.data[i]);
        }
        else if(ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) {
            /* TODO: need to refactor such that step 3 is integrated in the handling functions like AweGetNamedControl,
             * so that we don't have to run if-else statements here based on the control type.
             */
            if (mixCtl->ctrl_mode == EVENT_CONTROL) {
                memcpy(ctl_elem_payload.value.tlv.tlv, AweCtrl->elem_payload.value.tlv.tlv, AweCtrl->elem_count);
                for(int i=0; i<AweCtrl->elem_count; i++)
                    AWE_LOGD("DcRead tlv bytes data[%d]: 0x%x", i, AweCtrl->elem_payload.value.tlv.tlv[i]);
            } else if (mixCtl->ctrl_mode == NAMED_CONTROL) {
                AweCtrlVectorValue_t * AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];
                memcpy(ctl_elem_payload.value.tlv.tlv, AweCtrldata, AweCtrldata->count * sizeof(uint32_t) + sizeof(AweCtrlVectorValue_t));
                for(int i=0; i<(AweCtrldata->count * sizeof(uint32_t) + sizeof(AweCtrlVectorValue_t)); i++)
                    AWE_LOGD("DcRead tlv bytes data[%d]: 0x%x", i, ctl_elem_payload.value.tlv.tlv[i]);

                //clear offset, count and do_set in AweCtrl
                AweCtrldata->offset = 0;
                AweCtrldata->count = 0;
                AweCtrldata->do_set = 0;
            } else if (mixCtl->ctrl_mode == STATIC_CONTROL_DSP) {
                if (!strncmp(ctl_elem_payload.name.c_str(), "AWE getParam", strlen("AWE getParam"))) {
                    AWE_LOGI("DcRead tlv bytes copied in AweGetStaticControl");
                }
            }
        }
        else {
            AWE_LOGE("Unsupported Access type 0x%x for AWE control %s",
                ctl_elem_payload.access_control, ctl_elem_payload.name.c_str());
            ret = AWE_EBADPARAM;
        }
    }
#else // __QNX__
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
        if (ctl_elem_payload.access_control == ACCESS_READWRITE) {
            memcpy(&ctl_elem_payload.value.bytes.data, &AweCtrl->elem_payload.value.bytes.data,
                AweCtrl->elem_count * sizeof(unsigned char));
            for(int i=0; i<AweCtrl->elem_count; i++)
                AWE_LOGD("DcRead bytes data[%d]: 0x%x", i, ctl_elem_payload.value.bytes.data[i]);
        }
        else if(ctl_elem_payload.access_control == ACCESS_TLV_READWRITE) {
            /* TODO: need to refactor such that step 3 is integrated in the handling functions like AweGetNamedControl,
             * so that we don't have to run if-else statements here based on the control type.
             */
            if (mixCtl->ctrl_mode == EVENT_CONTROL) {
                memcpy(ctl_elem_payload.value.tlv.tlv, AweCtrl->elem_payload.value.tlv.tlv, AweCtrl->elem_count);
                for(int i=0; i<AweCtrl->elem_count; i++)
                    AWE_LOGD("DcRead tlv bytes data[%d]: 0x%x", i, AweCtrl->elem_payload.value.tlv.tlv[i]);
            } else if (mixCtl->ctrl_mode == NAMED_CONTROL) {
                AweCtrlVectorValue_t * AweCtrldata = (AweCtrlVectorValue_t *)&AweCtrl->elem_payload.value.tlv.tlv[0];
                memcpy(ctl_elem_payload.value.tlv.tlv, AweCtrldata, AweCtrldata->count * sizeof(uint32_t) + sizeof(AweCtrlVectorValue_t));
                for(int i=0; i<(AweCtrldata->count * sizeof(uint32_t) + sizeof(AweCtrlVectorValue_t)); i++)
                    AWE_LOGD("DcRead tlv bytes data[%d]: 0x%x", i, ctl_elem_payload.value.tlv.tlv[i]);

                //clear offset, count and do_set in AweCtrl
                AweCtrldata->offset = 0;
                AweCtrldata->count = 0;
                AweCtrldata->do_set = 0;
            } else if (mixCtl->ctrl_mode == STATIC_CONTROL_DSP) {
                if (!strncmp(ctl_elem_payload.name.c_str(), "AWE getParam", strlen("AWE getParam"))) {
                    AWE_LOGI("DcRead tlv bytes copied in AweGetStaticControl");
                }
            }
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

uint32_t AweAudioPlugin::StreamGetTimestamp(string fe_dai_name, int64_t *timestamp)
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

    if (stream->state != PCM_STATE_RUNNING){
        AWE_LOGI("The AWE Stream %s is not in the running state", fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }

    // Use ioctl to get the timestamp for this stream
    ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_GET_TIMESTAMP, timestamp, sizeof(int64_t));
    if (ret != AWE_EOK) {
        AWE_LOGE("Failed to get timestamp for stream %s, with error: %d", fe_dai_name.c_str(), ret);
        return ret;
    }

    AWE_LOGD("Stream %s, timestamp: %lld", fe_dai_name.c_str(), *timestamp);
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

    stream->is_push_poll = set_param_payload.is_mmap_noirq;
    // only cache period_size, period_cnt;
    stream->buffer_config.period_size  =  set_param_payload.period_bytes;
    stream->buffer_config.period_cnt   =  set_param_payload.buffer_bytes / set_param_payload.period_bytes;

    if (!stream->is_push_poll) {
        //AWE controller will allocate memory in the regular read/write mode.
        stream->buffer_config.transfer_buffer_fd = set_param_payload.buff_fd;
        stream->buffer_config.position_buffer_fd = 0;
        AWE_LOGI("AWE stream: %s, set buffer config in hw_param, transfer_buff_fd=%d", fe_dai_name.c_str(), set_param_payload.buff_fd);
        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_BUFFER_CONFIG, &stream->buffer_config, sizeof(awe_buffer_config));
        if (ret != AWE_EOK) {
            AWE_LOGE("Failed to set buffer config for stream : %s", fe_dai_name.c_str());
            return ret;
        }

        ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_GET_BUFFER_RAW_INFO, &stream->raw_buff_info, sizeof(struct awe_buf_raw_info));
        if (ret != AWE_EOK) {
            AWE_LOGE("Failed to get buffer raw info for stream : %s", fe_dai_name.c_str());
            return ret;
        }
    }

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
        return AWE_EFAILED;
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
    memset(&stream->raw_buff_info, 0, sizeof(awe_buf_raw_info));
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
    }

    struct awe_threshold_config threshold_config;
    threshold_config.start_level = 0;
    threshold_config.stop_level = stream->buffer_config.period_size * stream->buffer_config.period_cnt;
    AWE_LOGI("AWE stream: %s StreamStart set threshold: start level %d, stop level %d",fe_dai_name.c_str(), threshold_config.start_level, threshold_config.stop_level);
    ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_SET_THRESHOLD, &threshold_config, sizeof(struct awe_threshold_config));
    if (ret != AWE_EOK) {
        AWE_LOGE("StreamStart set threshold failed for stream : %s", fe_dai_name.c_str());
        return ret;
    }

    AWE_LOGI("AWE stream: %s StreamStart.", fe_dai_name.c_str());
    ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_START, nullptr, 0);
    if (ret != AWE_EOK) {
        AWE_LOGE("Failed to start AWE stream : %s", fe_dai_name.c_str());
        return ret;
    }
    stream->state = PCM_STATE_RUNNING;

    std::lock_guard<std::mutex> rw_lock(stream->stream_mutex);
    stream->stream_cond.notify_one();

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

    AWE_LOGI("AWE stream: %s StreamStop start", fe_dai_name.c_str());
    ret = awe_ioctl(stream->handle, AWE_IOCTL_STREAM_STOP, nullptr, 0);
    if (ret != AWE_EOK && ret != AWE_ESUBSYSRESET) {
        AWE_LOGE("Failed to stop AWE stream : %s", fe_dai_name.c_str());
        return ret;
    }
    AWE_LOGI("AWE stream: %s StreamStop end", fe_dai_name.c_str());
    stream->state = PCM_STATE_SETUP;
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

    // block to wait StreamStart
    std::unique_lock<std::mutex> write_lock(stream->stream_mutex);
    if (!stream->stream_cond.wait_for(write_lock, std::chrono::milliseconds(500), [&] {
        return stream->state == PCM_STATE_RUNNING;
    })) {
        AWE_LOGE("timeout, return directly, msg drop might happen");
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
    }

    return AWE_EOK;
}

uint32_t AweAudioPlugin::StreamWriteDone(virtio_snd_pcm_buf_msg &msg)
{
    struct awe_pos_buf *pos_buff = NULL;
    uint32_t period_frames = 0;
    uint32_t buffer_frames = 0;
    uint32_t target_wait_index = 0;
    uint32_t old_read_index = 0;
    uint32_t read_index = 0;
    uint32_t retry_count = 0;
    bool readround_flag = false;
    uint32_t waiting_frames = 0;
    uint32_t target_wait_timeUs = 0;
    uint32_t closing_wait_timeUS = 0;

    // Find the stream
    auto it = fe_dai_awe_map_.find(msg.fe_dai_name);
    if (it == fe_dai_awe_map_.end()) {
        AWE_LOGE("AWE stream: %s not found", msg.fe_dai_name.c_str());
        return AWE_EBADPARAM;
    }
    AweStream* stream = it->second;
    pos_buff = (struct awe_pos_buf *)stream->raw_buff_info.pos_buf_vaddr;
    old_read_index = pos_buff->read_index;

    period_frames = audio_convert_byte_to_frame(stream->buffer_config.period_size,
                                stream->format.format,
                                stream->format.channels);
    buffer_frames = period_frames * stream->buffer_config.period_cnt;
    target_wait_index = audio_convert_byte_to_frame(
                                msg.offset + msg.size,
                                stream->format.format,
                                stream->format.channels);

    target_wait_index = target_wait_index % buffer_frames;
    retry_count = 0;
    if (old_read_index < target_wait_index) {
        waiting_frames = target_wait_index - old_read_index;
    } else {
        waiting_frames = (target_wait_index + buffer_frames) - old_read_index;
    }
    // theoretic duration to reach the target index for first
    target_wait_timeUs = (waiting_frames * 1000000) / stream->format.rate;
    // closing target index by 1/8 per step for rest
    closing_wait_timeUS = ((period_frames * 1000000) / stream->format.rate) / 8;

    if (old_read_index < target_wait_index) {
        // 0 ... old_read_index ... target_index ... buff_bound
        // break when
        // 0 ... old_read_index ... target_index ... read_index ... buff_bound
        do {
            if (retry_count == 0)
                SLEEP_US(target_wait_timeUs);
            else
                SLEEP_US(closing_wait_timeUS);

            retry_count++;
            read_index = pos_buff->read_index;
            AWE_LOGD("wait case1 r_i %d, t_i %d cnt %d", read_index, target_wait_index, retry_count);
        } while ((read_index < target_wait_index) && retry_count < MAX_POLL_CNT);
    } else {
        // 0 ... target_index ... old_read_index ... buff_bound
        // break when
        // 0 ... target_index ... read_index ... old_read_index ... buff_bound
        do {
            if (retry_count == 0)
                SLEEP_US(target_wait_timeUs);
            else
                SLEEP_US(closing_wait_timeUS);

            retry_count++;
            read_index = pos_buff->read_index;
            /* ensure read one round done */
            if (read_index <= old_read_index) {
                readround_flag = true;
            }
            AWE_LOGD("wait case2 r_i %d, t_i %d cnt %d", read_index, target_wait_index, retry_count);
        } while ((read_index < target_wait_index || !readround_flag) && retry_count < MAX_POLL_CNT);
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

    // block to wait StreamStart
    std::unique_lock<std::mutex> read_lock(stream->stream_mutex);
    if (!stream->stream_cond.wait_for(read_lock, std::chrono::milliseconds(500), [&] {
        return stream->state == PCM_STATE_RUNNING;
    })) {
        AWE_LOGE("timeout, return directly, msg drop might happen");
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
            newCtrlValue->elem_payload.access_control = ACCESS_TLV_READWRITE;
            newCtrlValue->elem_payload.value.tlv.tlv = new uint8_t[MAX_TLV_SIZE];
            memset(newCtrlValue->elem_payload.value.tlv.tlv, 0, MAX_TLV_SIZE);
            AWE_LOGI("ctrl is tlv %s.",newCtrlValue->elem_payload.name.c_str());
        }
        else {
            newCtrlValue->elem_payload.access_control = ACCESS_READWRITE;
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
        if (iter.second->elem_payload.access_control == ACCESS_TLV_READWRITE && iter.second->elem_payload.value.tlv.tlv != nullptr)
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
        if (iter.second->elem_payload.access_control == ACCESS_TLV_READWRITE && iter.second->elem_payload.value.tlv.tlv != nullptr)
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
#ifndef __QNX__
    awe_core_error_info_t *ctrl_param = (awe_core_error_info_t *)&AweCtrl->elem_payload.value.bytes.data;
    if (!ctrl_param){
        return AWE_EBADPARAM;
    }
    memset(ctrl_param->description, 0 ,sizeof(ctrl_param->description));
    rc = awe_ioctl(awe_snd_card_handle_, AWE_IOCTL_SND_CARD_GET_AWE_CORE_ERROR_INFO, ctrl_param, sizeof(awe_core_error_info_t));
#endif
    return rc;

}