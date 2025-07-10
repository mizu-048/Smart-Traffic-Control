#include <AccelStepper.h>

// Pin connections for stepper motor driver
const int dirPin = 2;
const int stepPin = 3;

// Traffic Lights Pin
const int colPins[3] = { 12, 11, 10 };  //Red, Yellow, Green
const int rowPins[4] = { 8, 7, 6, 5 };  // Traffic Signal 1-4 (indexed 0-3)

// Define motor interface type (1 for A4988/DRV8825 style drivers)
#define motorInterfaceType 1

// Create an AccelStepper instance
AccelStepper myStepper(motorInterfaceType, stepPin, dirPin);

// Stepper motor parameters
// High speeds might lead to skipped steps and less rotation than was intended, either reduce speed or change current limiting resistor accordingly
const long stepsPer90Degrees = 512;       // 2048 steps for 360 degrees, 512 for 90
const float MAX_MOTOR_SPEED = 1000.0;     // Max speed in steps per second
const float MOTOR_ACCELERATION = 1000.0;  // Max acceleration ( only noticed when starting or stopping rotation )

// Stepper motor operational modes controlled by Python
enum StepperMode {
  IDLE,              // Motor is stopped, waiting for explicit commands (P, T, M)
  TURNING,           // Motor is currently performing a 90-degree turn commanded by 'T'
  MANUAL_ADJUSTMENT  // Motor is performing a user-commanded manual move via 'M'
};
StepperMode currentMode = IDLE;  // Start in IDLE mode

// Variable to store the target position when a 'T' command is received
long turnTargetPosition = 0;


// ==== Traffic Light Configuration Timing (in milliseconds) ====
const unsigned long signalCycleTime = 10000;   // Total time per signal (10 seconds)
const unsigned long greenBlinkTime = 3000;     // Green blinking duration (e.g., 3 seconds)
const unsigned long yellowCurrentTime = 2000;  // Yellow ON duration for the CURRENT signal (2 seconds)
const unsigned long greenSteadyTime = 5000;    // Green steady ON duration (first 5 seconds)
const unsigned long blinkInterval = 700;       // Blink every 0.7 seconds

// IMPORTANT: signalCycleTime MUST equal sum of phases for current signal:
// greenSteadyTime + greenBlinkTime + yellowCurrentTime = 5000 + 3000 + 2000 = 10000 (matches signalCycleTime)

const unsigned long yellowLeadTime = 3000;  // Time before switch when yellow turns ON for the *NEXT* signal.
                                            // This will overlap with the current signal's green/yellow phases.

// ==== LED state array (matching colPins mapping: [row][RED, YELLOW, GREEN]) ====
int ledState[4][3];  // Will be initialized in setup

// ==== Traffic Light Timing Control ====
unsigned long previousMillisTraffic = 0;  // Separate millis for traffic light timing
unsigned long previousBlinkMillis = 0;
unsigned long elapsedMillisTraffic = 0;

int currentSignal = 0;          // Current active traffic signal (0 to 3)
int nextSignalToActivate = -1;  // -1 indicates default (currentSignal + 1)

// --- Variables for priority order and graceful transition ---
const int NUM_SIDES = 4;  // Define this globally for clarity

int lastAppliedPriorityOrder[NUM_SIDES];  // Stores the last 'O' command that was applied as active
int newPriorityOrder[NUM_SIDES];          // Stores the *new* priority received from Python, awaiting application
bool newPriorityReceived = false;         // Flag to indicate if a new priority order is pending
int priorityOrderIndex = 0;               // Current position/index being served in lastAppliedPriorityOrder
bool customPriorityActive = false;        // Flag to indicate if we are following a custom order (lastAppliedPriorityOrder)

bool greenBlinkState = false;  // True for green ON, false for green OFF during blinking


