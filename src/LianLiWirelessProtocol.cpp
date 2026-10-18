/*---------------------------------------------------------*\
| LianLiWirelessProtocol.cpp                                |
|                                                           |
|   USB/RF protocol for the Lian Li SL V3 wireless dongle   |
|   pair (SLV3TX 0416:8040, SLV3RX 0416:8041; second        |
|   revision 1A86:E304 / 1A86:E305, untested).              |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <chrono>
#include <cstring>
#include <thread>
#include <libusb.h>
#include "LianLiWirelessProtocol.h"

/*---------------------------------------------------------*\
| Known dongle USB IDs. The first revision (Nuvoton VID) is |
| tested; the second revision (WCH VID) is taken from       |
| sgtaziz/lian-li-linux and has not been tested.            |
\*---------------------------------------------------------*/
struct DongleId
{
    uint16_t    vid;
    uint16_t    pid;
};

static const DongleId tx_ids[] = { { 0x0416, 0x8040 }, { 0x1A86, 0xE304 } };
static const DongleId rx_ids[] = { { 0x0416, 0x8041 }, { 0x1A86, 0xE305 } };

#define EP_OUT                      0x01
#define EP_IN                       0x81
#define USB_PACKET_SIZE             64
#define USB_WRITE_TIMEOUT_MS        5000

#define USB_CMD_SEND_RF             0x10

#define RF_SELECT                   0x12
#define RF_CLOCK_SYNC               0x14
#define RF_SET_RGB                  0x20
#define RF_DATA_SIZE                240
#define RF_CHUNK_SIZE               60
#define RF_CHUNKS                   (RF_DATA_SIZE / RF_CHUNK_SIZE)
#define RF_BROADCAST                0xFF

#define RECORD_SIZE                 42
#define RECORD_MARKER               0x1C
#define RECORDS_PER_PAGE            10
#define MAX_PAGES                   26

#define RGB_CHUNK_BYTES             220
#define RGB_MAX_COMPRESSED_BYTES    12288
#define RGB_HEADER_REPEATS          2

/*---------------------------------------------------------*\
| A single frame "loops" every 5 s; interval is expressed   |
| in 0.625 ms ticks plus hundredths of a tick               |
\*---------------------------------------------------------*/
#define STATIC_INTERVAL_MS          5000

#define GAP_AFTER_RGB_MS            10
#define GAP_BETWEEN_FRAMES_MS       5

