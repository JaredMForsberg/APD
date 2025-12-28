# APD
Central code repository for our Automatic Smart Pill Dispenser (APD) project — an IoT-based system designed to automate medication dispensing, provide reminders, and support remote scheduling for caregivers.

The repo currently includes the M5StickC+ Wi-Fi alarm scheduler, which handles time synchronization, scheduling of dosage events, on-device notifications, and web-based configuration. Over time, this repository will expand to include firmware for additional system components such as the dispenser control board, UI layer, real-time monitoring, networking protocol, cloud integration, and user interface applications.

Planned modules inside this repo:
• Alarm scheduler + reminder engine (M5StickC Plus)
• Dispenser firmware (main board, motors/servos, sensors)
• Raspberry Pi / microcontroller interface logic
• Local + remote scheduling API
• LCD/TFT UI and menu system
• Data logging + medical adherence tracking
• Caregiver/mobile dashboard integration
