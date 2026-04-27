/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <typeinfo>
#include "XMLParser.h"

// Maps are used to aid in ctl extn XML parsing. When a tag is encountered, it is
// referenced against these maps to see if the tag (as a string) is valid and
// whether its text content should be stored.
ExtnTagMap const XMLParser::extn_tag_map_ = {{"fe_common_extns", kAllFrontends},
                            {"fe_tx_extns", kTXFrontends},
                            {"fe_rx_extns", kRXFrontends},
                            {"be_common_extns", kAllBackends},
                            {"be_tx_extns", kTXBackends}, // unused
                            {"be_rx_extns", kRXBackends}}; // unused

ExtnIFaceMap const XMLParser::extn_iface_map_ =
    {{"SNDRV_CTL_ELEM_IFACE_CARD", SNDRV_CTL_ELEM_IFACE_CARD},
     {"SNDRV_CTL_ELEM_IFACE_HWDEP", SNDRV_CTL_ELEM_IFACE_HWDEP},
     {"SNDRV_CTL_ELEM_IFACE_MIXER", SNDRV_CTL_ELEM_IFACE_MIXER},
     {"SNDRV_CTL_ELEM_IFACE_PCM", SNDRV_CTL_ELEM_IFACE_PCM},
     {"SNDRV_CTL_ELEM_IFACE_RAWMIDI", SNDRV_CTL_ELEM_IFACE_RAWMIDI},
     {"SNDRV_CTL_ELEM_IFACE_TIMER", SNDRV_CTL_ELEM_IFACE_TIMER},
     {"SNDRV_CTL_ELEM_IFACE_SEQUENCER", SNDRV_CTL_ELEM_IFACE_SEQUENCER}};

ExtnTypeMap const XMLParser::extn_type_map_ =
    {{"SNDRV_CTL_ELEM_TYPE_BYTES", SNDRV_CTL_ELEM_TYPE_BYTES},
     {"SNDRV_CTL_ELEM_TYPE_ENUMERATED", SNDRV_CTL_ELEM_TYPE_ENUMERATED},
     {"SNDRV_CTL_ELEM_TYPE_INTEGER", SNDRV_CTL_ELEM_TYPE_INTEGER}};

ExtnAccessMap const XMLParser::extn_access_map_ =
    {{"SNDRV_CTL_ELEM_ACCESS_READWRITE", SNDRV_CTL_ELEM_ACCESS_READWRITE},
     {"SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE", SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE}};

ExtnEnumMap const XMLParser::extn_enum_map_ =
    {{"none", kNone},
     {"all_frontends", kAllFrontends},
     {"tx_frontends", kTXFrontends},
     {"rx_frontends", kRXFrontends},
     {"all_backends", kAllBackends},
     {"tx_backends", kTXBackends},
     {"rx_backends", kRXBackends}};

/* Parses three XMLs containing the frontend, backend, and ctl extn info.
   The frontends XML file name is determined by the gvm_id.
   The backends and ctl extns file name is common for all GVMs.
*/
uint32_t XMLParser::ParseAllXMLs(FrontendVectors &frontends, BackendVectors &backends,
                             struct MixerCtlExtns &ctl_extns, const int gvm_id) {
    uint32_t rc = SCIU_EOK;

    XMLData parsed_xml;
    std::string frontends_xml = kGVMIdtoFrontEndXML[gvm_id];

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Parsing frontends XML");
    rc = ParseOneXML(frontends_xml, parsed_xml, FEStartTag, FEEndTag);
    if (rc) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "failed to parse frontends XML, rc = ", rc);
        return rc;
    }

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Parsing backends XML");
    rc = ParseOneXML(BACKENDS_XML, parsed_xml, BEStartTag, BEEndTag);
    if (rc) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "failed to parse backends XML, rc = ", rc);
        return rc;
    }

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Parsing ctlextns XML");
    rc = ParseOneXML(CTL_EXTNS_XML, parsed_xml, ExtnStartTag, ExtnEndTag);
    if (rc) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "failed to parse ctl extns XML, rc = ", rc);
        return rc;
    }

    // The frontends and ctl extns are directly stored into their function params
    // The backends data needs to be converted from a map to the BackendVector form
    frontends = parsed_xml.frontends;
    backends = ConvertBEData(parsed_xml.backends);
    ctl_extns = parsed_xml.ctl_extns;

    return rc;
}

