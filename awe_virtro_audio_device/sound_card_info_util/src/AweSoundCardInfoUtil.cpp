/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <string.h>
#include "AweSoundCardInfoUtil.h"
#include "awe_log_util.h"

#define DEVICE_HOSTLESS_PREFIX "Hostless"

static uint32_t getSndCtlElemValueCount(uint32_t ctl_type, uint32_t ctl_access)
{
    uint32_t limit_count = 0;
#if !defined(__QNX__)
    uint32_t tlv_mask = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
            SNDRV_CTL_ELEM_ACCESS_TLV_WRITE |
            SNDRV_CTL_ELEM_ACCESS_TLV_COMMAND;

    if ((ctl_type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) || (ctl_type == SNDRV_CTL_ELEM_TYPE_INTEGER)){
        limit_count = sizeof(snd_ctl_elem_value().value.integer.value) / sizeof(long);
    } else if (ctl_type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
        limit_count = sizeof(snd_ctl_elem_value().value.integer64.value) / sizeof(long long);
    } else if (ctl_type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        limit_count = sizeof(snd_ctl_elem_value().value.enumerated.item) / sizeof(unsigned int);
    } else if (ctl_type == SNDRV_CTL_ELEM_TYPE_BYTES) {
        if (ctl_access & tlv_mask)
            limit_count = 1024;
        else
            limit_count = sizeof(snd_ctl_elem_value().value.bytes.data) / sizeof(unsigned char);
    }
#else
    uint32_t tlv_mask = VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_READ |
            VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_WRITE |
            VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_COMMAND;
    if ((ctl_type == VIRTIO_MIXER_CTL_TYPE_MASK_BOOLEAN) || (ctl_type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER)){
        limit_count = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_SIZE / sizeof(long);
    } else if (ctl_type == VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER64) {
        limit_count = VIRTIO_CTL_ELEM_VALUE_INTEGER64_VALUE_SIZE / sizeof(long long);
    } else if (ctl_type == VIRTIO_MIXER_CTL_TYPE_MASK_ENUMERATED) {
        limit_count = VIRTIO_CTL_ELEM_VALUE_ENUMERATED_ITEM_SIZE / sizeof(unsigned int);
    } else if (ctl_type == VIRTIO_MIXER_CTL_TYPE_MASK_BYTES) {
        if (ctl_access & tlv_mask)
            limit_count = 1024;
        else
            limit_count = VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_SIZE / sizeof(unsigned char);
    }
#endif
    AWE_LOGI("ctl_type = %d, ctl_access = %d, limit_count = %d",
        ctl_type, ctl_access, limit_count);
    return limit_count;
}

/* Returns an instance of the SoundCardInfoUtil for a given gvm_id.
   If no gvm_id is provided, the default of 0 is used (i.e. single GVM case).
*/
AweSoundCardInfoUtil::AweSoundCardInfoUtil(const int gvm_id)
    : gvm_id_(gvm_id),
      num_pcm_devices_(0),
      num_mixer_ctrls_(0) {
}

AweSoundCardInfoUtil::~AweSoundCardInfoUtil()
{
}