void setup() {
  Serial.begin(9999);  // Initialize serial communication for both systems. Using 9999 for higher speed.

  // --- Stepper Motor Setup ---
  myStepper.setMaxSpeed(MAX_MOTOR_SPEED);
  myStepper.setAcceleration(MOTOR_ACCELERATION);
  myStepper.setSpeed(MAX_MOTOR_SPEED);  // Set speed for moves
  myStepper.setCurrentPosition(0);      // Initialize current position to 0
  myStepper.stop();                     // Stop any ongoing motor movement immediately.
  currentMode = IDLE;                   // Switch to IDLE mode.

  // --- Traffic Light Setup ---
  // Initialize column pins
  for (int i = 0; i < 3; i++) {
    pinMode(colPins[i], OUTPUT);
    digitalWrite(colPins[i], LOW);  // Ensure all column pins are low initially
  }
  // Initialize row pins
  for (int i = 0; i < NUM_SIDES; i++) {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], HIGH);  // Deactivate all rows initially (assuming common anode driver)
  }

  // Initialize ledState array to all OFF, then set default to all RED
  for (int row = 0; row < NUM_SIDES; row++) {
    for (int col = 0; col < 3; col++) {
      ledState[row][col] = 0;  // All LEDs off
    }
    ledState[row][0] = 1;  // Set all signals to RED by default initially
  }

  // Initialize lastAppliedPriorityOrder with default sequential order
  for (int i = 0; i < NUM_SIDES; i++) {
    lastAppliedPriorityOrder[i] = i;  // 0, 1, 2, 3
  }

  // Set initial state for current signal (e.g., signal 0 starts green)
  currentSignal = lastAppliedPriorityOrder[0];  // Set to the first in the initial default list (Side 1)
  ledState[currentSignal][0] = 0;               // Red OFF
  ledState[currentSignal][1] = 0;               // Yellow OFF
  ledState[currentSignal][2] = 1;               // Green ON

  Serial.println("Arduino Traffic Light System Ready.");
  Serial.println("Traffic Light Commands: C<signal_num> (Set Current), N<signal_num> (Set Next), O<s1s2s3s4> (Set Order)");
  Serial.print("Initial Current Signal: ");
  Serial.println(currentSignal + 1);  // Display 1-based for user
}

// --- Traffic Light Functions ---
void setLEDStates() {
  // This function rapidly cycles through the rows to give the illusion of all LEDs being on simultaneously.
  for (int row = 0; row < NUM_SIDES; row++) {
    digitalWrite(rowPins[row], LOW);  // Activate row (LOW for common anode)
    for (int col = 0; col < 3; col++) {
      // Set column state based on ledState array
      digitalWrite(colPins[col], ledState[row][col] ? HIGH : LOW);
    }
    delayMicroseconds(500);            // Small delay for persistence (adjust if flickering)
    digitalWrite(rowPins[row], HIGH);  // Deactivate row
    for (int col = 0; col < 3; col++) {
      digitalWrite(colPins[col], LOW);  // Clear columns to prevent ghosting
    }
  }
}

// Helper function to find the next signal to go green based on a given priority list
// It searches for the first signal in the provided list that is currently RED.
// Returns the 0-indexed signal number, or -1 if no suitable RED signal is found.
int findNextAvailableSignalInPriority(int (&priorityList)[NUM_SIDES]) {
  for (int i = 0; i < NUM_SIDES; i++) {
    int requestedSide = priorityList[i];  // This is 0-indexed signal number

    // A side is "available" if its red light OR its yellow light is currently on.
    // This correctly finds the signal that updateTrafficSignals() has already turned yellow.
    if (ledState[requestedSide][0] == 1 || ledState[requestedSide][1] == 1) {  // Check if RED or YELLOW is ON
      return requestedSide;                                                    // Found the highest priority side that is ready to go green
    }
  }
  return -1;  // No suitable red or yellow light found in the priority list
}


