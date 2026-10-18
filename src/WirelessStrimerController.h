/*---------------------------------------------------------*\
| WirelessStrimerController.h                               |
|                                                           |
|   One wireless Strimer exposed to OpenRGB as a virtual    |
|   RGBController                                           |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "OpenRGBPluginInterface.h"
#include "LianLiWirelessProtocol.h"

struct StrimerConfig
{
    std::string     name;           /* empty = default model name   */
    bool            enabled = true;
    bool            flip_x  = false;/* mirror along the cable       */
    bool            flip_y  = false;/* mirror across the lanes      */
};

class WirelessStrimerController
{
public:
    WirelessStrimerController(OpenRGBPluginAPIInterface* api, const LianLiWirelessRecord& record, const StrimerConfig& config, std::function<void()> wake);
    ~WirelessStrimerController();

    /*-----------------------------------------------------*\
    | Must be called from a non-UI thread                   |
    \*-----------------------------------------------------*/
    void                    Register();
    void                    Unregister();
    void                    ApplyConfig(const StrimerConfig& config);

    bool                    IsRegistered();
    std::string             GetDisplayName();
    unsigned int            GetLEDCount();

    /*-----------------------------------------------------*\
    | Returns true and copies the pending frame if the      |
    | colors changed since the last call                    |
    \*-----------------------------------------------------*/
    bool                    TakeFrame(std::vector<uint8_t>& rgb);
    void                    GetFrame(std::vector<uint8_t>& rgb);
    void                    MarkDirty();

    /*-----------------------------------------------------*\
    | Profile support: brightness and per-LED colors as     |
    | stored in the plugin section of an OpenRGB profile    |
    \*-----------------------------------------------------*/
    nlohmann::json          GetProfileState();
    bool                    ApplyProfileState(const nlohmann::json& state);

    LianLiWirelessRecord    record;

private:
    OpenRGBPluginAPIInterface*  api;
    RGBControllerInterface*     controller;
    StrimerConfig               config;
    StrimerGeometry             geometry;
    std::function<void()>       wake;

    std::mutex                  controller_mutex;   /* guards controller against callbacks */
    std::mutex                  frame_mutex;
    std::vector<uint8_t>        frame;
    bool                        dirty;

    void                        BuildSetup(RGBController_Setup& setup, unsigned int brightness);
    void                        CaptureColors();

    static void                 OnUpdateLEDs(void* self);
    static void                 OnUpdateZoneLEDs(void* self, int zone);
    static void                 OnUpdateSingleLED(void* self, int led);
    static void                 OnUpdateMode(void* self);
};
