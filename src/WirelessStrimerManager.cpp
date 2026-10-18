/*---------------------------------------------------------*\
| WirelessStrimerManager.cpp                                |
|                                                           |
|   Owns the wireless dongle pair, discovers Strimers and   |
|   streams colors to them from a worker thread             |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <algorithm>
#include <chrono>
#include "WirelessStrimerManager.h"

#define SETTINGS_KEY                "LianLiWirelessStrimer"
#define LOG_TAG                     "[LianLiWirelessStrimer] "

#define REOPEN_INTERVAL_MS          3000
#define POLL_INTERVAL_MS            1000
#define CLOCK_SYNC_INTERVAL_MS      1000
#define CONFIRM_TIMEOUT_MS          1500
#define MAX_RESENDS                 5
#define IDENTIFY_DURATION_MS        3000

#define DEFAULT_MAX_FPS             15
#define MIN_FPS                     1
#define MAX_FPS                     30

/*---------------------------------------------------------*\n| Log levels, matching OpenRGB's LogManager                 |
\*---------------------------------------------------------*/
enum
{
    PLUGIN_LL_WARNING   = 2,
    PLUGIN_LL_INFO      = 3,
};

#define PLUGIN_LOG(level, ...)      api->LogEntry(__FILE__, __LINE__, level, LOG_TAG __VA_ARGS__)

using json = nlohmann::json;

static uint64_t NowMs()
{
    return((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

WirelessStrimerManager::WirelessStrimerManager(OpenRGBPluginAPIInterface* api_ptr)
{
    api                 = api_ptr;
    running             = false;
    wake_flag           = false;
    master_mac.fill(0);
    master_channel      = 8;
    have_master         = false;
    dongle_open         = false;
    rescan_requested    = false;
    max_fps             = DEFAULT_MAX_FPS;
    clock_sync          = true;
    message             = "Starting";

    LoadSettings();
}

WirelessStrimerManager::~WirelessStrimerManager()
{
    Stop();
}

void WirelessStrimerManager::Start()
{
    if(running)
    {
        return;
    }

    running = true;
    worker  = std::thread(&WirelessStrimerManager::WorkerThread, this);
}

void WirelessStrimerManager::Stop()
{
    if(running)
    {
        running = false;
        Wake();
    }

    if(worker.joinable())
    {
        worker.join();
    }

    /*-----------------------------------------------------*\
    | Unregister controllers. Leave the Strimers showing    |
    | the last uploaded colors; they keep it on their own.  |
    \*-----------------------------------------------------*/
    std::map<std::string, StrimerEntry> old;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        old.swap(strimers);
    }
    old.clear();

    dongle.Close();
}

void WirelessStrimerManager::Wake()
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        wake_flag = true;
    }
    wake_cv.notify_all();
}

/*---------------------------------------------------------*\
| Settings                                                  |
\*---------------------------------------------------------*/
void WirelessStrimerManager::LoadSettings()
{
    json settings = api->GetSettings(SETTINGS_KEY);

    if(settings.is_object())
    {
        if(settings.contains("max_fps") && settings["max_fps"].is_number_unsigned())
        {
            max_fps = std::clamp<unsigned int>(settings["max_fps"].get<unsigned int>(), MIN_FPS, MAX_FPS);
        }
        if(settings.contains("clock_sync") && settings["clock_sync"].is_boolean())
        {
            clock_sync = settings["clock_sync"].get<bool>();
        }
    }
}

StrimerConfig WirelessStrimerManager::GetSavedConfig(const std::string& mac)
{
    StrimerConfig config;
    json          settings = api->GetSettings(SETTINGS_KEY);

    if(settings.is_object() && settings.contains("devices") && settings["devices"].contains(mac))
    {
        const json& dev = settings["devices"][mac];

        config.name     = dev.value("name",    std::string());
        config.enabled  = dev.value("enabled", true);
        config.flip_x   = dev.value("flip_x",  false);
        config.flip_y   = dev.value("flip_y",  false);
    }

    return(config);
}

