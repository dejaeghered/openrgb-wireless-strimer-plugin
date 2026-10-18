# OpenRGB Lian Li Wireless Strimer plugin

An OpenRGB plugin that gives you per-LED control of **Lian Li Strimer Wireless** cables (the wireless 24-pin and 12VHPWR/12V-2x6 Strimers). It talks to them through Lian Li's 2.4 GHz wireless dongle pair, the same dongles L-Connect 3 uses.

> **Built with AI-assisted coding.** This plugin was written with Claude (Anthropic's AI coding assistant), working from published reverse-engineering notes of the protocol. It has been tested on a single PC (see [Hardware](#hardware)). Treat it as experimental. Please report what works and what doesn't on your hardware.

## Features

- **Automatic discovery.** Every Strimer paired to your dongle appears in OpenRGB's **Devices** tab. Nothing is hard-coded to one PC: the plugin reads the dongle's device list, picks the master dongle and radio channel from it, and works out each cable's LED layout from its device type.
- **Direct mode, per-LED colour.** Each cable is a matrix zone with one row per lane and one column per LED along the cable. Brightness is 0–100.
- **Profiles.** Strimer colours are saved into OpenRGB profiles and restored when a profile loads. This includes a profile the OpenRGB service loads at startup: the colours are applied as soon as the cable is discovered.
- **Reliable delivery.** Every update is checked against what the cable reports back, and re-sent if it didn't arrive.
- **A "Wireless Strimer" tab** where you can:
  - rename each cable, or enable and disable it,
  - flip its matrix along the cable or across the lanes, to match how it's mounted,
  - flash a cable white with **Identify**,
  - cap the number of updates per second,
  - switch the 1 Hz wireless clock heartbeat on or off.

## Hardware

### Tested

| Product | Identified as | LEDs (lanes × LEDs) |
|---|---|---|
| Lian Li wireless dongle pair, first revision: SLV3TX `0416:8040`, SLV3RX `0416:8041` (firmware V1.6) | — | — |
| Strimer Wireless 24-pin | device type 2 | 132 (6 × 22) |
| Strimer Wireless 12VHPWR, wide, 12 light guides | device type 3 | 174 (6 × 29) |

The test system was Windows 11 with OpenRGB 1.1 (commit `8e16de2`) and Qt 6.8.3. OpenRGB was tested both standalone and connected to the OpenRGB Windows service.

### Not tested

These are supported in the code, based on other projects' reverse-engineering notes, but have never been tried with this plugin:

| Product | Identified as | Assumed LEDs | Source of the assumption |
|---|---|---|---|
| Wireless dongle pair, second revision: `1A86:E304` (TX), `1A86:E305` (RX) | — | — | USB IDs from [lian-li-linux](https://github.com/sgtaziz/lian-li-linux) |
| Strimer Wireless 12VHPWR, standard, 8 light guides | device type 1 | 116 (4 × 29) | retail LED counts, lian-li-linux |
| Unknown Strimer variant | device type 4 | 88 (4 × 22) | lian-li-linux; no matching product known |
| Any other Strimer device type (5–9) | — | 88 (4 × 22), a guess | none |

An untested cable is labelled **(untested)** in the Wireless Strimer tab, and the plugin logs a warning when it finds one. **The LED count must match the cable exactly.** If it's wrong, the cable silently ignores every update. If you own one of these and it works, or doesn't, please open an issue with the line from the log that starts `Found Strimer`.

### Not supported

Other Lian Li wireless devices, such as UNI FAN wireless fans and wireless AIO pumps, appear in the tab under "Other wireless devices" but can't be controlled by this plugin.

## Requirements

- **OpenRGB 1.0 / 1.1 or newer with plugin API version 5**, Windows x64, built with Qt 6.8. Older OpenRGB releases such as 0.9 won't load the plugin.
- **The cables must already be paired with the dongle in L-Connect 3.** The plugin can't pair new devices.
- **L-Connect must not be running while OpenRGB uses the dongle.** That includes its background service (`LConnectService` / `L-Connect-Service.exe`); set it to Manual or Disabled. Only one program can use the dongle at a time.

## Install

1. Download `OpenRGBWirelessStrimerPlugin.dll` from the releases page, or build it yourself (see below).
2. Copy it to `%APPDATA%\OpenRGB\plugins\`.
3. Fully quit OpenRGB (tray icon → Exit) and start it again. The plugin appears under **Settings → Plugins**, and a **Wireless Strimer** tab appears.

## Known limitations

- **The plugin only runs while the OpenRGB window is open.** OpenRGB plugins are loaded by the window, not by the OpenRGB Windows service. If you use the service, the Strimers are only controllable, and a startup profile only reaches them, once the window is open. Until then the cables keep showing the last colours they received, including across reboots.
- **Only one program can use the dongle.** This includes a second OpenRGB window: it shows "Dongle: not connected" because the first window has the dongle. Quit OpenRGB from the tray icon before you start it again.
- **No pairing,** and no control of other wireless devices on the same dongle. With L-Connect closed, wireless fans on the same dongle keep their last effect.
- **Direct mode only.** No effects are stored on the cable itself. Animated effects come from OpenRGB, so they only run while OpenRGB runs.
- **Limited update rate.** Every colour change is a radio upload, so the default limit is 15 updates per second per cable, and the maximum is 30. Fast effects can look choppier than on wired devices.
- **No per-lane segments.** OpenRGB 1.1's LED view crashes on a matrix zone that has segments.
- **Windows only, as built and tested.** The protocol code is portable, but Linux builds are untested and would need udev rules for the dongles.

## Troubleshooting

- **"not found or in use (close L-Connect)" / "Dongle: not connected"**: another program has the dongle. This might be L-Connect or its service, or a second OpenRGB window. Close it, then press **Rescan**.
- **A cable doesn't light up at all**: check the log for the "has not been tested" warning; its LED count may be wrong. Please report it.
- **Effects stutter, or a cable misses updates**: lower *Max updates per second* to 10.
- **The colours are mirrored**: tick *Flip along cable* and/or *Flip lanes* for that cable.
- **A profile doesn't restore the Strimers**: re-save the profile with this version of the plugin. Profiles saved with older versions contain no Strimer colours.
- **Logs**: `%APPDATA%\OpenRGB\logs`. Lines from this plugin are tagged `[LianLiWirelessStrimer]`.

## Build

You need:
- Visual Studio 2022 Build Tools (MSVC, plus the CMake that comes with them),
- Qt 6.8.x for `msvc2022_64`, for example: `pip install aqtinstall && python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O C:\Qt`,
- an OpenRGB source checkout matching your OpenRGB build, in `third_party/OpenRGB`:

```sh
git clone https://gitlab.com/CalcProgrammer1/OpenRGB.git third_party/OpenRGB
git -C third_party/OpenRGB checkout 8e16de27858cf2be6af54a33ce003df74cb9fd3e   # "OpenRGB version 1.1"
```

Then configure and build:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build --config Release
```

The build links libusb statically from OpenRGB's `dependencies/` folder.

`build/Release/strimer_probe.exe [RRGGBB]` is a standalone test tool. It lists the devices paired to the dongle. With a colour argument, it also sets every Strimer to that colour and checks that the colour was applied. Quit OpenRGB before you run it.

## How it works

- **Discovery.** Every second the plugin asks the RX dongle for its device list. The list is made of 42-byte records, one per device. Each record gives the device MAC, master MAC, RF channel, RX slot, device type, and the effect ID the device is currently playing.
- **Colours.**
  - Every colour change is uploaded as a one-frame "effect". Colour order is R, G, B. Lanes are stored one after another, starting with the bottom lane; each lane starts at the end farthest from the connector.
  - The upload is compressed with tinyuz (a literal-only stream), then split into 240-byte radio frames: one header frame, sent twice, followed by data frames. Each radio frame goes to the TX dongle as four 64-byte USB packets.
  - Each upload carries an FNV-1a hash as its effect ID. If a cable doesn't report that ID within 1.5 s, the frame is sent again, up to five times.
- **Brightness** is applied in software, by scaling the colours before upload.
- **Timing.** Uploads to different cables are spaced at least 10 ms apart, and limited to the configured maximum updates per second.
- **Profiles.** Each cable's brightness and colours are stored in the plugin's section of the OpenRGB profile, keyed by the cable's MAC address.

## Credits

The protocol knowledge comes from these open-source reverse-engineering projects:
- [sgtaziz/lian-li-linux](https://github.com/sgtaziz/lian-li-linux)
- [SlackEight/lian-li-wireless](https://github.com/SlackEight/lian-li-wireless)
- [SignalRGB HydroShift II TL Wireless](https://github.com/ShkourBashtawi/SignalRGB-HydroShift-II-TL-Wireless)

This project isn't affiliated with Lian Li or the OpenRGB project. The code was written with Claude, Anthropic's AI coding assistant.

## License

GPL-2.0-or-later, the same as OpenRGB.
