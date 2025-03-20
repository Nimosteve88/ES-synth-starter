#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>
#include <cmath>
#include <ES_CAN.h>
#include <STM32FreeRTOS.h>   // Include FreeRTOS for STM32


// Uncomment the following lines for test builds:
//#define DISABLE_THREADS
//#define DISABLE_ISRS
//#define TEST_SCANKEYS
//#define TEST_DECODE
//#define TEST_CAN_TX
//#define TEST_DISPLAYUPDATE



//#define MEASURE_TASK_TIMES  // Uncomment to enable task/ISR timing measurements

#ifdef MEASURE_TASK_TIMES
// Global variables to store worst-case (max) execution times in microseconds
volatile uint32_t maxScanKeysTime = 0;
volatile uint32_t maxDisplayUpdateTime = 0;
volatile uint32_t maxDecodeTime = 0;
volatile uint32_t maxCAN_TX_Time = 0;
volatile uint32_t maxSampleISRTime = 0;

// Macro to mark start and end of a task section
#define TASK_START()  uint32_t tStart = micros()
#define TASK_END(maxVar)  do {                      \
    uint32_t tEnd = micros();                       \
    uint32_t elapsed = tEnd - tStart;               \
    if (elapsed > maxVar) maxVar = elapsed;         \
} while(0)

#else
// If measurement is disabled, define empty macros
#define TASK_START()   
#define TASK_END(maxVar)
#endif




enum ModuleRole { SENDER, RECEIVER };
ModuleRole moduleRole = SENDER;   // Change to RECEIVER for receiver modules

// Global variable for choosing the octave number for this module.
uint8_t moduleOctave = 4;         // Default octave number (can be changed at runtime)

volatile int joyX12Val = 6;  // Default mid value (0 to 12)
volatile int joyY12Val = 6;  // Default mid value (0 to 12)

enum WaveformType { SAWTOOTH = 0, PIANO, RISE, TRIANGLE, SINE, SQUARE, PULSE, NOISE };
volatile WaveformType currentWaveform = SAWTOOTH;  // Default waveform

//create a sine lookup table
const int SINE_TABLE_SIZE = 256;



// ------------------------- Knob Class -------------------------------------- //

class Knob {
    public:
        // Constructor: initialise limits, starting value, and internal state.
        Knob(int lower = 0, int upper = 8)
            : lowerLimit(lower), upperLimit(upper), rotation(lower), knobPrev(0), lastLegalDelta(0)
        {
        }
    
        // Update the knob's rotation value using the new quadrature state (2-bit value)
        void update(uint8_t quadratureState) {
            int delta = 0;
            uint8_t diff = knobPrev ^ quadratureState;
            if (diff == 0) {
                // No change in the state
                delta = 0;
            }
            else if (diff == 0b11) {
                // Both bits changed: an impossible transition.
                // Assume the same direction as the last legal transition.
                delta = (lastLegalDelta != 0) ? lastLegalDelta : 0;
            }
            else {
                // Legal transition (only one bit changed).
                uint8_t transition = (knobPrev << 2) | quadratureState;
                switch (transition) {
                    case 0b0001: // 00 -> 01: Clockwise => +1
                        delta = +1;
                        lastLegalDelta = +1;
                        break;
                    case 0b0100: // 01 -> 00: Anticlockwise => -1
                        delta = -1;
                        lastLegalDelta = -1;
                        break;
                    case 0b1011: // 10 -> 11: Anticlockwise => -1
                        delta = -1;
                        lastLegalDelta = -1;
                        break;
                    case 0b1110: // 11 -> 10: Clockwise => +1
                        delta = +1;
                        lastLegalDelta = +1;
                        break;
                    default:
                        // For intermediate or inconclusive transitions, make no change.
                        delta = 0;
                        break;
                }
            }
            // Save the current state for the next update.
            knobPrev = quadratureState;
    
            // Read the current rotation value atomically.
            int current = __atomic_load_n(&rotation, __ATOMIC_RELAXED);
            int newVal = current + delta;
            // Clamp to the permitted limits.
            if (newVal < lowerLimit) newVal = lowerLimit;
            if (newVal > upperLimit) newVal = upperLimit;
            // Atomically store the new rotation value.
            __atomic_store_n(&rotation, newVal, __ATOMIC_RELAXED);
        }
    
        // Get the current knob rotation (thread-safe atomic load)
        int getRotation() const {
            return __atomic_load_n(&rotation, __ATOMIC_RELAXED);
        }
    
        // Set new lower and upper limits, and adjust the current value if needed.
        void setLimits(int lower, int upper) {
            lowerLimit = lower;
            upperLimit = upper;
            int current = getRotation();
            if (current < lowerLimit) current = lowerLimit;
            if (current > upperLimit) current = upperLimit;
            __atomic_store_n(&rotation, current, __ATOMIC_RELAXED);
        }
    
    private:
        int rotation;      // Current rotation value (atomically accessed)
        int lowerLimit;    // Minimum allowed value
        int upperLimit;    // Maximum allowed value
    
        // Internal state for quadrature decoding:
        uint8_t knobPrev;      // Previous 2-bit state {B, A}
        int lastLegalDelta;    // Last legal delta (+1 or -1)
    };


// ------------------------ GLOBAL STRUCT & GLOBALS ------------------------ //

// Shared system state (used by more than one thread)
struct {
    std::bitset<32> inputs;
    SemaphoreHandle_t mutex;  
    int knob3Rotation;
    Knob knob3;
    Knob knob2;
    Knob knob1;
    Knob knob0;
    uint8_t RX_Message[8];
    bool eastDetected;
    bool westDetected;
    } sysState;


// Global variable for the current note step size (accessed by ISR)
uint32_t currentStepSize = 0;

// Phase accumulator for audio generation
static uint32_t phaseAcc = 0;
HardwareTimer sampleTimer(TIM1);

volatile uint8_t TX_Message[8] = {0}; 

QueueHandle_t msgInQ;
#ifdef TEST_SCANKEYS
  // Increase the queue size in test mode to avoid blocking.
  QueueHandle_t msgOutQ;  // We’ll create a larger queue below.