void WirelessStrimerManager::SaveSettings()
{
    json settings = api->GetSettings(SETTINGS_KEY);

    if(!settings.is_object())
    {
        settings = json::object();
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex);

        settings["max_fps"]    = max_fps;
        settings["clock_sync"] = clock_sync;

        for(const auto& [mac, entry] : strimers)
        {
            json dev;
            dev["name"]    = entry.config.name;
            dev["enabled"] = entry.config.enabled;
            dev["flip_x"]  = entry.config.flip_x;
            dev["flip_y"]  = entry.config.flip_y;
            settings["devices"][mac] = dev;
        }
    }

    api->SetSettings(SETTINGS_KEY, settings);
    api->SaveSettings();
}

/*---------------------------------------------------------*\
| UI-facing API                                             |
\*---------------------------------------------------------*/
ManagerStatus WirelessStrimerManager::GetStatus()
{
    std::lock_guard<std::mutex> lock(state_mutex);

    ManagerStatus status;
    status.dongle_open   = dongle_open;
    status.message       = message;
    status.master_mac    = have_master ? LianLiMacToString(master_mac) : "";
    status.max_fps       = max_fps;
    status.clock_sync    = clock_sync;
    status.other_devices = other_devices;

    for(const auto& [mac, entry] : strimers)
    {
        StrimerGeometry geometry = GetStrimerGeometry(entry.controller->record.device_type);

        StrimerStatus s;
        s.mac           = mac;
        s.model         = std::string(geometry.model) + (geometry.verified ? "" : " (untested)");
        s.display_name  = entry.config.name.empty() ? geometry.model : entry.config.name;
        s.device_type   = entry.controller->record.device_type;
        s.led_count     = geometry.lanes * geometry.leds_per_lane;
        s.lanes         = geometry.lanes;
        s.channel       = entry.controller->record.channel;
        s.rx_type       = entry.controller->record.rx_type;
        s.config        = entry.config;
        s.registered    = entry.config.enabled;
        s.confirmed     = entry.confirmed;
        s.seen          = entry.seen;
        s.uploads       = entry.uploads;
        s.resends       = entry.resends;
        status.strimers.push_back(s);
    }

    return(status);
}

void WirelessStrimerManager::SetStrimerConfig(const std::string& mac, const StrimerConfig& config)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);

        auto it = strimers.find(mac);
        if(it == strimers.end())
        {
            return;
        }

        it->second.config         = config;
        it->second.config_pending = true;
        wake_flag                 = true;
    }

    wake_cv.notify_all();
    SaveSettings();
}

void WirelessStrimerManager::Identify(const std::string& mac)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);

        auto it = strimers.find(mac);
        if(it == strimers.end())
        {
            return;
        }

        it->second.identify_until = NowMs() + IDENTIFY_DURATION_MS;
        it->second.identify_sent  = false;
        wake_flag                 = true;
    }

    wake_cv.notify_all();
}

void WirelessStrimerManager::Rescan()
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        rescan_requested = true;
        wake_flag        = true;
    }

    wake_cv.notify_all();
}

void WirelessStrimerManager::SetMaxFps(unsigned int fps)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        max_fps = std::clamp<unsigned int>(fps, MIN_FPS, MAX_FPS);
    }

    SaveSettings();
}

void WirelessStrimerManager::SetClockSync(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        clock_sync = enabled;
    }

    SaveSettings();
}