uint32_t AweSoundCardInfoUtil::BuildSoundCard()
{
    uint32_t ret = AWE_EOK;
    awe_device_list_t *awe_dev_list = nullptr;
    awe_device_t *awe_dev = nullptr;
    uint32_t total_size;
    uint32_t awe_dev_count;
    awe_control_list_t *awe_ctl_list = nullptr;
    awe_control_t *awe_ctl = nullptr;
    uint32_t awe_ctl_count;
    awe_event_list_t *awe_event_list = nullptr;
    awe_event_t *awe_event = nullptr;
    uint32_t awe_event_count;
    uint32_t handle;
    awe_snd_card_open_config_t open_cfg = {AWE_VM_ID_INVALID, 0};

    open_cfg.vmid = AWE_VM_ID_1;
    ret = awe_open(AWE_MODULE_TYPE_SND_CARD, (void *)&open_cfg, sizeof(open_cfg), &handle);
    if (ret) {
        AWE_LOGE("Null device pointer provided.");
        goto err;
    }
    ret = awe_ioctl(handle, AWE_IOCTL_SND_CARD_GET_DEVICE_COUNT, &awe_dev_count, sizeof(awe_device_count_t));
    if (ret) {
        AWE_LOGE("awe_ioctl AWE_IOCTL_SND_CARD_GET_DEVICE_COUNT fails, ret = %d", ret);
        goto err;
    }
    AWE_LOGI("awe_ioctl AWE_IOCTL_SND_CARD_GET_DEVICE_COUNT success, device count = %d",awe_dev_count);

    if (awe_dev_count > 0) {
        total_size = sizeof(awe_device_list_t) + awe_dev_count * sizeof(awe_device_t);
        awe_dev_list = (awe_device_list_t *)calloc(1, total_size);
        if (awe_dev_list == nullptr) {
            AWE_LOGE("alloc awe_get_param failed");
            ret = AWE_ENOMEMORY;
            goto err;
        }
        awe_dev_list->device_count = awe_dev_count;
        ret = awe_ioctl(handle, AWE_IOCTL_SND_CARD_GET_DEVICE_LIST, awe_dev_list, total_size);
        if (ret != AWE_EOK) {
            AWE_LOGE("awe_ioctl: AWE_IOCTL_SND_CARD_GET_DEVICE_LIST failed");
            goto err;
        }
        /* Get the actual payload and fill it into */
        awe_dev = (awe_device_t *)(awe_dev_list + 1);
        for (size_t i = 0; i < awe_dev_count; i++) {
            AWE_LOGD("device_id = %d",awe_dev->device_id);
            AWE_LOGD("device_dir = 0x%x",awe_dev->device_dir);
            AWE_LOGD("device_name = %s",awe_dev->device_name);
            ret = AddAwePcmDevice(awe_dev);
            if (ret != AWE_EOK) {
                AWE_LOGE("AddAwePcmDevice failed ret = %d", ret);
                goto err;
            }
            awe_dev++;
        }
    }
    num_pcm_devices_ = all_pcm_devices_.size();

    ret = awe_ioctl(handle, AWE_IOCTL_SND_CARD_GET_NAMED_CONTROL_COUNT, &awe_ctl_count, sizeof(awe_control_count_t));
    if (ret != AWE_EOK) {
        AWE_LOGE("awe_ioctl AWE_IOCTL_SND_CARD_GET_NAMED_CONTROL_COUNT fails, ret = %d", ret);
        goto err;
    }
    AWE_LOGI("awe control count = %d",awe_ctl_count);
    total_size = sizeof(awe_control_list_t) + awe_ctl_count * sizeof(awe_control_t);
    awe_ctl_list = (awe_control_list_t *)calloc(1, total_size);
    if (awe_ctl_list == nullptr) {
        AWE_LOGE("alloc awe_ctl_list failed");
        ret = AWE_ENOMEMORY;
        goto err;
    }
    awe_ctl_list->control_count = awe_ctl_count;

    ret = awe_ioctl(handle, AWE_IOCTL_SND_CARD_GET_NAMED_CONTROL_LIST, awe_ctl_list, total_size);
    if (ret != AWE_EOK) {
        AWE_LOGE("awe_ioctl: AWE_IOCTL_SND_CARD_GET_NAMED_CONTROL_LIST failed");
        goto err;
    }

    awe_ctl = (awe_control_t *)(awe_ctl_list + 1);
    for (size_t i = 0; i < awe_ctl_count; i++) {
        AWE_LOGD("control_id  = %d",awe_ctl->control_id);
        AWE_LOGD("control_type  = 0x%x",awe_ctl->control_type);
        AWE_LOGD("control_name  = %s",awe_ctl->control_name);
        ret = AddAweMixCtl(awe_ctl);
        if (ret != AWE_EOK) {
            AWE_LOGE("AddAweMixCtl failed ret = %d", ret);
            goto err;
        }
        awe_ctl++;
    }

    ret = awe_ioctl(handle, AWE_IOCTL_SND_CARD_GET_EVENT_COUNT, &awe_event_count, sizeof(awe_event_count_t));
    if (ret != AWE_EOK) {
        AWE_LOGE("awe_ioctl AWE_IOCTL_SND_CARD_GET_EVENT_COUNT failed, ret = %d", ret);
        goto err;
    }
    AWE_LOGI("awe event count = %d",awe_ctl_count);
    total_size = sizeof(awe_event_list_t) + awe_event_count * sizeof(awe_event_t);
    awe_event_list = (awe_event_list_t *)calloc(1, total_size);
    if (awe_event_list == nullptr) {
        AWE_LOGE("alloc awe_event_list failed");
        ret = AWE_ENOMEMORY;
        goto err;
    }
    awe_event_list->event_count = awe_event_count;

    ret = awe_ioctl(handle, AWE_IOCTL_SND_CARD_GET_EVENT_LIST, awe_event_list, total_size);
    if (ret != AWE_EOK) {
        AWE_LOGE("awe_ioctl: AWE_IOCTL_SND_CARD_GET_EVENT_LIST failed");
        goto err;
    }

    awe_event = (awe_event_t *)(awe_event_list + 1);
    for (size_t i = 0; i < awe_event_count; i++) {
        AWE_LOGD("event instance_id  = %d",awe_event->instance_id);
        AWE_LOGD("event object id  = 0x%x",awe_event->object_id);
        AWE_LOGD("event name  = %s",awe_event->event_name);
        ret = AddAweEventCtl(awe_event);
        if (ret != AWE_EOK) {
            AWE_LOGE("AddAweEventCtl failed ret = %d", ret);
            goto err;
        }
        awe_event++;
    }

    AddStaticMixerCtls();

    num_mixer_ctrls_ = all_mixer_ctrls_.size();
    awe_close(AWE_MODULE_TYPE_SND_CARD, handle);
err:
    if(awe_dev_list != nullptr)
        free(awe_dev_list);
    if(awe_ctl_list != nullptr)
        free(awe_ctl_list);
    if(awe_event_list != nullptr)
        free(awe_event_list);
    AWE_LOGI("Build sound card exit with ret = %d", ret);
    return ret;
}

