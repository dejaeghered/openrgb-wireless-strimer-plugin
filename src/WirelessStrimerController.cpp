/*---------------------------------------------------------*\
| WirelessStrimerController.cpp                             |
|                                                           |
|   One wireless Strimer exposed to OpenRGB as a virtual    |
|   RGBController                                           |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <algorithm>
#include "WirelessStrimerController.h"

WirelessStrimerController::WirelessStrimerController(OpenRGBPluginAPIInterface* api_ptr, const LianLiWirelessRecord& rec, const StrimerConfig& cfg, std::function<void()> wake_fn)
{
    api         = api_ptr;
    controller  = nullptr;
    record      = rec;
    config      = cfg;
    geometry    = GetStrimerGeometry(rec.device_type);
    wake        = wake_fn;
    dirty       = false;

    frame.assign(GetLEDCount() * 3, 0);
}

WirelessStrimerController::~WirelessStrimerController()
{
    Unregister();
}

unsigned int WirelessStrimerController::GetLEDCount()
{
    return(geometry.lanes * geometry.leds_per_lane);
}

std::string WirelessStrimerController::GetDisplayName()
{
    return(config.name.empty() ? geometry.model : config.name);
}

bool WirelessStrimerController::IsRegistered()
{
    return(controller != nullptr);
}

void WirelessStrimerController::BuildSetup(RGBController_Setup& setup, unsigned int brightness)
{
    std::string mac = LianLiMacToString(record.mac);

    setup.name          = GetDisplayName();
    setup.vendor        = "Lian Li";
    setup.description   = std::string(geometry.model) + " (L-Wireless)";
    setup.version       = "Device type " + std::to_string(record.device_type);
    setup.serial        = mac;
    setup.location      = "L-Wireless: " + mac;
    setup.configuration = "";
    setup.active_mode   = 0;
    setup.flags         = CONTROLLER_FLAG_VIRTUAL;
    setup.type          = DEVICE_TYPE_LEDSTRIP;

    /*-----------------------------------------------------*\
    | Single Direct mode; brightness is applied host-side   |
    \*-----------------------------------------------------*/
    mode direct;
    direct.name           = "Direct";
    direct.value          = 0;
    direct.flags          = MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS;
    direct.color_mode     = MODE_COLORS_PER_LED;
    direct.brightness_min = 0;
    direct.brightness_max = 100;
    direct.brightness     = brightness;
    setup.modes.push_back(direct);

    /*-----------------------------------------------------*\
    | LEDs are stored lane-major: lane 0 is the bottom band |
    | and index 0 of each lane is the end farthest from the |
    | connector.                                            |
    \*-----------------------------------------------------*/
    unsigned int lanes  = geometry.lanes;
    unsigned int length = geometry.leds_per_lane;

    for(unsigned int lane = 0; lane < lanes; lane++)
    {
        for(unsigned int pos = 0; pos < length; pos++)
        {
            led new_led;
            new_led.name  = "Lane " + std::to_string(lane + 1) + ", LED " + std::to_string(pos + 1);
            new_led.value = lane * length + pos;
            setup.leds.push_back(new_led);
        }
    }

    /*-----------------------------------------------------*\
    | Matrix map: row 0 is the top lane, column 0 the far   |
    | end, unless flipped                                   |
    \*-----------------------------------------------------*/
    std::vector<unsigned int> map(lanes * length);

    for(unsigned int row = 0; row < lanes; row++)
    {
        unsigned int lane = config.flip_y ? row : (lanes - 1 - row);

        for(unsigned int col = 0; col < length; col++)
        {
            unsigned int pos = config.flip_x ? (length - 1 - col) : col;
            map[row * length + col] = lane * length + pos;
        }
    }

    zone strimer_zone;
    strimer_zone.name       = "Strimer";
    strimer_zone.type       = ZONE_TYPE_MATRIX;
    strimer_zone.leds_count = lanes * length;
    strimer_zone.leds_min   = lanes * length;
    strimer_zone.leds_max   = lanes * length;
    strimer_zone.matrix_map.Set(lanes, length, map.data());

    /*-----------------------------------------------------*\
    | No segments: OpenRGB's DeviceView does not lay out    |
    | segments of matrix zones but still paints their       |
    | labels, indexing past its segment list and crashing.  |
    \*-----------------------------------------------------*/

    setup.zones.push_back(strimer_zone);

    setup.object_ptr                                    = this;
    setup.DeviceConfigureZone                           = nullptr;
    setup.DeviceUpdateLEDs                              = &WirelessStrimerController::OnUpdateLEDs;
    setup.DeviceUpdateZoneLEDs                          = &WirelessStrimerController::OnUpdateZoneLEDs;
    setup.DeviceUpdateSingleLED                         = &WirelessStrimerController::OnUpdateSingleLED;
    setup.DeviceUpdateMode                              = &WirelessStrimerController::OnUpdateMode;
    setup.DeviceSaveMode                                = nullptr;
    setup.DeviceUpdateZoneMode                          = nullptr;
    setup.DeviceUpdateDeviceSpecificConfiguration       = nullptr;
    setup.DeviceUpdateDeviceSpecificZoneConfiguration   = nullptr;
}