/*---------------------------------------------------------*\
| Worker                                                    |
\*---------------------------------------------------------*/
bool WirelessStrimerManager::PollDevices()
{
    std::vector<LianLiWirelessRecord> records;

    if(!dongle.GetDevices(records))
    {
        return(false);
    }

    if(records.empty())
    {
        /*-------------------------------------------------*\
        | The RX occasionally returns nothing; keep the     |
        | previous view rather than dropping devices        |
        \*-------------------------------------------------*/
        return(true);
    }

    /*-----------------------------------------------------*\
    | Master MAC and channel: the most common pairing among |
    | Strimer records (GET_MAC can return junk)             |
    \*-----------------------------------------------------*/
    std::map<std::pair<LianLiMac, uint8_t>, unsigned int> master_votes;

    for(const LianLiWirelessRecord& record : records)
    {
        if(IsStrimerDeviceType(record.device_type) && record.channel >= 1 && record.channel <= 39)
        {
            master_votes[{ record.master_mac, record.channel }]++;
        }
    }

    std::vector<std::string>                        others;
    std::vector<std::pair<std::string, LianLiWirelessRecord>> new_strimers;

    {
        std::lock_guard<std::mutex> lock(state_mutex);

        if(!master_votes.empty())
        {
            auto best = std::max_element(master_votes.begin(), master_votes.end(),
                [](const auto& a, const auto& b) { return(a.second < b.second); });

            if(!have_master || master_mac != best->first.first)
            {
                PLUGIN_LOG(PLUGIN_LL_INFO, "Master dongle %s, channel %u", LianLiMacToString(best->first.first).c_str(), best->first.second);
            }

            master_mac     = best->first.first;
            master_channel = best->first.second;
            have_master    = true;
        }

        for(auto& [mac, entry] : strimers)
        {
            entry.seen = false;
        }

        for(const LianLiWirelessRecord& record : records)
        {
            std::string mac = LianLiMacToString(record.mac);

            if(!IsStrimerDeviceType(record.device_type) || (have_master && record.master_mac != master_mac))
            {
                others.push_back(mac + " (type " + std::to_string(record.device_type) + ")");
                continue;
            }

            auto it = strimers.find(mac);

            if(it == strimers.end())
            {
                new_strimers.push_back({ mac, record });
                continue;
            }

            StrimerEntry& entry = it->second;

            entry.controller->record = record;
            entry.seen               = true;

            /*---------------------------------------------*\
            | Delivery check: the device reports the effect |
            | index of the last upload it applied           |
            \*---------------------------------------------*/
            if(entry.last_effect != 0)
            {
                if(record.effect_index == entry.last_effect)
                {
                    entry.confirmed   = true;
                    entry.retry_count = 0;
                }
                else if(NowMs() - entry.last_upload_ms > CONFIRM_TIMEOUT_MS)
                {
                    entry.confirmed = false;

                    if(entry.retry_count < MAX_RESENDS && entry.identify_until == 0)
                    {
                        entry.retry_count++;
                        entry.resends++;
                        entry.force_upload = true;
                    }
                }
            }
        }

        other_devices = others;
    }

    /*-----------------------------------------------------*\
    | Create controllers for newly discovered Strimers.     |
    | Registration happens outside state_mutex since it     |
    | calls back into OpenRGB.                              |
    \*-----------------------------------------------------*/
    for(auto& [mac, record] : new_strimers)
    {
        StrimerConfig   config     = GetSavedConfig(mac);
        StrimerGeometry geometry   = GetStrimerGeometry(record.device_type);

        PLUGIN_LOG(PLUGIN_LL_INFO, "Found %s %s (device type %u, %u LEDs, channel %u, rx %u)",
                   geometry.model, mac.c_str(), record.device_type, geometry.lanes * geometry.leds_per_lane, record.channel, record.rx_type);

        if(!geometry.verified)
        {
            PLUGIN_LOG(PLUGIN_LL_WARNING, "Device type %u has not been tested; if the cable does not light up, its LED count is likely wrong. Please report it.", record.device_type);
        }

        auto controller = std::make_unique<WirelessStrimerController>(api, record, config, [this]() { Wake(); });

        if(config.enabled)
        {
            controller->Register();
        }

        WirelessStrimerController* registered = controller->IsRegistered() ? controller.get() : nullptr;
        nlohmann::json             profile_state;

        {
            std::lock_guard<std::mutex> lock(state_mutex);

            StrimerEntry& entry = strimers[mac];
            entry.controller    = std::move(controller);
            entry.config        = config;
            entry.seen          = true;

            auto pending = pending_profile.find(mac);
            if(registered != nullptr && pending != pending_profile.end())
            {
                profile_state = pending->second;
                pending_profile.erase(pending);
            }
        }

        /*-------------------------------------------------*\
        | A profile loaded before this Strimer was found    |
        | (e.g. the service's startup profile) applies now  |
        \*-------------------------------------------------*/
        if(!profile_state.is_null() && registered->ApplyProfileState(profile_state))
        {
            PLUGIN_LOG(PLUGIN_LL_INFO, "Applied pending profile colors to %s", mac.c_str());
        }
    }

    return(true);
}

