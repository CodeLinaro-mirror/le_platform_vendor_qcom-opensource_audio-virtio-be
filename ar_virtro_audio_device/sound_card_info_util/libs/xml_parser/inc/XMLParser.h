/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

// XMLParser.h
//
// Resource Manager for XML resources
//
// Example XMLs include:
// - card-defs.xml
// - backends.xml (formerly called XMLParser.xml)
// - ctl_extns.xml

#include <expat.h>
#include "SoundCardInfoTypes.h"

/* Config Files Directory Path */
#ifndef XML_PATH
#define XML_PATH   "/etc/sciu_cfg/"
#endif

#define BACKENDS_XML  XML_PATH "backends.xml"
#define CTL_EXTNS_XML XML_PATH "ctl_extns.xml"

// Add card_defs files for other GVMs to kGVMIdtoFrontEndXML here
const std::vector<std::string> kGVMIdtoFrontEndXML = {XML_PATH "card_defs.xml"};

/*********************         XMLParser Class          ***********************/

class XMLParser {
public:
    static ExtnTagMap const extn_tag_map_;
    static ExtnIFaceMap const extn_iface_map_;
    static ExtnTypeMap const extn_type_map_;
    static ExtnAccessMap const extn_access_map_;
    static ExtnEnumMap const extn_enum_map_;
    static uint32_t ParseAllXMLs(FrontendVectors &frontends,
                             BackendVectors &backends,
                             struct MixerCtlExtns &ctl_extns,
                             const int gvm_id);
protected:

    template <typename ST, typename ET>
    static uint32_t ParseOneXML(const std::string xml_file, XMLData &parsed_contents,
                            ST StartTag, ET EndTag);
    static void ClearDataBuf(struct XMLData *xml_data);
    static void TextHandler(void *userdata, const XML_Char *text, int len);
    // Extn Tag Handlers
    static void ExtnStartTag(void *userdata, const XML_Char *tag_name, const XML_Char **attr);
    static uint32_t AddExtnInfo(XMLData *xml_data, const XML_Char *tag_name,
                            std::vector<CtlExtn> &extn_list,
                            std::unordered_set<std::string> &extn_name_set);
    static void ExtnEndTag(void *userdata, const XML_Char *tag_name);
    // FE Tag Handlers
    static void FEStartTag(void *userdata, const XML_Char *tag_name, const XML_Char **attr);
    static uint32_t AddPCMConstrs(FrontendInfo &fe_node);
    static void FEEndTag(void *userdata, const XML_Char *tag_name);
    // BE Tag Handlers
    static void BEStartTag(void *userdata, const XML_Char *tag_name, const XML_Char **attr);
    static void BEEndTag(void *userdata, const XML_Char *tag_name);
    static BackendVectors ConvertBEData(StrToStrMap backends);
};