void updateTrafficSignals() {
  // Determine the actual next signal for the yellow lead time.
  // This logic determines which signal's yellow light should turn on PREEMPTIVELY.
  // It should look at the *next* signal in the *effective* active sequence.

  int effectiveNextSignal;

  if (nextSignalToActivate != -1) {  // Priority 1: Manual 'N' override
    effectiveNextSignal = nextSignalToActivate;
  } else if (newPriorityReceived) {  // Priority 2: A new 'O' command is pending. Use its first element for yellow lead.
    effectiveNextSignal = newPriorityOrder[0];
  } else if (customPriorityActive) {  // Priority 3: Following a previously applied 'O' command
    // Determine the next in the lastAppliedPriorityOrder
    int nextIndexInApplied = (priorityOrderIndex + 1) % NUM_SIDES;
    effectiveNextSignal = lastAppliedPriorityOrder[nextIndexInApplied];
  } else {  // Priority 4: Default sequential
    effectiveNextSignal = (currentSignal + 1) % NUM_SIDES;
  }

  // Default: all signals red (start clean for each update cycle)
  for (int i = 0; i < NUM_SIDES; i++) {
    ledState[i][0] = 1;  // Red ON
    ledState[i][1] = 0;  // Yellow OFF
    ledState[i][2] = 0;  // Green OFF
  }

  // --- Logic for Current Signal ---
  // Green Steady Phase
  if (elapsedMillisTraffic < greenSteadyTime) {
    ledState[currentSignal][0] = 0;  // Red OFF
    ledState[currentSignal][1] = 0;  // Yellow OFF
    ledState[currentSignal][2] = 1;  // Green ON
  }
  // Green Blinking Phase
  else if (elapsedMillisTraffic < greenSteadyTime + greenBlinkTime) {
    ledState[currentSignal][0] = 0;                        // Red OFF
    ledState[currentSignal][1] = 0;                        // Yellow OFF
    ledState[currentSignal][2] = greenBlinkState ? 1 : 0;  // Blinking Green
  }
  // Yellow Phase for Current Signal
  else if (elapsedMillisTraffic < greenSteadyTime + greenBlinkTime + yellowCurrentTime) {
    ledState[currentSignal][0] = 0;  // Red OFF
    ledState[currentSignal][1] = 1;  // Yellow ON
    ledState[currentSignal][2] = 0;  // Green OFF
  }
  // After yellowCurrentTime, currentSignal effectively becomes Red again (due to default setting at start of function)

  // --- Yellow ON for the NEXT signal during yellowLeadTime ---
  // This phase overlaps with the current signal's green blink and yellow phases.
  // It indicates the cross-traffic is preparing to go.
  // The 'effectiveNextSignal' here is based on what *would* be next if no new priority was pending.
  if (elapsedMillisTraffic >= (signalCycleTime - yellowLeadTime)) {
    // Ensure the effectiveNextSignal is not the currentSignal, to prevent a signal from yellowing itself
    // And ensure it's not already the current active signal
    if (effectiveNextSignal != currentSignal) {
      ledState[effectiveNextSignal][0] = 0;  // Red OFF for the next signal
      ledState[effectiveNextSignal][1] = 1;  // Yellow ON for the next signal
      ledState[effectiveNextSignal][2] = 0;  // Green OFF for the next signal
    }
  }
}