#else
  QueueHandle_t msgOutQ;
#endif

SemaphoreHandle_t CAN_TX_Semaphore;

// ------------------------- CONSTANTS & PIN DEFINITIONS ------------------------ //

constexpr uint32_t SAMPLE_RATE = 22050; // Audio sample rate (Hz)

//Pin definitions
  //Row select and enable
  const int RA0_PIN = D3;
  const int RA1_PIN = D6;
  const int RA2_PIN = D12;
  const int REN_PIN = A5;

  //Matrix input and output
  const int C0_PIN = A2;
  const int C1_PIN = D9;
  const int C2_PIN = A6;
  const int C3_PIN = D1;
  const int OUT_PIN = D11;

  //Audio analogue out
  const int OUTL_PIN = A4;
  const int OUTR_PIN = A3;

  //Joystick analogue in
  const int JOYY_PIN = A0;
  const int JOYX_PIN = A1;

  //Output multiplexer bits
  const int DEN_BIT = 3;
  const int DRST_BIT = 4;
  const int HKOW_BIT = 5;
  const int HKOE_BIT = 6;


// LED indicator (ensure LED_BUILTIN is defined for your board)
#ifndef LED_BUILTIN
  #define LED_BUILTIN PC13   // Change to the correct LED pin for your board
#endif

// Display driver instance
U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

// -------------------------- NOTE CALCULATION ------------------------------- //

// Calculate the phase step size for a given frequency
constexpr uint32_t calculateStepSize(float frequency) {
    return static_cast<uint32_t>((pow(2, 32) * frequency) / SAMPLE_RATE);
}

// Step sizes for the 12 semitones (C to B)
const uint32_t stepSizes[12] = {
    calculateStepSize(261.63f),  // C
    calculateStepSize(277.18f),  // C#
    calculateStepSize(293.66f),  // D
    calculateStepSize(311.13f),  // D#
    calculateStepSize(329.63f),  // E
    calculateStepSize(349.23f),  // F
    calculateStepSize(369.99f),  // F#
    calculateStepSize(392.00f),  // G
    calculateStepSize(415.30f),  // G#
    calculateStepSize(440.00f),  // A
    calculateStepSize(466.16f),  // A#
    calculateStepSize(493.88f)   // B
};

const char* noteNames[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};


// Compute the sample based on the phase accumulator and current waveform
int computeWaveform(uint32_t phase) {
    uint8_t x = phase >> 24;  // Use the top 8 bits (0-255) as our phase index
    int sample = 0;
    switch(currentWaveform) {
        case SAWTOOTH:
            // Linear ramp from -128 to +127.
            sample = (int)x - 128;
            break;
        case TRIANGLE:
            // Triangle waveform by folding the sawtooth.
            if (x < 128)
                sample = (x * 2) - 128;
            else
                sample = ((255 - x) * 2) - 128;
            break;
        case SINE:
            {
                // Compute sine using floating-point math.
                float angle = (x / 256.0f) * 6.28318530718f;  // Convert x to an angle (0 to 2π)
                sample = (int)(sinf(angle) * 127.0f);
            }
            break;
        case SQUARE:
            // Square wave: output high for first half of the cycle, low for the second half.
            sample = (x < 128) ? 127 : -127;
            break;
        case PULSE:
            {
                // Pulse wave: like square but with an adjustable duty cycle.
                // Let's use knob3 as the duty cycle control (range 0 to 8).
                int duty = sysState.knob3.getRotation(); // 0 (short pulse) to 8 (long pulse)
                // Map duty (0-8) to a threshold value in the range 0-255.
                int threshold = (duty * 256) / 9;  
                sample = (x < threshold) ? 127 : -127;
            }
            break;
        case NOISE:
            {
                // Generate pseudo-random noise. This simple LCG uses a static seed.
                static uint32_t noiseSeed = 0x12345678;
                noiseSeed = noiseSeed * 1664525UL + 1013904223UL;
                // Use the lower 8 bits and center the output.
                sample = (int)(noiseSeed & 0xFF) - 128;
            }
            break;
        
            
        default:
            sample = (int)x - 128;
            break;
    }
    return sample;
}


// --------------------------- HELPER FUNCTIONS ------------------------------ //

// Set the row lines on the 3-to-8 decoder based on a row number
void setRow(uint8_t row) {
    digitalWrite(REN_PIN, LOW);
    digitalWrite(RA0_PIN, (row & 0x01) ? HIGH : LOW);
    digitalWrite(RA1_PIN, (row & 0x02) ? HIGH : LOW);
    digitalWrite(RA2_PIN, (row & 0x04) ? HIGH : LOW);
    delayMicroseconds(2);
    digitalWrite(REN_PIN, HIGH);
}

void setOutMuxBit(const uint8_t bitIdx, const bool value) {
    digitalWrite(REN_PIN,LOW);
    digitalWrite(RA0_PIN, bitIdx & 0x01);
    digitalWrite(RA1_PIN, bitIdx & 0x02);
    digitalWrite(RA2_PIN, bitIdx & 0x04);
    digitalWrite(OUT_PIN,value);
    digitalWrite(REN_PIN,HIGH);
    delayMicroseconds(2);
    digitalWrite(REN_PIN,LOW);
}

// Read the column values from the 4 inputs (assuming pull-up + key press pulls LOW)
std::bitset<4> readCols() {
    std::bitset<4> cols;
    cols[0] = digitalRead(C0_PIN);
    cols[1] = digitalRead(C1_PIN);
    cols[2] = digitalRead(C2_PIN);
    cols[3] = digitalRead(C3_PIN);
    return cols;
}


    
struct ActiveNote {
    uint32_t stepSize;
    uint32_t phaseAcc;
    uint32_t elapsed;
};

#define MAX_POLYPHONY 12  // Maximum number of simultaneous notes

ActiveNote activeNotes[MAX_POLYPHONY];
uint8_t activeNoteCount = 0;


// ----------------------- FREE RTOS TASKS ----------------------------------- //

