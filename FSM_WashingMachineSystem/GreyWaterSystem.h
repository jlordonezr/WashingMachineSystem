#ifndef GREYWATER_SYSTEM_H
#define GREYWATER_SYSTEM_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include "FlowMeter.h"
#include "TankLevel.h"
#include "AgileStateMachine.h"
#include "GreyWaterSettings.h"
#include "GreyWaterOutputs.h"
#include "GreyWaterSensors.h"

// OLED display object declaration
// This is the external declaration for the OLED display object used for visual feedback.
extern Adafruit_SSD1306 oled;

// Flow sensor pulse counters, incremented by the interrupts (glitch-filtered)
extern volatile uint32_t tapPulseCount;
extern volatile uint32_t drainPulseCount;

// Pin Definitions
// These define the pins used for various sensors, buttons, and relays in the system.
#define TAP_PIN D2               // Pin for tap water flow sensor
#define DRAIN_PIN D3             // Pin for drain water flow sensor
#define OUT_DRAIN_EV D4          // Pin to control the drain solenoid valve
#define OUT_FILL_EV D5           // Pin to control the fill solenoid valve
#define OUT_TAP_EV D6            // Pin to control the tap solenoid valve
#define OUT_WASH_EV D0           // Pin to control the Wash tank solenoid valve.
                                 // D0 is also the board's RX0 (UART0) pin: do not use
                                 // Serial0 in this sketch (Serial goes over USB).
#define OUT_MOTOR D7             // Pin to control the motor
#define IN_HIGHLEVEL D8          // Pin for high-level water sensor
#define IN_LOWLEVEL D9           // Pin for low-level water sensor
#define STOP_BUTTON D10          // Pin for the stop button
#define START_BUTTON D11         // Pin for the start button


// Constants
// These constants define sensor types, OLED dimensions, relay states, and delays.
#define TAP_PULSES_PER_LITRE   660.0f // YF-B1 tap flow sensor
#define DRAIN_PULSES_PER_LITRE 352.0f // YF-B10 drain flow sensor
#define FLOW_ON_LPM     1.0f   // default: flow counts above this (L/min) for the detection time; stopped below it for 2 s
#define FLOW_GAP_MS     1000   // gaps shorter than this do not interrupt a starting flow
#define FLOW_STOP_MS    2000   // flow must stay below the stop level this long to count as stopped
#define FLOW_GLITCH_US  1000   // pulses closer than this are electrical noise and ignored
#define MIN_FILL_LITRES 3.0f   // default: a fill must take this much water before a drain can end the step
#define LEVEL_FAST_MS   200    // float switch: time to accept the SAFE reading (high wet, low dry)
#define LEVEL_SLOW_MS   2000   // float switch: time to accept the other reading (see TankLevel.h)
#define LEVEL_FAULT_MS  3000   // switches must disagree this long before it is reported
#define SLOW_LOOP_MS    1000   // a loop() pass longer than this is logged (once a minute at most)
#define BUZZER_PIN D12          // optional active buzzer (module with its own driver); off unless enabled in Settings
#define EVENT_LOG_SIZE 50       // events kept for the Service tab (in RAM; cleared at restart)
#define PUMP_START_GRACE_MS 1000 // after the pump starts, flow has this long to establish
#define PUMP_NO_FLOW_MS     300  // pump stops when no tap-sensor pulse arrives for this long
#define PUMP_DRY_ALERT      3    // alert after this many pump starts without flow in one cycle
#define OLED_WIDTH 128           // Width of the OLED display
#define OLED_HEIGHT 32           // Height of the OLED display
#define OPERATION_DELAY 250      // Delay of * miliseconds to operate each state.
#define CONTRL_SSR_DELAY 0     // Delay of * miliseconds to control SSR.
// Factory defaults for the settings below. At run time the values come from
// `cfg` (GreyWaterSettings.h) and can be changed on the web dashboard.
#define TRANSITION_DELAY 1200000 // Length of step 4 (20 minutes) before returning to Idle
#define EDGE_DETECTION_DELAY 4000 // Flow must last this long (ms) to count as "flow seen"
#define TANK_WASH_PERIOD 10000   // ms between tank-wash valve openings while the machine
                                 // fills with tank water (valve opens every 10 s)
#define TANK_WASH_OPEN_TIME 2000 // ms the tank-wash valve stays open in each period.
                                 // Must be smaller than TANK_WASH_PERIOD.
#define STEP_ALERT_DELAY 5400000UL // Alert when step 1-3 runs longer than 90 minutes
#define TANK_MAX_AGE_HOURS 48    // Alert when tank water is older than 48 hours
#define MANUAL_WASH_TIME 600000UL // Manual tank wash: safety time limit (10 min). The valve
                                 // normally closes earlier, when the high-level switch reports full
