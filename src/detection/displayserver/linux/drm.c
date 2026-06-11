#include "displayserver_linux.h"
#include "common/io.h"
#include "common/edidHelper.h"
#include "common/strutil.h"

#ifdef __linux__
    #include <dirent.h>

static const char* drmParseSysfs(FFDisplayServerResult* result) {
    const char* drmDirPath = "/sys/class/drm/";

    FF_AUTO_CLOSE_DIR DIR* dirp = opendir(drmDirPath);
    if (dirp == NULL) {
        return "opendir(drmDirPath) failed";
    }

    FF_STRBUF_AUTO_DESTROY drmDir = ffStrbufCreateA(64);
    ffStrbufAppendS(&drmDir, drmDirPath);

    uint32_t drmDirLength = drmDir.length;

    struct dirent* entry;
    while ((entry = readdir(dirp)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        ffStrbufAppendS(&drmDir, entry->d_name);
        uint32_t drmDirWithDnameLength = drmDir.length;

        char buf;
        ffStrbufAppendS(&drmDir, "/enabled");
        if (ffReadFileData(drmDir.chars, sizeof(buf), &buf) <= 0 || buf != 'e') {
            /* read failed or enabled != "enabled" */
            ffStrbufSubstrBefore(&drmDir, drmDirWithDnameLength);
            ffStrbufAppendS(&drmDir, "/status");
            buf = 'd';
            ffReadFileData(drmDir.chars, sizeof(buf), &buf);
            if (buf != 'c') {
                /* read failed or status != "connected" */
                ffStrbufSubstrBefore(&drmDir, drmDirLength);
                continue;
            }
        }

        unsigned width = 0, height = 0, physicalWidth = 0, physicalHeight = 0;
        double refreshRate = 0;
        FF_STRBUF_AUTO_DESTROY name = ffStrbufCreate();

        ffStrbufSubstrBefore(&drmDir, drmDirWithDnameLength);
        ffStrbufAppendS(&drmDir, "/edid");

        const char* plainName = entry->d_name;
        if (ffStrStartsWith(plainName, "card")) {
            const char* tmp = strchr(plainName + strlen("card"), '-');
            if (tmp) {
                plainName = tmp + 1;
            }
        }

        uint8_t edidData[512];
        ssize_t edidLength = ffReadFileData(drmDir.chars, ARRAY_SIZE(edidData), edidData);
        if (edidLength <= 0 || edidLength % 128 != 0) {
            edidLength = 0;
            ffStrbufSubstrBefore(&drmDir, drmDirWithDnameLength);
            ffStrbufAppendS(&drmDir, "/modes");

            char modes[32];
            if (ffReadFileData(drmDir.chars, ARRAY_SIZE(modes), modes) >= 3) {
                sscanf(modes, "%ux%u", &width, &height);
                ffStrbufAppendS(&name, plainName);
            }
        } else {
            ffEdidGetName(edidData, &name);
            ffEdidGetPreferredResolutionAndRefreshRate(edidData, &width, &height, &refreshRate);
            ffEdidGetPhysicalSize(edidData, &physicalWidth, &physicalHeight);
        }

        FFDisplayResult* item = ffdsAppendDisplay(
            result,
            width,
            height,
            refreshRate,
            0,
            0,
            0,
            0,
            0,
            &name,
            ffdsGetDisplayType(plainName),
            false,
            0,
            physicalWidth,
            physicalHeight,
            "sysfs-drm");
        if (item && edidLength) {
            item->hdrStatus = ffEdidGetHdrCompatible(edidData, (uint32_t) edidLength) ? FF_DISPLAY_HDR_STATUS_SUPPORTED : FF_DISPLAY_HDR_STATUS_UNSUPPORTED;
            ffEdidGetSerialAndManufactureDate(edidData, &item->serial, &item->manufactureYear, &item->manufactureWeek);
        }

        ffStrbufSubstrBefore(&drmDir, drmDirLength);
    }

    return NULL;
}
#endif

#ifdef FF_HAVE_DRM

    #include <fcntl.h>
    #include <sys/ioctl.h>
    #include <drm.h>
    #include <drm_mode.h>

// https://gitlab.freedesktop.org/mesa/drm/-/blob/main/xf86drmMode.c#L1785
// It's not supported on Ubuntu 20.04
static inline const char* drmType2Name(uint32_t connector_type) {
    /* Keep the strings in sync with the kernel's drm_connector_enum_list in
     * drm_connector.c. */
    switch (connector_type) {
        case DRM_MODE_CONNECTOR_Unknown:
            return "Unknown";
        case DRM_MODE_CONNECTOR_VGA:
            return "VGA";
        case DRM_MODE_CONNECTOR_DVII:
            return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID:
            return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA:
            return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite:
            return "Composite";
        case DRM_MODE_CONNECTOR_SVIDEO:
            return "SVIDEO";
        case DRM_MODE_CONNECTOR_LVDS:
            return "LVDS";
        case DRM_MODE_CONNECTOR_Component:
            return "Component";
        case DRM_MODE_CONNECTOR_9PinDIN:
            return "DIN";
        case DRM_MODE_CONNECTOR_DisplayPort:
            return "DP";
        case DRM_MODE_CONNECTOR_HDMIA:
            return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB:
            return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV:
            return "TV";
        case DRM_MODE_CONNECTOR_eDP:
            return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL:
            return "Virtual";
        case DRM_MODE_CONNECTOR_DSI:
            return "DSI";
        case DRM_MODE_CONNECTOR_DPI:
            return "DPI";
        case DRM_MODE_CONNECTOR_WRITEBACK:
            return "Writeback";
        case 19 /*DRM_MODE_CONNECTOR_SPI*/:
            return "SPI";
        case 20 /*DRM_MODE_CONNECTOR_USB*/:
            return "USB";
        default:
            return "Unsupported";
    }
}

FF_A_UNUSED static const char* drmGetEdidByConnId(uint32_t connId, uint8_t* edidData, ssize_t* edidLength) {
    const char* drmDirPath = "/sys/class/drm/";

    FF_AUTO_CLOSE_DIR DIR* dirp = opendir(drmDirPath);
    if (dirp == NULL) {
        return "opendir(drmDirPath) failed";
    }

    FF_STRBUF_AUTO_DESTROY drmDir = ffStrbufCreateA(64);
    ffStrbufAppendS(&drmDir, drmDirPath);

    uint32_t drmDirLength = drmDir.length;

    struct dirent* entry;
    while ((entry = readdir(dirp)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        ffStrbufAppendS(&drmDir, entry->d_name);
        uint32_t drmDirWithDnameLength = drmDir.length;

        char connectorId[16] = {};

        ffStrbufAppendS(&drmDir, "/connector_id");
        ffReadFileData(drmDir.chars, ARRAY_SIZE(connectorId), connectorId);
        if (strtoul(connectorId, NULL, 10) != connId) {
            ffStrbufSubstrBefore(&drmDir, drmDirLength);
            continue;
        }

        ffStrbufSubstrBefore(&drmDir, drmDirWithDnameLength);
        ffStrbufAppendS(&drmDir, "/edid");
        *edidLength = ffReadFileData(drmDir.chars, (uint32_t) *edidLength, edidData);
        return NULL;
    }

    return "Failed to match connector ID";
}

static const char* drmConnectLibdrm(FFDisplayServerResult* result) {
    const char* drmDirPath = "/dev/dri/";
    FF_AUTO_CLOSE_DIR DIR* dirp = opendir(drmDirPath);
    if (dirp == NULL) {
        return "opendir(/dev/dri/) failed";
    }

    FF_STRBUF_AUTO_DESTROY pathBuf = ffStrbufCreateA(64);
    ffStrbufAppendS(&pathBuf, drmDirPath);
    uint32_t pathLen = pathBuf.length;

    struct dirent* entry;
    while ((entry = readdir(dirp)) != NULL) {
        if (entry->d_name[0] == '.' || !ffStrStartsWith(entry->d_name, "card")) {
            continue;
        }

        ffStrbufAppendS(&pathBuf, entry->d_name);

    #if __linux__
        FF_STRBUF_AUTO_DESTROY powerStatusBuf = ffStrbufCreateA(128);
        ffStrbufSetF(&powerStatusBuf, "/sys/class/drm/%s/device/power/runtime_status", entry->d_name);

        char buffer[8] = "";
        if (ffReadFileData(powerStatusBuf.chars, strlen("suspend"), buffer) > 0 && ffStrStartsWith(buffer, "suspend")) {
            ffStrbufSubstrBefore(&pathBuf, pathLen);
            continue;
        }
    #endif

        FF_AUTO_CLOSE_FD int fd = open(pathBuf.chars, O_RDWR | O_CLOEXEC);
        ffStrbufSubstrBefore(&pathBuf, pathLen);
        if (fd < 0) {
            continue;
        }

        struct drm_mode_card_res res = {0};
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || res.count_connectors <= 0) {
            continue;
        }

        uint32_t* connectors = malloc(res.count_connectors * sizeof(uint32_t));
        if (!connectors) {
            continue;
        }
        struct drm_mode_card_res resFull = {0};
        resFull.connector_id_ptr = (uintptr_t)connectors;
        resFull.count_connectors = res.count_connectors;
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resFull) < 0) {
            free(connectors);
            continue;
        }

        for (int iConn = 0; iConn < resFull.count_connectors; ++iConn) {
            struct drm_mode_get_connector conn = {0};
            conn.connector_id = connectors[iConn];
            if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
                continue;
            }

            if (conn.connection == DRM_MODE_DISCONNECTED) {
                continue;
            }

            uint32_t width = 0, height = 0, refreshRate = 0;
            uint8_t bitDepth = 0;

            if (conn.encoder_id > 0) {
                struct drm_mode_get_encoder enc = {0};
                enc.encoder_id = conn.encoder_id;
                if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) >= 0 && enc.crtc_id > 0) {
                    struct drm_mode_crtc crtc = {0};
                    crtc.crtc_id = enc.crtc_id;
                    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) >= 0) {
                        width = crtc.mode.hdisplay;
                        height = crtc.mode.vdisplay;
                        refreshRate = crtc.mode.vrefresh;

                        if (crtc.fb_id > 0) {
                            struct drm_mode_fb_cmd fb = {0};
                            fb.fb_id = crtc.fb_id;
                            if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb) >= 0) {
                                bitDepth = (uint8_t)(fb.depth / 3);
                            }
                        }
                    }
                }
            }

            uint32_t preferredWidth = 0, preferredHeight = 0, preferredRefreshRate = 0;
            drmModeModeInfo* modes = malloc(conn.count_modes * sizeof(drmModeModeInfo));
            if (modes && conn.count_modes > 0) {
                struct drm_mode_get_connector connModes = {0};
                connModes.connector_id = connectors[iConn];
                connModes.modes_ptr = (uintptr_t)modes;
                connModes.count_modes = conn.count_modes;
                if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connModes) >= 0) {
                    for (int iMode = 0; iMode < connModes.count_modes; ++iMode) {
                        if (modes[iMode].type & DRM_MODE_TYPE_PREFERRED) {
                            preferredWidth = modes[iMode].hdisplay;
                            preferredHeight = modes[iMode].vdisplay;
                            preferredRefreshRate = modes[iMode].vrefresh;
                            break;
                        }
                    }
                }
                free(modes);
            }

            if (width == 0 || height == 0) {
                width = preferredWidth;
                height = preferredHeight;
                refreshRate = preferredRefreshRate;
            }

            FF_STRBUF_AUTO_DESTROY name = ffStrbufCreate();
            uint16_t myear = 0, mweak = 0;
            uint32_t serial = 0;
            FFDisplayHdrStatus hdrStatus = FF_DISPLAY_HDR_STATUS_UNKNOWN;

            uint32_t* props = malloc(conn.count_props * sizeof(uint32_t));
            uint64_t* propValues = malloc(conn.count_props * sizeof(uint64_t));
            if (props && propValues && conn.count_props > 0) {
                struct drm_mode_get_connector connProps = {0};
                connProps.connector_id = connectors[iConn];
                connProps.props_ptr = (uintptr_t)props;
                connProps.prop_values_ptr = (uintptr_t)propValues;
                connProps.count_props = conn.count_props;
                if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connProps) >= 0) {
                    for (int iProp = 0; iProp < connProps.count_props; ++iProp) {
                        struct drm_mode_get_property prop = {0};
                        prop.prop_id = props[iProp];
                        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0) {
                            continue;
                        }

                        uint32_t type = prop.flags & (DRM_MODE_PROP_LEGACY_TYPE | DRM_MODE_PROP_EXTENDED_TYPE);
                        if (type == DRM_MODE_PROP_BLOB && ffStrEquals(prop.name, "EDID")) {
                            uint32_t blobId = (uint32_t)propValues[iProp];
                            if (prop.count_blobs > 0 && prop.blob_ids != NULL) {
                                blobId = prop.blob_ids[0];
                            }

                            struct drm_mode_get_blob blob = {0};
                            blob.blob_id = blobId;
                            if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) >= 0 && blob.length > 0) {
                                void* blobData = malloc(blob.length);
                                if (blobData) {
                                    struct drm_mode_get_blob blobDataReq = {0};
                                    blobDataReq.blob_id = blobId;
                                    blobDataReq.data = (uintptr_t)blobData;
                                    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blobDataReq) >= 0 && blob.length >= 128) {
                                        ffEdidGetName(blobData, &name);
                                        hdrStatus = ffEdidGetHdrCompatible(blobData, blob.length) ? FF_DISPLAY_HDR_STATUS_SUPPORTED : FF_DISPLAY_HDR_STATUS_UNSUPPORTED;
                                        ffEdidGetSerialAndManufactureDate(blobData, &serial, &myear, &mweak);
                                    }
                                    free(blobData);
                                }
                            }
                            break;
                        }
                    }
                }
            }
            free(props);
            free(propValues);

    #if __linux__
            if (name.length == 0) {
                uint8_t edidData[512];
                ssize_t edidLength = 0;
                drmGetEdidByConnId(conn.connector_id, edidData, &edidLength);
                if (edidLength > 0 && edidLength % 128 == 0) {
                    ffEdidGetName(edidData, &name);
                    hdrStatus = ffEdidGetHdrCompatible(edidData, (uint32_t) edidLength) ? FF_DISPLAY_HDR_STATUS_SUPPORTED : FF_DISPLAY_HDR_STATUS_UNSUPPORTED;
                    ffEdidGetSerialAndManufactureDate(edidData, &serial, &myear, &mweak);
                }
            }
    #endif

            if (name.length == 0) {
                const char* connectorTypeName = drmType2Name(conn.connector_type);
                if (connectorTypeName == NULL) {
                    connectorTypeName = "Unknown";
                }
                ffStrbufSetF(&name, "%s-%d", connectorTypeName, iConn + 1);
            }

            FFDisplayResult* item = ffdsAppendDisplay(result,
                width,
                height,
                refreshRate,
                0,
                preferredWidth,
                preferredHeight,
                preferredRefreshRate,
                0,
                &name,
                conn.connector_type == DRM_MODE_CONNECTOR_eDP || conn.connector_type == DRM_MODE_CONNECTOR_LVDS
                    ? FF_DISPLAY_TYPE_BUILTIN
                    : conn.connector_type == DRM_MODE_CONNECTOR_HDMIA || conn.connector_type == DRM_MODE_CONNECTOR_HDMIB || conn.connector_type == DRM_MODE_CONNECTOR_DisplayPort
                    ? FF_DISPLAY_TYPE_EXTERNAL
                    : FF_DISPLAY_TYPE_UNKNOWN,
                false,
                conn.connector_id,
                conn.mm_width,
                conn.mm_height,
                "libdrm");

            if (item) {
                item->hdrStatus = hdrStatus;
                item->serial = serial;
                item->manufactureYear = myear;
                item->manufactureWeek = mweak;
                item->bitDepth = bitDepth;
            }
        }

        free(connectors);
    }

    return NULL;
}

#endif

const char* ffdsConnectDrm(FF_A_UNUSED FFDisplayServerResult* result) {
#ifdef FF_HAVE_DRM
    if (instance.config.general.dsForceDrm != FF_DS_FORCE_DRM_TYPE_SYSFS_ONLY) {
        if (drmConnectLibdrm(result) == NULL) {
            return NULL;
        }
    }
#endif

#ifdef __linux__
    return drmParseSysfs(result);
#endif

    return "fastfetch was compiled without drm support";
}