// In your global variables, change the previous state bitset to track 16 keys:
static std::bitset<16> prevKeys;  // Now tracks keys 0-15
static bool prevKnob1SPressed = false;
static bool prevKnob0SPressed = false;

// Task to scan the key matrix at a 20-50ms interval (priority 2)
void scanKeysTask(void * pvParameters) {
#ifdef TEST_SCANKEYS
    // In test mode, we simulate a worst-case scenario:
    // For every call, generate a key press message for each of the 12 keys.
    for (uint8_t key = 0; key < 12; key++) {
        uint8_t TX_Message[8] = {0};
        TX_Message[0] = 'P';
        TX_Message[1] = 4;
        TX_Message[2] = key;
        // Send only if in SENDER mode.
        if (moduleRole == SENDER) {
            xQueueSend(msgOutQ, TX_Message, 0);
        }
    }
#else
    const TickType_t xFrequency = 20 / portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    static int prevTranspose = 0;

    while (1) {
        TASK_START(); // Mark start time
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        // 1) Scan the full 8x4 matrix into localInputs (16 keys)
        std::bitset<32> localInputs;
        for (uint8_t row = 0; row < 8; row++) {
            setRow(row);
            delayMicroseconds(2);
            std::bitset<4> rowInputs = readCols();
            for (uint8_t col = 0; col < 4; col++) {
                localInputs[row * 4 + col] = rowInputs[col];
            }
        }

        // 2) Update shared key state (if needed)
        xSemaphoreTake(sysState.mutex, portMAX_DELAY);
        sysState.inputs = localInputs;
        xSemaphoreGive(sysState.mutex);

        // 3) Process note keys (first 12 keys, rows 0-2) as before
        uint32_t localStepSize = 0;
        for (uint8_t i = 0; i < 12; i++) {
            if (!localInputs[i]) {
                localStepSize = stepSizes[i];
                break;
            }
        }
        currentStepSize = localStepSize;

        // Process note press/release events for keys 0-11:
        uint8_t currentOctave = moduleOctave;
        for (uint8_t key = 0; key < 12; key++) {
            bool currentState = !localInputs[key];
            bool previousState = prevKeys[key];
            if (currentState != previousState) {
                // Send messages only in SENDER mode.
                //if (moduleRole == SENDER) {
                    uint8_t TX_Message[8] = {0};
                    TX_Message[0] = currentState ? 'P' : 'R';
                    TX_Message[1] = currentOctave;
                    TX_Message[2] = key;
                    xQueueSend(msgOutQ, TX_Message, portMAX_DELAY);
                //}
            }
            prevKeys[key] = currentState;
        }
        

        // 4) Decode knob 3 (remains unchanged)
        uint8_t knob3A = localInputs[12];
        uint8_t knob3B = localInputs[13];
        uint8_t knob3Curr = (knob3B << 1) | knob3A;  // Quadrature state {B, A}
        sysState.knob3.update(knob3Curr);

        // 5) Decode knob 2 (remains unchanged)
        uint8_t knob2A = localInputs[14];
        uint8_t knob2B = localInputs[15];
        uint8_t knob2Curr = (knob2B << 1) | knob2A;  // Quadrature state {B, A}
        sysState.knob2.update(knob2Curr);

        // 6) Decode knob 1 (remains unchanged)
        uint8_t knob1A = localInputs[16];
        uint8_t knob1B = localInputs[17];
        uint8_t knob1Curr = (knob1B << 1) | knob1A;  // Quadrature state {B, A}
        sysState.knob1.update(knob1Curr);

        // 7) Decode knob 0 (remains unchanged)
        uint8_t knob0A = localInputs[18];
        uint8_t knob0B = localInputs[19];
        uint8_t knob0Curr = (knob0B << 1) | knob0A;  // Quadrature state {B, A}
        prevTranspose = sysState.knob0.getRotation();
        sysState.knob0.update(knob0Curr);
        int newTranspose = sysState.knob0.getRotation(); 

        if (newTranspose > prevTranspose) {
            Serial.println("Transposing Up");

        } else if (newTranspose < prevTranspose) {
            Serial.println("Transposing Down");
        }

        //Serial.println(newTranspose - 4);

        bool knob1SPressed = !localInputs[25];
        bool knob0SPressed = !localInputs[24];

        if (!localInputs[20]){
            Serial.println("Knob 2S pressed");
        } else if (!localInputs[21]){
            Serial.println("Knob 3S pressed");
        } else if (!localInputs[22]){
            Serial.println("Joystick S pressed");
        } else if (knob0SPressed && !prevKnob0SPressed){
            //Serial.println("Knob 0S pressed");
            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            currentWaveform = (WaveformType)(((int)currentWaveform + 1) % 6);
            xSemaphoreGive(sysState.mutex);
            Serial.print("Waveform changed to: ");
            if (currentWaveform == SAWTOOTH) Serial.println("Sawtooth");
            else if (currentWaveform == TRIANGLE) Serial.println("Triangle");
            else if (currentWaveform == SINE) Serial.println("Sine");
            else if (currentWaveform == SQUARE) Serial.println("Square");
            else if (currentWaveform == PULSE) Serial.println("Pulse");
            else if (currentWaveform == NOISE) Serial.println("Noise");
            else if (currentWaveform == PIANO) Serial.println("Piano");
            else if (currentWaveform == RISE) Serial.println("Rise");

        } else if (knob1SPressed && !prevKnob1SPressed){
            // Knob 1 S (!localInputs[25])
            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            moduleRole = (moduleRole == SENDER) ? RECEIVER : SENDER;
            xSemaphoreGive(sysState.mutex);
            Serial.println("Role changed");        
        }
        else if (localInputs[23]){
            Serial.println("West Detect Initiated");
        }
        else if (localInputs[27]){
            Serial.println("East Detect Initiated");
        }
        prevKnob1SPressed = knob1SPressed;
        prevKnob0SPressed = knob0SPressed;

        TASK_END(maxScanKeysTime); // Update worst-case time

    }
#endif
}