int AweSoundCardInfoUtil::GetNumPCMSubstreams()
{
    return num_pcm_devices_;
}

int AweSoundCardInfoUtil::GetNumMixerCtls() {
    return num_mixer_ctrls_;
}

std::vector<FrontendInfo> AweSoundCardInfoUtil::GetPCMSubstreams() {
    return all_pcm_devices_;
}

std::vector<SoundCardMixerCtl> AweSoundCardInfoUtil::GetMixerCtls() {
    return all_mixer_ctrls_;
}

uint32_t AweSoundCardInfoUtil::GetEnumsForControl(const int control_id, std::vector<VirtioSndDCEnum> &ctl_enums)
{

    // Find the enum control
    AWE_LOGI("GetEnumsForControl start");
    auto it = control_enum_map_.find(control_id);
    if (it != control_enum_map_.end()) {

        AWE_LOGI("Enum control : %d was found", control_id);
        ctl_enums = it->second;
        for (auto& enumItem : ctl_enums) {
            AWE_LOGI("Enum value is: %s ", reinterpret_cast<char*>(enumItem.dai_name));
        }
        return AWE_EOK;
    } else {
        AWE_LOGE("Enum control : %d was not found", control_id);
        return AWE_EBADPARAM;
    }
    AWE_LOGI("GetEnumsForControl exit");
}