nlohmann::json WirelessStrimerManager::GetProfileData()
{
    nlohmann::json data;
    data["devices"] = nlohmann::json::object();

    std::lock_guard<std::mutex> lock(state_mutex);

    for(auto& [mac, entry] : strimers)
    {
        nlohmann::json state = entry.controller->GetProfileState();

        if(!state.is_null())
        {
            data["devices"][mac] = state;
        }
    }

    return(data);
}

void WirelessStrimerManager::LoadProfileData(const nlohmann::json& data)
{
    if(!data.is_object() || !data.contains("devices") || !data["devices"].is_object())
    {
        return;
    }

    std::vector<std::pair<WirelessStrimerController*, nlohmann::json>> apply_now;

    {
        std::lock_guard<std::mutex> lock(state_mutex);

        pending_profile.clear();

        for(auto& [mac, state] : data["devices"].items())
        {
            auto it = strimers.find(mac);

            if(it != strimers.end() && it->second.controller->IsRegistered())
            {
                apply_now.push_back({ it->second.controller.get(), state });
            }
            else
            {
                pending_profile[mac] = state;
            }
        }
    }

    for(auto& [controller, state] : apply_now)
    {
        controller->ApplyProfileState(state);
    }

    PLUGIN_LOG(PLUGIN_LL_INFO, "Profile loaded: %u Strimer(s) applied, %u pending discovery", (unsigned int)apply_now.size(), (unsigned int)pending_profile.size());
}

void WirelessStrimerManager::ApplyPendingConfigs()
{
    std::vector<std::pair<WirelessStrimerController*, StrimerConfig>> pending;

    {
        std::lock_guard<std::mutex> lock(state_mutex);

        for(auto& [mac, entry] : strimers)
        {
            if(entry.config_pending)
            {
                entry.config_pending = false;
                pending.push_back({ entry.controller.get(), entry.config });
            }
        }
    }

    for(auto& [controller, config] : pending)
    {
        if(config.enabled && !controller->IsRegistered())
        {
            controller->ApplyConfig(config);
            controller->Register();
        }
        else if(!config.enabled && controller->IsRegistered())
        {
            controller->Unregister();
            controller->ApplyConfig(config);
        }
        else
        {
            controller->ApplyConfig(config);
        }
    }
}

bool WirelessStrimerManager::ServiceUploads(uint64_t now)
{
    struct Job
    {
        std::string                 mac;
        WirelessStrimerController*  controller;
        LianLiWirelessRecord        record;
        std::vector<uint8_t>        rgb;
    };

    std::vector<Job> jobs;
    LianLiMac        master;

    {
        std::lock_guard<std::mutex> lock(state_mutex);

        if(!have_master)
        {
            return(true);
        }

        master = master_mac;

        uint64_t min_interval = 1000 / max_fps;

        for(auto& [mac, entry] : strimers)
        {
            if(!entry.config.enabled || !entry.seen)
            {
                continue;
            }

            Job job;
            job.mac        = mac;
            job.controller = entry.controller.get();
            job.record     = entry.controller->record;

            if(entry.identify_until != 0)
            {
                if(now < entry.identify_until)
                {
                    if(!entry.identify_sent)
                    {
                        job.rgb.assign(job.controller->GetLEDCount() * 3, 255);
                        entry.identify_sent = true;
                        jobs.push_back(job);
                    }
                    continue;
                }

                /*-----------------------------------------*\
                | Identify finished: restore the colors     |
                \*-----------------------------------------*/
                entry.identify_until = 0;
                entry.force_upload   = true;
            }

            if(now - entry.last_upload_ms < min_interval)
            {
                continue;
            }

            if(entry.controller->TakeFrame(job.rgb) || entry.force_upload)
            {
                if(job.rgb.empty())
                {
                    entry.controller->GetFrame(job.rgb);
                }
                entry.force_upload = false;
                jobs.push_back(job);
            }
        }
    }

    for(Job& job : jobs)
    {
        uint32_t effect = dongle.UploadStaticFrame(job.record, master, job.rgb);

        std::lock_guard<std::mutex> lock(state_mutex);

        auto it = strimers.find(job.mac);
        if(it == strimers.end())
        {
            continue;
        }

        if(effect == 0)
        {
            it->second.force_upload = true;
            return(false);
        }

        it->second.last_effect    = effect;
        it->second.last_upload_ms = NowMs();
        it->second.confirmed      = false;
        it->second.uploads++;
    }

    return(true);
}

