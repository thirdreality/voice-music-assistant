# Voice&Music Assistant

<div align="center">
  <img src="doc/images/voice-music-speaker.jpg" alt="voice-music-speaker" width="300">
</div>

ThirdReality Voice&Music Assistant is an open-source speaker that supports connecting to the Home Assistant Voice Assistant and Music Assistant. You need to have a device running Home Assistant in order to use this speaker. If you do not have Home Assistant installed yet, refer to the [installation documentation](https://www.home-assistant.io/installation/) for instructions. [Buy it on ThirdReality Shop](https://thirdreality.com/product/voice-music-assistant-dev-edition/)


## Architecture

The firmware consists of two main application components:

- **Voice** — built on [linux-voice-assistant-cpp](buildroot/package/thirdreality/linux-voice-assistant-cpp/), a C++ rewrite of [OHF-Voice/linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant.git). Implements the ESPHome native API so Home Assistant discovers the speaker as a voice satellite. See its [README](buildroot/package/thirdreality/linux-voice-assistant-cpp/README.md) for details.
- **Music** — built on [Sendspin](https://www.sendspin-audio.com/), a lightweight streaming protocol integrated with [Music Assistant](https://www.music-assistant.io/).

<div align="center">
  <img src="doc/images/button_functions.png" alt="button-functions" width="600">
</div>

---

- [Voice\&Music Assistant](#voicemusic-assistant)
  - [Architecture](#architecture)
  - [Build](#build)
    - [Docker Build](#docker-build)
    - [Native Build](#native-build)
  - [Flash](#flash)
  - [Debugging](#debugging)
    - [Serial](#serial)
    - [ADB](#adb)
    - [SSH](#ssh)
  - [Set up the voice assistant](#set-up-the-voice-assistant)
  - [Setup through HA APP](#setup-through-ha-app)
  - [Smart Home control with voice](#smart-home-control-with-voice)
  - [Smart Home control with button](#smart-home-control-with-button)
  - [Play Music](#play-music)
  - [Multi-Room Music](#multi-room-music)
    - [Work with Apple HomePod](#work-with-apple-homepod)
    - [Work with Sonos](#work-with-sonos)
  - [License](#license)

---

## Build

Clone the repository:
```bash
git clone https://github.com/thirdreality/voice-music-assistant.git
cd <YOUR PATH>/voice-music-assistant
git submodule update --init
```

### Docker Build

No host dependencies required other than Docker.

```bash
./go --docker trspk <version>          # Build inside Docker (recommended)
./go --docker-shell                    # Enter container interactively for debugging
./go --docker trspk rebuild <package>  # Rebuild a single package in Docker
```

### Native Build

Requires:
  - Ubuntu 20.04

Install dependencies:
```bash
sudo apt-get update

sudo apt-get install -y build-essential bash bc binutils build-essential bzip2 cpio g++ gcc git gzip locales libncurses5-dev libdevmapper-dev libsystemd-dev make mercurial whois patch perl python rsync sed tar vim unzip wget bison flex libssl-dev libc6:i386 libncurses5:i386 libstdc++6:i386 zlib1g-dev:i386 zip python3-pip pkg-config automake gsettings-ubuntu-schemas libglib2.0-dev gcc-multilib g++-multilib

pip install pycrypto

wget http://ftp.cn.debian.org/debian/pool/main/a/automake-1.16/automake_1.16.1-4_all.deb && sudo dpkg -i automake_1.16.1-4_all.deb && rm -f automake_1.16.1-4_all.deb
```

Build:
```bash
./go trspk <version>               # If no version number is specified, the date will be used
./go trspk rebuild <package>       # Rebuild a single package
```

The generated image is located at:
```
<YOUR PATH>/voice-music-assistant/image
```

## Flash
1. Download and extract [Aml_Burn_Tool.zip](https://raw.githubusercontent.com/thirdreality/voice-music-assistant/master/tools/Aml_Burn_Tool.zip)

2. If this is your first time using the tool, click on Setup_Aml_Burn_Tool_V3.1.0.exe to install necessary drivers.

3. Next, navigate to the v2 folder and run Aml_Burn_Tool.exe.

4. Load the compiled **.img firmware file. Or you can download the latest firmware [here](https://github.com/thirdreality/voice-music-assistant/releases).

5. Click on Start to initiate the burn process.

<div align="left">
  <img src="doc/images/usb_burnning_tool.png" alt="usb_burnning_tool" width="400">
</div>

6. Use debug board to connect the speaker to the PC. If you don’t have a debug board, you can use a Type-C data cable. Make sure to use a data cable. Then power it on.

<div align="left">
  <img src="doc/images/device_connect.jpg" alt="device-connect" width="400">
</div>


## Debugging

### Serial

1. Use debug board to connect the speaker to the PC. Make sure to use data cables.

2. Open your serial debugging tool, select the corresponding port, and set the baud rate to 115200.

<div align="left">
  <img src="doc/images/serial-debug.png" alt="serial-debug" width="400">
</div>

### ADB

ADB is enabled on development images and starts automatically at boot
(`/etc/init.d/S55adbd`). It listens over **USB** and, by default, also over
**TCP** on port `5555`.

> **Warning:** the TCP socket binds to `0.0.0.0` with no authentication.
> Anyone on the same network can obtain a root shell. To disable TCP, set `ADB_TCP_PORT=` in `/etc/default/adbd`
> (USB stays available).

**USB** — connect a Type-C data cable to the PC:

```bash
adb devices
adb shell
```

**TCP** — over the network (device and PC on the same LAN):

```bash
adb connect <device-ip>:5555
adb shell
```

**Multiple devices** — when more than one device is connected, `adb shell`
fails with `more than one device/emulator`. You must first list the devices and
then target one explicitly with `-s <serial>`. The serial differs by transport:

- **USB** devices are listed by their **MAC address** (for example `a1b2c3d4e5f6`).
- **TCP** devices are listed as **`<ip>:5555`** (for example `10.1.0.33:5555`).

```bash
$ adb devices
List of devices attached
a1b2c3d4e5f6    device        # USB, serial is the MAC address
10.1.0.33:5555  device        # TCP, serial is <ip>:5555

# pick the USB device by its MAC address
adb -s a1b2c3d4e5f6 shell

# pick the TCP device by its <ip>:5555
adb -s 10.1.0.33:5555 shell
```

Note: the same speaker connected over both USB and TCP shows up as two separate
entries with different serials.

### SSH

```bash
ssh root@<device-ip>
```

- Username: `root`
- Password: `hello3r`

## Set up the voice assistant

Speech-to-text (STT) and text-to-speech (TTS) can run in three places.
For the first two, set up the service and then add an assistant under
**Settings → Voice Assistants**.

- Home Assistant Cloud

  Easiest; needs a Home Assistant Cloud subscription (30-day free trial).
  Open the Home Assistant app or the Home Assistant web interface, Go to **Settings → Home Assistant Cloud**. Create or log in to your account.
  <div align="left">
    <img src="doc/images/ha-cloud-1.png" width="10%">
    <img src="doc/images/ha-cloud-2.png" width="10%">
  </div>

- Local voice on your Home Assistant server

  Wyoming add-ons on your Home Assistant host. Needs a reasonably fast
  host. Please refer to:

  <https://github.com/rhasspy/wyoming-piper>

  <https://github.com/rhasspy/wyoming-faster-whisper>

- Speech on the speaker (3R Speaker)

  STT (whisper.cpp) and TTS (the Piper "amy" voice, via sanoTTS) run on
  the speaker; no extra service is needed. Home Assistant still
  understands the command (intents) and runs the action, so the speaker
  must stay connected to Home Assistant. English only. Replies start
  after a short pause: the speaker synthesizes the whole reply first,
  which takes about as long as the reply itself.

  1. In Home Assistant, go to **Settings → Devices & services → ESPHome**,
     click **Configure** on the speaker and turn on **Allow the device to
     perform Home Assistant actions**. Without it the speaker gets no
     reply and no notifications are shown.
  2. On the device page, under **Configuration**, set **Speech
     Processing** to **3R Speaker**.
  3. The first time, the speaker downloads a 31 MB speech model (it
     starts downloading at boot, so it is often ready already). Until
     then it keeps using Home Assistant for voice, and a notification
     shows the progress. When the download finishes the speaker switches
     by itself; voice is unavailable for about 10 seconds while it
     restarts. If the download fails, **Speech Processing** goes back to
     **Home Assistant**; pick **3R Speaker** again to retry.
  4. Optional: **Speech Rate (Local TTS)** changes how fast the speaker
     talks (165 = normal). It only applies to 3R Speaker; with Home
     Assistant the voice is set in the assistant settings.

  Set **Speech Processing** back to **Home Assistant** at any time to use
  the Home Assistant voice pipeline again.

---

## Setup through HA APP

1. You need to install the iOS or Android version of the [Home Assistant app](https://companion.home-assistant.io/) first. And please make sure the app is up to date.
2. Make sure the speaker is in a yellow blinking state. Otherwise, please try factory reset. (Press and hold the Home button for 15 seconds, then release it after you hear the prompt sound)
3. Open the Home Assistant app on your phone. Go to **Settings → Devices & services** and under Discovered, you should see the device as **"3RSPK-XXXXX Improv via BLE"**. (If the device is not found, please check whether Bluetooth and Nearby Devices permissions are enabled in the app)

<div align="left">
  <img src="doc/images/setup-1.png" width="10%">
</div>

4. Enter your Wi-Fi SSID and password. Only 2.4 GHz networks are supported.

<div align="left">
  <img src="doc/images/setup-2.png" width="10%">
  <img src="doc/images/setup-3.png" width="10%">
</div>

5. A few seconds after the Wi-Fi connection is successful, the speaker will play "Your device is ready to connect to Home Assistant." Go to **Settings → Devices & services** and under Discovered, you should see the device as **"3RSPK-XXXXXXXXXXXX ESPHome"**.

<div align="left">
  <img src="doc/images/setup-4.png" width="10%">
</div>

6. Add device

<div align="left">
  <img src="doc/images/setup-5.png" width="10%">
  <img src="doc/images/setup-6.png" width="10%">
  <img src="doc/images/setup-7.png" width="10%">
  <img src="doc/images/setup-8.png" width="10%">
  <img src="doc/images/setup-9.png" width="10%">
</div>

7. Select the voice assistant you created in step 1 (Setup the voice assist)

<div align="left">
  <img src="doc/images/setup-10.png" width="10%">
</div>

Now you can try waking the device with **"OK Nabu"** and start a conversation. You can check the device status in **Settings → Devices & Services → ESPHome**.

<div align="left">
  <img src="doc/images/setup-11.png" width="10%">
  <img src="doc/images/setup-12.png" width="10%">
</div>

---

## Smart Home control with voice

Supported voice commands: <https://www.home-assistant.io/voice_control/builtin_sentences/>

- For example, *"What's the time"* or *"Turn on the light in the living room"*.
- Make sure you're using the area name exactly as you defined it in Home Assistant.

Is the device you want to control via Assist (for example a specific light) not responding to your voice commands? Make sure the device is exposed to Assist:
<https://www.home-assistant.io/voice_control/voice_remote_expose_devices/>

---

## Smart Home control with button

We can create automation scripts based on the speaker's Home button trigger events to control devices. Supports single-click, double-click, and triple-click actions.

**Settings → Devices & Services → ESPHome → Your device → Automations**

<div align="left">
  <img src="doc/images/button-control-1.png" width="30%">
  <img src="doc/images/button-control-2.png" width="30%">
  <img src="doc/images/button-control-3.png" width="30%">
  <img src="doc/images/button-control-4.png" width="30%">
  <img src="doc/images/button-control-5.png" width="30%">
</div>

---

## Play Music

We can use [Music Assistant](https://www.home-assistant.io/integrations/music_assistant/) to play music.

> **Note:** current firmware needs Music Assistant **2.10.7 or newer**
> (or beta 2.11.0b4 or newer). Older versions cannot connect to the
> speaker.

**Settings → Add-ons** → search for and add **Music Assistant**. After adding it, you can access it via port 8095.

If it doesn’t exist, please add it using [Music Assistant github repository](https://github.com/music-assistant/home-assistant-addon.git)

<div align="left">
  <img src="doc/images/music-1.png" width="30%">
</div>

Go to the Music Assistant: **Settings → Player Providers → Add a player provider**. Search for Sendspin and add it.

<div align="left">
  <img src="doc/images/music-2.png" width="30%">
</div>

Then you can go to **Settings → Players** and find the player named 3RSPK-XXXXXXXXXXXX. 

<div align="left">
  <img src="doc/images/music-3.png" width="30%">
</div>

If it doesn’t show up, try reloading Sendspin.

<div align="left">
  <img src="doc/images/music-4.png" width="30%">
</div>

Next, we need to add music sources. Go to **Settings → Music Sources → Add a music source**, then select your desired music source.

<div align="left">
  <img src="doc/images/music-5.png" width="30%">
</div>


Finally, select a song and the speaker, and you can start playback! 🎵

<div align="left">
  <img src="doc/images/music-6.png" width="30%">
</div>

---

## Multi-Room Music

Official documentation: https://www.music-assistant.io/faq/groups/#groups

We use [Sendspin](https://www.sendspin-audio.com/) as the playback protocol, so it can work with any device that supports AirPlay.

### Work with Apple HomePod

Go to the Music Assistant: **Settings → Player Providers → Add a player provider**. Search for and add AirPlay and Sync Group Player.

Then you can create a sync group player with your thirdreality speaker and HomePod.

<div align="left">
  <img src="doc/images/music-7.png" width="30%">
  <img src="doc/images/music-8.png" width="30%">
  <img src="doc/images/music-9.png" width="30%">
</div>

### Work with Sonos

Go to the Music Assistant: **Settings → Player Providers → Add a player provider**. Search for and add SONOS and Universal Group Player.

<div align="left">
  <img src="doc/images/music-10.png" width="30%">
</div>

Then you can create a universal group player with your thirdreality speaker and Sonos.

<div align="left">
  <img src="doc/images/music-11.png" width="30%">
  <img src="doc/images/music-12.png" width="30%">
  <img src="doc/images/music-13.png" width="30%">
</div>

## License

The source code in this repository is licensed under the
[Apache License 2.0](LICENSE). The firmware also includes third-party
components under their own licences, including GPL-3.0-or-later
components (eSpeak-NG and the sanoTTS runtime used for on-device speech).
See
[THIRD_PARTY_NOTICES.md](buildroot/package/thirdreality/linux-voice-assistant-cpp/THIRD_PARTY_NOTICES.md);
on the device the notices are in `/usr/share/thirdreality/licenses/`.
Buildroot's `make legal-info` lists the licences of every package in the
image.