/* Parses a single XML file and stores the parsed data in the parsed_contents param.
   StartTag and EndTag are XML-specific functions.
   parsed_contents holds all of the parsed data for all XMLs.
*/
template <typename ST, typename ET>
uint32_t XMLParser::ParseOneXML(const std::string xml_file, XMLData &parsed_contents,
                            ST StartTag, ET EndTag) {
    uint32_t rc = SCIU_EOK;
    FILE *file = NULL;
    XML_Parser parser;
    int BUFF_SIZE = 1024;

    file = fopen(xml_file.c_str(), "r");

    if (!file) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "failed to parse XML %s", xml_file);
        return SCIU_EBADPARAM;
    }

    parser = XML_ParserCreate(NULL);
    if (!parser) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "parser creation unsuccessful");
        rc = SCIU_EBADPARAM;
        goto err_close_file;
    }

    // Handler set up for contents and start/end tag functions
    XML_SetUserData(parser, &parsed_contents);
    XML_SetElementHandler(parser, StartTag, EndTag);
    XML_SetCharacterDataHandler(parser, TextHandler);

    for (;;) {
        int bytes_read;
        void *buff = XML_GetBuffer(parser, BUFF_SIZE);
        if (buff == NULL) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Unable to get XML buffer");
            rc = SCIU_EFAILED;
            goto err_free_parser;
        }

        bytes_read = fread(buff, 1, BUFF_SIZE, file);
        if (bytes_read < 0) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to read from buffer");
            rc = SCIU_EFAILED;
            goto err_free_parser;
        }

        if (!XML_ParseBuffer(parser, bytes_read, bytes_read == 0)) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Buffer parsing failed");
            rc = SCIU_EFAILED;
            goto err_free_parser;
        }

        if (bytes_read == 0)
            break;
    }
err_free_parser:
    XML_ParserFree(parser);
err_close_file:
    fclose(file);
    return rc;
}

void XMLParser::ClearDataBuf(struct XMLData *xml_data) {
    // Reset the buffer for the next text section

    if (xml_data == nullptr) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Invalid xml data");
        return;
    }

    xml_data->offset = 0;
    memset(xml_data->data_buf, 0, sizeof xml_data->data_buf);
}

void XMLParser::TextHandler(void *userdata, const XML_Char *text, int len) {
    XMLData *xml_data = (struct XMLData *)userdata;
    /* From expat docs:
    The string your handler receives is NOT zero terminated.
    You have to use the length argument to deal with the end of the string.
    A single block of contiguous text free of markup may still result in a
    sequence of calls to this handler. In other words, if you're searching for
    a pattern in the text, it may be split across calls to this handler.*/
    if (len + xml_data->offset >= sizeof(xml_data->data_buf) ) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "XML text larger than buffer, not copying");
        return;
    }
    if (len < sizeof(xml_data->data_buf)) {
        // Keep concating the text into the buffer
        memcpy(xml_data->data_buf + xml_data->offset, text, len);
        xml_data->offset += len;
        // Do not zero terminate the buffer here as there's no guarantee that
        // your contiguous text from the XML is done
    }
}