#define DEFAULT_NTFY_SERVER  "https://ntfy.sh"
#define DEFAULT_TIME_ZONE    "<-05>5"     // POSIX TZ string: UTC-5, no daylight saving
#define DEFAULT_WEB_PASSWORD "greywater"  // change on first use (dashboard user: admin)
#define DEFAULT_OTA_PASSWORD "greywater"  // change on first use
#define WIFI_SETUP_HOLD_MS 5000  // hold STOP this long while READY to open Wi-Fi setup
#define TANK_CHECK_PERIOD 10000  // ms between tank level / water age checks
#define DEBOUNCE_DELAY 20        // Debounce delay for button presses in milliseconds
// OLED display
#define DISPLAY_FRAME_MS 40      // ms between display frames (25 fps) while scrolling
#define SCROLL_SPEED_PX 2        // px the message moves per frame (2 px x 25 fps = 50 px/s)
#define SCROLL_PAUSE_MS 1500     // ms a long message rests at the start before scrolling
#define SCROLL_GAP_PX 40         // px of blank space between the end and the repeat
#define NOTICE_DURATION 4000     // ms a notice ("Cycle stopped", "Cycle complete") stays on

// Debugging
// Macro for debugging messages. Prints messages to the Serial Monitor if DEBUG_MODE is enabled.
#define DEBUG_MODE false
#define DEBUG_PRINT(x) \
  if (DEBUG_MODE) Serial.println(x)
// Shared Variables
// Variables for edge detection of tap and drain water flow.

// Variables for debouncing
// These variables store the last press time and state for the start and stop buttons.
extern unsigned long lastStartButtonPressTime; // Last press time for the start button
extern unsigned long lastStopButtonPressTime;  // Last press time for the stop button
extern bool lastStartButtonState;              // Last state of the start button
extern bool lastStopButtonState;               // Last state of the stop button

//Variables for SSR
extern int ssrON;
extern int ssrOFF;

// Variables for start/stop button states
// These variables indicate whether the start or stop button is currently pressed.
extern bool inStart, inStop;

// Variables to detect the FSM (Finite State Machine) status
// These flags indicate the status of various states in the FSM.
extern bool filled1, drained1; // Flags for the first fill and drain cycle
extern bool filled2;           // Flag for the second fill cycle

// Time tracking variables
// These variables are used to track timing for flow rate calculations and state transitions.
extern unsigned long lastCheckTime;  // Timer for periodic checks

// Enumerations
// Enumeration for state IDs in the FSM.
enum StateID {
  stIdle,    // Idle state
  stRun,     // Running state
  stFill1,   // First fill state
  stDrain1,  // First drain state
  stFill2,   // Second fill state
  stDrain2,  // Second drain state
  STATE_COUNT // Total number of states
};

// Enumeration for water levels in the tank.
enum WaterLevel {
  low,    // Low water level
  medium, // Medium water level
  high    // High water level
};

// Function Prototypes
// These are the declarations for all functions used in the system.

// State machine setup and logic
void onEnter(); // Function called when entering a state
void setupStateMachine(); // Function to initialize the state machine

// Input and SSR control
void updateInputs(); // Function to update button states
void controlSSR(int pin, int control, int wait); // Function to control a Solid State Relay (SSR)
//void setRelays(unsigned char tapStatus, unsigned char drainStatus, unsigned char fillStatus, unsigned char motorStatus); // Function to control relays

// Water management functions
void fillWashingMachineWithTapWater(); // Function to fill the washing machine with tap water
void fillWashingMachineWithTankWater(); // Function to fill the Washing Machine with Tank Water.
void drainSoapyWater(); // Function to drain soapy water
const char* checkAndFillWashingMachine(); // Fills the washing machine; returns an OLED-ready label describing whether tap or tank water was used
const char* checkAndFillWaterTank(); // Fills the water tank; returns an OLED-ready label describing whether water was saved to the tank or the tank was full
void noSavingWater(); // Function for "No Saving Water" mode

// Water level detection (debounced; see TankLevel.h)
WaterLevel getWaterLevelTank(int inPinLow, int inPinHigh); // kept for compatibility: returns tankLevel(), the pins are not read again
WaterLevel tankLevel();     // low / medium / high from the debounced float switches; high whenever the high float is wet
bool tankHasWater();        // low float wet: the pump may draw from the tank
bool tankFull();            // high float wet: nothing may add water to the tank
bool tankFault();           // high float wet but low float dry: check the switches

// OLED display functions
bool displayBegin();  // starts the OLED; false if it does not answer (the controller then runs without a screen)
bool displayAvailable();
void displaySetStatus(const char* title, const char* message, const char* detail = nullptr); // OLED title + short message; detail = longer sentence for the dashboard (defaults to message)
void displayShowNotice(const char* title, const char* message, unsigned long durationMs); // Shows a temporary message that overrides the status for durationMs
void displayClearNotice(); // Removes an active notice immediately
void displayUpdate(bool force = false); // Draws the OLED; call on every loop() pass (non-blocking, frame-timed). force = draw now
const char* displayGetTitle();   // Text currently shown in the OLED header
const char* displayGetMessage(); // Text currently shown as the OLED message
const char* displayGetDetail();  // Longer description of the current status (dashboard)

