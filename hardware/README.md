# Hardware
See the [schematic](Schematic.pdf) for the circuit diagram of the project.

The project used a 12V DC power supply. An isolated, 5V supply was used to power the ESP32 from this to keep it isolated from the 12V. An optocoupler was used to isolate the ESP32 from the pump, and the pump could be controlled using a MOSFET. I mostly used parts I had on hand, so some components might not be considered optimal, but I have checked that they would all work safely together. For example, the MOSFET used is overkill for this application.