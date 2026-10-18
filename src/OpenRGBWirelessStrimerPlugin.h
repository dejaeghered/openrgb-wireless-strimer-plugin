/*---------------------------------------------------------*\
| OpenRGBWirelessStrimerPlugin.h                            |
|                                                           |
|   OpenRGB plugin for Lian Li Strimer Wireless cables      |
|   driven through the SL V3 2.4 GHz TX/RX USB dongles.     |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <QObject>
#include <QString>
#include <QtPlugin>
#include "OpenRGBPluginInterface.h"

class WirelessStrimerManager;

class OpenRGBWirelessStrimerPlugin : public QObject, public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID FILE "OpenRGBWirelessStrimerPlugin.json")
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    OpenRGBWirelessStrimerPlugin();
    ~OpenRGBWirelessStrimerPlugin() override;

    OpenRGBPluginInfo   GetPluginInfo() override;
    unsigned int        GetPluginAPIVersion() override;

    void                Load(OpenRGBPluginAPIInterface* plugin_api_ptr) override;
    QWidget*            GetWidget() override;
    QMenu*              GetTrayMenu() override;
    void                Unload() override;
    void                OnProfileAboutToLoad() override;
    void                OnProfileLoad(nlohmann::json profile_data) override;
    nlohmann::json      OnProfileSave() override;
    unsigned char*      OnSDKCommand(unsigned int pkt_id, unsigned char* pkt_data, unsigned int* pkt_size) override;

    void                ProfileManagerUpdated(unsigned int update_reason) override;
    void                ResourceManagerUpdated(unsigned int update_reason) override;
    void                SettingsManagerUpdated(unsigned int update_reason) override;

private:
    OpenRGBPluginAPIInterface*  api     = nullptr;
    WirelessStrimerManager*     manager = nullptr;
};