/*********************      Ctl Extn Handlers          ***********************/
void XMLParser::ExtnStartTag(void *userdata, const XML_Char *tag_name, const XML_Char **attr) {
    XMLData *xml_data = (struct XMLData *)userdata;

    ExtnTagMap::const_iterator iter = extn_tag_map_.find(tag_name);
    // Determine if the tag is an extn dir tag (e.g. fe_common_extns)
    if (iter != extn_tag_map_.end()) {
        xml_data->extn_dir_tag = iter->second; // enum representation of tag
    }

    // Reset the buffer for the upcoming text section
    ClearDataBuf(xml_data);
}

uint32_t XMLParser::AddExtnInfo(XMLData *xml_data, const XML_Char *tag_name,
                            std::vector<CtlExtn> &extn_list,
                            std::unordered_set<std::string> &extn_name_set) {
    uint32_t rc = SCIU_EOK;
    CtlExtn extn_node = {};

    // Since we're using an XML schema, we can safely assume that
    // name, type, access, value are always present and arrive in that order.
    // the "name" tag means that this is a new extn to be added
    if (strcmp(tag_name, "name") == 0) {
        // check for duplicates, add to extn_list if none
        std::string extn_name(xml_data->data_buf);
        extn_node.id.name = extn_name;
        std::pair<StrSet::iterator, bool> ret;

        // .insert() is successful if no duplicates exist
        ret = extn_name_set.insert(extn_name);
        // False means the entry already exists
        if (ret.second == false) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "A duplicate entry in the extns xml exists!");
            return SCIU_EBADPARAM;
        }
        else
            extn_list.push_back(extn_node);
    }
    // Any other tag implies we need to update the node at the back of the extn_list
    else if (!extn_list.empty()) {
        extn_node = extn_list.back();
        if (strcmp(tag_name, "iface") == 0)
            extn_node.id.iface = extn_iface_map_.at(xml_data->data_buf);
        else if (strcmp(tag_name, "type") == 0)
            extn_node.type = extn_type_map_.at(xml_data->data_buf);
        else if (strcmp(tag_name, "access") == 0)
            extn_node.access = extn_access_map_.at(xml_data->data_buf);
        else if (strcmp(tag_name, "dai_enum") == 0) {
            DAIGroup enum_dai_type = extn_enum_map_.at(xml_data->data_buf);
            if ((extn_node.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED) &&
                (enum_dai_type != kNone)) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Non-enum extns must use the kNone dai_enum");
                rc = SCIU_EUNSUPPORTED;
            }
            if ((extn_node.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) &&
                (enum_dai_type == kNone)) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Enum extns must use a valid dai_enum value");
                rc = SCIU_EUNSUPPORTED;
            }
            extn_node.enum_dai_type = enum_dai_type;
        }
        // Min, max, and step should be contained within "integer" tag
        else if (strcmp(tag_name, "count") == 0)
            extn_node.count = std::stoi(xml_data->data_buf);
        else if (strcmp(tag_name, "min") == 0)
            extn_node.value.integer.min = std::stoi(xml_data->data_buf);
        else if (strcmp(tag_name, "max") == 0)
            extn_node.value.integer.max = std::stoi(xml_data->data_buf);
        else if (strcmp(tag_name, "step") == 0)
            extn_node.value.integer.step = std::stoi(xml_data->data_buf);

        extn_list.back() = extn_node;
    }

    return rc;
}

void XMLParser::ExtnEndTag(void *userdata, const XML_Char *tag_name) {
    uint32_t rc = SCIU_EOK;
    XMLData *xml_data = (struct XMLData *)userdata;

    // Zero terminate your buffer because it's guaranteed that the text is all processed now
    if (xml_data->offset > 0)
        xml_data->data_buf[xml_data->offset] = '\0';

    switch (xml_data->extn_dir_tag){
        case kAllFrontends:
            rc = AddExtnInfo(xml_data, tag_name,
                        xml_data->ctl_extns.common_frontend_extns,
                        xml_data->ctl_extn_names.common_frontend_extns);
            break;
        case kTXFrontends:
            rc = AddExtnInfo(xml_data, tag_name,
                        xml_data->ctl_extns.tx_frontend_extns,
                        xml_data->ctl_extn_names.tx_frontend_extns);
            break;
        case kRXFrontends:
            rc = AddExtnInfo(xml_data, tag_name,
                        xml_data->ctl_extns.rx_frontend_extns,
                        xml_data->ctl_extn_names.rx_frontend_extns);
            break;
        case kAllBackends:
            rc = AddExtnInfo(xml_data, tag_name,
                        xml_data->ctl_extns.backend_extns,
                        xml_data->ctl_extn_names.backend_extns);
            break;
        default: {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Unsupported extn_dir_tag!");
            return;
        }

    }
    // Reset the buffer for the next text section since this tag is done now
    ClearDataBuf(xml_data);
    if (rc)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Error adding extn info, ret = %d", rc);
}