// Flow sensor functions
void  flowSensorsBegin();  // pin modes and interrupts for both flow sensors (call in setup)
float getTapFlowRate();    // tap flow in L/s (2 s average)
float getDrainFlowRate();  // drain flow in L/s (2 s average)

// State logic functions
void idleStateLogic(); // Logic for the idle state
void runStateLogic(); // Logic for the running state
void fill1StateLogic(); // Logic for the first fill state
void drain1StateLogic(); // Logic for the first drain state
void fill2StateLogic(); // Logic for the second fill state
void drain2StateLogic(); // Logic for the second drain state

// Washing machine status functions

// Sensor reset
void resetFlowSensors(); // Re-arms flow detection for the step that is starting

// Tank wash
void resetTankWash(); // Restarts the tank-wash timer so the next tank-water fill starts with an opening


// Helper functions for onEnter() function
void handleIdleStateEntry(); // Helper function for entering the Idle state
void handleRunStateEntry(); // Helper function for entering the Run state
void handleFill1StateEntry(); // Helper function for entering the 1st Fill state
void handleDrain1StateEntry(); // Helper function for entering the 1st Drain state
void handleFill2StateEntry(); // Helper function for entering the 2nd Fill state
void handleDrain2StateEntry(); // Helper function for entering the 2nd Drain state

// ── Wi-Fi era additions ────────────────────────────────────────────────────

// RelayMode (what the relays are doing) is defined in GreyWaterOutputs.h.

// Snapshot of everything the dashboard shows. Filled by getSystemStatus().
struct SystemStatus {
  int        state;            // StateID
  const char* stateName;       // "Ready", "Fill", ...
  const char* title;           // OLED header
  const char* message;         // OLED message
  WaterLevel tankLevel;
  bool       washValveOpen;
  uint32_t   tapPulses, drainPulses;   // running pulse totals (sensor check)
  bool       tapFlowing, drainFlowing; // debounced flow state
  float      stepFillL;                // litres the machine has taken in this fill step
  uint8_t    outputs;                  // real output states: bit0 tap, 1 drain, 2 fill, 3 pump, 4 tank wash
  bool       fillComplete;             // "machine is filled" guard set in this step
  float      tapLpm;           // litres per minute
  float      drainLpm;
  uint32_t   stepElapsedS;     // seconds in the current step (0 in Idle)
  uint32_t   drain2LeftS;      // seconds left in step 4 (0 otherwise)
  bool       stuck;            // step 1-3 ran past the alert time
  float      tankAgeH;         // hours since the tank started holding water, -1 unknown/empty
  int32_t    lastAgoS;         // seconds since the last wash ended, -1 unknown or none
  float      cycleTapL, cycleReusedL, cycleSavedL;
  bool       tankFault;        // float switches disagree (high wet, low dry)
  bool       manualWash;       // manual tank wash running
  uint32_t   manualWashLeftS;  // seconds left before the manual wash time limit
  const char* manualWashResult; // how the last manual wash ended: "", "full", "timeout", "stopped", "cycle"
};

extern int  activeState;       // current StateID, set in onEnter()
extern bool drain2Done;        // step 4 finished (replaces the fixed-time transition)

void getSystemStatus(SystemStatus& s);
void requestRemoteStop();      // dashboard Stop
bool requestRemoteStart();     // dashboard Start; false if not allowed right now
void applyRemoteRequests();    // call right after updateInputs() in loop()
void sampleFlowSensors();      // call every loop(): keeps flow rate and volumes current
void systemPeriodic();         // call every loop(): tank level, water age, stats saving
void outputsAllOff();          // all SSRs off (boot, firmware update)

// ── 3.0 additions ──
void     loopTimingMark();           // call first thing in loop(): measures the time between passes
uint32_t loopMaxMs();                // longest pass since start-up
uint32_t loopMaxLastMinuteMs();      // longest pass in the last full minute

// ── 2.9 additions ──
void logEvent(const char* fmt, ...);          // add a line to the Service tab's event log
int  eventCount();
bool eventAt(int i, uint32_t& epoch, uint32_t& uptimeS, const char*& text);   // i = 0 is the newest
bool systemBusy();                            // a cycle or a manual tank wash is running
void bootChecks();                            // call once in setup(), after netBegin()
const char* resetReasonText();
const char* resetReasonTextLocal();   // same, in the language chosen in Settings
bool interruptedInfo(int& state, uint32_t& epoch);   // last cycle was cut short (until dismissed)
void dismissInterrupted();
void buzzerBegin();
// Result of a request to start the manual tank wash.
enum ManualWashStart { washStarted, washBusy, washTankFull };
ManualWashStart startManualWash(); // dashboard "Wash tank": only while Idle and the tank is not full
void stopManualWash();         // dashboard "Stop washing"

#endif