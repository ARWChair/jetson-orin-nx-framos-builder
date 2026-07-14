# Nvidia Jetson Orin NX + Framos FSM:GO IMX900C VR-passthrough system

### This project uses one Nvidia Jetson Orin NX 8GB and 2 Framos FSM:GO to create a VR-passthrough/AR system.


| Folders | Purpose |
| :--- | :--- |
| `Installer` | Automatically instantiates the cameras and sets up all components required for the Jetson to run both cameras. |
| `VR_passthrough` | Contains the C++ source files and Makefile to build and run the passthrough system. |

## Whats the purpose of the repo 
This project originated from the Idea to create a fursuit with a 2 cam vr headset custom made to look outside of the world and to to have a funny HUD. Since I didnt find any I had to create a simple solution that is low latency and works out of the box.
<br></br>

## Why have I choosen the Framos sensors
The decission wasn't made purely based on a single factor. It was a based on multiple reasons.
- Good low light performance, when used with infrared sensor
- Global shutter instead of rolling shutter
- Sony Pregius S (4. Gen) Global shutter sensor
- 4:3 sensor, so nearly full FOV utilization
- 2064x1552 Pixel, so 1440x1440 resolution per eye
- 125 FPS at 8Bit and 117 FPS at 10 Bit
- Industrial Camera + temperature performance. Meaning that even if run days or weeks you will have 0 dropped frames
- Framos software support for Jetson specifically
<br></br>


## Folder Structure
📂 [jetson-orin-nx-framos-builder](./)  
├── 📁 [installer](./installer)  
│&emsp;&emsp;└── 📁 [playbooks](./installer/playbooks)  
│&emsp;&emsp;└── 📁 [data](./installer/data)  
├── 📁 [vr_passthrough](./vr_passthrough)  
├── 📁 [resources](./resources)  
│&emsp;&emsp;└── 🖼️ [orin_nx_flash_img1.png](./resources/orin_nx_flash_img1.png)  
│&emsp;&emsp;└── 🖼️ [orin_nx_flash_img2.png](./resources/orin_nx_flash_img2.png)  
│&emsp;&emsp;└── 🖼️ [orin_nx_flash_img3.png](./resources/orin_nx_flash_img3png)  
│&emsp;&emsp;└── 🖼️ [orin_nx_settings.png](./resources/orin_nx_settings)  
└── 📄 [README.md](./README.md)
<br></br>

## How to contribute
If you have a different Framos sensor, then you can just Fork this repo and add your sensor. I would appreciate it so that the project can diversify with other carrier boards, modules and sensors
<br></br>

### If you have never worked with jetson before or are completely new, then check these things before powering it on
- Dont hotplug in the sensors. always have the board powered off and cut from power.
- The board should have 2x 4-lane CSI2 to utilize full power of the framos sensors.
- You should run 90W Power supply to not run into Overcurrent mode.
<br></br>

### Before starting, reflash the Jetson on Ubuntu 22.04LTS with these settings
![Orin NX Settings](resources/orin_nx_settings.png)
![Orin NX Flash IMG 1](resources/orin_nx_flash_img1.png)
![Orin NX Flash IMG 2](resources/orin_nx_flash_img2.png)
![Orin NX Flash IMG 2](resources/orin_nx_flash_img3.png)
<br></br>

### When Flashed
- cut from power
- plug in cameras to the CSI ports
- plug in power to jetson
- run the sripts in [](./)