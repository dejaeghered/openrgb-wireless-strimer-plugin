/*---------------------------------------------------------*\
| LianLiWirelessProtocol.h                                  |
|                                                           |
|   USB/RF protocol for the Lian Li SL V3 wireless dongle   |
|   pair (SLV3TX 0416:8040, SLV3RX 0416:8041; second        |
|   revision 1A86:E304 / 1A86:E305, untested).              |
|                                                           |
|   Protocol notes derived from sgtaziz/lian-li-linux,      |
|   SlackEight/lian-li-wireless and the SignalRGB           |
|   HydroShift II TL Wireless plugin.                       |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

typedef std::array<uint8_t, 6> LianLiMac;

std::string LianLiMacToString(const LianLiMac& mac);

/*---------------------------------------------------------*\
| One 42-byte record from the RX dongle device list         |
\*---------------------------------------------------------*/
struct LianLiWirelessRecord
{
    LianLiMac       mac;
    LianLiMac       master_mac;
    uint8_t         channel;
    uint8_t         rx_type;
    uint8_t         device_type;
    uint8_t         fan_count;
    uint32_t        effect_index;
};

/*---------------------------------------------------------*\
| Strimer geometry, keyed by record device type             |
\*---------------------------------------------------------*/
struct StrimerGeometry
{
    unsigned int    lanes;          /* logical channels, each driving two light guides  */
    unsigned int    leds_per_lane;
    const char*     model;
    bool            verified;       /* LED count confirmed on real hardware             */
};

bool IsStrimerDeviceType(uint8_t device_type);
StrimerGeometry GetStrimerGeometry(uint8_t device_type);

/*---------------------------------------------------------*\
| tinyuz stream encoder (literal-only). The device firmware |
| decompresses uploads with tinyuz; a literal-only stream   |
| is always valid and stays far below the 12 KiB limit for  |
| a single frame of up to 255 LEDs.                         |
\*---------------------------------------------------------*/
std::vector<uint8_t> TinyuzEncodeLiteral(const std::vector<uint8_t>& data);

class LianLiWirelessDongle
{
public:
    LianLiWirelessDongle();
    ~LianLiWirelessDongle();

    bool            Open(std::string& error);
    void            Close();
    bool            IsOpen();

    /*-----------------------------------------------------*\
    | Query the RX dongle for paired devices. Returns false |
    | on a USB error (the dongle pair should be reopened).  |
    \*-----------------------------------------------------*/
    bool            GetDevices(std::vector<LianLiWirelessRecord>& records);

    /*-----------------------------------------------------*\
    | Upload a single static frame. rgb holds R,G,B bytes   |
    | per LED in device order. Returns the effect index the |
    | device will report once the upload is applied, or 0   |
    | on a USB error.                                       |
    \*-----------------------------------------------------*/
    uint32_t        UploadStaticFrame(const LianLiWirelessRecord& device, const LianLiMac& master, const std::vector<uint8_t>& rgb);

    /*-----------------------------------------------------*\
    | Master clock heartbeat, expected about once a second  |
    \*-----------------------------------------------------*/
    bool            SendClockSync(const LianLiMac& master, uint8_t master_channel);

private:
    libusb_context*         ctx;
    libusb_device_handle*   tx;
    libusb_device_handle*   rx;
    bool                    tx_bulk;
    bool                    rx_bulk;
    std::mutex              usb_mutex;

    /*-----------------------------------------------------*\
    | Air-gap pacing between RF frames for different        |
    | targets or opcodes                                    |
    \*-----------------------------------------------------*/
    LianLiMac               last_target;
    uint8_t                 last_opcode;
    uint64_t                last_frame_time_ms;

    bool                    OpenDongle(bool is_tx, libusb_device_handle** handle, bool* bulk, std::string& error);
    void                    CloseHandle(libusb_device_handle** handle);
    bool                    Write(libusb_device_handle* handle, bool bulk, const uint8_t* data, int length);
    int                     Read(libusb_device_handle* handle, bool bulk, uint8_t* data, int length, unsigned int timeout);
    void                    Flush(libusb_device_handle* handle, bool bulk);
    bool                    SendRF(const uint8_t* rf_data, uint8_t channel, uint8_t rx_target);
    void                    PaceFrame(const uint8_t* rf_data);
};