/*********************      Frontend Handlers          ***********************/
void XMLParser::FEStartTag(void *userdata, const XML_Char *tag_name, const XML_Char **attr) {
    XMLData *xml_data = (struct XMLData *)userdata;

    if (strcmp(tag_name, "card") == 0)
        // REVIEW
        // consider storing the card name for the future?
        // don't we set the card name to virt-snd, so do we need this card name?
        ;
    // The "pcm-device" or "compress-device" tag indicates that the following XML section will
    // describe a frontend DAI.
    else if ((strcmp(tag_name, "pcm-device") == 0) ||
             (strcmp(tag_name, "compress-device") == 0))
        xml_data->parent_tag = tag_name;

    // Reset the buffer for the upcoming text section
    ClearDataBuf(xml_data);
}

// Can only be called after name and direction have been filled in.
uint32_t XMLParser::AddPCMConstrs(FrontendInfo &fe_node) {
    uint32_t rc = SCIU_EOK;
    if (fe_node.id < 0) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "ID, name, and direction must be filled before PCM constraints!");
        return SCIU_EBADPARAM;
    }

    // Formats taken and translated from agm_pcm_constrs
    fe_node.formats = ((1ULL << VIRTIO_SND_PCM_FMT_S16) |
                       (1ULL << VIRTIO_SND_PCM_FMT_S24) |
                       (1ULL << VIRTIO_SND_PCM_FMT_S24_3) |
                       (1ULL << VIRTIO_SND_PCM_FMT_S32));
    fe_node.rates = ((1ULL << VIRTIO_SND_PCM_RATE_5512) |
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
    fe_node.channels_min = 1;
    fe_node.channels_max = 32;

    // Loopback feature
    if (fe_node.sess_mode == kSessModeNoHost)
        fe_node.features |= (1U << VIRTIO_SND_PCM_F_HOSTLESS);
    return rc;
}

