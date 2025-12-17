## Overview

The [audio driver](https://github.com/tuya/TuyaOpen/tree/master/src/peripherals/audio_codecs) is a core component within TuyaOpen responsible for handling audio input and output. It provides a unified interface to manage different types of audio devices, such as microphones and speakers. Through this driver, applications can easily perform audio capture, playback, and configuration without needing to manage the specific implementation details of the underlying hardware.


## Audio connection architecture

The audio connection architecture varies depending on the main controller chip. For example, the `T5AI` includes built-in ADC and DAC interfaces and can implement an audio system without a codec chip. In contrast, the `ESP32-S3` does not support DAC and requires an external codec chip to build an audio system.


## Functional modules

TuyaOpen aims to deliver a standardized, platform-agnostic audio solution. Its core design philosophy centers on layered decoupling, effectively separating the audio requirements of the application layer from the specific hardware implementations at the underlying level.

* **For application developers**: Regardless of whether the underlying hardware utilizes the T5AI chip or another chip's audio codec, the application layer only needs to invoke a single set of unified, standardized APIs (the `tdl_audio_xxx` function series), such as `tdl_audio_open` and `tdl_audio_play`. This significantly reduces development complexity and enhances code portability.
* **For driver developers**: When adding support for a new audio chip, developers simply need to adhere to the standard interface defined in `tdl_audio_driver.h` to write a new TDD-layer driver (similar to `tdd_audio.c`), and then register it with the TDL management layer. This process requires no modifications to any application-layer code.

#audiodriver, #driver

### Abstract management module (Tuya Driver Layer - TDL)

This is the highest level of abstraction, providing unified audio service interfaces to the application layer.

* `tdl_audio_manage.c/h`: Implements the core logic for audio driver management. It maintains a linked list for registering and managing audio device drivers of different types (or for different platforms). Applications use functions like `tdl_audio_find` and `tdl_audio_open` to access audio functionality without being concerned with the underlying implementation details.
* `tdl_audio_driver.h`: Defines the standardized interface (`TDD_AUDIO_INTFS_T`) that all audio device drivers must adhere to. This includes function pointers for operations like `open`, `play`, `config`, and `close`. This ensures that `tdl_audio_manage` can uniformly interact with any underlying driver that conforms to this standard.


### Instantiation & registration module (Tuya Device Driver - TDD)

This is the driver's intermediate layer, containing the concrete implementations for specific hardware platforms.

`tdd_audio.c/h`: Implements the audio driver for different platforms. It acts as a bridge, fulfilling the `TDD_AUDIO_INTFS_T` standard interface defined by the upper TDL layer, while calling the TKL layer or the hardware abstraction interfaces provided by the chip vendor to control the actual hardware. The `tdd_audio_register` function registers this driver's implementation (function pointers) with the TDL layer.