uint32_t AweSoundCardInfoUtil::AddAwePcmDevice(const awe_device_t *awe_dev)
{
    if (awe_dev == nullptr) {
        AWE_LOGE("Null device pointer provided.");
        return AWE_EBADPARAM;
    }
    // Creating a new FrontendInfo object
    FrontendInfo newDevice;
    newDevice.name = std::string(awe_dev->device_name, strnlen(awe_dev->device_name, AWE_MAX_DEVICE_NAME_LENGTH));
    newDevice.id = awe_dev->device_id;
    // Convert from AWE: playback 2, capture 1 to VIOSND: OUTPUT 0, INPUT 1
    if (awe_dev->device_dir == 2) {
#if !defined(__QNX__)
        newDevice.direction = SNDRV_PCM_STREAM_PLAYBACK;
#else
        newDevice.direction = VIRTIO_STREAM_PLAYBACK;
#endif
    } else if (awe_dev->device_dir == 1) {
#if !defined(__QNX__)
        newDevice.direction = SNDRV_PCM_STREAM_CAPTURE;
#else
        newDevice.direction = VIRTIO_STREAM_CAPTURE;
#endif
    } else {
        AWE_LOGE("Unsupported dir");
        return AWE_EBADPARAM;
    }

    // Unique if no matches found
    for (const auto& device : all_pcm_devices_) {
        if (device.name == newDevice.name && device.id == newDevice.id) {
            AWE_LOGI("Device has been added.");
            return AWE_EOK;
        }
    }

    // TODO: We need to get formats info from DSP side
    newDevice.formats = ((1ULL << VIRTIO_SND_PCM_FMT_S16) |
                    (1ULL << VIRTIO_SND_PCM_FMT_S24) |
                    (1ULL << VIRTIO_SND_PCM_FMT_S24_3) |
                    (1ULL << VIRTIO_SND_PCM_FMT_S32));
    newDevice.rates = ((1ULL << VIRTIO_SND_PCM_RATE_5512) |
                    (1ULL << VIRTIO_SND_PCM_RATE_8000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_11025) |
                    (1ULL << VIRTIO_SND_PCM_RATE_12000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_16000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_22050) |
                    (1ULL << VIRTIO_SND_PCM_RATE_24000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_32000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_44100) |
                    (1ULL << VIRTIO_SND_PCM_RATE_48000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_64000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_88200) |
                    (1ULL << VIRTIO_SND_PCM_RATE_96000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_176400) |
                    (1ULL << VIRTIO_SND_PCM_RATE_192000) |
                    (1ULL << VIRTIO_SND_PCM_RATE_384000));
    newDevice.channels_min = 1;
    newDevice.channels_max = 32;
    newDevice.sess_mode = kSessModeDefault;
    newDevice.features = 0;

    // Loopback feature
    if (strstr(newDevice.name.c_str(), DEVICE_HOSTLESS_PREFIX) != NULL) {
        newDevice.sess_mode = kSessModeNoHost;
    }

    if (newDevice.sess_mode == kSessModeNoHost) //temp value
        newDevice.features |= (1U << VIRTIO_SND_PCM_F_HOSTLESS);

     AWE_LOGI("sess_mode = %d, features= 0x%x.", newDevice.sess_mode, newDevice.features);
    // Add the device to all_frontends_
    all_pcm_devices_.push_back(newDevice);
    AWE_LOGI("Device : %s has been successfully added.", newDevice.name.c_str());
    return AWE_EOK;
}