// Task to update the display and poll for received CAN messages (priority 1)
void displayUpdateTask(void * pvParameters) {
    const TickType_t xFrequency = 100 / portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        TASK_START();
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));

        // Update joystick values outside of the ISR
        int rawJoyX = analogRead(JOYX_PIN);
        int rawJoyY = analogRead(JOYY_PIN);
        // Use the same ymin and ymax as in sampleISR (adjust if needed)
        joyX12Val = map(rawJoyX, 800, 119, 0, 12);
        joyY12Val = map(rawJoyY, 800, 119, 0, 12);


        // Read sysState.knob3Rotation under the mutex
        xSemaphoreTake(sysState.mutex, portMAX_DELAY);
        int rotationCopy = sysState.knob3.getRotation();
        xSemaphoreGive(sysState.mutex);

        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.drawStr(2,10,moduleRole == SENDER ? "SENDER" : "RECEIVER");

        // Display current JOYX and JOYY values in the form (12,34)
        u8g2.setCursor(75, 10);
        u8g2.print("(");
        u8g2.print(joyX12Val);
        u8g2.print(",");
        u8g2.print(joyY12Val);
        u8g2.print(")");
    

        // Display key matrix state
        // u8g2.setCursor(2,20);
        // xSemaphoreTake(sysState.mutex, portMAX_DELAY);
        // u8g2.print(sysState.inputs.to_ulong(), HEX);
        // xSemaphoreGive(sysState.mutex);

        // Display the current waveform
        u8g2.setCursor(2, 20);
        if (currentWaveform == SAWTOOTH) u8g2.print("Sawtooth");
        else if (currentWaveform == TRIANGLE) u8g2.print("Triangle");
        else if (currentWaveform == SINE) u8g2.print("Sine");
        else if (currentWaveform == SQUARE) u8g2.print("Square");
        else if (currentWaveform == PULSE) u8g2.print("Pulse");
        else if (currentWaveform == NOISE) u8g2.print("Noise");
        else if (currentWaveform == PIANO) u8g2.print("Piano");
        else if (currentWaveform == RISE) u8g2.print("Rise");


        u8g2.setCursor(2, 30);
        u8g2.print("Volume: ");
        // Read the knob value using the class method
        u8g2.print(rotationCopy);

        u8g2.setCursor(66, 20);
        u8g2.print("Pitch: ");
        u8g2.print(moduleOctave);


        // Now display the last received CAN message.
        xSemaphoreTake(sysState.mutex, portMAX_DELAY);
        u8g2.setCursor(66,30);
        // Serial.print("RX: ");
        // for (uint8_t i = 0; i < 8; i++) {
        //     Serial.print(sysState.RX_Message[i], HEX);
        //     Serial.print(" ");
        // }
        //Serial.println(sysState.RX_Message[0]);
        u8g2.print(sysState.RX_Message[0] == 'P' ? "P" : "R");
        u8g2.print(sysState.RX_Message[1]);
        u8g2.print(sysState.RX_Message[2]);
        xSemaphoreGive(sysState.mutex);

        u8g2.sendBuffer();
        digitalToggle(LED_BUILTIN);
        TASK_END(maxDisplayUpdateTime);
    }
}





// -------------------------- DECODE TASK  ----------------------------------- //

void decodeTask(void * pvParameters) {
    uint8_t localMsg[8];
    for (;;) {
        // Block until a message is available:
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

            // Debug: print current polyphony
            // Serial.print("Active notes count: ");
            // Serial.println(activeNoteCount);
            // Serial.print("Active notes: ");
            // for (uint8_t i = 0; i < activeNoteCount; i++) {
            //     Serial.print(activeNotes[i].stepSize);
            //     Serial.print(" ");
            // }
            // Serial.println();

            // Reset activeNotes
            // for (uint8_t i = 0; i < activeNoteCount; i++) {
            //     activeNotes[i].stepSize = 0;
            //     activeNotes[i].phaseAcc = 0;
            // }
            
            // Update the global RX_Message
            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            memcpy(sysState.RX_Message, localMsg, sizeof(sysState.RX_Message));
            xSemaphoreGive(sysState.mutex);
            TASK_END(maxDecodeTime);
            
        }
    }
}



void CAN_TX_Task (void * pvParameters) {
	// If not SENDER, suspend this task.
    if (moduleRole != SENDER) {
        while (1) { vTaskDelay(portMAX_DELAY); }
    }
    uint8_t msgOut[8];
    while (1) {
        xQueueReceive(msgOutQ, msgOut, portMAX_DELAY);
        TASK_START();
        xSemaphoreTake(CAN_TX_Semaphore, portMAX_DELAY);
        CAN_TX(0x123, msgOut);
        TASK_END(maxCAN_TX_Time);
    }
}

// Returns an attack envelope that linearly rises from 0 to 1 over 50ms.
float getAttackEnvelope(uint32_t elapsed) {
    const float attackTime = 0.3f;  // 50 ms in seconds
    float t = elapsed / (float)SAMPLE_RATE;  // time in seconds
    if (t >= attackTime) return 1.0f;
    return t / attackTime;
}

// Returns a pitch factor that rises from 0.95 to 1.0 over 50ms.
float getRisePitchFactor(uint32_t elapsed) {
    const float attackTime = 0.05f;  // 50 ms
    float t = elapsed / (float)SAMPLE_RATE;
    if (t >= attackTime) return 1.0f;
    // Linear interpolation: at t=0, factor=0.95; at t=attackTime, factor=1.0.
    return 0.95f + 0.05f * (t / attackTime);
}



// Compute an exponential decay envelope.
// elapsed is in samples; SAMPLE_RATE is 22050 Hz.
float getEnvelope(uint32_t elapsed) {
    // Convert samples to seconds.
    float t = elapsed / (float)SAMPLE_RATE;
    // A decay rate multiplier (adjust to taste; higher value = faster decay).
    return expf(-t * 3.0f);
}

