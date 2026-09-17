# Smart Door Security and Anti-Tailgating System

A smart access-control prototype developed using an Arduino UNO R3 and ESP32-S3 CAM.

The system improves a normal RFID door lock by detecting how many people actually pass through the doorway. Two IR sensors determine movement direction, while an ESP32-S3 camera provides a second visual detection channel.

## Key Features

- RFID-based access control using MFRC522
- Directional people counting using two FC-51 IR sensors
- Tailgating detection
- Magnetic reed-switch forced-entry detection
- ESP32-S3 camera monitoring
- Sensor fusion between camera and IR detection
- OLED status display
- Audible alarm patterns
- Relay-controlled 12V solenoid door lock

## Communication

- MFRC522 RFID → SPI
- SSD1306 OLED → I2C
- ESP32-S3 Camera → WiFi/HTTP
- ESP32 alert → Arduino using open-drain GPIO

## Prototype

![Smart Door Security Prototype](prototype.jpg)