void AweSoundCardInfoUtil::AddStaticMixerCtls()
{
#if !defined(__QNX__)
    SoundCardMixerCtl staticMixerCtrls[] = {
        {
            {"AWE setParam", 0, SNDRV_CTL_ELEM_IFACE_MIXER},
            kNone,
            SNDRV_CTL_ELEM_TYPE_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_DSP,
            SNDRV_CTL_ELEM_ACCESS_READWRITE,
            getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, SNDRV_CTL_ELEM_ACCESS_READWRITE),
        },
        {
            {"AWE getParam", 0, SNDRV_CTL_ELEM_IFACE_MIXER},
            kNone,
            SNDRV_CTL_ELEM_TYPE_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_DSP,
            SNDRV_CTL_ELEM_ACCESS_READWRITE,
            getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, SNDRV_CTL_ELEM_ACCESS_READWRITE),
        },
        {
            {"ShmemMap", 0, SNDRV_CTL_ELEM_IFACE_MIXER},
            kNone,
            SNDRV_CTL_ELEM_TYPE_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_NODSP,
            SNDRV_CTL_ELEM_ACCESS_READWRITE,
            12*4,
        },
        {
            {"ShmemUnmap", 0, SNDRV_CTL_ELEM_IFACE_MIXER},
            kNone,
            SNDRV_CTL_ELEM_TYPE_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_NODSP,
            SNDRV_CTL_ELEM_ACCESS_READWRITE,
            12*4,
        },
        {
            {"PCM event", 0, SNDRV_CTL_ELEM_IFACE_MIXER},
            kNone,
            SNDRV_CTL_ELEM_TYPE_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_DSP,
            SNDRV_CTL_ELEM_ACCESS_TLV_READ | SNDRV_CTL_ELEM_ACCESS_TLV_WRITE,
            getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, SNDRV_CTL_ELEM_ACCESS_TLV_READ | SNDRV_CTL_ELEM_ACCESS_TLV_WRITE),
        },
        {
            {"AWE getMmapFD", 0, SNDRV_CTL_ELEM_IFACE_MIXER},
            kNone,
            SNDRV_CTL_ELEM_TYPE_INTEGER,
            AWE_CONTROL_TYPE_INT,
            STATIC_CONTROL_NODSP,
            SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_WRITE,
            1,
        },
        {
            {"AWE coreErrorInfo", 0, SNDRV_CTL_ELEM_IFACE_MIXER},
            kNone,
            SNDRV_CTL_ELEM_TYPE_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_DSP,
            SNDRV_CTL_ELEM_ACCESS_READWRITE,
            getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, SNDRV_CTL_ELEM_ACCESS_READWRITE),
        },
    };
#else
    SoundCardMixerCtl staticMixerCtrls[] = {
        {
            {"AWE setParam", 0, VIRTIO_CTL_ID_IFACE_MIXER},
            kNone,
            VIRTIO_MIXER_CTL_TYPE_MASK_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_DSP,
            VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE,
            getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE),
        },
        {
            {"AWE getParam", 0, VIRTIO_CTL_ID_IFACE_MIXER},
            kNone,
            VIRTIO_MIXER_CTL_TYPE_MASK_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_DSP,
            VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE,
            getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE),
        },
        {
            {"ShmemMap", 0, VIRTIO_CTL_ID_IFACE_MIXER},
            kNone,
            VIRTIO_MIXER_CTL_TYPE_MASK_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_NODSP,
            VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE,
            12*4,
        },
        {
            {"ShmemUnmap", 0, VIRTIO_CTL_ID_IFACE_MIXER},
            kNone,
            VIRTIO_MIXER_CTL_TYPE_MASK_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_NODSP,
            VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE,
            12*4,
        },
        {
            {"PCM event", 0, VIRTIO_CTL_ID_IFACE_MIXER},
            kNone,
            VIRTIO_MIXER_CTL_TYPE_MASK_BYTES,
            AWE_CONTROL_TYPE_BYTE,
            STATIC_CONTROL_DSP,
            VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_READ | VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_WRITE,
            getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_READ | VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_WRITE),
        },
        {
            {"AWE getMmapFD", 0, VIRTIO_CTL_ID_IFACE_MIXER},
            kNone,
            VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER,
            AWE_CONTROL_TYPE_INT,
            STATIC_CONTROL_NODSP,
            VIRTIO_MIXER_CTL_ACCESS_MASK_READ | VIRTIO_MIXER_CTL_ACCESS_MASK_WRITE,
            1,
        },
    };
