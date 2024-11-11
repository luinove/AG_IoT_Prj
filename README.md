# MUST TO READ
This project based on 'NRF CONNECT' SDK v2.7.0 && toolchains v2.7.0.
---
1. 'coap_client_v0' should be flashed onto A Nordic NRF52840 board without soil sensor.
2. 'coap_server_v0' should be flashed onto A Nordic NRF52840 board which connecting a soil sensor.

---
IN NRF CONNECT SDK2.7.0 the modbus module has a little timing issue.
SO strongly recommand repalce the contents of the 'modbus' folder with those found in the 'modbus 2.7.0_2.7.0  can work code' folder.
