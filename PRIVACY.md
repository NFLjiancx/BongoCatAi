# Privacy Policy

## Introduction

**BongoCat** is a desktop application that displays an interactive desktop
pet and responds to local keyboard, mouse, and gamepad input. We value your
privacy. This policy explains what BongoCat does and does not collect, and
how information is stored.

## Data Collection and Usage

### Information We Do **Not** Collect

BongoCat does not:

* Create accounts, unique user identifiers, trackers, or profiling systems
* Collect usage statistics, telemetry, analytics, or advertising data
* Upload keyboard, mouse, gamepad, model, configuration, or system data
* Record the content of your keystrokes or input events for transmission
* Sell or share personal data with third parties

Keyboard, mouse, and gamepad input is processed locally to animate the pet and
is not sent to a remote service. The optional voice assistant is the only
feature that transmits user content off the device, and only after you enable
it explicitly. See [Voice Assistant](#voice-assistant) below.

On Linux, experimental Wayland evdev monitoring is disabled by default and
requires explicit opt-in with `BONGOCAT_ENABLE_EVDEV=1`. When enabled and
device permissions allow it, raw keyboard events can include input in other
applications and password fields. Hiding the pet does not stop this listener,
and it does not detect screen locking or session switching. See
[Linux Input in SECURITY.md](SECURITY.md#linux-input) before enabling it.

### Locally Stored Data

BongoCat stores application data only on your device. The exact platform
locations are:

* **Windows:** `%LOCALAPPDATA%\BongoCat\` (or the private package storage for
  the Microsoft Store/MSIX version)
* **macOS:** `~/Library/Application Support/BongoCat/`, with caches and logs
  in the corresponding macOS cache and log directories
* **Linux:** the XDG configuration, data, cache, and state directories for
  `bongocat` (normally under `~/.config`, `~/.local/share`, `~/.cache`, and
  `~/.local/state`)

The following categories may be stored locally:

1. **Settings**
   User preferences, window and input options, selected models, shortcuts,
   labels, and other application settings are stored in `config/settings.json`.

2. **Session and runtime state**
   Temporary session information and startup state are stored under the local
   state directory. These files support normal operation and crash recovery.

3. **Logs**
   Diagnostic messages, startup and shutdown records, warnings, errors, and
   limited platform information may be written to `logs/BongoCat.log`.
   Logs are intended for local troubleshooting only and are never uploaded by
   BongoCat.

4. **Models and user resources**
   Built-in or imported model packages, behavior definitions, sounds, fonts,
   and other resources are stored in the local data and models directories.
   BongoCat does not transmit these files.

5. **Voice assistant settings and credentials**
   When the optional voice assistant is used, the `ai` section of
   `config/settings.json` stores only non-secret preferences: enabled state,
   privacy consent, and the speech recognition, speech synthesis and assistant
   model names. Your DashScope API key is stored in a
   separate `dashscope.key` file in the configuration directory with
   owner-only permissions, and the latest synthesized reply audio is cached as
   `tts-last.wav`. The key is never written to settings, logs, or any file
   other than `dashscope.key`, and is only sent to the DashScope service as
   request authentication when the assistant is enabled.

## Network Access

BongoCat does not require a network connection for normal operation. Network
access may occur in these cases:

* **Update checking on Windows:** When an update check is initiated or
  scheduled, BongoCat sends a basic HTTPS `GET` request to the GitHub Releases
  API endpoint
  `https://api.github.com/repos/vladelaina/BongoCat/releases/latest`.
  The request retrieves release information and does not include personal
  data. The user agent identifies the application as `BongoCat Update
  Checker/1.0`. Automatic update checks are not currently available on macOS
  or Linux.
* **Opening external links:** If you choose a website or feedback link in the
  application, BongoCat asks your operating system to open that URL in your
  default browser. The browser and the destination website then apply their
  own privacy policies.

BongoCat does not use remote images, advertising networks, or analytics
services as part of its normal runtime.

### Voice Assistant

The voice assistant is disabled by default and requires both an API key and an
explicit privacy consent on the AI preferences page. When it is enabled,
BongoCat connects to Alibaba Cloud DashScope over TLS and sends:

* **Microphone audio while you speak:** Short utterances captured from the
  default microphone are streamed for speech recognition
* **Recognized text and conversation turns:** The recognized sentence, the
  assistant's tool calls and their local results, and the reply text are sent
  to `https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions`,
  with live web search enabled, so recent information may be retrieved on your
  behalf
* **Synthesis requests:** The final reply text is sent as a speech synthesis
  request when a spoken reply is produced

No audio is transmitted while the assistant is disabled, and your microphone
audio is never written to disk: utterances are processed in a bounded
in-memory buffer.
The assistant runs inside BongoCat: when it asks to play a behavior, show or
hide the pet, read or write a file, or run a shell command, that work happens
locally on your device with your user's permissions, and only the request and
its textual result are shared with DashScope as conversation context.

## Data Protection and Deletion

Application data remains on your device and is not synchronized by BongoCat.
You can remove it by closing BongoCat and deleting its platform-specific
configuration, data, cache, state, and log directories. Deleting these files
also removes your settings, imported models, local logs, and stored voice
assistant preferences and API key file.

## Permission Usage

Permissions used by BongoCat are limited to core features:

* **File system access:** Reading and writing settings, session state, logs,
  models, and user-provided resources
* **Keyboard, mouse, and gamepad access:** Detecting local input so the pet can
  react and follow the configured behavior
* **Network access:** Checking GitHub for release information on Windows when
  update checking is used, and the DashScope speech service when the optional
  voice assistant is enabled
* **Microphone access:** Only while the optional voice assistant is enabled,
  to stream short utterances for speech recognition
* **Opening external URLs:** Only when you select a link in the application
* **Startup integration:** Only when you enable a start-with-system option,
  where supported

## Changes to This Policy

If this policy changes materially, the updated version will be published on
the BongoCat project homepage or repository.

## Contact

For questions or suggestions about this privacy policy, please use the GitHub
project page: <https://github.com/vladelaina/BongoCat>.

> Last Updated: August 28, 2026