// --- Combined Serial Input Handler ---
void handleSerialInput() {
  if (Serial.available()) {
    String commandString = Serial.readStringUntil('\n');
    commandString.trim();
    char commandChar = commandString.charAt(0);
    // Value for M, C, N commands. Note: toInt() will return 0 if the string is empty after the command char.
    int value = commandString.substring(1).toInt();

    switch (commandChar) {
      // --- Stepper Motor Commands ---
      case 'P':  // 'P' for Pause/Stop
        myStepper.stop();
        currentMode = IDLE;
        Serial.println("Received: Pause command. Stepper IDLE.");
        break;

      case 'T':  // 'T' for Turn 90 Degrees
        if (currentMode == IDLE) {
          turnTargetPosition = myStepper.currentPosition() + stepsPer90Degrees;
          myStepper.moveTo(turnTargetPosition);
          currentMode = TURNING;
          // Serial.println("Received: Turn command. Initiating 90-degree turn.");
        } else {
          Serial.print("Warning: Motor busy, cannot initiate turn. Current mode: ");
          Serial.println(currentMode == MANUAL_ADJUSTMENT ? "MANUAL_ADJUSTMENT" : "TURNING");
        }
        break;

      case 'R':  // 'R' for Turn -90 Degrees
        if (currentMode == IDLE) {
          turnTargetPosition = myStepper.currentPosition() - stepsPer90Degrees;
          myStepper.moveTo(turnTargetPosition);
          currentMode = TURNING;
          // Serial.println("Received: Turn command. Initiating 90-degree turn.");
        } else {
          Serial.print("Warning: Motor busy, cannot initiate turn. Current mode: ");
          Serial.println(currentMode == MANUAL_ADJUSTMENT ? "MANUAL_ADJUSTMENT" : "TURNING");
        }
        break;



      case 'M':            // 'M' for Manual Move
        if (value != 0) {  // Ensure value is not 0 for a meaningful move
          myStepper.moveTo(myStepper.currentPosition() + value);
          currentMode = MANUAL_ADJUSTMENT;
          Serial.print("Received: Manual move command. Moving ");
          Serial.print(value);
          Serial.println(" steps.");
        } else {
          Serial.println("Invalid 'M' command. Format: M<steps> (e.g., M50, M-20).");
        }
        break;

      // --- Traffic Light Commands ---
      case 'C':                       // 'C' for Set Current Signal (e.g., C1, C2, C3, C4)
        {                             // Use a block for local variable scope
          int newSignal = value - 1;  // Convert 1-based to 0-based
          if (newSignal >= 0 && newSignal < NUM_SIDES) {
            // Immediately switch to new signal for 'C' command (direct override)
            currentSignal = newSignal;
            elapsedMillisTraffic = 0;      // Reset timer for the new signal
            greenBlinkState = true;        // Start with steady green for the new signal
            nextSignalToActivate = -1;     // Reset any manually set next signal
            customPriorityActive = false;  // Disable custom priority if 'C' is used
            newPriorityReceived = false;   // Clear any pending new priority (as 'C' takes precedence)
            Serial.print("\nManually set Current Signal to: ");
            Serial.println(currentSignal + 1);
          } else {
            Serial.println("Invalid signal number. Please use C1-C4.");
          }
        }
        break;

      case 'N':                           // 'N' for Set Next Signal (e.g., N1, N2, N3, N4)
        {                                 // Use a block for local variable scope
          int newNextSignal = value - 1;  // Convert 1-based to 0-based
          if (newNextSignal >= 0 && newNextSignal < NUM_SIDES && newNextSignal != currentSignal) {
            nextSignalToActivate = newNextSignal;
            customPriorityActive = false;  // Disable custom priority if 'N' is used
            newPriorityReceived = false;   // Clear any pending new priority (as 'N' takes precedence)
            Serial.print("\nManually set Next Signal to: ");
            Serial.println(newNextSignal + 1);  // Print 1-based for readability
          } else {
            Serial.println("Invalid next signal. Use N1-N4, must not be current signal, and be a valid digit.");
          }
        }
        break;

      // --- 'O' (Order) Command - QUEUES the new priority for graceful transition ---
      case 'O':  // 'O' for Set Priority Order (e.g., O3142)
        {
          // Check for 'O' plus 4 digits (total length 5)
          if (commandString.length() == NUM_SIDES + 1) {
            bool validInput = true;
            for (int i = 0; i < NUM_SIDES; i++) {
              int signalNum = commandString.charAt(i + 1) - '0';  // Convert char to int ('1' becomes 1, '2' becomes 2, etc.)
              if (signalNum >= 1 && signalNum <= NUM_SIDES) {
                newPriorityOrder[i] = signalNum - 1;  // Store as 0-indexed (e.g., 1 becomes 0, 4 becomes 3)
              } else {
                validInput = false;
                break;  // Invalid character found
              }
            }

            if (validInput) {
              newPriorityReceived = true;  // Set flag to indicate new priority is pending
              nextSignalToActivate = -1;   // Clear any pending single next signal (N command)
              // customPriorityActive will be set to true ONLY when the new priority is APPLIED in loop()

              Serial.print("\nReceived new Priority Order: ");
              for (int i = 0; i < NUM_SIDES; i++) {
                Serial.print(newPriorityOrder[i] + 1);  // Print 1-based for readability
              }
              Serial.println(". It will be applied after the current signal finishes its cycle.");
            } else {
              Serial.println("Invalid 'O' command. Format: O<s1s2s3s4> where s is a digit 1-4.");
            }
          } else {
            Serial.println("Invalid 'O' command length. Format: O<s1s2s3s4> (e.g., O3142).");
          }
        }
        break;

      default:
        Serial.print("Unknown command received: ");
        Serial.println(commandString);
        break;
    }
  }
}


