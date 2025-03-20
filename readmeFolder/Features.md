# ES-Synth Keyboard Advanced Features

This document details the advanced features implemented in the ES-Synth Keyboard, describing both the hardware interactions and the underlying algorithms.

## Contents
- [ES-Synth Keyboard Features](#es-synth-keyboard-features)
- [1. Sound Generation](#1-sound-generation)
  - [Waveform Modes](#waveform-modes)
- [1.5 Polyphony Implementation](#15-polyphony-implementation)
- [1.6 Transposition and Octave Control](#16-transposition-and-octave-control)
- [2. Key Matrix Scanning](#2-key-matrix-scanning)
- [3. Control Inputs](#3-control-inputs)
- [4. Display and Communication](#4-display-and-communication)
- [Future Enhancements](#future-enhancements)

## 1. Control Inputs
The following list presents all the control inputs implemented in the system. 

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

## 2. Sound Generation

## 2.1 Waveform Modes
Different waveforms are customized to produce different sound effects. These configurations can be selected by Knob 0. 

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

## 2.2 Polyphony Implementation

In our implementation (main.cpp), polyphony is achieved by maintaining a pool of active voices that can overlap. When a key is pressed, the system allocates a voice from the available pool and assigns it the corresponding note parameters, including waveform type, frequency, and envelope settings. The code manages voice allocation dynamically, allowing for multiple simultaneous notes. When the note's envelope decays past a threshold or the key is released, the corresponding voice is deactivated and returned to the pool. Additionally, if the number of simultaneous note requests exceeds the available voices, a simple voice-stealing strategy is employed to ensure continuous sound generation without interruption.
```
struct ActiveNote {
    uint32_t stepSize;
    uint32_t phaseAcc;
    uint32_t elapsed;
};

#define MAX_POLYPHONY 12  // Maximum number of simultaneous notes

ActiveNote activeNotes[MAX_POLYPHONY];
uint8_t activeNoteCount = 0;
```

The function is implemented using the following logic:
```
if (xQueueReceive(msgInQ, localMsg, portMAX_DELAY) == pdPASS) {
            TASK_START();
            if (localMsg[0] == 'R') {  // Release message: remove the note.
                uint8_t note = localMsg[2];
                for (uint8_t i = 0; i < activeNoteCount; i++) {
                    if (activeNotes[i].stepSize == stepSizes[note]) {
                        // Remove the note by shifting the remaining notes
                        for (uint8_t j = i; j < activeNoteCount - 1; j++) {
                            activeNotes[j] = activeNotes[j + 1];
                        }
                        activeNoteCount--;
                        break;
                    }
                }
            }
            else if (localMsg[0] == 'P') {  // Press message: add the note.
                uint8_t octave = localMsg[1];
                uint8_t note = localMsg[2];
                if (note < 12) {
                    uint32_t step = stepSizes[note];
                    // If there's room, add a new note.
                    if (activeNoteCount < MAX_POLYPHONY) {
                        activeNotes[activeNoteCount].stepSize = step;
                        activeNotes[activeNoteCount].phaseAcc = 0;
                        activeNotes[activeNoteCount].elapsed = 0; // reset elapsed time
                        activeNoteCount++;
                    }
                    else {
                        // Voice stealing: find the oldest note and replace it.
                        uint8_t idxToSteal = 0;
                        uint32_t maxElapsed = activeNotes[0].elapsed;
                        for (uint8_t i = 1; i < activeNoteCount; i++) {
                            if (activeNotes[i].elapsed > maxElapsed) {
                                maxElapsed = activeNotes[i].elapsed;
                                idxToSteal = i;
                            }
                        }
                        activeNotes[idxToSteal].stepSize = step;
                        activeNotes[idxToSteal].phaseAcc = 0;
                        activeNotes[idxToSteal].elapsed = 0;
                    }
                }
            }
```

## 2.3 Transposition and Octave Control

Transposition and octave control are handled in main.cpp by combining adjustments from both the joystick and Knob 0 for fine and coarse transposition shifts, along with octave selection via Knob 2.

### Transposition (Knob 0 & Joystick)
The base frequency of a note (f_base) is adjusted based on a transposition value, T, which is the sum of the coarse adjustment from Knob 0 and a fine, fractional adjustment derived from the joystick input. The transposed frequency is calculated using the formula:
   
$f_{transposed} = f_{base}$ * 2^(T/12)

Where:
- T = knob0_value + (joystick_offset * fineTuneFactor)
- knob0_value maps to integer semitone shifts.
- joystick_offset provides a fractional enhancement for smoother pitch modulation.
- fineTuneFactor scales the joystick's contribution to semitone shifts.

### Octave Control (Knob 2)
Octave control scales the transposed frequency by a power-of-two factor, effectively doubling or halving the frequency for each octave change. This is given by:

$f_{octave} = f_{transposed}$ * 2^(N - $N_{ref})$

Where:
- N is the octave selected via Knob 2.
- $N_{ref}$ is the reference or middle octave.

### Final Frequency Calculation
Combining both adjustments, the final frequency for a note is computed as:

$f_{final} = f_{base}$ * 2^((knob0_value + (joystick_offset * fineTuneFactor)) / 12) * 2^(N - $N_{ref})$

This formula allows for seamless integration of transposition and octave adjustments, ensuring that pitch modifications remain musically accurate and responsive to user input.

However, when the formula was initially implemented, the timing constraints were not met properly as the computation time to calculate exponents is signifcantly high. To solve this issue, the transposed frequencies were replaced with pre-computed values that can scale the frequency, also using the same formula above. These scaling values are stored in a lookup table, and depending on the amount of transposing required, the current frequency will be multiplied with the corresponding constant in the table. This would remove the extra computational time for exponential calculation, and after testing, satisfy the timing requirements of the system. 

## 3. Key Matrix Scanning

- **8x4 Key Matrix:**  
  A dedicated key matrix is scanned to detect key presses (notes). This enables real-time generation and removal of notes based on key activity.

- **Note Mapping:**  
  Each key (0–11) maps to a specific frequency by selecting a corresponding step size within the waveform generation algorithm.

## 4. East and West Keyboard Detection

- **Combining multiple synthesizers:**
  Using the East and West Detection local input, the synthesizer can detected if a separate synthesizer is attached on its left or right.
  ```
  else if (localInputs[23]){
            Serial.println("West Detect Initiated");
        }
        else if (localInputs[27]){
            Serial.println("East Detect Initiated");
        }
  ```

## Future Enhancements

- **Enhanced Debugging Functions:**  
  Additional debug messages are already provided (e.g., for Knob 2S, Knob 3S, and Joystick S), making it easier to integrate advanced functionality in future revisions.
- **Extended Effects and Modulations:**  
  Plans exist for expanding the function of the joystick and integrating more sophisticated modulation effects.
- **Adding dependent octaves with combined synthesizers:**  
  When a multiple synthesizers is connected, an algorithm can be implemented to determine its octaves such that a synthesizer on the right would always be one octave higher than the one on its left. 

This detailed breakdown not only highlights the core hardware interactions but also explains the signal processing and modulation techniques that contribute to the rich feature set of the ES-Synth Keyboard.