static uint64_t NowMs()
{
    return((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string LianLiMacToString(const LianLiMac& mac)
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return(std::string(buf));
}

bool IsStrimerDeviceType(uint8_t device_type)
{
    return((device_type >= 1) && (device_type <= 9));
}

StrimerGeometry GetStrimerGeometry(uint8_t device_type)
{
    switch(device_type)
    {
        /*-------------------------------------------------*\
        | Types 2 and 3 are verified on hardware. Type 1 is |
        | inferred from retail LED counts; type 4 comes     |
        | from lian-li-linux with no known product.         |
        \*-------------------------------------------------*/
        case 1:  return(StrimerGeometry{ 4, 29, "Strimer Wireless 12VHPWR",        false });
        case 2:  return(StrimerGeometry{ 6, 22, "Strimer Wireless 24-Pin",         true  });
        case 3:  return(StrimerGeometry{ 6, 29, "Strimer Wireless 12VHPWR (Wide)", true  });
        case 4:  return(StrimerGeometry{ 4, 22, "Strimer Wireless",                false });
        default: return(StrimerGeometry{ 4, 22, "Strimer Wireless (Unknown Type)", false });
    }
}

std::vector<uint8_t> TinyuzEncodeLiteral(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t>    code    = { 1, 0, 0, 0 };   /* dict size header, no matches used */
    std::size_t             types_index = 0;
    unsigned int            type_count  = 0;

    auto out_type = [&](unsigned int bit)
    {
        if(type_count == 0)
        {
            types_index = code.size();
            code.push_back(0);
        }
        code[types_index] |= (uint8_t)((bit & 1) << type_count);
        type_count = (type_count + 1) % 8;
    };

    for(uint8_t byte : data)
    {
        out_type(1);
        code.push_back(byte);
    }

    /*-----------------------------------------------------*\
    | Control code STREAM_END (3), packed with packBit = 1  |
    \*-----------------------------------------------------*/
    out_type(0);

    unsigned int value = 3;
    unsigned int count = 1;
    while(value >= (1u << count))
    {
        value -= (1u << count);
        count++;
    }
    while(count--)
    {
        out_type((value >> count) & 1);
        out_type(count > 0 ? 1 : 0);
    }

    if(!data.empty())
    {
        out_type(0);
    }
    code.push_back(0);

    return(code);
}

LianLiWirelessDongle::LianLiWirelessDongle()
{
    ctx                 = nullptr;
    tx                  = nullptr;
    rx                  = nullptr;
    tx_bulk             = true;
    rx_bulk             = true;
    last_target.fill(0);
    last_opcode         = 0;
    last_frame_time_ms  = 0;
}

LianLiWirelessDongle::~LianLiWirelessDongle()
{
    Close();
}

bool LianLiWirelessDongle::Open(std::string& error)
{
    std::lock_guard<std::mutex> lock(usb_mutex);

    if(ctx == nullptr)
    {
        /*-------------------------------------------------*\
        | Use a private context; OpenRGB may switch the     |
        | default context to the UsbDk backend              |
        \*-------------------------------------------------*/
        int ret = libusb_init(&ctx);
        if(ret != LIBUSB_SUCCESS)
        {
            error = std::string("libusb_init failed: ") + libusb_error_name(ret);
            ctx   = nullptr;
            return(false);
        }
    }

    if(tx == nullptr && !OpenDongle(true, &tx, &tx_bulk, error))
    {
        return(false);
    }

    if(rx == nullptr && !OpenDongle(false, &rx, &rx_bulk, error))
    {
        CloseHandle(&tx);
        return(false);
    }

    return(true);
}

bool LianLiWirelessDongle::OpenDongle(bool is_tx, libusb_device_handle** handle, bool* bulk, std::string& error)
{
    const char*     name    = is_tx ? "TX" : "RX";
    const DongleId* ids     = is_tx ? tx_ids : rx_ids;
    std::size_t     id_ct   = is_tx ? (sizeof(tx_ids) / sizeof(tx_ids[0])) : (sizeof(rx_ids) / sizeof(rx_ids[0]));

    *handle = nullptr;

    for(std::size_t id_idx = 0; id_idx < id_ct && *handle == nullptr; id_idx++)
    {
        *handle = libusb_open_device_with_vid_pid(ctx, ids[id_idx].vid, ids[id_idx].pid);
    }

    if(*handle == nullptr)
    {
        error = std::string(name) + (is_tx ? " dongle (0416:8040 or 1A86:E304)" : " dongle (0416:8041 or 1A86:E305)") + " not found or in use (close L-Connect)";
        return(false);
    }

    libusb_set_auto_detach_kernel_driver(*handle, 1);

    int ret = libusb_claim_interface(*handle, 0);
    if(ret != LIBUSB_SUCCESS)
    {
        error = std::string("Cannot claim ") + name + " dongle: " + libusb_error_name(ret) + " (is L-Connect running?)";
        libusb_close(*handle);
        *handle = nullptr;
        return(false);
    }

    /*-----------------------------------------------------*\
    | Detect endpoint transfer type. Never send             |
    | SET_INTERFACE: the dongle drops the next OUT packet.  |
    \*-----------------------------------------------------*/
    *bulk = true;

    libusb_config_descriptor* config = nullptr;
    if(libusb_get_active_config_descriptor(libusb_get_device(*handle), &config) == LIBUSB_SUCCESS)
    {
        if(config->bNumInterfaces > 0 && config->interface[0].num_altsetting > 0)
        {
            const libusb_interface_descriptor* intf = &config->interface[0].altsetting[0];

            for(int ep_idx = 0; ep_idx < intf->bNumEndpoints; ep_idx++)
            {
                if(intf->endpoint[ep_idx].bEndpointAddress == EP_OUT)
                {
                    *bulk = ((intf->endpoint[ep_idx].bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK);
                }
            }
        }
        libusb_free_config_descriptor(config);
    }

    return(true);
}

void LianLiWirelessDongle::CloseHandle(libusb_device_handle** handle)
{
    if(*handle != nullptr)
    {
        libusb_release_interface(*handle, 0);
        libusb_close(*handle);
        *handle = nullptr;
    }
}

void LianLiWirelessDongle::Close()
{
    std::lock_guard<std::mutex> lock(usb_mutex);

    CloseHandle(&tx);
    CloseHandle(&rx);

    if(ctx != nullptr)
    {
        libusb_exit(ctx);
        ctx = nullptr;
    }
}

bool LianLiWirelessDongle::IsOpen()
{
    std::lock_guard<std::mutex> lock(usb_mutex);
    return(tx != nullptr && rx != nullptr);
}

bool LianLiWirelessDongle::Write(libusb_device_handle* handle, bool bulk, const uint8_t* data, int length)
{
    int transferred = 0;
    int ret;

    if(bulk)
    {
        ret = libusb_bulk_transfer(handle, EP_OUT, (unsigned char*)data, length, &transferred, USB_WRITE_TIMEOUT_MS);
    }
    else
    {
        ret = libusb_interrupt_transfer(handle, EP_OUT, (unsigned char*)data, length, &transferred, USB_WRITE_TIMEOUT_MS);
    }

    return(ret == LIBUSB_SUCCESS && transferred == length);
}

int LianLiWirelessDongle::Read(libusb_device_handle* handle, bool bulk, uint8_t* data, int length, unsigned int timeout)
{
    int transferred = 0;
    int ret;

    if(bulk)
    {
        ret = libusb_bulk_transfer(handle, EP_IN, data, length, &transferred, timeout);
    }
    else
    {
        ret = libusb_interrupt_transfer(handle, EP_IN, data, length, &transferred, timeout);
    }

    if(ret == LIBUSB_ERROR_TIMEOUT)
    {
        return(0);
    }
    if(ret != LIBUSB_SUCCESS)
    {
        return(-1);
    }
    return(transferred);
}

void LianLiWirelessDongle::Flush(libusb_device_handle* handle, bool bulk)
{
    uint8_t buf[USB_PACKET_SIZE];

    for(int i = 0; i < 16; i++)
    {
        if(Read(handle, bulk, buf, sizeof(buf), 5) <= 0)
        {
            break;
        }
    }
}

bool LianLiWirelessDongle::GetDevices(std::vector<LianLiWirelessRecord>& records)
{
    std::lock_guard<std::mutex> lock(usb_mutex);

    records.clear();

    if(rx == nullptr)
    {
        return(false);
    }

    unsigned int pages = 1;

    for(int attempt = 0; attempt < 2; attempt++)
    {
        uint8_t cmd[USB_PACKET_SIZE] = { 0 };
        cmd[0] = USB_CMD_SEND_RF;
        cmd[1] = (uint8_t)pages;

        Flush(rx, rx_bulk);

        if(!Write(rx, rx_bulk, cmd, sizeof(cmd)))
        {
            return(false);
        }

        std::vector<uint8_t> response;
        unsigned int         timeout = 200;

        while(response.size() < pages * 512)
        {
            uint8_t chunk[USB_PACKET_SIZE];
            int     len = Read(rx, rx_bulk, chunk, sizeof(chunk), timeout);

            if(len < 0)
            {
                return(false);
            }
            if(len == 0)
            {
                break;
            }
            response.insert(response.end(), chunk, chunk + len);
            timeout = 20;
        }

        if(response.size() < 4 || response[0] != USB_CMD_SEND_RF)
        {
            /*---------------------------------------------*\
            | No answer this time; not a USB failure        |
            \*---------------------------------------------*/
            return(true);
        }

        unsigned int count = response[1];

        if(count > pages * RECORDS_PER_PAGE && pages < MAX_PAGES)
        {
            pages = (count + RECORDS_PER_PAGE - 1) / RECORDS_PER_PAGE;
            if(pages > MAX_PAGES)
            {
                pages = MAX_PAGES;
            }
            continue;
        }

        for(unsigned int idx = 0; idx < count; idx++)
        {
            std::size_t offset = 4 + idx * RECORD_SIZE;

            if(offset + RECORD_SIZE > response.size())
            {
                break;
            }

            const uint8_t* r = &response[offset];

            if(r[41] != RECORD_MARKER || r[18] == 0xFF)
            {
                continue;
            }

            LianLiWirelessRecord record;
            memcpy(record.mac.data(), &r[0], 6);
            memcpy(record.master_mac.data(), &r[6], 6);
            record.channel      = r[12];
            record.rx_type      = r[13];
            record.device_type  = r[18];
            record.fan_count    = r[19];
            record.effect_index = ((uint32_t)r[20] << 24) | ((uint32_t)r[21] << 16) | ((uint32_t)r[22] << 8) | r[23];

            bool zero_mac = true;
            for(uint8_t b : record.mac)
            {
                if(b != 0)
                {
                    zero_mac = false;
                }
            }
            if(zero_mac)
            {
                continue;
            }

            records.push_back(record);
        }

        return(true);
    }

    return(true);
}

void LianLiWirelessDongle::PaceFrame(const uint8_t* rf_data)
{
    /*-----------------------------------------------------*\
    | A device hearing a frame too soon after another       |
    | device's frame can latch the wrong address. Leave an  |
    | air gap when the target or opcode changes.            |
    \*-----------------------------------------------------*/
    bool same_target = (memcmp(last_target.data(), &rf_data[2], 6) == 0) && (last_opcode == rf_data[1]);

    if(!same_target && last_frame_time_ms != 0)
    {
        uint64_t gap     = (last_opcode == RF_SET_RGB) ? GAP_AFTER_RGB_MS : GAP_BETWEEN_FRAMES_MS;
        uint64_t elapsed = NowMs() - last_frame_time_ms;

        if(elapsed < gap)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(gap - elapsed));
        }
    }

    memcpy(last_target.data(), &rf_data[2], 6);
    last_opcode = rf_data[1];
}

bool LianLiWirelessDongle::SendRF(const uint8_t* rf_data, uint8_t channel, uint8_t rx_target)
{
    PaceFrame(rf_data);

    /*-----------------------------------------------------*\
    | The TX discards a frame unless all four chunks arrive |
    \*-----------------------------------------------------*/
    for(uint8_t chunk_idx = 0; chunk_idx < RF_CHUNKS; chunk_idx++)
    {
        uint8_t packet[USB_PACKET_SIZE];

        packet[0] = USB_CMD_SEND_RF;
        packet[1] = chunk_idx;
        packet[2] = channel;
        packet[3] = rx_target;
        memcpy(&packet[4], &rf_data[chunk_idx * RF_CHUNK_SIZE], RF_CHUNK_SIZE);

        if(!Write(tx, tx_bulk, packet, sizeof(packet)))
        {
            return(false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    last_frame_time_ms = NowMs();
    return(true);
}

uint32_t LianLiWirelessDongle::UploadStaticFrame(const LianLiWirelessRecord& device, const LianLiMac& master, const std::vector<uint8_t>& rgb)
{
    std::lock_guard<std::mutex> lock(usb_mutex);

    if(tx == nullptr || rgb.empty() || (rgb.size() % 3) != 0 || rgb.size() / 3 > 255)
    {
        return(0);
    }

    std::vector<uint8_t> compressed = TinyuzEncodeLiteral(rgb);

    if(compressed.size() > RGB_MAX_COMPRESSED_BYTES)
    {
        return(0);
    }

    uint8_t     led_count           = (uint8_t)(rgb.size() / 3);
    uint16_t    frame_count         = 1;
    uint32_t    interval_hundredths = STATIC_INTERVAL_MS * 160;
    uint16_t    interval_ticks      = (uint16_t)(interval_hundredths / 100);
    uint8_t     interval_fraction   = (uint8_t)(interval_hundredths % 100);

    /*-----------------------------------------------------*\
    | Effect index: FNV-1a over the payload and timing. It  |
    | must change whenever the colors change or the device  |
    | ignores the upload. High bit set so it never looks    |
    | like an RX slot.                                      |
    \*-----------------------------------------------------*/
    uint32_t hash = 0x811C9DC5;
    auto fnv = [&hash](uint8_t byte)
    {
        hash = (hash ^ byte) * 0x01000193;
    };

    for(uint8_t byte : compressed)
    {
        fnv(byte);
    }
    fnv((uint8_t)(interval_ticks >> 8));
    fnv((uint8_t)(interval_ticks & 0xFF));
    fnv(interval_fraction);
    fnv((uint8_t)(frame_count >> 8));
    fnv((uint8_t)(frame_count & 0xFF));
    fnv(led_count);
    fnv(0);     /* secondary interval ticks */
    fnv(0);
    fnv(0);     /* secondary frame count    */
    fnv(0);
    fnv(0);     /* outer longest            */

    uint32_t effect_index = hash | 0x80000000;

    unsigned int data_packets  = (unsigned int)((compressed.size() + RGB_CHUNK_BYTES - 1) / RGB_CHUNK_BYTES);
    unsigned int total_packets = data_packets + 1;

    uint8_t channel = (device.channel >= 1 && device.channel <= 39) ? device.channel : 8;

    for(unsigned int index = 0; index < total_packets; index++)
    {
        uint8_t packet[RF_DATA_SIZE] = { 0 };

        packet[0]  = RF_SELECT;
        packet[1]  = RF_SET_RGB;
        memcpy(&packet[2], device.mac.data(), 6);
        memcpy(&packet[8], master.data(), 6);
        packet[14] = (uint8_t)(effect_index >> 24);
        packet[15] = (uint8_t)(effect_index >> 16);
        packet[16] = (uint8_t)(effect_index >> 8);
        packet[17] = (uint8_t)(effect_index);
        packet[18] = (uint8_t)index;
        packet[19] = (uint8_t)total_packets;

        if(index == 0)
        {
            uint32_t size = (uint32_t)compressed.size();

            packet[20] = (uint8_t)(size >> 24);
            packet[21] = (uint8_t)(size >> 16);
            packet[22] = (uint8_t)(size >> 8);
            packet[23] = (uint8_t)(size);
            packet[25] = (uint8_t)(frame_count >> 8);
            packet[26] = (uint8_t)(frame_count);
            packet[27] = led_count;
            packet[32] = (uint8_t)(interval_ticks >> 8);
            packet[33] = (uint8_t)(interval_ticks);
            packet[34] = interval_fraction;

            for(int repeat = 0; repeat < RGB_HEADER_REPEATS; repeat++)
            {
                if(!SendRF(packet, channel, device.rx_type))
                {
                    return(0);
                }
                if(repeat + 1 < RGB_HEADER_REPEATS)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        }
        else
        {
            std::size_t offset = (index - 1) * RGB_CHUNK_BYTES;
            std::size_t length = std::min<std::size_t>(RGB_CHUNK_BYTES, compressed.size() - offset);

            memcpy(&packet[20], &compressed[offset], length);

            if(!SendRF(packet, channel, device.rx_type))
            {
                return(0);
            }
        }
    }

    return(effect_index);
}

bool LianLiWirelessDongle::SendClockSync(const LianLiMac& master, uint8_t master_channel)
{
    std::lock_guard<std::mutex> lock(usb_mutex);

    if(tx == nullptr)
    {
        return(false);
    }

    uint8_t packet[RF_DATA_SIZE] = { 0 };

    packet[0] = RF_SELECT;
    packet[1] = RF_CLOCK_SYNC;
    memcpy(&packet[8], master.data(), 6);

    /*-----------------------------------------------------*\
    | 0x14 fill marks the sensor fields as unset; fans that |
    | latch other values there can drop off the receiver    |
    \*-----------------------------------------------------*/
    memset(&packet[14], RF_CLOCK_SYNC, 64 - 14);

    return(SendRF(packet, master_channel, RF_BROADCAST));
}