void WirelessStrimerManager::WorkerThread()
{
    uint64_t next_open  = 0;
    uint64_t next_poll  = 0;
    uint64_t next_clock = 0;

    while(running)
    {
        uint64_t now = NowMs();

        if(!dongle.IsOpen())
        {
            if(now >= next_open)
            {
                std::string error;

                if(dongle.Open(error))
                {
                    PLUGIN_LOG(PLUGIN_LL_INFO, "Opened wireless TX/RX dongles");

                    std::lock_guard<std::mutex> lock(state_mutex);
                    dongle_open = true;
                    message     = "Connected";
                    next_poll   = 0;

                    for(auto& [mac, entry] : strimers)
                    {
                        entry.force_upload = true;
                    }
                }
                else
                {
                    std::lock_guard<std::mutex> lock(state_mutex);

                    if(message != error)
                    {
                        PLUGIN_LOG(PLUGIN_LL_WARNING, "%s", error.c_str());
                    }
                    dongle_open = false;
                    message     = error;
                    next_open   = now + REOPEN_INTERVAL_MS;
                }
            }

            if(!dongle.IsOpen())
            {
                std::unique_lock<std::mutex> lock(state_mutex);
                wake_cv.wait_for(lock, std::chrono::milliseconds(REOPEN_INTERVAL_MS), [this]() { return(wake_flag || !running); });
                wake_flag = false;
                continue;
            }
        }

        bool ok = true;

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if(rescan_requested)
            {
                rescan_requested = false;
                next_poll        = 0;
            }
        }

        if(ok && now >= next_poll)
        {
            ok        = PollDevices();
            next_poll = NowMs() + POLL_INTERVAL_MS;
        }

        if(ok)
        {
            ApplyPendingConfigs();
        }

        bool send_clock;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            send_clock = clock_sync && have_master;
        }

        if(ok && send_clock && NowMs() >= next_clock)
        {
            LianLiMac master;
            uint8_t   channel;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                master  = master_mac;
                channel = master_channel;
            }
            ok         = dongle.SendClockSync(master, channel);
            next_clock = NowMs() + CLOCK_SYNC_INTERVAL_MS;
        }

        if(ok)
        {
            ok = ServiceUploads(NowMs());
        }

        if(!ok)
        {
            PLUGIN_LOG(PLUGIN_LL_WARNING, "USB error, reopening dongles");
            dongle.Close();

            std::lock_guard<std::mutex> lock(state_mutex);
            dongle_open = false;
            message     = "USB error, reconnecting";
            next_open   = NowMs() + REOPEN_INTERVAL_MS;
            continue;
        }

        /*-------------------------------------------------*\
        | Sleep until the next frame slot, poll, or a wake  |
        \*-------------------------------------------------*/
        std::unique_lock<std::mutex> lock(state_mutex);
        uint64_t frame_ms = 1000 / max_fps;
        wake_cv.wait_for(lock, std::chrono::milliseconds(frame_ms), [this]() { return(wake_flag || !running); });
        wake_flag = false;
    }
}