// Compute a pitch drop factor: start slightly high and drop to 1.0 within ~50ms.
float getPitchFactor(uint32_t elapsed) {
    float t = elapsed / (float)SAMPLE_RATE; // time in seconds
    // For instance, start at 1.05 and drop to 1.0 within 50ms.
    float factor = 1.05f - 0.05f * fminf(t / 0.05f, 1.0f);
    return factor;
}


// ------------------------- TIMER ISR FOR AUDIO ----------------------------- //

void sampleISR() {

    // Do not generate audio in SENDER mode.
    if (moduleRole == SENDER) {
        return;
    }
#ifdef MEASURE_TASK_TIMES
    uint32_t startISR = DWT->CYCCNT;
#endif
    // Transposition multipliers for non-piano modes.
    const float transposeMultipliers[9] = {
        0.7937098f, 0.8409038f, 0.8909039f, 0.943877f,
        1.000000f,  1.0594600f, 1.1224555f, 1.1891967f, 1.2599063f
    };

    // Update octave from knob2.
    int knobOctave = sysState.knob2.getRotation();
    if (knobOctave < 0) knobOctave = 0;
    if (knobOctave > 8) knobOctave = 8;
    moduleOctave = knobOctave;

    // For piano mode, process each active note with its own envelope and pitch drop.
    if (currentWaveform == PIANO) {
        int32_t mixSum = 0;
        uint8_t voices = 0;
        // Iterate over active notes, and remove those that have decayed completely.
        for (uint8_t i = 0; i < activeNoteCount; ) {
            activeNotes[i].elapsed++;
            float env = getEnvelope(activeNotes[i].elapsed);
            // If the note is nearly silent, remove it from the list.
            if (env < 0.01f) {
                // Shift remaining notes down.
                for (uint8_t j = i; j < activeNoteCount - 1; j++) {
                    activeNotes[j] = activeNotes[j + 1];
                }
                activeNoteCount--;
                // Do not increment i, as a new note is now at position i.
                continue;
            }
            float pitchFactor = getPitchFactor(activeNotes[i].elapsed);
            uint32_t noteStep = activeNotes[i].stepSize;
            if (moduleOctave > 4) {
                noteStep <<= (moduleOctave - 4);
            } else if (moduleOctave < 4) {
                noteStep >>= (4 - moduleOctave);
            }
            uint32_t modifiedStep = (uint32_t)(noteStep * pitchFactor);
            activeNotes[i].phaseAcc += modifiedStep;
    
            uint8_t phase = activeNotes[i].phaseAcc >> 24;
            float angle = (phase / 256.0f) * 6.28318530718f; // 2π radians
            int sample = (int)(sinf(angle) * 127.0f);
            sample = (int)(sample * env);
            mixSum += sample;
            voices++;
            i++;
        }
        int normalizedSample = (voices > 0) ? mixSum / voices : 0;
        int volume = sysState.knob3.getRotation();
        if (volume < 0) volume = 0;
        if (volume > 8) volume = 8;
        int scaledSample = (normalizedSample * volume) / 8;
        int finalOutput = scaledSample + 128;
        if (finalOutput < 0) finalOutput = 0;
        if (finalOutput > 255) finalOutput = 255;
        analogWrite(OUTR_PIN, finalOutput);
    } else if (currentWaveform == RISE) {
        int32_t mixSum = 0;
        uint8_t voices = 0;
        for (uint8_t i = 0; i < activeNoteCount; ) {
            activeNotes[i].elapsed++;
            // Get rising envelope and pitch factor.
            float env = getAttackEnvelope(activeNotes[i].elapsed);
            float pitchFactor = getRisePitchFactor(activeNotes[i].elapsed);
            
            // Remove note if it has decayed (or if, for some reason, envelope remains 0 for too long)
            // (In RISE mode we expect the envelope to reach 1 quickly, so we may not remove it here.)
            // For example, if a note remains at 0 for > 100ms, remove it.
            if (activeNotes[i].elapsed > SAMPLE_RATE / 10 && env < 0.01f) {
                for (uint8_t j = i; j < activeNoteCount - 1; j++) {
                    activeNotes[j] = activeNotes[j + 1];
                }
                activeNoteCount--;
                continue;
            }
            
            // Apply octave scaling.
            uint32_t noteStep = activeNotes[i].stepSize;
            if (moduleOctave > 4) {
                noteStep <<= (moduleOctave - 4);
            } else if (moduleOctave < 4) {
                noteStep >>= (4 - moduleOctave);
            }
            
            // Apply the pitch rise factor to the note's step size.
            uint32_t modifiedStep = (uint32_t)(noteStep * pitchFactor);
            activeNotes[i].phaseAcc += modifiedStep;
    
            // Use a sine oscillator to generate the tone.
            uint8_t phase = activeNotes[i].phaseAcc >> 24;
            float angle = (phase / 256.0f) * 6.28318530718f;
            int sample = (int)(sinf(angle) * 127.0f);
    
            // Apply the rising amplitude envelope.
            sample = (int)(sample * env);
            mixSum += sample;
            voices++;
            i++;
        }
        int normalizedSample = (voices > 0) ? mixSum / voices : 0;
        int volume = sysState.knob3.getRotation();
        if (volume < 0) volume = 0;
        if (volume > 8) volume = 8;
        int scaledSample = (normalizedSample * volume) / 8;
        int finalOutput = scaledSample + 128;
        if (finalOutput < 0) finalOutput = 0;
        if (finalOutput > 255) finalOutput = 255;
        analogWrite(OUTR_PIN, finalOutput);
    } 
    else {
        // Non-PIANO mode processing as before.
        int transposition = sysState.knob0.getRotation();
        uint32_t effectiveStep = currentStepSize;
        effectiveStep = effectiveStep * transposeMultipliers[transposition];
        effectiveStep += ((int32_t)(joyY12Val - 6) * (effectiveStep / 100));
    
        uint32_t scaledEffectiveStep = effectiveStep;
        if (moduleOctave > 4) {
            scaledEffectiveStep <<= (moduleOctave - 4);
        } else if (moduleOctave < 4) {
            scaledEffectiveStep >>= (4 - moduleOctave);
        }
    
        phaseAcc += scaledEffectiveStep;
        int mainSample = computeWaveform(phaseAcc);
    
        int32_t mixSum = mainSample;
        uint8_t voices = 1;
        for (uint8_t i = 0; i < activeNoteCount; i++) {
            uint32_t noteStep = activeNotes[i].stepSize;
            if (moduleOctave > 4) {
                noteStep <<= (moduleOctave - 4);
            } else if (moduleOctave < 4) {
                noteStep >>= (4 - moduleOctave);
            }
            activeNotes[i].phaseAcc += noteStep;
            mixSum += computeWaveform(activeNotes[i].phaseAcc);
            voices++;
        }
        int normalizedSample = mixSum / voices;
        int volume = sysState.knob3.getRotation();
        if (volume < 0) volume = 0;
        if (volume > 8) volume = 8;
        int scaledSample = (normalizedSample * volume) / 8;
        int finalOutput = scaledSample + 128;
        if (finalOutput < 0) finalOutput = 0;
        if (finalOutput > 255) finalOutput = 255;
        analogWrite(OUTR_PIN, finalOutput);
    }
#ifdef MEASURE_TASK_TIMES
    uint32_t endISR = DWT->CYCCNT;
    // Convert cycles to microseconds:
    uint32_t elapsedCycles = endISR - startISR;
    // Assuming SystemCoreClock is defined (e.g., in Hz)
    uint32_t elapsed = elapsedCycles / (SystemCoreClock / 1000000);
    if (elapsed > maxSampleISRTime) {
        maxSampleISRTime = elapsed;
    }
#endif
    
}