#endif
    for (int i =0; i < sizeof(staticMixerCtrls) / sizeof(staticMixerCtrls[0]); i++) {
        staticMixerCtrls[i].id.ctl_id = all_mixer_ctrls_.size();
        all_mixer_ctrls_.push_back(staticMixerCtrls[i]);
        AWE_LOGI("Mixer control : %s has been successfully added.", staticMixerCtrls[i].id.name.c_str());
    }
}
uint32_t AweSoundCardInfoUtil::AddAweMixCtl(const awe_control_t *awe_ctl)
{
    uint32_t limit_count = 0;
    if (awe_ctl == nullptr) {
        AWE_LOGE("Null control pointer provided.");
        return AWE_EBADPARAM;
    }
    // Creating a new SoundCardMixerCtl object
    SoundCardMixerCtl newCtl;
    newCtl.id.name = std::string(awe_ctl->control_name, strnlen(awe_ctl->control_name, AWE_MAX_CONTROL_NAME_LENGTH));
    newCtl.id.ctl_id = awe_ctl->control_id;
#if !defined(__QNX__)
    newCtl.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
#else
    newCtl.id.iface = VIRTIO_CTL_ID_IFACE_MIXER;
#endif
    newCtl.awe_ctl_type = awe_ctl->control_type;
    newCtl.ctrl_mode = NAMED_CONTROL;
#if !defined(__QNX__)
    newCtl.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
#else
    newCtl.access = VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE;
#endif
    newCtl.count = awe_ctl->count; //TODO: EXTQCMOI-150

    AWE_LOGD("awe_ctl->count %d", awe_ctl->count);
#if !defined(__QNX__)
    if (awe_ctl->count == 1) {
        switch (newCtl.awe_ctl_type) {
            case AWE_CONTROL_TYPE_BOOL: {
                newCtl.type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
                break;
            }
            case AWE_CONTROL_TYPE_INT: {
                newCtl.type = SNDRV_CTL_ELEM_TYPE_INTEGER;
                // TODO: need to get info from DSP side
                // Hard code for temp
                newCtl.value.integer.min = 0;
                newCtl.value.integer.max = 65536;
                newCtl.value.integer.step = 1;
                break;
            }
            case AWE_CONTROL_TYPE_ENUM: {
                newCtl.type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
                // TODO: need to get enum info from DSP side
                // Hard code for test
                newCtl.value.enumerated.items = 3;

                std::vector<VirtioSndDCEnum> enum_info = {{"Enum_One"}, {"Enum_Two"},{"Enum_Tree"}};
                std::pair<int, std::vector<VirtioSndDCEnum>>enum_info_pair(all_mixer_ctrls_.size(), enum_info);
                AWE_LOGI("Insert the %s to control_enum_map_.",newCtl.id.name.c_str());
                control_enum_map_.insert(enum_info_pair);
                break;
            }
            case AWE_CONTROL_TYPE_BYTE: {
                newCtl.type = SNDRV_CTL_ELEM_TYPE_BYTES;
                break;
            }
            case AWE_CONTROL_TYPE_IEC958: {
                newCtl.type = SNDRV_CTL_ELEM_TYPE_IEC958;
                break;
            }
            case AWE_CONTROL_TYPE_INT64: {
                newCtl.type = SNDRV_CTL_ELEM_TYPE_INTEGER64;
                break;
            }
            case AWE_CONTROL_TYPE_FLOAT: {
                newCtl.type = SNDRV_CTL_ELEM_TYPE_BYTES;
                newCtl.count = newCtl.count * sizeof(float);
                break;
            }
            default: {
                AWE_LOGE("Unrecognized awe_control_type %d", newCtl.awe_ctl_type);
                newCtl.type = SNDRV_CTL_ELEM_TYPE_NONE;
                break;
            }
        }
    } else {
        newCtl.type = SNDRV_CTL_ELEM_TYPE_BYTES;
        newCtl.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE;
        newCtl.count = newCtl.count * sizeof(uint32_t) + 2 * sizeof(uint32_t);//allocate extra for count, offset and do_set.
    }
#else
    if (awe_ctl->count == 1) {
        switch (newCtl.awe_ctl_type) {
            case AWE_CONTROL_TYPE_BOOL: {
                newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_BOOLEAN;
                break;
            }
            case AWE_CONTROL_TYPE_INT: {
                newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER;
                // TODO: need to get info from DSP side
                // Hard code for temp
                newCtl.value.integer.min = 0;
                newCtl.value.integer.max = 65536;
                newCtl.value.integer.step = 1;
                break;
            }
            case AWE_CONTROL_TYPE_ENUM: {
                newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_ENUMERATED;
                // TODO: need to get enum info from DSP side
                // Hard code for test
                newCtl.value.enumerated.items = 3;

                std::vector<VirtioSndDCEnum> enum_info = {{"Enum_One"}, {"Enum_Two"},{"Enum_Tree"}};
                std::pair<int, std::vector<VirtioSndDCEnum>>enum_info_pair(all_mixer_ctrls_.size(), enum_info);
                AWE_LOGI("Insert the %s to control_enum_map_.",newCtl.id.name.c_str());
                control_enum_map_.insert(enum_info_pair);
                break;
            }
            case AWE_CONTROL_TYPE_BYTE: {
                newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_BYTES;
                break;
            }
            case AWE_CONTROL_TYPE_IEC958: {
                newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_IEC958;
                break;
            }
            case AWE_CONTROL_TYPE_INT64: {
                newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER64;
                break;
            }
            case AWE_CONTROL_TYPE_FLOAT: {
                newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_BYTES;
                newCtl.count = newCtl.count * sizeof(float);
                break;
            }
            default: {
                AWE_LOGE("Unrecognized awe_control_type %d", newCtl.awe_ctl_type);
                newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_NONE;
                break;
            }
        }
    } else {
        newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_BYTES;
        newCtl.access = VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE;
        newCtl.count = newCtl.count * sizeof(uint32_t) + 2 * sizeof(uint32_t);//allocate extra for count, offset and do_set.
    }
#endif

    limit_count = getSndCtlElemValueCount(newCtl.type, newCtl.access);
    if (limit_count == 0)
    {
        AWE_LOGE("Unsupported type[%d] or access[%d]", newCtl.type, newCtl.access);
        return AWE_EBADPARAM;
    }
    if (newCtl.count > limit_count){
        AWE_LOGE("Count input is %d too large, should be limited in %d.", newCtl.count, limit_count);
//        return AWE_EBADPARAM;
//        TODO: temp fix, resolve this by using TLV control
        newCtl.count = limit_count;
    }
    // Check for uniqueness
    for (const auto& ctl : all_mixer_ctrls_) {
        if (ctl.id.name == newCtl.id.name || ctl.id.ctl_id == newCtl.id.ctl_id) {
            AWE_LOGI("The mixer control has been added. ctl_id_name %s, new name %s, ctl_id_id %d, new id %d", ctl.id.name.c_str(), newCtl.id.name.c_str(), ctl.id.ctl_id, newCtl.id.ctl_id);
            return AWE_EOK;
        }
    }
    all_mixer_ctrls_.push_back(newCtl);
    AWE_LOGI("Mixer control : %s has been successfully added, mixer control size = %d", newCtl.id.name.c_str(), all_mixer_ctrls_.size());
    return AWE_EOK;
}

