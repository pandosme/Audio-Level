# Audio-Level

Audio-Level is a modern ACAP solution for Axis cameras with audio input. It monitors real-time dB-levels, dynamically tracks ambient sound, and publishes robust MQTT events for easy third-party integration and advanced automation.

***

## Key Features
- **Live audio level monitoring (in dBFS)**
- **Automatic ambient/background noise measurement**
- **Supports adaptive (relative-to-ambient) and fixed thresholds**
- **Visual audio graph: current, ambient, and threshold lines**
- **MQTT event publishing for easy automation**
- **Configurable from built-in web interface**

***

## Prerequisites
- Axis device with AxisOS 12.6 or newer and an audio input (integrated mic, external mic, or line-in)
- MQTT broker with WebSocket support
- Modern web browser (for the frontend UI)

***

## Pre-compiled download

If you want a ready-to-go version, [Download Latest ZIP](https://www.dropbox.com/scl/fi/n9r2fxqetlj26p86sawf8/audio-level.zip?rlkey=aftk6v00azstcq5geoh6hp90u&dl=1) 
The ZIP contains pre-built binaries (armv7hf, aarch64) and this documentation.  
**Tip:** Check for updates regularly to benefit from new features and bugfixes!

***

If you find Audio-Level valuable, please consider [buying me a coffee](https://buymeacoffee.com/fredjuhlinl) ☕.

***

## Usage

1. **Install the ACAP** via the Axis camera web interface or VAPIX.
2. **Configure your preferred audio input** in the camera's Audio Device settings.
3. **Point the ACAP to your MQTT broker** in the UI.
4. **Review and adjust settings** for your environment in the Audio-Level UI.
    - Set **interval** (how often samples are reported)
    - Select **Dynamic** thresholds (relative to ambient) or **Fixed** thresholds (absolute dBFS)
    - Adjust **Alert/Normal** threshold values as needed.

#### More Details
- **Ambient Level:** The application maintains a continuously updated average of background noise (dBFS), using a fast convergence at startup and robust tracking thereafter.
- **Interval:** Controls how frequently the level and events are computed/published (in seconds).
- **Relative (Dynamic) Threshold:** Triggers an alert when the audio exceeds your set dB above the measured ambient level (e.g., 6 dB above ambient = clear event).
- **Absolute (Fixed) Threshold:** Triggers an alert when the level passes a fixed dBFS (e.g., -16 dBFS = loud speech).
- **Hysteresis:** The "normal" value prevents event oscillation—state returns to normal only below this value.

***

## Integration

- **ONVIF/Axis Events:** Fires stateful ONVIF (Generic) Events for above/below threshold transitions.
- **MQTT:** Easy to consume MQTT messages for your automation system.

Example payload:
```
Topic: pipelevel/level/{SERIAL}
{
  "state": true,
  "level": -41.91,
  "ambient": -68.93,
  "serial": "{SERIAL}"
}
```


**Tip:** Use MQTT to trigger notifications, camera actions, or any smart workflow!

***

## Support & Feedback
- For bugs, feature requests, or ideas, please use the Issue tracker or email.
- Want to support continued development? [Buy me a coffee!](https://buymeacoffee.com/fredjuhlinl)

***

## History

### 1.0.0 Oct 5, 2025
- Initial public release

---