void loop() {
  unsigned long currentMillis = millis();  // Get current time once per loop

  // --- Stepper Motor Logic ---
  switch (currentMode) {
    case IDLE:
      // Motor is stopped, waiting for commands.
      break;

    case TURNING:
      myStepper.run();  // Keep moving the stepper towards its current target.
      if (myStepper.distanceToGo() == 0) {
        myStepper.stop();
        currentMode = IDLE;
        Serial.println("TURN_DONE");  // Signal Python that the 90-degree turn is complete.
      }
      break;

    case MANUAL_ADJUSTMENT:
      myStepper.run();  // Continue moving the stepper for the manual adjustment.
      if (myStepper.distanceToGo() == 0) {
        myStepper.stop();
        currentMode = IDLE;
        Serial.println("Manual move complete. Now in IDLE mode.");
      }
      break;
  }

  // --- Traffic Light Logic ---
  // Update elapsed time for the current signal's phase
  elapsedMillisTraffic += currentMillis - previousMillisTraffic;
  previousMillisTraffic = currentMillis;

  // Handle blinking timing for the current signal's green light
  if (currentMillis - previousBlinkMillis >= blinkInterval) {
    previousBlinkMillis = currentMillis;
    if (elapsedMillisTraffic >= greenSteadyTime && elapsedMillisTraffic < greenSteadyTime + greenBlinkTime) {
      greenBlinkState = !greenBlinkState;  // Toggle green blink state
    } else {
      greenBlinkState = true;  // Default to ON outside blinking phase (updateTrafficSignals will correct if red/yellow)
    }
  }

  updateTrafficSignals();  // Recalculate LED states based on elapsed time and current phase
  setLEDStates();          // Update physical LEDs on the matrix (multiplexing)

  if (elapsedMillisTraffic >= signalCycleTime) {
    elapsedMillisTraffic = 0;  // Reset timer for the new cycle
    greenBlinkState = true;    // Start the new signal with steady green

    previousMillisTraffic = currentMillis;  // Reset the reference point for the new cycle

    int chosenNextSignal = -1;  // Initialize to an invalid signal

    if (nextSignalToActivate != -1) {  // Priority 1: Manual 'N' override (one-time jump)
      chosenNextSignal = nextSignalToActivate;
      nextSignalToActivate = -1;     // Clear this override after one use
      customPriorityActive = false;  // If 'N' is used, stop following any custom order
      newPriorityReceived = false;   // Clear any pending new priority
      Serial.print("Signal changed to (N override): ");
    } else if (newPriorityReceived) {  // Priority 2: A new 'O' command was just received and is pending
      // COPY the new priority to lastAppliedPriorityOrder
      for (int i = 0; i < NUM_SIDES; i++) {
        lastAppliedPriorityOrder[i] = newPriorityOrder[i];
      }
      customPriorityActive = true;  // Now actively following a custom order
      newPriorityReceived = false;  // Reset the flag, new priority is now active
      priorityOrderIndex = 0;       // Always start from the beginning of the *newly applied* list

      // Find the first available (RED or YELLOW) signal in the *newly applied* list, starting from the beginning
      chosenNextSignal = findNextAvailableSignalInPriority(lastAppliedPriorityOrder);

      if (chosenNextSignal != -1) {  // Found a suitable signal in the new priority list
        // Update priorityOrderIndex to match the chosen signal's position in the new list
        for (int i = 0; i < NUM_SIDES; i++) {
          if (lastAppliedPriorityOrder[i] == chosenNextSignal) {
            priorityOrderIndex = i;  // This is the new starting point for the sequence
            break;
          }
        }
        Serial.print("Signal changed to (New 'O' priority applied): ");
      } else {
        // This means the new priority was applied, but its first (or any) signal wasn't available.
        // This is an extremely rare edge case if `yellowLeadTime` and red states are managed.
        // If this happens, we must fall back, but still remain in custom priority mode.

        Serial.println("New 'O' priority applied, but no suitable RED/YELLOW signal found in its order. Attempting to find next available.");

        // Try to find *any* available red/yellow signal from the *newly set* lastAppliedPriorityOrder
        // starting from the next position after where the *previous* currentSignal was, if it's in the new list.
        // This is a safety net; ideally, the above `findNextAvailableSignalInPriority` should find something.

        int tempStartIndex = 0;  // Default to start from beginning of new list
        bool foundCurrentInNewList = false;
        for (int i = 0; i < NUM_SIDES; i++) {
          if (lastAppliedPriorityOrder[i] == currentSignal) {  // Find where the just-finished signal is in the new list
            tempStartIndex = (i + 1) % NUM_SIDES;              // Start searching from the next one
            foundCurrentInNewList = true;
            break;
          }
        }

        // Search from tempStartIndex to find the first available (RED/YELLOW) signal in the new list.
        for (int k = 0; k < NUM_SIDES; k++) {
          int checkIndex = (tempStartIndex + k) % NUM_SIDES;
          int candidateSignal = lastAppliedPriorityOrder[checkIndex];
          if (ledState[candidateSignal][1] == 1 || ledState[candidateSignal][0] == 1) {  // Is this candidate signal YELLOW or RED?
            chosenNextSignal = candidateSignal;
            priorityOrderIndex = checkIndex;  // Update the new starting index for this order
            Serial.print("Signal changed to (New 'O' priority, next available after current, fallback): ");
            break;
          }
        }

        if (chosenNextSignal == -1) {  // Still no suitable signal, fallback to default sequential
          chosenNextSignal = (currentSignal + 1) % NUM_SIDES;
          customPriorityActive = false;  // Cannot follow custom priority if no path found
          Serial.print("Signal changed to (New 'O' priority, no red/yellow found, sequential fallback): ");
        }
      }
    } else if (customPriorityActive) {  // Priority 3: Following an existing custom priority order
      // This is the crucial change: Increment only if the *currentSignal* is the one pointed to by priorityOrderIndex.
      // This implicitly links advancing the index to the completion of that specific signal.

      // First, confirm the currentSignal is indeed the one we expected from the priority list
      if (currentSignal == lastAppliedPriorityOrder[priorityOrderIndex]) {
        priorityOrderIndex = (priorityOrderIndex + 1) % NUM_SIDES;  // Move to the next in the list, wrap around
      } else {
        // This should ideally not happen if logic is sound.
        // It implies currentSignal was changed by 'C' or 'N' without clearing customPriorityActive correctly,
        // or there's a serious sync issue. For robustness, find where currentSignal is now.
        Serial.print("Warning: currentSignal (");
        Serial.print(currentSignal + 1);
        Serial.print(") mismatch with priorityOrderIndex (");
        Serial.print(lastAppliedPriorityOrder[priorityOrderIndex] + 1);
        Serial.println("). Resyncing.");
        // Attempt to resync priorityOrderIndex to currentSignal's position in the list
        for (int i = 0; i < NUM_SIDES; i++) {
          if (lastAppliedPriorityOrder[i] == currentSignal) {
            priorityOrderIndex = i;  // Set it to the current position
            break;
          }
        }
        // After resync, advance to the next one if it was just completed
        priorityOrderIndex = (priorityOrderIndex + 1) % NUM_SIDES;
      }

      chosenNextSignal = lastAppliedPriorityOrder[priorityOrderIndex];
      Serial.print("Signal changed to (continuing last 'O' priority): ");
    } else {  // Priority 4: Default sequential
      chosenNextSignal = (currentSignal + 1) % NUM_SIDES;
      Serial.print("Signal changed to (sequential): ");
    }

    // Apply the chosen next signal
    if (chosenNextSignal != -1) {
      currentSignal = chosenNextSignal;
    } else {
      Serial.println("Error: No next signal could be determined. Defaulting to Side 1 (index 0).");
      currentSignal = 0;
    }
    Serial.println(currentSignal + 1);
  }

  // --- Handle Serial Input for both Stepper and Traffic Lights ---
  handleSerialInput();
}