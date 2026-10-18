/*---------------------------------------------------------*\
| WirelessStrimerManager.h                                  |
|                                                           |
|   Owns the wireless dongle pair, discovers Strimers and   |
|   streams colors to them from a worker thread             |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "OpenRGBPluginInterface.h"
#include "LianLiWirelessProtocol.h"
#include "WirelessStrimerController.h"

struct StrimerStatus
{
    std::string     mac;
    std::string     model;
    std::string     display_name;
    unsigned int    device_type;
    unsigned int    led_count;
    unsigned int    lanes;
    unsigned int    channel;
    unsigned int    rx_type;
    StrimerConfig   config;
    bool            registered;
    bool            confirmed;      /* device reports our last upload   */
    bool            seen;           /* present in the latest device list*/
    unsigned int    uploads;
    unsigned int    resends;
};

struct ManagerStatus
{
    bool                        dongle_open;
    std::string                 message;
    std::string                 master_mac;
    unsigned int                max_fps;
    bool                        clock_sync;
    std::vector<StrimerStatus>  strimers;
    std::vector<std::string>    other_devices;
};

class WirelessStrimerManager
{
public:
    WirelessStrimerManager(OpenRGBPluginAPIInterface* api);
    ~WirelessStrimerManager();

    void            Start();
    void            Stop();

    ManagerStatus   GetStatus();

    void            SetStrimerConfig(const std::string& mac, const StrimerConfig& config);
    void            Identify(const std::string& mac);
    void            Rescan();
    void            SetMaxFps(unsigned int fps);
    void            SetClockSync(bool enabled);

    /*-----------------------------------------------------*\
    | Profile support, called from the plugin's             |
    | OnProfileSave / OnProfileLoad                         |
    \*-----------------------------------------------------*/
    nlohmann::json  GetProfileData();
    void            LoadProfileData(const nlohmann::json& data);

private:
    struct StrimerEntry
    {
        std::unique_ptr<WirelessStrimerController>  controller;
        StrimerConfig                               config;
        bool                                        config_pending  = false;
        bool                                        seen            = false;
        uint32_t                                    last_effect     = 0;
        uint64_t                                    last_upload_ms  = 0;
        bool                                        confirmed       = false;
        unsigned int                                uploads         = 0;
        unsigned int                                resends         = 0;
        unsigned int                                retry_count     = 0;
        uint64_t                                    identify_until  = 0;
        bool                                        identify_sent   = false;
        bool                                        force_upload    = false;
    };

    OpenRGBPluginAPIInterface*              api;
    LianLiWirelessDongle                    dongle;

    std::thread                             worker;
    std::atomic<bool>                       running;
    std::mutex                              state_mutex;
    std::condition_variable                 wake_cv;
    bool                                    wake_flag;

    std::map<std::string, StrimerEntry>     strimers;
    std::map<std::string, nlohmann::json>   pending_profile;    /* profile state for Strimers not yet registered */
    std::vector<std::string>                other_devices;
    LianLiMac                               master_mac;
    uint8_t                                 master_channel;
    bool                                    have_master;
    std::string                             message;
    bool                                    dongle_open;
    bool                                    rescan_requested;

    unsigned int                            max_fps;
    bool                                    clock_sync;

    void            Wake();
    void            WorkerThread();
    bool            PollDevices();
    bool            ServiceUploads(uint64_t now);
    void            ApplyPendingConfigs();

    void            LoadSettings();
    void            SaveSettings();
    StrimerConfig   GetSavedConfig(const std::string& mac);
};