uint32_t AweSoundCardInfoUtil::AddAweEventCtl(const awe_event_t *awe_event)
{
    uint32_t limit_count = 0;
    if (awe_event == nullptr) {
        AWE_LOGE("Null event pointer provided.");
        return AWE_EBADPARAM;
    }
    // Creating a new SoundCardMixerCtl object
    SoundCardMixerCtl newCtl;
    newCtl.id.name = std::string(awe_event->event_name, strnlen(awe_event->event_name, AWE_MAX_CONTROL_NAME_LENGTH));
    newCtl.id.ctl_id = all_mixer_ctrls_.size();
#if !defined(__QNX__)
    newCtl.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    newCtl.awe_ctl_type = AWE_CONTROL_TYPE_BYTE;
    newCtl.type = SNDRV_CTL_ELEM_TYPE_BYTES;
    newCtl.ctrl_mode = EVENT_CONTROL;
    newCtl.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE;
    newCtl.count = getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE);
#else
    newCtl.id.iface = VIRTIO_CTL_ID_IFACE_MIXER;
    newCtl.awe_ctl_type = AWE_CONTROL_TYPE_BYTE;
    newCtl.type = VIRTIO_MIXER_CTL_TYPE_MASK_BYTES;
    newCtl.ctrl_mode = EVENT_CONTROL;
    newCtl.access = VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE;
    newCtl.count = getSndCtlElemValueCount(AWE_CONTROL_TYPE_BYTE, VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE);
#endif

    all_mixer_ctrls_.push_back(newCtl);
    AWE_LOGI("Event control : %s has been successfully added, mixer control size = %d", newCtl.id.name.c_str(), all_mixer_ctrls_.size());
    return AWE_EOK;
}
