/*---------------------------------------------------------*\
| strimer_probe.cpp                                         |
|                                                           |
|   Standalone test for the wireless protocol layer: lists  |
|   paired devices and optionally sets every Strimer to one |
|   color, then verifies the upload was applied.            |
|                                                           |
|   Usage: strimer_probe [RRGGBB]                           |
|   OpenRGB must not be using the plugin at the same time.  |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include "LianLiWirelessProtocol.h"

int main(int argc, char** argv)
{
    std::vector<uint8_t> literal = TinyuzEncodeLiteral({ 0xFF, 0x00, 0x00 });
    const uint8_t expected[] = { 0x01, 0x00, 0x00, 0x00, 0x67, 0xFF, 0x00, 0x00, 0x00, 0x00 };
    if(literal != std::vector<uint8_t>(expected, expected + sizeof(expected)))
    {
        printf("tinyuz self-test FAILED\n");
        return(1);
    }
    printf("tinyuz self-test ok\n");

    LianLiWirelessDongle dongle;
    std::string          error;

    if(!dongle.Open(error))
    {
        printf("Open failed: %s\n", error.c_str());
        return(1);
    }

    std::vector<LianLiWirelessRecord> records;
    if(!dongle.GetDevices(records))
    {
        printf("GetDevices failed\n");
        return(1);
    }

    for(const LianLiWirelessRecord& r : records)
    {
        printf("%s master %s ch %u rx %u type %u effect %08x%s\n", LianLiMacToString(r.mac).c_str(), LianLiMacToString(r.master_mac).c_str(),
               r.channel, r.rx_type, r.device_type, r.effect_index, IsStrimerDeviceType(r.device_type) ? "  [Strimer]" : "");
    }

    if(argc < 2)
    {
        return(0);
    }

    unsigned long color = strtoul(argv[1], nullptr, 16);
    std::map<std::string, uint32_t> sent;

    for(const LianLiWirelessRecord& r : records)
    {
        if(!IsStrimerDeviceType(r.device_type))
        {
            continue;
        }

        StrimerGeometry g = GetStrimerGeometry(r.device_type);
        std::vector<uint8_t> rgb;
        for(unsigned int i = 0; i < g.lanes * g.leds_per_lane; i++)
        {
            rgb.push_back((uint8_t)(color >> 16));
            rgb.push_back((uint8_t)(color >> 8));
            rgb.push_back((uint8_t)(color));
        }

        uint32_t effect = dongle.UploadStaticFrame(r, r.master_mac, rgb);
        printf("upload %s -> %08x\n", LianLiMacToString(r.mac).c_str(), effect);
        sent[LianLiMacToString(r.mac)] = effect;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    dongle.GetDevices(records);
    int failures = 0;
    for(const LianLiWirelessRecord& r : records)
    {
        auto it = sent.find(LianLiMacToString(r.mac));
        if(it != sent.end())
        {
            bool ok = (r.effect_index == it->second);
            failures += ok ? 0 : 1;
            printf("verify %s: %s\n", it->first.c_str(), ok ? "applied" : "NOT applied");
        }
    }

    return(failures == 0 ? 0 : 2);
}
