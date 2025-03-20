# ES-Synth Keyboard

- [Overview](#overview)
- [Keyboard Controls](#keyboard-controls)
- [What the Keyboard Does](#what-the-keyboard-does)
- [Additional Resources (Including Report)](#additional-resources)
- [Contributors](#contributors)

## Overview

The ES-Synth Keyboard is a versatile synthesizer that supports multiple sound modes. It uses a variety of digital synthesis techniques—including numerically controlled oscillators and phase accumulation—to generate different waveforms such as sawtooth, sine, triangle, square, and more. The algorithms for these sound modes are described in detail in a separate documentation.

### Keyboard Controls

| Keyboard Control         | Function                                                                                                                 |
|--------------------------|--------------------------------------------------------------------------------------------------------------------------|
| Note Keys (Keys 0–11)    | Pressing a key triggers a note (with a frequency based on the key) by selecting the corresponding step size.            |
| Knob 0 Rotation          | Adjusts transposition in non-piano modes — rotating up increases pitch and rotating down decreases pitch.              |
| Knob 0S (button)         | When pressed, cycles through the available waveform modes (e.g., Sawtooth, Triangle, Sine, Square, Pulse, Noise, Piano, Rise). |
| Knob 1S (button)         | When pressed, toggles the module role between SENDER and RECEIVER.                                                      |
| Knob 2 Rotation          | Sets the active octave number for the module, affecting note pitch scaling.                                             |
| Knob 2S (button)         | When pressed, prints a debug message ("Knob 2S pressed") – reserved for future functionality.                           |
| Knob 3 Rotation          | Controls output volume; also used for adjusting the pulse duty cycle in Pulse waveform mode.                             |
| Knob 3S (button)         | When pressed, prints a debug message ("Knob 3S pressed") – reserved for future functionality.                           |
| Joystick S (button)      | When pressed, prints a debug message ("Joystick S pressed") – reserved for potential future features.                    |
| Joystick (analog inputs) | The X and Y analog readings are used for modulating effect parameters (e.g., pitch modulation in non-piano mode via Y-axis).|


### What the Keyboard Does

- **Sound Generation:**  
  Provides different sound modes using digital algorithms to adjust tone and timbre.

- **Key Matrix Scanning:**  
  Reads a dedicated 8x4 key matrix that detects key presses, enabling real-time note generation.

- **Control Inputs:**  
  Uses knobs and a joystick to adjust parameters like volume, pitch, and waveform selection.

- **Display Feedback:**  
  Updates an OLED display to visually present system state and user interactions.

- **Inter-Module Communication:**  
  Implements CAN bus communication for data exchange between stacked modules.

## Additional Resources

- **Lab Report:** [Report README](readmeFolder/Report.md)  
- **Advanced Feature Details:** [Features README](readmeFolder/Features.md)


## Contributors
- Steve Nimo - Nimosteve88
- Justin Lam - justinlam24
- Chiedoze Ihebuzor - Chiedozie20
