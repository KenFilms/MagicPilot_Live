# MagicPilot LIVE – Cam Monitor & BT remote for the BMPCC6K on the M5Stack Tab5

MagicPilot LIVE turns an **M5Stack Tab5** into a monitor and touch remote for the
**Blackmagic Pocket Cinema Camera 6K**: the camera's live HDMI picture, with the camera's settings
laid over it. Tap a setting to change it over Bluetooth LE,
using the Blackmagic Camera Control protocol of
[MagicPilot_Remote](https://github.com/KenFilms/MagicPilot_Remote). 

No phone, no Wi-Fi, no Blackmagic Camera app.

## At a glance

| | Feature | What it does |
|---|---|---|
| 📺 | **Live picture** | The camera's HDMI output, full screen in landscape, 30 fps, or a remote-only mode without it. |
| 📊 | **Camera status** | FPS, shutter, iris, timecode, ISO, white balance, tint, LUT, recording and card slots, like on the camera. |
| 💾 | **Camera presets** | 12 named presets, as in MagicPilot_Remote, kept on the Tab5. |
| 🎬 | **Slate** | The camera's slate: scene, take, shot type, project, lens data. |
| 🎯 | **Focus** | Focus slider and one-shot autofocus. |
| 🎥 | **Record** | Start and stop recording. |
| ⚙️ | **Record settings** | Codec, resolution, dynamic range, frame rates, sharpening. |
| 📐 | **Display tools** | Frame guides, grid, safe area, brightness. |
| 🔗 | **Pairing** | Asks for the camera's PIN once, then reconnects automatically. |

## Hardware

![M5Stack Tab5 on a wireless HDMI receiver, with an HDMI to USB-A capture adapter in its USB-A port](images/rig-overview.jpg)

- **M5Stack Tab5** (ESP32-P4). Tested: board version 3, ESP32-C6 with its factory firmware.
- **HDMI to USB capture adapter** (UVC, MJPEG, e.g. MS2109 or MS2130 sticks) in the Tab5's **USB-A**
  port, on an HDMI cable from the camera. For a wireless picture, put an HDMI transmitter and
  receiver between the camera and the adapter. Optional: with *Settings → REMOTE → Live view → Off (remote only)* the Tab5 works as a
  Bluetooth remote without it and leaves the USB-A port unpowered.
- **Blackmagic Pocket Cinema Camera 6K** with Bluetooth on. At every connection the Tab5 asks the
  camera to switch its status text off on HDMI; if it still shows, switch the HDMI overlays off on
  the camera (*Monitor → HDMI*). The 6K G2, 6K Pro and 4K use the same protocol but are not tested.

## Build and flash

PlatformIO (pioarduino platform):

```sh
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor
```

If PlatformIO reports *"Python's lzma module is unavailable"*, install `xz` (`brew install xz`);
PlatformIO's bundled Python links against Homebrew's `liblzma`.

or ESP-IDF ≥ 5.5.3:

```sh
idf.py set-target esp32p4
idf.py build flash monitor
```

`sdkconfig.m5stack-tab5` is generated from `sdkconfig.defaults`. After changing the defaults, delete
it so it is regenerated (settings already in the file are not overwritten).

## Using the remote

**Pairing.** The Tab5 looks for the camera at power-on and whenever the link drops. The first time a 6-digit PIN from the BMPCC6K Setup Menu page 6 is needed. 

### Views

The VIEW button switches between LITE and PRO (advanced). With *Settings → REMOTE → Live view → Off (remote only)*,
or without the capture adapter, the Tab5 is a Bluetooth remote with bigger controls. With HDMI input, the
CLEAN FEED … FALSE COLOR buttons act on the camera's HDMI output, so they change the video on the Tab5; without
it, they change the view on the camera's own LCD.

![LITE and PRO view, with and without HDMI input](images/views.jpg)

The screens are the firmware's own drawing code, rendered with `tools/preview` over an example picture.

### Screen

| Area | Shows | Tap |
|---|---|---|
| Frame icon | `LUT` below it while the camera's display LUT is on | Display tools |
| FPS, SHUTTER, IRIS, ISO, WB, TINT | The camera's values (`48/24`: off-speed sensor/project rate) | Panel |
| Timecode | Timecode (`TC`) or clip counter, red while recording | Timecode ↔ clip |
| Bluetooth symbol | Blue while connected | Menu, REMOTE tab |
| CAM badge | Camera power: `AC` or battery voltage | `AC` ↔ voltage |
| M5 Tab badge | `USB-C` on outside power; the Tab5's battery while it runs on it | Percentage ↔ voltage |
| Settings icon | | Menu, RECORD tab |
| Button row under FPS … IRIS (PRO view) | CLEAN FEED, ZEBRA, F.-ASSIST, GUIDE, GRID, SAFE AREA, FALSE COLOR, outlined in yellow when on | Switches them on the camera over Bluetooth, on its HDMI output, or on its LCD while live view is off; GUIDE, GRID and SAFE AREA are the camera's own; grid and safe area need their HDMI switch on in its monitor menu (see Display tools). Clean feed greys out the others. There is no LUT button: the BMPCC6K's LUT can't be switched over Bluetooth, see [docs/LUT_settings_findings.md](docs/LUT_settings_findings.md) |
| VIEW button (under the settings icon) | *LITE* on green, or *PRO* on violet | Switches; LITE leaves out the slate and codec/quality and spreads the rest of the bottom bar, larger, over the whole width |
| Slate (bottom left) | The camera's slate as a clapperboard: production, reel, scene, take, director, camera operator, INT/EXT, DAY/NIGHT, shot type, take tag | Tap: scene, take, INT/EXT, DAY/NIGHT; hold: [slate](#slate) page |
| Film icon (right of the slate) | Recording format: codec and quality, resolution, dynamic range. While the camera's display LUT is on it reports Film as Video too, so then the box says *FILM/VIDEO* | Menu, RECORD tab |
| PRESET button | *Preset Menu*, or the name of the loaded preset | [Presets](#presets) |
| Record button | Red ring while recording | Start / stop |
| Card slots 1–3 | A row each: the medium as a small card (CFast, SD, and SSD for the drive on USB-C), then the recording time left, or *No Card* / *No Drive*.  | |
| ▶ (right of the record button) | Play clip | Plays the camera's clips: the playback panel |
| FOCUS, AF | Focus slider from NEAR to FAR, autofocus button | [Focus](#focus) |


### Presets

The PRESET button opens the presets page where 12 presets can be managed and renamed. A preset holds ISO, shutter, frame rates (with off-speed), white balance and tint, iris, codec and quality, and resolution. Loading one sets the quality within the camera's codec, and the resolution together with the frame rates; a preset from the other codec (BRAW
or ProRes), or a ProRes resolution, says so, as the camera only changes those on its own menu.

**SAVE TO SD** writes all presets to a microSD card in the Tab5, as `MPLIVE/PRESETS.TXT` (readable
JSON, with codec, quality and resolution by their menu names, e.g. `"BRAW"`, `"8:1"`, `"6K"`). The card must be FAT32: cards above 32 GB come as exFAT and need formatting as FAT32 first. 

### Slate

The slate at the bottom left shows the camera's slate like a real clapperboard: production, reel,
scene and take, director and camera operator (only when set), then interior/exterior, day/night,
shot type and take tag. Short press for on screen simple settings. Long press for all details related to the slate.
The reel and the clip the
slate is for are shown but can only be changed on the camera.

### Focus

The focus slider starts at 50 % on every connection: that is where the lens is. Moving the slider
moves the lens by the same amount towards NEAR or FAR. **AF** runs the camera's one-shot autofocus.

### Zoom

Spread two fingers on the picture to zoom in 2x on the spot between them, for checking focus.
While zoomed, drag one finger over the picture to move the zoomed area; pinch two fingers or tap the
yellow **2x** badge under the frame icon to zoom out.

### Display tools

Frame guides (2.40:1 … 1:1 and mask opacity), grid (thirds, centre cross or both) and safe area
(5–100 %) are the camera's own, in its HDMI picture: the GUIDE, GRID and SAFE AREA buttons and their
tabs switch and set them, and the buttons show the camera's state. Backlight brightness is the
Tab5's own, kept after power-off.

The camera draws grid and safe area only on outputs where they are switched on in its monitor menu,
and that switch can't be reached over Bluetooth: switch Grid and Safe Area on for HDMI on the camera
once. The Tab5 then switches them by setting the grid type to none and the safe area to 0 % and
back, and reminds of that switch each time one goes on. Frame guides it switches directly.

## Not available over Bluetooth

The camera does not send card names, capacity and formatting, audio levels, custom white balance,
zebra, focus assist, false colour or playback over Bluetooth, so the Tab5 cannot show them. It
reports its LUT but takes no command that switches it on or off.

## Configuration

`pio run -t menuconfig` / `idf.py menuconfig` → *MagicPilot LIVE*

| Option | Default | Meaning |
|---|---|---|
| Preferred capture width/height/fps | 1280x720 @ 30 | Requested from the adapter (60 fps lowers lag) |
| Scaling mode | Fill | *Fill* covers the screen, *Fit* letterboxes |
| Landscape orientation | 90° | 270° if the picture is upside down |
| Turn touch input by 180 degrees | off | If taps land on the opposite side |
| Swap red/blue of MJPEG | off | If colours look wrong |
| Grid line width / Display backlight | 2 px / 100 % | |
| Performance → Frame pacing | Auto | Even frame cadence |
| Performance → Render in bands through internal SRAM | on | ~2x faster render |

## Picture, frame rate and lag

- The adapter's best format is used: MJPEG, preferably 1280x720 (1:1 on the screen), up to
  1920x1080. Hot-plug works; without a picture the camera UI shows on black.
- Measured with 1280x720 @ 30 MJPEG and the camera connected: 30.0 fps shown, no frames dropped
  (a few while a panel is open); decode ~16 ms, render with the camera UI ~26–29 ms.
- Most of the lag is the capture adapter (cheap ones buffer 1–3 frames). Capturing at 60 fps lowers
  it even though about 30 fps are shown; a lower frame rate does not.

## How it works

```
HDMI ─► capture adapter ─► USB ─► JPEG decoder ─► GDMA bands → SRAM ─► camera UI ─► PPA rotate ─► display
BMPCC6K ◄─ Bluetooth LE ─► ESP32-C6 (controller) ◄─ SDIO, ESP-Hosted ─► ESP32-P4 (Bluedroid host)
```

- **Bluetooth.** The ESP32-P4 has no radio: the Tab5's ESP32-C6 runs the controller, connected over
  SDIO with `espressif/esp_hosted` 2.12.13. The C6's factory firmware (ESP-Hosted 1.4.1) works as
  it is; `sdkconfig.defaults` sets the C6 reset to active-high, as the Tab5 profile's active-low
  keeps the C6 in reset. Writes go through a queue that keeps the newest value of a dragged slider.
- **Picture.** The PPA reads PSRAM slowly, so the GDMA copies 32-row bands into internal SRAM, both
  cores composite the camera UI into each band there, and the PPA rotates it onto the portrait panel.
- **Camera UI.** `ui_draw.c` draws into a 1280x720 canvas and `overlay.c` run-length encodes the
  changed rows, so compositing costs ~2–3 ms per frame and only changed parts are redrawn.
- **Presets** are one blob in NVS. LOAD steps the iris as MagicPilot_Remote does: one ordinal step
  (parameter 0.4), wait for the camera's aperture report, repeat until the stored aperture is
  reached or passed.
- **Focus** sends parameter 0.0 as an offset (the BMPCC6K does not report the lens position);
  offsets queued during a drag add up. **AF** sends parameter 0.1.
- **Tab5 battery:** INA226 at 0x41 on the internal I2C bus, 2S pack, 3.3–4.15 V per cell.
- **Vendored:** `components/usb_host_uvc` (2.6.0 with fixed payload reassembly for MacroSilicon
  adapters) and `components/m5stack_tab5_noglib` (BSP 1.3.2 with a 60 Hz panel refresh option).

## Development

| Path | Content |
|---|---|
| `main/main.c` | Capture, decode, band renderer, pacing |
| `main/cam_ble.c` | Bluetooth link, pairing, write queue |
| `main/cam.c`, `cam_fmt.c` | Camera state, packets, commands, value formatting |
| `main/ui.c`, `ui_draw.c` | Touch and actions; layout, drawing and hit testing |
| `main/overlay.c`, `gfx.c`, `fonts.c` | Canvas and compositing, drawing primitives, fonts |
| `main/tab5_bat.c` | Tab5 battery |
| `docs/BT_findings.md` | Every value the BMPCC6K reads and accepts over Bluetooth, documented or not |
| `docs/LUT_settings_findings.md` | Findings for later work: switching the camera's LUT |
| `tools/preview/` | Desktop preview: `render.py <image> <out_dir>` writes PNGs of every screen |
| `tools/check/` | `check.py` checks a change on the computer in seconds: every preview screen, what a touch finds in each, and every Bluetooth packet of the camera commands, against the references in `tools/check/ref` (`--update` takes the current results; changed screens go to `tools/check/out/changed` as before, after and changed pixels) |

Fonts: Open Sans (SIL Open Font License 1.1), converted by `tools/gen_fonts.py`. The serial log
prints the frame rate and stage times every 5 s, the link state and the camera's packets.

## Disclaimer

⚠️ Use at your own risk. MagicPilot LIVE is provided "as is" without warranty. The developer is
not responsible or liable for damage, malfunction, loss of footage, data loss, or any other loss or
damage to cameras, lenses, recording media, accessories, equipment, or property resulting from the
use of this software or firmware. Always verify settings, compatibility, and operation before using
it with valuable or professional equipment.

MagicPilot LIVE is an independent open-source project and is not affiliated with, endorsed
by, or sponsored by Blackmagic Design Pty. Ltd. Blackmagic Design, Blackmagic Pocket Cinema Camera,
BMPCC, and related names and trademarks are the property of Blackmagic Design Pty. Ltd.