void CAN_RX_ISR (void) {
	uint8_t RX_Message_ISR[8];
	uint32_t ID;
	CAN_RX(ID, RX_Message_ISR);
	xQueueSendFromISR(msgInQ, RX_Message_ISR, NULL); // Send the received message to the queue
}

void CAN_TX_ISR (void) {
	xSemaphoreGiveFromISR(CAN_TX_Semaphore, NULL);
}


////////////////////////////////////////////////// DEBUG MONITOR TASK //////////////////////////////////////////////////
#ifdef MEASURE_TASK_TIMES
extern volatile uint32_t maxScanKeysTime;
extern volatile uint32_t maxDisplayUpdateTime;
extern volatile uint32_t maxDecodeTime;
extern volatile uint32_t maxCAN_TX_Time;
extern volatile uint32_t maxSampleISRTime;
#endif



void debugMonitorTask(void * pvParameters) {
    const TickType_t xFrequency = 1000 / portTICK_PERIOD_MS; // once per second
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

#ifdef MEASURE_TASK_TIMES
        Serial.println("----- Task Timing (us) -----");
        Serial.print("maxScanKeysTime: "); Serial.println(maxScanKeysTime);
        Serial.print("maxDisplayUpdateTime: "); Serial.println(maxDisplayUpdateTime);
        Serial.print("maxDecodeTime: "); Serial.println(maxDecodeTime);
        Serial.print("maxCAN_TX_Time: "); Serial.println(maxCAN_TX_Time);
        Serial.print("maxSampleISRTime: "); Serial.println(maxSampleISRTime);
        Serial.println("----------------------------\n");
#endif
    }
}

