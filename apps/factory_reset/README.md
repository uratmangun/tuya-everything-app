# Factory Reset Application

This application performs a **factory reset** on your Tuya DevKit board, clearing all activation data.

## What it does

1. Initializes the Tuya IoT client
2. Calls `tuya_iot_reset()` to trigger a factory reset
3. Clears all activation data from the device
4. Device will need to be re-paired through the Tuya Smart Life app

## How to Build and Flash

### Step 1: Set up the environment

```bash
cd /home/uratmangun/CascadeProjects/tuya-ai-recognition/TuyaOpen
source export.sh
```

### Step 2: Build the factory reset app

```bash
tos build apps/factory_reset
```

### Step 3: Flash to your device

Make sure your device is connected via USB, then:

```bash
tos flash apps/factory_reset
```

### Step 4: Monitor the output

```bash
tos monitor
```

You should see output like:
```
========================================
    TUYA DEVKIT FACTORY RESET
========================================

Starting factory reset process...
Device is currently activated.
Initiating factory reset...
Factory reset command sent successfully!

========================================
    FACTORY RESET COMPLETED!
========================================

Device has been reset to factory defaults.
All activation data has been cleared.

Next steps:
1. Open Tuya Smart Life app
2. Add the device again
3. Follow the pairing instructions
```

## After Reset

After the factory reset completes:

1. **Power off** the device
2. Open the **Tuya Smart Life** app on your phone
3. Tap **Add Device** (+)
4. Select your device type
5. Follow the on-screen pairing instructions
6. Flash your actual application firmware

## Alternative: Hardware Reset

If you prefer not to flash, you can also reset by:
- **Restart the device 3 times in quick succession** (for T5AI boards)

## Troubleshooting

- If the reset doesn't complete, try power cycling the device
- Make sure the device is properly connected via USB
- Check that the correct serial port is being used
