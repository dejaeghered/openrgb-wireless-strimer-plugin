/*---------------------------------------------------------*\
| OpenRGBWirelessStrimerPlugin.cpp                          |
|                                                           |
|   OpenRGB plugin for Lian Li Strimer Wireless cables      |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "OpenRGBWirelessStrimerPlugin.h"
#include "WirelessStrimerManager.h"
#include "WirelessStrimerWidget.h"

OpenRGBWirelessStrimerPlugin::OpenRGBWirelessStrimerPlugin()
{
}

OpenRGBWirelessStrimerPlugin::~OpenRGBWirelessStrimerPlugin()
{
    Unload();
}

OpenRGBPluginInfo OpenRGBWirelessStrimerPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;

    info.Name           = "Lian Li Wireless Strimer";
    info.Description    = "Direct per-LED control of Lian Li Strimer Wireless cables through the SL V3 wireless dongle";
    info.Version        = PLUGIN_VERSION_STRING;
    info.Commit         = "";
    info.URL            = "https://github.com/dejaeghered/openrgb-wireless-strimer-plugin";
    info.Location       = OPENRGB_PLUGIN_LOCATION_TOP;
    info.Label          = "Wireless Strimer";
    info.ProtocolVersion= OPENRGB_PLUGIN_API_VERSION;

    return(info);
}

unsigned int OpenRGBWirelessStrimerPlugin::GetPluginAPIVersion()
{
    return(OPENRGB_PLUGIN_API_VERSION);
}

void OpenRGBWirelessStrimerPlugin::Load(OpenRGBPluginAPIInterface* plugin_api_ptr)
{
    api     = plugin_api_ptr;
    manager = new WirelessStrimerManager(api);
    manager->Start();
}

QWidget* OpenRGBWirelessStrimerPlugin::GetWidget()
{
    if(manager == nullptr)
    {
        return(nullptr);
    }

    return(new WirelessStrimerWidget(manager));
}

QMenu* OpenRGBWirelessStrimerPlugin::GetTrayMenu()
{
    return(nullptr);
}

void OpenRGBWirelessStrimerPlugin::Unload()
{
    if(manager != nullptr)
    {
        manager->Stop();
        delete manager;
        manager = nullptr;
    }
}

void OpenRGBWirelessStrimerPlugin::OnProfileAboutToLoad()
{
}

void OpenRGBWirelessStrimerPlugin::OnProfileLoad(nlohmann::json profile_data)
{
    if(manager != nullptr)
    {
        manager->LoadProfileData(profile_data);
    }
}

nlohmann::json OpenRGBWirelessStrimerPlugin::OnProfileSave()
{
    if(manager == nullptr)
    {
        return(nlohmann::json());
    }

    return(manager->GetProfileData());
}

unsigned char* OpenRGBWirelessStrimerPlugin::OnSDKCommand(unsigned int /*pkt_id*/, unsigned char* /*pkt_data*/, unsigned int* pkt_size)
{
    *pkt_size = 0;
    return(nullptr);
}

void OpenRGBWirelessStrimerPlugin::ProfileManagerUpdated(unsigned int /*update_reason*/)
{
}

void OpenRGBWirelessStrimerPlugin::ResourceManagerUpdated(unsigned int /*update_reason*/)
{
}

void OpenRGBWirelessStrimerPlugin::SettingsManagerUpdated(unsigned int /*update_reason*/)
{
}
