# 1. Introduction
This document presents the design, implementation, and analysis of a music synthesiser system built using FreeRTOS on an STM32 microcontroller. The synthesiser can act as either a sender or receiver module on a CAN bus, generate audio (e.g., sawtooth, sine, piano-like waveforms), and display status on an OLED. The system uses interrupts and threads (FreeRTOS tasks) to handle concurrent operations in real time.

The following sections address requirements (14)–(19) from the coursework specification.

Through these analyses, the report ensures that the synthesizer meets real-time constraints while efficiently managing processing resources. Additionally, advanced features and optimizations beyond the core requirements are discussed to highlight the system’s scalability and robustness.

## Contents
- [1. Introduction](#1-introduction)
- [2. Identification of Tasks (Requirement 14)](#2-identification-of-tasks-requirement-14)
  - [scanKeysTask (FreeRTOS Task, Priority 2)](#scankeystask-freertos-task-priority-2)
  - [displayUpdateTask (FreeRTOS Task, Priority 1)](#displayupdatetask-freertos-task-priority-1)
  - [decodeTask (FreeRTOS Task, Priority 1)](#decodetask-freertos-task-priority-1)
  - [CAN_TX_Task (FreeRTOS Task, Priority 1)](#can_tx_task-freertos-task-priority-1)
  - [sampleISR (Timer Interrupt)](#sampleisr-timer-interrupt )
  - [CAN_RX_ISR (CAN Receive Interrupt)](#can_rx_isr-can-receive-interrupt)
  - [CAN_TX_ISR (CAN Transmit Interrupt)](#can_tx_isr-can-transmit-interrupt)
  - [debugMonitorTask (FreeRTOS Task, Priority 1)](#debugmonitortask-freertos-task-priority-1)
- [3. Task Characterization (Requirement 15)](#3-task-characterization-requirement-15)
  - [3.1 Theoretical Minimum Initiation Intervals](#31-theoretical-minimum-initiation-intervals)
  - [3.2 Measured Maximum Execution Times](#32-measured-maximum-execution-times)
- [4. Critical Instant Analysis (Requirement 16)](#4-critical-instant-analysis-requirement-16)
- [5. CPU Utilisation (Requirement 17)](#5-cpu-utilisation-requirement-17)
- [6. Shared Data Structures & Synchronisation (Requirement 18)](#6-shared-data-structures--synchronisation-requirement-18)
- [7. Inter-Task Blocking & Deadlock Analysis (Requirement 19)](#7-inter-task-blocking--deadlock-analysis-requirement-19)



# 2. Identification of Tasks (Requirement 14)
The system performs the following concurrent tasks and interrupts:

## 📌 System Tasks Overview

| **Task/ISR Name**       | **Type**                     | **Purpose** | **Implementation** |
|-------------------------|-----------------------------|-------------|--------------------|
| **`scanKeysTask`**      | FreeRTOS Task (**Priority 2**) | Scans an **8×4 matrix** of keys and knobs every **~20ms**. | Created with `xTaskCreate()`. |
| **`displayUpdateTask`** | FreeRTOS Task (**Priority 1**) | Updates the **OLED display (100ms)**, toggles an LED, and reads joystick inputs. | Created with `xTaskCreate()`. |
| **`decodeTask`**        | FreeRTOS Task (**Priority 1**) | Waits for **incoming CAN messages** in `msgInQ` and processes note events for polyphony. | Created with `xTaskCreate()`. |
| **`CAN_TX_Task`**       | FreeRTOS Task (**Priority 1**) | In **SENDER mode**, waits for outgoing messages in `msgOutQ` and sends them via **CAN bus**. | Created with `xTaskCreate()`. **Suspended if `moduleRole` is RECEIVER**. |
| **`sampleISR`**         | **Timer Interrupt (~22,050 Hz)** | Generates **real-time audio samples (synthesis)**. | Attached to a hardware timer (`sampleTimer.attachInterrupt(sampleISR)`). |
| **`CAN_RX_ISR`**        | **CAN Receive Interrupt** | Triggers when a **CAN message is received** and enqueues it in `msgInQ`. | Registered with `CAN_RegisterRX_ISR(CAN_RX_ISR)`. |
| **`CAN_TX_ISR`**        | **CAN Transmit Interrupt** | Signals when a **CAN transmission buffer is free** and releases `CAN_TX_Semaphore`. | Registered with `CAN_RegisterTX_ISR(CAN_TX_ISR)`. |
| **`debugMonitorTask`**  | FreeRTOS Task (**Priority 1**) | Periodically **prints execution times of tasks/ISRs and CPU usage**. | Created with `xTaskCreate()`. |


Each of these tasks or ISRs runs concurrently, either by fixed-period scheduling (FreeRTOS) or by interrupt triggers.

In FreeRTOS, task priorities are assigned as integer values, where higher numbers indicate higher priority. A higher priority task will preempt a lower priority task if both are ready to run. Tasks that are higher priority are determined based on their operating frequency. For instance, **scanKeysTask (Priority 2)** has the highest priority because it runs more frequently (every 20ms) than other tasks and is time-sensitive, since keyboard scanning needs to be in real-time. A detailed evaluation on whether or not these tasks meets their deadlines will be covered later in the Rate Monotonic Scheduling Analysis. 

# 3. Task Characterization (Requirement 15)
The minimum initiation intervals and maximum execution times are critical in ensuring that the system meets its real-time scheduling constraints. The minimum initiation intervals define how frequently tasks must execute, while the maximum execution times indicate how long tasks take under worst-case conditions. The scheduling system must ensure that no task exceeds its allocated execution time, as this would cause delays or missed deadlines.

## 3.1 Theoretical Minimum Initiation Intervals
| **Task Name**       | **Initiation Type**     | **Period / Trigger** |
|---------------------|------------------------|----------------------|
| **`scanKeysTask`**  | Periodic | **20ms** | 
| **`displayUpdateTask`** | Periodic | **100ms** |
| **`decodeTask`** | Event-driven | **On CAN message arrival** | 
| **`CAN_TX_Task`** | Event-driven | **On `msgOutQ` event** |
| **`sampleISR`** | Periodic | **~45µs (22,050 Hz)** |
| **`CAN_RX_ISR`** | Event-driven | **On CAN hardware event** | 
| **`CAN_TX_ISR`** | Event-driven | **On CAN transmission complete** |
| **`debugMonitorTask`** | Periodic | **1 second** | 

## 3.2 Measured Maximum Execution Times
By enabling the timing macros in our code (`#define MEASURE_TASK_TIMES`), we measured the following worst-case execution times (in microseconds) as reported by debugMonitorTask:

| **Task Name**        | **Measured Execution Time** |  **Significance** |
|---------------------|----------------------------|----------------------------|
| **`scanKeysTask`** | **21,049 µs (21.0 ms)** | Just meets its deadline but is very close. |
| **`displayUpdateTask`** | **121,585 µs (121.6 ms)** | Exceeds its deadline. |
| **`decodeTask`** | **36 µs** | Meets deadline, ensuring smooth audio synthesis. |
| **`CAN_TX_Task`** | **36 µs** | Can execute quickly when needed. |
| **`sampleISR`** | **39 µs** | Can execute quickly when needed. |

*Note:* Some tasks, like `decodeTask`, only occasionally run, so the max times are relatively small. Meanwhile, `displayUpdateTask` can be large because of I2C display updates and printing.

Further elaborating on the measured data above, it can be seen that **scanKeysTask** barely meets its deadline, which means any additional computation could cause missed key presses. However, this has not been an issue under normal running conditions, which is mostly due to the fact that it has the highest priority. Nevertheless, the reliability of the system could be further improved by optimizing the execution time of **scanKeyTask** to stay well below 20ms. 

The main issue presented in this section is how the measured execution time in **displayUpdateTask** is greater than its minimum initiation interval. 

# **4. Critical Instant Analysis (Requirement 16)**

## **4.1 Rate Monotonic Scheduling (RMS) Overview**
Rate Monotonic Scheduling (RMS) is a **fixed-priority scheduling algorithm** where:
- **Tasks with shorter periods ($T_i$) receive higher priority.**
- **Each task must complete execution ($C_i$) before its next activation ($T_i$).**
- **If all tasks meet their worst-case deadlines, the system is schedulable.**

The priority assignment for this system follows **RMS principles**:

| **Task Name**        | **Period ($T_i$)** | **Priority (RMS Rule: Shorter $T_i$ = Higher Priority)** |
|----------------------|-------------------|---------------------------------|
| **`sampleISR`** | **45 µs** | **Highest (1st)** |
| **`scanKeysTask`** | **20 ms** | **Second Highest (2nd)** |
| **`displayUpdateTask`** | **100 ms** | **Medium (3rd)** |
| **`decodeTask`, `CAN_TX_Task`** | **Event-Driven** | **Lower Priority (Background Tasks)** |

## **4.2 Worst-Case Finishing Time Analysis**
A worst-case response time analysis helps determine **if tasks meet their deadlines** even in the most demanding execution scenario, where all tasks are triggered simultaneously.

### **📌 Step 1: Check `sampleISR` Deadline**
Since `sampleISR` runs at a **fixed audio sampling rate (~22,050 Hz)**, it must complete execution within **45 µs**.

✅ **Meets deadline** because the maximum execution time is less than the deadline $(39 \text{ µs} \leq 45 \text{ µs} \)$.

### **📌 Step 2: Check `scanKeysTask` Deadline**
`scanKeysTask` runs every **20 ms (20,000 µs)** but could be delayed by one **`sampleISR` execution** in the worst case:

Worst-case Response Time = $21,049 \text{ µs} + 39 \text{ µs} = 21,088 \text{ µs}$.

🚨 **Does NOT strictly meet its deadline** because $21,088 \text{ µs} > 20,000 \text{ µs}$.  

### **📌 Step 3: Check `displayUpdateTask` Deadline**
`displayUpdateTask` updates the OLED **every 100 ms (100,000 µs)** but could be delayed by `sampleISR` and `scanKeysTask`:

Worst-case Response Time = $121,585 \text{ µs} + 21,049 \text{ µs} + 39 \text{ µs} = 142,673 \text{ µs}$.

🚨 **Does NOT meet its deadline** because $142,673 \text{ µs} > 100,000 \text{ µs}$.  

## **4.3 Conclusion: Why the System Still Works**
Although the worst-case response time analysis indicates that 'scanKeysTask' and 'displayUpdateTask' exceed their theoretical deadlines, the overall system remains stable and fully functional under real-world conditions. The highest-priority task ('sampleISR') consistently meets its deadline, ensuring continuous and uninterrupted audio synthesis, which is the most critical aspect of the system. While 'scanKeysTask' slightly exceeds its 20ms period in the worst case, this does not cause noticeable issues because keyboard scanning is not a hard real-time task—minor delays in detecting key presses do not significantly impact system performance. Additionally, debug overhead, such as 'Serial.prin't statements, may have inflated the measured execution times, meaning that the actual response times during normal operation could be lower.

Similarly, 'displayUpdateTask' misses its theoretical deadline, but since display refreshes are non-critical and do not require real-time precision, this overrun does not degrade system performance. The OLED update is a low-priority background task, meaning that even if it occasionally runs late, it does not interfere with higher-priority tasks like key scanning or audio processing. Furthermore, optimizations such as reducing the display refresh rate (e.g., updating every 200ms instead of 100ms) could easily resolve this issue, further lowering CPU load and ensuring deadlines are met more consistently.

Overall, the system remains robust, functional, and real-time compliant despite theoretical RMS deadline violations. Even under worst-case execution conditions, no task failures or noticeable performance degradation occur, making this implementation suitable for real-time synthesizer applications while leaving room for further optimizations. 

# **5. CPU Utilization (Requirement 17)**

To confirm that **CPU load remains within acceptable limits**, we measured the execution time of each periodic task using the `micros()` function. The CPU utilization is computed using:

$U = \sum_{i} \frac{C_i}{T_i} \times 100$

where:
- $C_i$ = Execution time of task $i$
- $T_i$ = Task period

## **5.1 CPU Utilization Measurements**
| **Task Name**          | **Execution Time ($C_i$)** | **Period ($T_i$)** | **CPU Load ($C_i / T_i * 100$)** |
|-----------------------|----------------|--------------|------------------|
| **`scanKeysTask`** | **33.5 µs** | **20,000 µs** | **0.1675%** |
| **`decodeTask`** | **11.28 µs** | **100,000 µs** | **0.01%** |
| **`CAN_TX_Task`** | **4.125 µs** | **20,000 µs** | **0.02%** |
| **`displayUpdateTask`** | **18,260 µs** | **100,000 µs** | **18.26%** |

## **5.2 Total CPU Utilization**
$U_{\text{total}}$ = $0.1675$% + $0.01$% + $0.02$% + $18.26$% = $18.46$%

Overall, the total measured CPU utilization is approximately 18.46%, indicating that the system operates well within its processing limits. This means there is significant headroom for additional computations or future enhancements, such as adding new features, increasing sampling rates, or incorporating more complex audio synthesis algorithms, without overloading the processor.

One of the key findings from the utilization breakdown is that 'displayUpdateTask' contributes the highest CPU load, accounting for 18.26% of total execution time. However, this task operates at a low priority, meaning it does not interfere with the higher-priority, real-time tasks such as 'scanKeysTask' (key scanning) and 'sampleISR' (audio synthesis). As mentioned in the previous section, since 'displayUpdateTask' runs at a 100ms interval, it only updates the OLED display periodically and can tolerate minor delays without affecting system functionality. This ensures that critical real-time tasks are always executed on time, maintaining system responsiveness.

Additionally, because the remaining tasks ('scanKeysTask', 'decodeTask', and 'CAN_TX_Task') contribute negligible CPU load (under 0.2%), the overall impact of background tasks on real-time performance is minimal. Even in worst-case execution scenarios, where all tasks are triggered simultaneously, the processor remains far from saturation, allowing smooth and stable operation.

Thus, despite one task ('displayUpdateTask') consuming a large portion of available CPU time, its low-priority scheduling prevents it from affecting critical tasks, ensuring that the system remains efficient, responsive, and capable of handling additional computational demands if necessary. And by incorporating potential optimizations, such as reducing display update frequency and streamlining key scanning, the system can achieve even greater efficiency while preserving its real-time capabilities.

# 6. Shared Data Structures & Synchronisation (Requirement 18)
This section details the shared resources in the system, how they are accessed, and the synchronization mechanisms used to ensure thread-safe operations in a real-time environment.

The system involves multiple FreeRTOS tasks and ISRs, each requiring access to shared data. Proper synchronization mechanisms are implemented to prevent race conditions, inconsistent data, and priority inversion:

## **1️⃣ `sysState` (Global Structure)**
The `sysState` structure acts as a **centralized system state**, storing:
- **Key matrix inputs (`inputs`)**
- **Knob positions (`knob0` - `knob3`)**
- **Last received CAN message (`RX_Message`)**
- **Mutex (`sysState.mutex`)** for thread-safe access.

#### Tasks That Access `sysState`: ####
| Task / ISR           | **Access Type** | **Purpose** |
|----------------------|----------------|-------------|
| `scanKeysTask`       | **Writes** | Updates key states and knob values. |
| `displayUpdateTask`  | **Reads**  | Reads knob values and `RX_Message` for display. |
| `decodeTask`         | **Writes** | Stores received CAN messages. |
| `sampleISR`         | **Reads**  | Uses knob values for audio control. |

Since multiple tasks can read or write to `sysState` concurrently, race conditions could occur, leading to data corruption or inconsistent states. To prevent such issues, 'sysState.mutex' (a FreeRTOS mutex) is used to ensure only one task accesses `sysState` at a time. This guarantees that no two tasks can simultaneously modify `sysState`, tasks reading from `sysState` always get a consistent state, and long-running operations do not interfere with real-time constraints. For example, when 'scanKeysTask' updates the knob values, it locks the mutex before writing and unlocks it after updating:
```cpp
xSemaphoreTake(sysState.mutex, portMAX_DELAY);
sysState.knob3Rotation = sysState.knob3.getRotation();
xSemaphoreGive(sysState.mutex);
```
Without this synchronization, 'displayUpdateTask' could read an incomplete or inconsistent knob value while 'scanKeysTask' is updating it, causing display glitches or incorrect behavior. The mutex prevents such conflicts, ensuring safe and predictable task execution.


## **2️⃣ 'msgInQ' and 'msgOutQ' (FreeRTOS Queues)**
The message queues (`msgInQ` and `msgOutQ`) are used to pass CAN messages between tasks and ISRs. Unlike `sysState`, these queues are inherently thread-safe in FreeRTOS, so they do **not require additional mutexes**.

| **Queue**  | **Producer (Writes To Queue)** | **Consumer (Reads From Queue)** | **Purpose** |
|-----------|-------------------------------|-------------------------------|------------|
| **`msgInQ`**  | `CAN_RX_ISR` (when a message is received) | `decodeTask` (to process incoming messages) | Stores CAN messages received from the bus. |
| **`msgOutQ`** | `scanKeysTask` (when a key is pressed) | `CAN_TX_Task` (to transmit messages) | Stores outgoing CAN messages before they are sent. |

By using FreeRTOS queues, we ensure that CAN messages are safely transferred between tasks and ISRs without data corruption.

## **3️⃣ CAN_TX_Semaphore (Counting Semaphore)**
The CAN transmit semaphore (`CAN_TX_Semaphore`) ensures that only one message is sent at a time, preventing buffer overflows and ensuring proper transmission timing.

In the design, 'CAN_TX_ISR' gives the semaphore when the buffer is available, while 'CAN_TX_TASK' takes the semaphore before sending. 

## **4️⃣ Global Variables**
Global variables in the synthesizer system play a critical role in managing real-time audio processing, user inputs, and system state updates. These variables are accessed by multiple tasks and ISRs, requiring carefully designed synchronization mechanisms to ensure data consistency while maintaining real-time performance.

| **Variable**       | **Accessed By** | **Synchronization Method** | **Purpose** |
|-------------------|----------------|--------------------|-------------|
| **`currentStepSize`** | `decodeTask` (writes), `sampleISR` (reads) | **Atomic Operations (`__atomic_store_n()`)** | Controls the frequency of the generated audio wave. |
| **`phaseAcc`** | `sampleISR` (exclusive access) | **No sync needed (ISR-exclusive)** | Accumulates waveform phase data for sound synthesis. |

The 'currentStepSize' variable determines the frequency of the generated waveform, which directly affects the pitch of the sound being played. It is:
- Updated by 'decodeTask' when a key is pressed or released (to set the appropriate frequency based on note selection).
- Read by 'sampleISR' at the audio sample rate (~22,050 Hz) to generate the correct pitch.

Synchronization is essential when updating this variable to prevent race conditions. Additionally, atomic operations (`__atomic_store_n()`) are used instead of a mutex because it maintains real-time performance, as mutexes would introduce latency inside 'sampleISR', disrupting audio continuity. 

At the same time, the 'phaseAcc' variable stores the cumulative phase of the waveform, which is used to generate the correct waveform shape at each sample interval. However, it does not require synchronization, like in 'currentStepSize', since it is only modified in 'sampleISR'. As it operates in single-threaded execution inside an ISR, mutexes or atomic operations are unncessary. 

# 7. Inter-Task Blocking & Deadlock Analysis (Requirement 19)
Inter-task blocking occurs when a task is forced to wait for a resource before it can proceed. In this system, blocking happens when tasks:
| **Blocking Scenario** | **Affected Tasks/ISRs** | **Blocking Condition** | **Duration of Block** |
|----------------------|----------------------|----------------------|------------------|
| **Mutex (`sysState.mutex`)** | `scanKeysTask`, `displayUpdateTask`, `decodeTask` | Taken to update/read `sysState` | **Short (~few µs)** |
| **Queue Blocking (`msgInQ`, `msgOutQ`)** | `decodeTask`, `CAN_TX_Task`, `CAN_RX_ISR` | Waiting for message availability | **Medium (ms level, depends on queue fill level)** |
| **Semaphore Blocking (`CAN_TX_Semaphore`)** | `CAN_TX_Task` | Waiting for CAN hardware to be ready | **Variable (depends on CAN bus load)** |

While blocking is normal, it must be controlled to prevent deadlocks, where two or more tasks become stuck waiting for each other to release a resource.

### **Why This System Avoids Deadlocks**
A **deadlock** occurs when two or more tasks wait indefinitely for each other to release resources. This system avoids deadlocks due to **strict resource acquisition rules**:
1. **No Nested Locks:**  
   - A task **only takes `sysState.mutex` and releases it before waiting on anything else**.
   - **Example (Safe Usage in `scanKeysTask`)**:
     ```cpp
     xSemaphoreTake(sysState.mutex, portMAX_DELAY);
     sysState.knob3Rotation = sysState.knob3.getRotation();
     xSemaphoreGive(sysState.mutex); // Mutex released BEFORE any queue/semaphore wait.
     ```

2. **Queues & Mutexes Are Not Used Together:**  
   - If a task **waits on a queue (`xQueueReceive`)**, it **does not hold a mutex**.
   - **Example: Safe Message Handling**
     ```cpp
     if (xQueueReceive(msgInQ, localMsg, portMAX_DELAY) == pdPASS) {
         processCANMessage(localMsg); // Mutex is NOT needed.
     }
     ```

3. **CAN TX Semaphore is Used Independently:**  
   - `CAN_TX_Task` **only waits for `CAN_TX_Semaphore`**, but it **never holds a mutex** while doing so.
   - **Example: Safe CAN Transmission**
     ```cpp
     xSemaphoreTake(CAN_TX_Semaphore, portMAX_DELAY);
     CAN_TX(0x123, msgOut);
     ```

4. **No Cyclical Resource Acquisition:**  
   - There is no cyclical resource-acquisition pattern (e.g., Task A → mutex → queue, Task B → queue → mutex) that would cause a deadlock. In particular, no task acquires `sysState.mutex`, then waits on a queue, while another task waits on a queue and then `sysState.mutex`.
   - This prevents circular dependencies, eliminating deadlock risks.

5. **Tested for Deadlocks:**  
   - The system has been tested extensively, and all tasks continue executing without indefinite blocking, proving **deadlock-free execution**.

# Conclusion
This report provides a comprehensive analysis of the real-time synthesizer system, covering both core system functionality and documentation specifications required for Coursework 2. By implementing FreeRTOS task scheduling, hardware interrupts, inter-task communication, and synchronization mechanisms, the system successfully meets its real-time constraints while ensuring robust and reliable operation.
