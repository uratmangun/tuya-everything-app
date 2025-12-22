# Tuya Panel MiniApp Development Guide

## Overview

This guide explains how to create a custom Panel MiniApp to customize the UI in the Tuya Smart Life app for your devices. Panel MiniApps use standard web technologies (HTML, CSS, JavaScript) and run inside the Tuya Smart Life app.

> **Note**: As of January 2025, Tuya recommends Panel MiniApp over the deprecated React Native Panel SDK for new projects.

---

## Prerequisites

Before you start, make sure you have:
- ✅ Tuya IoT Developer Platform account ([platform.tuya.com](https://platform.tuya.com))
- ✅ Node.js >= v16.7.0 and npm >= v7.20.3 (or Yarn)
- ✅ Smart MiniApp IDE installed

---

## Step 1: Create/Verify Your Product

1. Go to [platform.tuya.com](https://platform.tuya.com)
2. Navigate to **Products**
3. Create a product or use existing one (e.g., Product ID: `ajuh8dan3i0hf1ob`)
4. Make sure your **Data Points (DPs)** are defined (Switch DP, etc.)

---

## Step 2: Create a Panel MiniApp Project on the Platform

1. Go to [iot.tuya.com](https://iot.tuya.com) → **Smart MiniApp Developer Platform**
   - Or navigate: **App** → **MiniApp** → **Panel MiniApp**

2. Click **Create MiniApp**:
   - **MiniApp Name**: e.g., "My Switch Panel"
   - **MiniApp Type**: Select **Panel MiniApp**
   - Click **Create**

3. Note down your **MiniApp ID** (you'll need it later)

---

## Step 3: Download & Install Smart MiniApp IDE

1. Go to the [Tuya MiniApp Developer Platform](https://developer.tuya.com/en/docs/miniapp)
2. Download the **Smart MiniApp IDE** for your OS (Windows/macOS/Linux)
3. Install and launch the IDE
4. **Log in** with your Tuya Developer Platform account

---

## Step 4: Create Project in the IDE

1. Open Smart MiniApp IDE
2. Click **Create** (or **New Project**)
3. Fill in project details:

| Field | Value |
|-------|-------|
| **Project Path** | Choose a local directory |
| **Project Name** | e.g., `my-switch-panel` |
| **Link MiniApp** | Select the MiniApp you created in Step 2 |
| **Debug Product** | Select your product |
| **Template** | Choose "Panel MiniApp" template |

4. Click **Create**

---

## Step 5: Install Dependencies & Start Dev Server

Open terminal in your project folder and run:

```bash
# Navigate to project
cd my-switch-panel

# Install dependencies
npm install
# OR
yarn install

# Start development server
npm run start:tuya
# OR
yarn start:tuya
```

---

## Step 6: Project Structure

Your project will look something like this:

```
my-switch-panel/
├── app.json           # App configuration
├── app.js             # App entry point
├── pages/
│   └── index/
│       ├── index.js   # Page logic
│       ├── index.json # Page config
│       ├── index.css  # Styles
│       └── index.html # Template (or .ttml)
├── utils/             # Utility functions
├── assets/            # Images, icons
└── package.json
```

---

## Step 7: Write Your Panel Code

### Example: Switch Control Panel

#### `pages/index/index.js`
```javascript
import { device } from '@ray-js/ray';

Page({
  data: {
    switchOn: false
  },

  onLoad() {
    // Get current switch state when page loads
    const dpState = device.getDpState();
    this.setData({
      switchOn: dpState['1'] || false  // DP ID 1 = switch
    });
    
    // Listen for DP changes from device
    device.onDpDataChange((data) => {
      if (data['1'] !== undefined) {
        this.setData({ switchOn: data['1'] });
      }
    });
  },

  // Toggle switch handler
  toggleSwitch() {
    const newState = !this.data.switchOn;
    
    // Send command to device
    device.publishDps({
      '1': newState  // DP ID 1 = switch, value = true/false
    });
    
    // Update UI
    this.setData({
      switchOn: newState
    });
  }
});
```

#### `pages/index/index.html` (or `.ttml`)
```html
<view class="container">
  <view class="header">
    <text class="title">My Custom Switch</text>
  </view>
  
  <view class="switch-container" bindtap="toggleSwitch">
    <view class="switch-icon {{switchOn ? 'on' : 'off'}}">
      <text>{{switchOn ? 'ON' : 'OFF'}}</text>
    </view>
  </view>
  
  <text class="status">
    Switch is currently: {{switchOn ? 'On' : 'Off'}}
  </text>
</view>
```

#### `pages/index/index.css`
```css
.container {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 40px 20px;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  min-height: 100vh;
}

.title {
  color: white;
  font-size: 24px;
  font-weight: bold;
  margin-bottom: 40px;
}

.switch-container {
  width: 150px;
  height: 150px;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
}

.switch-icon {
  width: 120px;
  height: 120px;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: all 0.3s ease;
}

.switch-icon.off {
  background: #ccc;
  box-shadow: 0 4px 15px rgba(0,0,0,0.2);
}

.switch-icon.on {
  background: #4CAF50;
  box-shadow: 0 4px 20px rgba(76, 175, 80, 0.5);
}

.switch-icon text {
  color: white;
  font-size: 20px;
  font-weight: bold;
}

.status {
  color: white;
  margin-top: 30px;
  font-size: 16px;
}
```

---

## Step 8: Debug & Test

### Option 1: Virtual Device (in IDE)
- The IDE has a built-in simulator
- You can test UI and interactions

### Option 2: Real Device
1. Get your device's `deviceId` from the Tuya Smart app
2. Pass it as startup parameter: `deviceId=xxxxx`
3. Or set your MiniApp as trial version and scan QR code

---

## Step 9: Build & Publish

1. In the IDE, click **Build** or **Upload**
2. Your MiniApp is uploaded to Tuya Platform
3. Go to MiniApp management on the platform
4. **Link** your MiniApp to your Product ID
5. Submit for **Review**
6. Once approved → **Publish**

---

## API Quick Reference

| Action | Code |
|--------|------|
| Get all DP states | `device.getDpState()` |
| Get single DP | `device.getDpState()['1']` |
| Set DP value | `device.publishDps({ '1': true })` |
| Listen for DP changes | `device.onDpDataChange(callback)` |
| Get device info | `device.getDeviceInfo()` |

---

## Common DP Types

| Type | Example Value | Usage |
|------|---------------|-------|
| Boolean | `true` / `false` | Switch on/off |
| Value (Integer) | `0-100` | Brightness, volume |
| Enum | `0`, `1`, `2` | Mode selection |
| String | `"hello"` | Text display |
| Raw | `[0x01, 0x02]` | Custom binary data |

---

## Panel Development Options Comparison

| Feature | **Panel Studio** | **Panel MiniApp** | **RN Panel SDK** (Legacy) |
|---------|-----------------|-------------------|---------------------------|
| **Difficulty** | ⭐ Easy | ⭐⭐ Medium | ⭐⭐⭐ Advanced |
| **Coding Required** | No (drag & drop) | Yes (JavaScript/HTML/CSS) | Yes (React Native) |
| **Customization** | Limited | High | Very High |
| **Learning Curve** | None | Low (web dev skills) | Medium (RN knowledge) |
| **Status** | ✅ Active | ✅ Recommended | ⚠️ Legacy (no new projects) |

---

## Useful Links

| Resource | URL |
|----------|-----|
| MiniApp Documentation | https://developer.tuya.com/en/docs/miniapp |
| Smart MiniApp IDE Download | Available in Tuya Developer Platform |
| MiniApp API Reference | https://developer.tuya.com/en/docs/miniapp/api |
| Sample Code (GitHub) | https://github.com/tuya/tuya-miniapp-demo |
| Codelabs/Tutorials | https://developer.tuya.com/en/docs/miniapp/codelabs |
| Tuya IoT Platform | https://platform.tuya.com |

---

## Troubleshooting

### MiniApp not showing in app
- Ensure MiniApp is linked to your Product ID
- Check if MiniApp is published or set as trial version
- Verify your account is in the whitelist for trial

### Device not responding
- Check your DP IDs match between code and product definition
- Ensure device is online and connected
- Verify network connectivity

### Build errors
- Run `npm install` to ensure all dependencies are installed
- Check Node.js version (>= v16.7.0)
- Clear cache and rebuild

---

*Created: December 17, 2025*
*Related: TuyaOpen switch_demo, T5AI DevKit*
