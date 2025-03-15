# ES-Synth Keyboard Features

This document details the features implemented in the ES-Synth Keyboard, describing both the hardware interactions and the underlying algorithms.

## 1. Sound Generation

### Waveform Modes

- **Sawtooth:**  
  Uses a linear phase accumulator to generate a continuously increasing ramp that resets at its maximum value. The basic formula resembles:  
  `output = (phase / MAX_PHASE) * amplitude`  
  where `phase` is incremented by a constant step size per sample.

- **Triangle:**  
  Obtained by linearly increasing the signal to a peak and then decreasing it symmetrically. It leverages the absolute value function around the midpoint of the period:  
  `output = amplitude * (1 - 2 * abs((phase / MAX_PHASE) - 0.5))`

- **Sine:**  
  Generated typically using a lookup table or a hardware/software sine function. The phase accumulator provides an index into the sine table, ensuring smooth periodic oscillation:  
  `output = amplitude * sin(2π * (phase / MAX_PHASE))`

- **Square:**  
  Created by comparing the phase accumulator against a threshold (usually 50% duty cycle). If the phase is less than half of the period, the output is high; otherwise, it is low:  
  `output = (phase < MAX_PHASE/2) ? amplitude : -amplitude`

- **Pulse:**  
  Similar to the square wave but with a variable duty cycle. The duty cycle (adjustable via Knob 3 Rotation) determines the point at which the output transitions from high to low.  
  `output = (phase < dutyCycle * MAX_PHASE) ? amplitude : -amplitude`

- **Noise:**  
  Typically generated using a pseudo-random number generator to create a white noise signal. This mode does not rely on phase accumulation but instead produces a random amplitude value within the specified range.

- **Piano:**  
  Incorporates a sampled dynamic envelope and a pitch drop algorithm to simulate acoustic piano characteristics. Beyond generating the basic waveform:
  - **Envelope:** Each note is assigned an envelope that decays over time to simulate string damping.  
    *Logic:* The code calculates an envelope value which scales the amplitude, and once the envelope drops below a threshold, the note is removed.
  - **Pitch Drop:** A transient pitch modulation is applied at the onset of a note to mimic the natural drop in pitch as the string vibrates.

- **Rise:**  
  Implements a rising envelope combined with pitch modulation:
  - **Envelope:** Instead of decaying, a rising envelope function (such as an exponential approach to a ceiling) is applied so that the note starts soft and grows to its full amplitude.  
    *Formula example:* `env = 1 - exp(-k * elapsedTime)` where `k` controls the speed of rise.
  - **Pitch Factor:** A secondary modulation factor adjusts the pitch over time, defined by a custom function (`getRisePitchFactor`) to create evolving tonal characteristics. This mode does not remove notes quickly since the envelope is expected to reach its peak value and sustain the tone.

## 2. Key Matrix Scanning

- **8x4 Key Matrix:**  
  A dedicated key matrix is scanned to detect key presses (notes). This enables real-time generation and removal of notes based on key activity.

- **Note Mapping:**  
  Each key (0–11) maps to a specific frequency by selecting a corresponding step size within the waveform generation algorithm.

## 3. Control Inputs

- **Knobs and Joystick:**
  - **Knob 0 Rotation:**  
    Adjusts pitch transposition in non-piano modes, where an upward rotation increases the pitch and a downward rotation decreases it.
  - **Knob 0S (Button):**  
    Cycles through the available waveform modes, switching between waveforms such as Sawtooth, Triangle, Sine, Square, Pulse, Noise, Piano, and Rise.
  - **Knob 1S (Button):**  
    Toggles the module role between SENDER and RECEIVER, affecting how the system communicates with other modules via CAN bus.
  - **Knob 2 Rotation:**  
    Sets the active octave number, thereby affecting the overall pitch scaling.
  - **Knob 2S (Button):**  
    Currently prints a debug message ("Knob 2S pressed") and is reserved for future functionality.
  - **Knob 3 Rotation:**  
    Controls output volume and is also used to adjust the pulse duty cycle when in Pulse waveform mode.
  - **Knob 3S (Button):**  
    Currently prints a debug message ("Knob 3S pressed") and is reserved for potential enhancements.
  - **Joystick and Joystick S (Button):**  
    The analog inputs (X and Y) from the joystick modulate effect parameters (e.g., pitch modulation in non-piano mode), while the joystick button currently outputs a debug message.

## 4. Display and Communication

- **OLED Display Feedback:**  
  The system updates an OLED display to visually indicate system status, current waveform, and other useful information for the user.
  
- **Inter-Module Communication (CAN Bus):**  
  Implements CAN bus communication allowing multiple modules to exchange data. The module role (SENDER/RECEIVER) affects how data is transmitted and received.

## 5. Real-Time Task Management

- **FreeRTOS-Based Tasks:**  
  - **scanKeysTask:**  
    This task scans keyboard inputs at regular intervals (every 20–50ms), handles debouncing, and updates state changes for controls.
  - **sampleISR:**  
    An interrupt service routine handles real-time audio sample generation. It adjusts envelopes, pitches, and combines notes efficiently.
  - **displayUpdateTask:**  
    Periodically updates the display and polls for CAN messages, ensuring that system feedback is current.

## Future Enhancements

- **Enhanced Debugging Functions:**  
  Additional debug messages are already provided (e.g., for Knob 2S, Knob 3S, and Joystick S), making it easier to integrate advanced functionality in future revisions.
- **Extended Effects and Modulations:**  
  Plans exist for expanding the function of the joystick and integrating more sophisticated modulation effects.

This detailed breakdown not only highlights the core hardware interactions but also explains the signal processing and modulation techniques that contribute to the rich feature set of the ES-Synth Keyboard.