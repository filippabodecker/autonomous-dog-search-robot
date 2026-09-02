# Bill of Materials

The main hardware components used in the autonomous dog search robot are listed below.

| Qty | Component | Model / Specification | Purpose | Product Reference |
|---:|---|---|---|---|
| 1 | Main controller | UNIHIKER K10 (DFR0992-EN) | Runs AI, navigation, voice recognition and mission logic | [DFRobot product page](https://www.dfrobot.com/product-2904.html) |
| 1 | Camera extension cable | 24-pin FFC/FPC flexible flat cable, 0.5 mm pitch | Extends the K10 camera connection so the camera can be repositioned and correctly oriented | [eBay product page](https://www.ebay.co.uk/itm/298132372284) |
| 1 | Robot platform | DFRobot Maqueen Plus V3 (MBT0050-18650) | Provides the chassis, motors, wheels and RGB LED feedback | [DFRobot product page](https://www.dfrobot.com/product-2939.html) |
| 1 | Distance sensor | DFRobot Matrix Laser Ranging Sensor, included with MBT0050-18650 | Provides left, center and right distance measurements | [Included with Maqueen Plus V3](https://www.dfrobot.com/product-2939.html) |
| 1 | Communication controller | DUBEUYEW ESP32-DevKitC V1 (30-pin) | Handles Telegram communication and image transfer | [Amazon product page](https://www.amazon.se/dp/B0CRRFT5L7) |
| 1 | Servo motor | DFRobot 9g Metal Gear Servo, from micro:Maqueen Mechanic – Beetle (ROB0156-B) | Operates the treat-dispensing mechanism | [DFRobot product page](https://www.dfrobot.com/product-2128.html) |
| 1 | Storage | 32 GB microSD card | Stores numbered BMP images and the recorded completion message | — |
| 1 | Robot power supply | Rechargeable 18650 Li-ion battery | Powers the Maqueen Plus V3 and main robot system | — |
| 1 | ESP32 power supply | USB power bank | Powers the external ESP32 communication controller | — |
| 1 | Mechanical assembly | Repurposed micro:Maqueen Mechanic parts and custom treat dispenser | Secures the servo and releases a treat after a confirmed detection | [Mechanic kit](https://www.dfrobot.com/product-2029.html) and custom-built parts |