void WirelessStrimerController::Register()
{
    std::lock_guard<std::mutex> lock(controller_mutex);

    if(controller != nullptr)
    {
        return;
    }

    RGBController_Setup setup;
    BuildSetup(setup, 100);

    controller = api->CreateVirtualRGBController(&setup);
    api->RegisterVirtualRGBController(controller);
}

void WirelessStrimerController::Unregister()
{
    RGBControllerInterface* old;

    /*-----------------------------------------------------*\
    | Release the lock before deleting: the controller's    |
    | destructor joins its call thread, which may be        |
    | waiting on this lock inside one of our callbacks.     |
    \*-----------------------------------------------------*/
    {
        std::lock_guard<std::mutex> lock(controller_mutex);

        old        = controller;
        controller = nullptr;
    }

    if(old == nullptr)
    {
        return;
    }

    api->UnregisterVirtualRGBController(old);
    api->DeleteVirtualRGBController(old);
}

void WirelessStrimerController::ApplyConfig(const StrimerConfig& new_config)
{
    std::lock_guard<std::mutex> lock(controller_mutex);

    config = new_config;

    if(controller == nullptr)
    {
        return;
    }

    unsigned int brightness = controller->GetModeBrightness(0);

    RGBController_Setup setup;
    BuildSetup(setup, brightness);
    api->UpdateVirtualRGBController(controller, &setup);
}

void WirelessStrimerController::CaptureColors()
{
    std::unique_lock<std::mutex> controller_lock(controller_mutex);

    if(controller == nullptr)
    {
        return;
    }

    unsigned int count = controller->GetLEDCount();

    if(count != GetLEDCount())
    {
        return;
    }

    unsigned int brightness = controller->GetModeBrightness(0);
    RGBColor*    colors     = controller->GetColorsPointer();

    std::vector<uint8_t> rgb(count * 3);

    for(unsigned int idx = 0; idx < count; idx++)
    {
        RGBColor color = colors[idx];
        rgb[idx * 3 + 0] = (uint8_t)(RGBGetRValue(color) * brightness / 100);
        rgb[idx * 3 + 1] = (uint8_t)(RGBGetGValue(color) * brightness / 100);
        rgb[idx * 3 + 2] = (uint8_t)(RGBGetBValue(color) * brightness / 100);
    }

    {
        std::lock_guard<std::mutex> lock(frame_mutex);

        if(rgb != frame)
        {
            frame = rgb;
            dirty = true;
        }
    }

    controller_lock.unlock();

    if(wake)
    {
        wake();
    }
}

bool WirelessStrimerController::TakeFrame(std::vector<uint8_t>& rgb)
{
    std::lock_guard<std::mutex> lock(frame_mutex);

    if(!dirty)
    {
        return(false);
    }

    rgb   = frame;
    dirty = false;
    return(true);
}

void WirelessStrimerController::GetFrame(std::vector<uint8_t>& rgb)
{
    std::lock_guard<std::mutex> lock(frame_mutex);
    rgb = frame;
}

void WirelessStrimerController::MarkDirty()
{
    {
        std::lock_guard<std::mutex> lock(frame_mutex);
        dirty = true;
    }

    if(wake)
    {
        wake();
    }
}

nlohmann::json WirelessStrimerController::GetProfileState()
{
    std::lock_guard<std::mutex> lock(controller_mutex);

    if(controller == nullptr)
    {
        return(nlohmann::json());
    }

    unsigned int count  = controller->GetLEDCount();
    RGBColor*    colors = controller->GetColorsPointer();

    nlohmann::json state;
    state["brightness"] = controller->GetModeBrightness(0);
    state["colors"]     = std::vector<RGBColor>(colors, colors + count);

    return(state);
}

bool WirelessStrimerController::ApplyProfileState(const nlohmann::json& state)
{
    std::lock_guard<std::mutex> lock(controller_mutex);

    if(controller == nullptr || !state.is_object() || !state.contains("colors") || !state["colors"].is_array())
    {
        return(false);
    }

    const nlohmann::json& colors = state["colors"];

    if(colors.size() != controller->GetLEDCount())
    {
        return(false);
    }

    if(state.contains("brightness") && state["brightness"].is_number_unsigned())
    {
        controller->SetModeBrightness(0, std::min(state["brightness"].get<unsigned int>(), 100u));
    }

    for(unsigned int idx = 0; idx < colors.size(); idx++)
    {
        if(colors[idx].is_number_unsigned())
        {
            controller->SetColor(idx, colors[idx].get<RGBColor>());
        }
    }

    /*-----------------------------------------------------*\
    | Queues DeviceUpdateLEDs on the controller's call      |
    | thread, which captures the colors via OnUpdateLEDs    |
    \*-----------------------------------------------------*/
    controller->UpdateLEDs();

    return(true);
}

void WirelessStrimerController::OnUpdateLEDs(void* self)
{
    ((WirelessStrimerController*)self)->CaptureColors();
}

void WirelessStrimerController::OnUpdateZoneLEDs(void* self, int /*zone*/)
{
    ((WirelessStrimerController*)self)->CaptureColors();
}

void WirelessStrimerController::OnUpdateSingleLED(void* self, int /*led*/)
{
    ((WirelessStrimerController*)self)->CaptureColors();
}

void WirelessStrimerController::OnUpdateMode(void* self)
{
    ((WirelessStrimerController*)self)->CaptureColors();
}
