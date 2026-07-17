# Nvidia Jetson Orin NX + Framos FSM:GO IMX900C VR-Passthrough System

### This project utilizes one Nvidia Jetson Orin NX (8GB) and two Framos FSM:GO sensors to create a VR-passthrough/AR system.

| Folders | Purpose |
| :--- | :--- |
| `Installer` | Automatically initializes the cameras and installs all components required for the Jetson to operate both cameras. |
| `VR_passthrough` | Contains the C++ source files and Makefile to build and run the passthrough system. |

## Purpose of the repository
This project originated from the idea of creating a custom VR headset for a fursuit, allowing the wearer to see the outside world with a heads-up display (HUD). Since I could not find a suitable existing solution, I developed this setup to ensure low latency and "out-of-the-box" functionality.

## Why I chose the Framos sensors
The decision was based on several key factors:
- Excellent low-light performance when used with infrared sensors.
- Global shutter instead of rolling shutter.
- Sony Pregius S (4th Gen) global shutter sensor.
- 4:3 sensor format for maximum field-of-view (FOV) utilization.
- 2064x1552 pixel resolution, providing 1440x1440 resolution per eye.
- High frame rates: 125 FPS at 8-bit and 117 FPS at 10-bit.
- Industrial-grade camera performance: Stable operation for days or weeks without dropped frames.
- Dedicated Framos software support for Jetson platforms.

## Folder Structure
📂 [jetson-orin-nx-framos-builder](./)  
├── 📁 [installer](./installer)  
│&emsp;&emsp;└── 📁 [playbooks](./installer/playbooks)  
│&emsp;&emsp;└── 📁 [data](./installer/data)  
├── 📁 [vr_passthrough](./vr_passthrough)  
├── 📁 [assets](./assets)  
│&emsp;&emsp;└── 🖼️ [orin_nx_flash_img1.png](./assets/orin_nx_flash_img1.png)  
│&emsp;&emsp;└── 🖼️ [orin_nx_flash_img2.png](./assets/orin_nx_flash_img2.png)  
│&emsp;&emsp;└── 🖼️ [orin_nx_flash_img3.png](./assets/orin_nx_flash_img3png)  
│&emsp;&emsp;└── 🖼️ [orin_nx_settings.png](./assets/orin_nx_settings)  
└── 📄 [README.md](./README.md)

## How to contribute
If you are using a different Framos sensor, feel free to fork this repository and add support for your specific module. Contributions to diversify support for additional carrier boards, modules, and sensors are highly appreciated!