#ifdef MEASURE_TASK_TIMES
// Call this once in setup to enable the DWT cycle counter on Cortex-M devices:
void enableCycleCounter() {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
#endif


// ------------------------- SETUP & LOOP ------------------------------------ //

void setup() {
    Serial.begin(9600);
#ifdef TEST_SCANKEYS
    delay(3000);
#endif
    Serial.println("Synth Initialized");

    // Configure pins
    pinMode(RA0_PIN, OUTPUT);
    pinMode(RA1_PIN, OUTPUT);
    pinMode(RA2_PIN, OUTPUT);
    pinMode(REN_PIN, OUTPUT);
    pinMode(OUT_PIN, OUTPUT);
    pinMode(OUTL_PIN, OUTPUT);
    pinMode(OUTR_PIN, OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);

    pinMode(C0_PIN, INPUT);
    pinMode(C1_PIN, INPUT);
    pinMode(C2_PIN, INPUT);
    pinMode(C3_PIN, INPUT);
    pinMode(JOYX_PIN, INPUT);
    pinMode(JOYY_PIN, INPUT);

    //Initialise display
    setOutMuxBit(DRST_BIT, LOW);  //Assert display logic reset
    delayMicroseconds(2);
    setOutMuxBit(DRST_BIT, HIGH);  //Release display logic reset
    u8g2.begin();
    setOutMuxBit(DEN_BIT, HIGH);  //Enable display power supply

    for (uint8_t i = 0; i < MAX_POLYPHONY; i++) {
        activeNotes[i].stepSize = 0;
        activeNotes[i].phaseAcc = 0;
    }
    activeNoteCount = 0;
    
    
    // Initialize audio sample timer
    sampleTimer.setOverflow(SAMPLE_RATE, HERTZ_FORMAT);
//#ifndef DISABLE_ISRS
    sampleTimer.attachInterrupt(sampleISR);
//#endif
    sampleTimer.resume();
    
    CAN_Init(true);
    setCANFilter(0x123, 0x7ff); 
//#ifndef DISABLE_ISRS
    CAN_RegisterRX_ISR(CAN_RX_ISR);
    CAN_RegisterTX_ISR(CAN_TX_ISR);
//#endif
    CAN_Start();
    
    sysState.mutex = xSemaphoreCreateMutex();
#ifdef MEASURE_TASK_TIMES
    enableCycleCounter();
#endif
    msgInQ = xQueueCreate(36, 8);
#ifdef TEST_SCANKEYS
    msgOutQ = xQueueCreate(384, 8);  // Larger queue for test iterations.
#else
    msgOutQ = xQueueCreate(36, 8);
#endif
    CAN_TX_Semaphore = xSemaphoreCreateCounting(3, 3);



#ifndef DISABLE_THREADS
    Serial.print("modulerole: ");
    Serial.println(moduleRole);
    TaskHandle_t scanKeysHandle = NULL;
    xTaskCreate(scanKeysTask, "scanKeys", 64, NULL, 2, &scanKeysHandle);
    
    TaskHandle_t displayUpdateHandle = NULL;
    xTaskCreate(displayUpdateTask, "displayUpdate", 256, NULL, 1, &displayUpdateHandle);
    
    // Always create decodeTask so that received messages are processed.
    TaskHandle_t decodeTaskHandle = NULL;
    xTaskCreate(decodeTask, "decodeTask", 128, NULL, 1, &decodeTaskHandle);



    if (moduleRole == SENDER) {
        TaskHandle_t CAN_TX_Handle = NULL;
        xTaskCreate(CAN_TX_Task, "CAN_TX_Task", 128, NULL, 1, &CAN_TX_Handle);
    }

    TaskHandle_t debugHandle = NULL;
    xTaskCreate(debugMonitorTask, "debugMonitor", 256, NULL, 1, &debugHandle);

    // Start the scheduler
    vTaskStartScheduler();

#endif

////////////////////////////// TEST CODE ///////////////////////////////////////
#ifdef TEST_SCANKEYS
{
    // In test mode, execute the scanKeysTask 32 times (without starting the scheduler)
    // Flush the transmit queue before timing.
    xQueueReset(msgOutQ);
    uint32_t startTime_scanKeys = micros();
    for (int iter = 0; iter < 32; iter++) {
        scanKeysTask(NULL);
    }
    uint32_t elapsed_scanKeys = micros() - startTime_scanKeys;
    Serial.print("32 iterations of scanKeysTask() took: ");
    Serial.print(elapsed_scanKeys);
    Serial.println(" microseconds");
    uint32_t avgTime_scanKeys = elapsed_scanKeys / 32;
    Serial.print("Average time per iteration: ");
    Serial.println(avgTime_scanKeys);
    
    while(1);
}
#endif

#ifdef TEST_DECODE
{
    // Preload msgInQ with 32 test messages.
    uint8_t testMsg[8] = { 'P', 4, 0, 0, 0, 0, 0, 0 };  // A sample press message.
    for (int i = 0; i < 32; i++) {
        xQueueSend(msgInQ, testMsg, portMAX_DELAY);
    }
  
    uint32_t startTime_decode = micros();
    for (int iter = 0; iter < 32; iter++) {
        uint8_t localMsg[8];
        // Wait (blocking) for a test message.
        if (xQueueReceive(msgInQ, localMsg, portMAX_DELAY) == pdPASS) {
            TASK_START();  // Begin timing this decode iteration

            // --- DecodeTask processing logic ---
            if (localMsg[0] == 'R') {  // Release message: remove note.
                uint8_t note = localMsg[2];
                for (uint8_t i = 0; i < activeNoteCount; i++) {
                    if (activeNotes[i].stepSize == stepSizes[note]) {
                        // Remove note by shifting remaining notes.
                        for (uint8_t j = i; j < activeNoteCount - 1; j++) {
                            activeNotes[j] = activeNotes[j + 1];
                        }
                        activeNoteCount--;
                        break;
                    }
                }
            } else if (localMsg[0] == 'P') {  // Press message: add note.
                uint8_t octave = localMsg[1];
                uint8_t note = localMsg[2];
                if (note < 12 && activeNoteCount < MAX_POLYPHONY) {
                    uint32_t step = stepSizes[note];
                    activeNotes[activeNoteCount].stepSize = step;
                    activeNotes[activeNoteCount].phaseAcc = 0;
                    activeNotes[activeNoteCount].elapsed = 0;
                    activeNoteCount++;
                }
            }
            // Update RX_Message for debug (protected by mutex)
            xSemaphoreTake(sysState.mutex, portMAX_DELAY);
            memcpy(sysState.RX_Message, localMsg, sizeof(sysState.RX_Message));
            xSemaphoreGive(sysState.mutex);
            // --- End decodeTask processing ---

            TASK_END(maxDecodeTime);  // End timing and update maxDecodeTime
        }
    }
    uint32_t elapsed_decode = micros() - startTime_decode;
    uint32_t avgTime_decode = elapsed_decode / 32;
    Serial.print("32 iterations of decodeTask took: ");
    Serial.print(elapsed_decode);
    Serial.println(" microseconds");
    Serial.print("Average time per iteration: ");
    Serial.print(avgTime_decode);
    Serial.println(" microseconds");
  
    // Assuming that decodeTask events occur roughly every 100ms, compute CPU load:
    float cpuLoad_decode = (avgTime_decode / 100000.0f) * 100.0f;
    Serial.print("DecodeTask CPU Load: ");
    Serial.print(cpuLoad_decode, 2);
    Serial.println(" %");
  
    while(1);
}
#endif

#ifdef TEST_CAN_TX
{
    // Preload msgOutQ with 32 test messages.
    for (int i = 0; i < 32; i++) {
        uint8_t testMsg[8] = { 'P', 4, (uint8_t)i, 0, 0, 0, 0, 0 };
        xQueueSend(msgOutQ, testMsg, portMAX_DELAY);
    }
  
    uint32_t startTime_canTX = micros();
    for (int i = 0; i < 32; i++) {
        TASK_START();  // Begin timing this iteration
        uint8_t msgOut[8];
        // Wait (blocking) for a test message from msgOutQ.
        xQueueReceive(msgOutQ, msgOut, portMAX_DELAY);
      
        // Simulate CAN_TX call. In real operation, you would call:
        // xSemaphoreTake(CAN_TX_Semaphore, portMAX_DELAY);
        // CAN_TX(0x123, msgOut);
        // For test mode, you might simply simulate a minimal delay.
      
        TASK_END(maxCAN_TX_Time);  // End timing this iteration and update maxCAN_TX_Time
    }
    uint32_t elapsed_canTX = micros() - startTime_canTX;
    uint32_t avgTime_canTX = elapsed_canTX / 32;
    Serial.print("32 iterations of CAN_TX_Task took: ");
    Serial.print(elapsed_canTX);
    Serial.println(" microseconds");
    Serial.print("Average time per iteration: ");
    Serial.print(avgTime_canTX);
    Serial.println(" microseconds");
  
    // Assuming a period of 20ms (20000 us) for CAN messages:
    float cpuLoad_canTX = (avgTime_canTX / 20000.0f) * 100.0f;
    Serial.print("CAN_TX_Task CPU Load: ");
    Serial.print(cpuLoad_canTX, 2);
    Serial.println(" %");
  
    while(1);
}
#endif

#ifdef TEST_DISPLAYUPDATE
{
    // Execute one iteration of displayUpdateTask (simulate the task code without the loop)
    TASK_START();
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));

    // Update joystick values
    int rawJoyX = analogRead(JOYX_PIN);
    int rawJoyY = analogRead(JOYY_PIN);
    joyX12Val = map(rawJoyX, 800, 119, 0, 12);
    joyY12Val = map(rawJoyY, 800, 119, 0, 12);

    // Read knob value under mutex
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    int rotationCopy = sysState.knob3.getRotation();
    xSemaphoreGive(sysState.mutex);

    // Prepare the display buffer
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(2, 10, moduleRole == SENDER ? "SENDER" : "RECEIVER");
    u8g2.setCursor(75, 10);
    u8g2.print("(");
    u8g2.print(joyX12Val);
    u8g2.print(",");
    u8g2.print(joyY12Val);
    u8g2.print(")");

    // Display current waveform
    u8g2.setCursor(2, 20);
    if (currentWaveform == SAWTOOTH) u8g2.print("Sawtooth");
    else if (currentWaveform == TRIANGLE) u8g2.print("Triangle");
    else if (currentWaveform == SINE) u8g2.print("Sine");
    else if (currentWaveform == SQUARE) u8g2.print("Square");
    else if (currentWaveform == PULSE) u8g2.print("Pulse");
    else if (currentWaveform == NOISE) u8g2.print("Noise");
    else if (currentWaveform == PIANO) u8g2.print("Piano");

    // Display volume and pitch
    u8g2.setCursor(2, 30);
    u8g2.print("Volume: ");
    u8g2.print(rotationCopy);
    u8g2.setCursor(66, 20);
    u8g2.print("Pitch: ");
    u8g2.print(moduleOctave);

    // Display last received CAN message
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    u8g2.setCursor(66, 30);
    u8g2.print(sysState.RX_Message[0] == 'P' ? "P" : "R");
    u8g2.print(sysState.RX_Message[1]);
    u8g2.print(sysState.RX_Message[2]);
    xSemaphoreGive(sysState.mutex);

    u8g2.sendBuffer();
    digitalToggle(LED_BUILTIN);
    TASK_END(maxDisplayUpdateTime);

    // Now perform 32 iterations of the same display update code:
    uint32_t startTime_display = micros();
    for (int iter = 0; iter < 32; iter++) {
         TASK_START();
         digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));

         int rawJoyX = analogRead(JOYX_PIN);
         int rawJoyY = analogRead(JOYY_PIN);
         joyX12Val = map(rawJoyX, 800, 119, 0, 12);
         joyY12Val = map(rawJoyY, 800, 119, 0, 12);

         xSemaphoreTake(sysState.mutex, portMAX_DELAY);
         int rotationCopy = sysState.knob3.getRotation();
         xSemaphoreGive(sysState.mutex);

         u8g2.clearBuffer();
         u8g2.setFont(u8g2_font_ncenB08_tr);
         u8g2.drawStr(2, 10, moduleRole == SENDER ? "SENDER" : "RECEIVER");
         u8g2.setCursor(75, 10);
         u8g2.print("(");
         u8g2.print(joyX12Val);
         u8g2.print(",");
         u8g2.print(joyY12Val);
         u8g2.print(")");

         u8g2.setCursor(2, 20);
         if (currentWaveform == SAWTOOTH) u8g2.print("Sawtooth");
         else if (currentWaveform == TRIANGLE) u8g2.print("Triangle");
         else if (currentWaveform == SINE) u8g2.print("Sine");
         else if (currentWaveform == SQUARE) u8g2.print("Square");
         else if (currentWaveform == PULSE) u8g2.print("Pulse");
         else if (currentWaveform == NOISE) u8g2.print("Noise");
         else if (currentWaveform == PIANO) u8g2.print("Piano");

         u8g2.setCursor(2, 30);
         u8g2.print("Volume: ");
         u8g2.print(rotationCopy);
         u8g2.setCursor(66, 20);
         u8g2.print("Pitch: ");
         u8g2.print(moduleOctave);

         xSemaphoreTake(sysState.mutex, portMAX_DELAY);
         u8g2.setCursor(66, 30);
         u8g2.print(sysState.RX_Message[0] == 'P' ? "P" : "R");
         u8g2.print(sysState.RX_Message[1]);
         u8g2.print(sysState.RX_Message[2]);
         xSemaphoreGive(sysState.mutex);

         u8g2.sendBuffer();
         digitalToggle(LED_BUILTIN);
         TASK_END(maxDisplayUpdateTime);
    }
    uint32_t elapsed_display = micros() - startTime_display;
    uint32_t avgTime_display = elapsed_display / 32;
    Serial.print("32 iterations of displayUpdateTask took: ");
    Serial.print(elapsed_display);
    Serial.println(" microseconds");
    Serial.print("Average time per iteration: ");
    Serial.print(avgTime_display);
    Serial.println(" microseconds");
    float cpuLoad_display = (avgTime_display / 100000.0f) * 100.0f; // Assuming period of 100ms
    Serial.print("displayUpdateTask CPU Load: ");
    Serial.print(cpuLoad_display, 2);
    Serial.println(" %");
    while(1);
}
#endif


////////////////////////////// END TEST CODE ///////////////////////////////////////

#ifndef DISABLE_THREADS
    // Start the FreeRTOS scheduler; this should never return in normal operation.
    vTaskStartScheduler();
#endif
}

void loop() {
    // Empty. All tasks run under FreeRTOS.    

}