void XMLParser::FEEndTag(void *userdata, const XML_Char *tag_name) {
    uint32_t rc = SCIU_EOK;
    XMLData *xml_data = (XMLData *)userdata;
    std::vector<FrontendInfo> &fe_list = xml_data->frontends.unsorted_dai;
    FrontendInfo fe_node = {};

    // Zero terminate your buffer because it's guaranteed that the text is all processed now
    if (xml_data->offset > 0)
        xml_data->data_buf[xml_data->offset] = '\0';

    if ((strcmp(xml_data->parent_tag.c_str(), "pcm-device") == 0) ||
        (strcmp(xml_data->parent_tag.c_str(), "compress-device") == 0)) {
        // the id tag indicates a new frontend entry
        if (strcmp(tag_name, "id") == 0) {
            // check for duplicates, add to extn_list if none
            std::pair<std::unordered_set<int>::iterator, bool> ret;
            int fe_id = std::stoi(xml_data->data_buf);
            fe_node.id = fe_id;

            // .insert() is successful if no duplicates exist
            ret = xml_data->frontend_ids.insert(fe_id);

            // False means the entry already exists
            if (ret.second == false) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "A duplicate entry in the frontends xml exists!");
                return;
            }
            else
                fe_list.push_back(fe_node);
        }
        // Any other tag implies we need to update the node at the back of the fe_list
        else {
            fe_node = fe_list.back();
            if (strcmp(tag_name, "name") == 0)
                fe_node.name = xml_data->data_buf;
            // Only mark the direction if the data_buf contains a 1
            else if ((strcmp(tag_name, "playback") == 0) && std::stoi(xml_data->data_buf))
                fe_node.direction = SNDRV_PCM_STREAM_PLAYBACK;
            else if (strcmp(tag_name, "capture") == 0 && std::stoi(xml_data->data_buf))
                fe_node.direction = SNDRV_PCM_STREAM_CAPTURE;
            else if (strcmp(tag_name, "session_mode") == 0) {
                fe_node.sess_mode = static_cast<SessModeType>(std::stoi(xml_data->data_buf));
            }
            // At the pcm-device or compress-device end tag, reset the parent tag and fill in
            // the common pcm constraints since there are no more elements to process
            if ((strcmp(tag_name, "pcm-device") == 0) ||
                (strcmp(tag_name, "compress-device") == 0)) {
                xml_data->parent_tag.clear();
                rc = AddPCMConstrs(fe_node);
                if (rc) {
                    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to add PCM constraints, ret = ", rc);
                    return;
                }
            }
            fe_list.back() = fe_node;
        }
    }

    // Reset the buffer for the next text section since this tag is done now
    ClearDataBuf(xml_data);
}

/*********************      Backend Handlers          ***********************/
void XMLParser::BEStartTag(void *userdata, const XML_Char *tag_name, const XML_Char **attr) {
    XMLData *xml_data = (struct XMLData *)userdata;

    // Store the direction to add to the backend node info later
    if ((strcmp(tag_name, "in-device") == 0) ||
        (strcmp(tag_name, "out-device") == 0)) {
        xml_data->parent_tag = tag_name;
    }

    // Reset the buffer for the upcoming text section
    ClearDataBuf(xml_data);
}

void XMLParser::BEEndTag(void *userdata, const XML_Char *tag_name) {
    XMLData *xml_data = (XMLData *)userdata;

    // Zero terminate your buffer because it's guaranteed that the text is all processed now
    if (xml_data->offset > 0)
        xml_data->data_buf[xml_data->offset] = '\0';

    if (strcmp(tag_name, "back_end_name") == 0) {
        std::string be_name(xml_data->data_buf);
        if (!be_name.empty()) {
            std::string current_dir(xml_data->parent_tag);
            std::pair<StrToStrMap::iterator,bool> ret;
            std::pair<std::string,std::string> be_pair(be_name, current_dir);

            // .insert() is successful if no duplicates exist
            ret = xml_data->backends.insert(be_pair);

            // False means the entry already exists
            // The current direction must match the direction found in the map
            if ((ret.second == false) && (current_dir != ret.first->second)) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "A duplicate entry in the resourcemanager xml changes direction!");
                return;
            }
        }
    }
    if ((strcmp(tag_name, "in-device") == 0) ||
        (strcmp(tag_name, "out-device") == 0))
        xml_data->parent_tag.clear();

    // Reset the buffer for the next text section since this tag is done now
    ClearDataBuf(xml_data);
}

/* Returns a BackendVectors representation of all of the backends parsed,
   from the backends map (mapping backend string name to direction string).
*/
BackendVectors XMLParser::ConvertBEData(StrToStrMap backend_map) {
    BackendVectors backends_v;
    for (auto const& be_iter : backend_map) {
        BackendInfo be_info;
        be_info.name = be_iter.first;
        if (be_iter.second == "in-device")
            be_info.direction = SNDRV_PCM_STREAM_CAPTURE;
        else
            be_info.direction = SNDRV_PCM_STREAM_PLAYBACK;

        backends_v.unsorted_dai.push_back(be_info);
    }
    return backends_v;
}
