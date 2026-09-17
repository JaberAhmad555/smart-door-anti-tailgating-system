#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


/* ============================ PIN NUMBERS ============================== */
#define TRACKER1_PIN    2
#define TRACKER2_PIN    3
#define REED_PIN        4
#define RELAY_PIN       5
#define BUZZER_PIN      6
#define ESP32_PIN       8
#define BUTTON_PIN      9
#define SS_PIN         10


/* ======================= SETTINGS YOU CAN CHANGE =======================
   If something behaves backwards, flip the number. 1 = yes, 0 = no.
   ======================================================================= */

#define RELAY_ACTIVE_LOW      1   // most blue relay modules turn on with LOW
#define TRACKER_ACTIVE_LOW    1   // most IR modules go LOW when blocked

// Set to 0 if the reed switch is not wired, otherwise the alarm never stops
#define REED_INSTALLED        1

#define UNLOCK_TIME_MS    10000UL  // how long the door stays unlocked
#define ALARM_TIME_MS     12000UL  // alarm holds this long - time to demo
#define CAMERA_MEMORY_MS  15000UL  // how long a camera alert is remembered

/* CROSSING TUNING
   CROSSING_WINDOW_MS : max gap allowed between beam 1 and beam 2
   CLEAR_TIME_MS      : both beams must be clear this long before the next
                        person can be counted. This is what stops one hand
                        wave counting as many people.
   BEAM_SETTLE_MS     : a reading must hold this long before we believe it  */
#define CROSSING_WINDOW_MS 3000UL
#define CLEAR_TIME_MS       600UL
#define BEAM_SETTLE_MS       40UL

#define BUTTON_LOCKOUT_MS  1500UL


/* ========================= AUTHORISED CARDS ============================
   Tap an unknown card and Serial Monitor prints its number. Paste it here.
   ======================================================================= */
byte authorisedCards[][4] = {
  { 0xDE, 0xAD, 0xBE, 0xEF },   // <-- replace with your card
  { 0x12, 0x34, 0x56, 0x78 }
};
const byte NUM_CARDS = sizeof(authorisedCards) / 4;


/* ========================== OBJECT SETUP =============================== */
#ifndef UNUSED_PIN
  #define UNUSED_PIN 255
#endif

MFRC522 mfrc522(SS_PIN, UNUSED_PIN);   // RST is tied to 3.3V

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);


/* ============================== STATES ================================= */
enum SystemState {
  STATE_READY,
  STATE_UNLOCKED,
  STATE_TAILGATE,
  STATE_FORCED,
  STATE_INTRUDER,     // beams only
  STATE_CAMERA,       // camera only
  STATE_CONFIRMED     // BOTH agree - highest level
};

SystemState state = STATE_READY;
unsigned long stateStartTime = 0;

byte peopleCounted = 0;
bool rfidWorking   = false;

// screen refresh memory
SystemState lastDrawnState = STATE_CONFIRMED;
byte lastDrawnCount = 255;
byte lastDrawnSecs  = 255;


/* ==================== CROSSING DETECTION STATE ==========================
   A crossing goes through clear stages. This is far more reliable than
   just looking at the beams every loop.
   ======================================================================= */
enum CrossStage {
  CROSS_IDLE,      // doorway empty, ready for somebody
  CROSS_SAW_1,     // beam 1 broke first, waiting for beam 2
  CROSS_SAW_2,     // beam 2 broke first, waiting for beam 1
  CROSS_DONE       // counted; waiting for the doorway to clear again
};

CrossStage crossStage = CROSS_IDLE;
unsigned long crossStartTime = 0;
unsigned long bothClearSince = 0;

// debounced beam readings
bool beam1Raw = false, beam2Raw = false;
bool beam1 = false, beam2 = false;
unsigned long beam1Changed = 0, beam2Changed = 0;
bool beam1Prev = false, beam2Prev = false;


/* ========================= CAMERA MEMORY =============================== */
unsigned long lastCameraTime = 0;
bool cameraWasLow = false;
bool lastEventConfirmed = false;


/* =========================== BUTTON MEMORY ============================= */
bool buttonWasDown = false;
unsigned long buttonLockUntil = 0;


/* =========================== BUZZER MEMORY ============================= */
unsigned long buzzerNextChange = 0;
byte buzzerPulsesLeft = 0;
bool buzzerIsOn = false;
#define BEEP_ON_MS   120
#define BEEP_OFF_MS  120


/* ======================= SMALL HELPER FUNCTIONS ======================== */

void lockDoor() {
#if RELAY_ACTIVE_LOW
  digitalWrite(RELAY_PIN, HIGH);
#else
  digitalWrite(RELAY_PIN, LOW);
#endif
}

void unlockDoor() {
#if RELAY_ACTIVE_LOW
  digitalWrite(RELAY_PIN, LOW);
#else
  digitalWrite(RELAY_PIN, HIGH);
#endif
}

bool rawBeam(byte pin) {
#if TRACKER_ACTIVE_LOW
  return (digitalRead(pin) == LOW);
#else
  return (digitalRead(pin) == HIGH);
#endif
}

bool doorIsOpen() {
#if REED_INSTALLED
  return (digitalRead(REED_PIN) == HIGH);
#else
  return false;
#endif
}

bool cameraAlert() { return (digitalRead(ESP32_PIN) == LOW); }

void beep(byte times) { buzzerPulsesLeft = times; }

void updateBuzzer() {
  unsigned long now = millis();
  if (buzzerIsOn) {
    if (now >= buzzerNextChange) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerIsOn = false;
      buzzerNextChange = now + BEEP_OFF_MS;
    }
  } else if (buzzerPulsesLeft > 0 && now >= buzzerNextChange) {
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerIsOn = true;
    buzzerPulsesLeft--;
    buzzerNextChange = now + BEEP_ON_MS;
  }
}

void changeState(SystemState s) {
  state = s;
  stateStartTime = millis();
}

byte secondsLeft() {
  unsigned long gone = millis() - stateStartTime;
  if (gone >= UNLOCK_TIME_MS) return 0;
  return (byte)((UNLOCK_TIME_MS - gone) / 1000UL) + 1;
}

void resetCrossing() {
  crossStage = CROSS_IDLE;
  crossStartTime = 0;
  bothClearSince = 0;
}

void grantAccess(const __FlashStringHelper *how) {
  Serial.print(F("ACCESS GRANTED by "));
  Serial.println(how);
  peopleCounted = 0;
  resetCrossing();
  unlockDoor();
  changeState(STATE_UNLOCKED);
  beep(1);
}


/* ========================= CAMERA MEMORY =============================== */

void updateCameraMemory() {
  bool nowLow = cameraAlert();
  if (nowLow && !cameraWasLow) {
    lastCameraTime = millis();
    Serial.println(F(">> Camera raised an alert"));
  }
  cameraWasLow = nowLow;
}

// True if the camera fired recently enough to count as agreeing
bool cameraAgrees() {
  if (cameraAlert()) return true;
  if (lastCameraTime == 0) return false;
  return (millis() - lastCameraTime < CAMERA_MEMORY_MS);
}


/* ======================= CROSSING DETECTION ============================
   Stage by stage. A person must complete a full crossing AND the doorway
   must go clear again before anybody else can be counted.
   ======================================================================= */

void updateBeams() {
  unsigned long now = millis();

  bool r1 = rawBeam(TRACKER1_PIN);
  bool r2 = rawBeam(TRACKER2_PIN);

  if (r1 != beam1Raw) { beam1Raw = r1; beam1Changed = now; }
  if (r2 != beam2Raw) { beam2Raw = r2; beam2Changed = now; }

  beam1Prev = beam1;
  beam2Prev = beam2;

  if (now - beam1Changed >= BEAM_SETTLE_MS) beam1 = beam1Raw;
  if (now - beam2Changed >= BEAM_SETTLE_MS) beam2 = beam2Raw;
}

void checkCrossing() {
  unsigned long now = millis();

  bool blocked1 = beam1;
  bool blocked2 = beam2;
  bool rising1 = beam1 && !beam1Prev;
  bool rising2 = beam2 && !beam2Prev;
  bool allClear = !blocked1 && !blocked2;

  // track how long the doorway has been completely empty
  if (allClear) {
    if (bothClearSince == 0) bothClearSince = now;
  } else {
    bothClearSince = 0;
  }
  bool doorwayEmpty = (bothClearSince != 0 &&
                       (now - bothClearSince) >= CLEAR_TIME_MS);

  switch (crossStage) {

    case CROSS_IDLE:
      if (rising1) { crossStage = CROSS_SAW_1; crossStartTime = now; }
      else if (rising2) { crossStage = CROSS_SAW_2; crossStartTime = now; }
      break;

    case CROSS_SAW_1:
      if (rising2) {
        peopleCounted++;
        Serial.print(F(">> PERSON ENTERED. Count = "));
        Serial.println(peopleCounted);
        crossStage = CROSS_DONE;
      } else if (now - crossStartTime > CROSSING_WINDOW_MS) {
        crossStage = CROSS_DONE;      // never completed; discard
      }
      break;

    case CROSS_SAW_2:
      if (rising1) {
        Serial.println(F(">> Person EXITED"));
        crossStage = CROSS_DONE;
      } else if (now - crossStartTime > CROSSING_WINDOW_MS) {
        crossStage = CROSS_DONE;
      }
      break;

    case CROSS_DONE:
      // Nothing new can be counted until the doorway is genuinely empty.
      // This is what stops one hand wave becoming eight people.
      if (doorwayEmpty) crossStage = CROSS_IDLE;
      break;
  }
}


/* ============================ THE SCREEN =============================== */

void drawScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("SMART DOOR"));
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 16);
  switch (state) {
    case STATE_READY:     display.println(F("READY"));    break;
    case STATE_UNLOCKED:  display.println(F("UNLOCKED")); break;
    case STATE_TAILGATE:  display.println(F("TAILGATE")); break;
    case STATE_FORCED:    display.println(F("FORCED!"));  break;
    case STATE_INTRUDER:  display.println(F("INTRUDER")); break;
    case STATE_CAMERA:    display.println(F("CAMERA"));   break;
    case STATE_CONFIRMED: display.println(F("CONFIRM!")); break;
  }

  display.setTextSize(1);
  display.setCursor(0, 38);
  switch (state) {
    case STATE_READY:
      if (rfidWorking) display.println(F("Tap card to enter"));
      else             display.println(F("Press button"));
      break;
    case STATE_UNLOCKED:  display.println(F("Please enter"));       break;
    case STATE_TAILGATE:
      if (lastEventConfirmed) display.println(F("2 people + camera"));
      else                    display.println(F("2 people, 1 entry"));
      break;
    case STATE_FORCED:    display.println(F("Door forced open"));   break;
    case STATE_INTRUDER:  display.println(F("IR beams only"));      break;
    case STATE_CAMERA:    display.println(F("Camera only"));        break;
    case STATE_CONFIRMED: display.println(F("BEAMS + CAMERA"));     break;
  }

  display.setCursor(0, 50);
  if (state == STATE_UNLOCKED) {
    display.print(F("People:"));
    display.print(peopleCounted);
    display.print(F("  Lock in "));
    display.print(secondsLeft());
    display.println(F("s"));
  } else if (state == STATE_READY) {
    display.println(F("System armed"));
  } else if (state == STATE_CONFIRMED) {
    display.println(F("*** BOTH AGREE ***"));
  } else {
    display.println(F("*** ALARM ***"));
  }

  display.display();
}

void refreshScreenIfNeeded() {
  byte secs = (state == STATE_UNLOCKED) ? secondsLeft() : 0;
  if (state != lastDrawnState || peopleCounted != lastDrawnCount ||
      secs != lastDrawnSecs) {
    drawScreen();
    lastDrawnState = state;
    lastDrawnCount = peopleCounted;
    lastDrawnSecs  = secs;
  }
}


/* ============================== SETUP ================================== */

void setup() {

  Serial.begin(9600);
  Serial.println(F("\n=== SMART DOOR STARTING ==="));

  /* RELAY SAFETY: digitalWrite BEFORE pinMode, so the lock cannot release
     while the microcontroller is resetting.                              */
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(RELAY_PIN, OUTPUT);
  lockDoor();
  Serial.println(F("Relay forced to LOCKED"));

  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(TRACKER1_PIN, INPUT);
  pinMode(TRACKER2_PIN, INPUT);
  pinMode(REED_PIN,     INPUT_PULLUP);
  pinMode(BUTTON_PIN,   INPUT_PULLUP);
  pinMode(ESP32_PIN,    INPUT_PULLUP);

  /* OLED: Wire.begin() switches on 5V internal pull-ups on A4/A5. The
     SSD1306 is a 3.3V part, so we switch them back off.                  */
  Wire.begin();
  digitalWrite(SDA, LOW);
  digitalWrite(SCL, LOW);
  Wire.setClock(400000L);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("OLED NOT FOUND at 0x3C"));
  } else {
    Serial.println(F("OLED OK"));
  }

  SPI.begin();
  mfrc522.PCD_Init();
  mfrc522.PCD_AntennaOn();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);
  delay(80);

  byte version = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.println();
  Serial.print(F(">>> RFID Firmware: 0x"));
  Serial.println(version, HEX);

  if (version == 0x90 || version == 0x91 || version == 0x92 ||
      version == 0x88 || version == 0xB2) {
    rfidWorking = true;
    Serial.println(F(">>> RC522 IS WORKING - tap a card to unlock"));
  } else {
    rfidWorking = false;
    Serial.println(F(">>> RC522 not responding - BUTTON MODE"));
    Serial.println(F(">>> Touch a wire from D9 to GND to unlock"));
  }
  Serial.println();

  // prime the beam readings so we do not fire on startup
  beam1Raw = beam1 = rawBeam(TRACKER1_PIN);
  beam2Raw = beam2 = rawBeam(TRACKER2_PIN);
  beam1Prev = beam1;
  beam2Prev = beam2;
  resetCrossing();

  changeState(STATE_READY);
  refreshScreenIfNeeded();

  beep(1);
  Serial.println(F("=== READY ===\n"));
}


/* ========================= CARD CHECKING =============================== */

bool cardIsAuthorised() {
  if (mfrc522.uid.size != 4) return false;
  for (byte c = 0; c < NUM_CARDS; c++) {
    bool match = true;
    for (byte i = 0; i < 4; i++)
      if (mfrc522.uid.uidByte[i] != authorisedCards[c][i]) { match = false; break; }
    if (match) return true;
  }
  return false;
}

void printUID() {
  Serial.print(F("Card UID -> copy this: { "));
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(F("0x"));
    if (mfrc522.uid.uidByte[i] < 0x10) Serial.print(F("0"));
    Serial.print(mfrc522.uid.uidByte[i], HEX);
    if (i < mfrc522.uid.size - 1) Serial.print(F(", "));
  }
  Serial.println(F(" }"));
}

void checkForCard() {
  if (!rfidWorking) return;
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial())   return;

  printUID();
  if (cardIsAuthorised()) grantAccess(F("card"));
  else { Serial.println(F("ACCESS DENIED - unknown card")); beep(3); }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}


/* ========================= BUTTON CHECKING ============================= */

void checkButton() {
  unsigned long now = millis();
  bool isDown = (digitalRead(BUTTON_PIN) == LOW);

  if (now < buttonLockUntil) { buttonWasDown = isDown; return; }

  if (isDown && !buttonWasDown) {
    buttonLockUntil = now + BUTTON_LOCKOUT_MS;
    if (state == STATE_READY) grantAccess(F("button"));
    else Serial.println(F("button ignored - not in READY state"));
  }
  buttonWasDown = isDown;
}


/* ==================== ALARM UPGRADE TO CONFIRMED =======================
   THIS IS THE KEY NEW PIECE.
   While a single-sensor alarm is running, we keep watching the OTHER
   sensor. If it agrees, we escalate to CONFIRMED. Order does not matter.
   ======================================================================= */

void tryUpgradeToConfirmed() {

  // Already confirmed, nothing to do
  if (state == STATE_CONFIRMED) return;

  bool beamsSaySomething = (peopleCounted >= 1);
  bool cameraSaysSomething = cameraAgrees();

  if (beamsSaySomething && cameraSaysSomething) {
    Serial.println();
    Serial.println(F("*********************************"));
    Serial.println(F("***   CONFIRMED INTRUDER      ***"));
    Serial.println(F("***   IR beams: person passed ***"));
    Serial.println(F("***   Camera:   motion seen   ***"));
    Serial.println(F("*********************************"));
    Serial.println();
    lastEventConfirmed = true;
    lockDoor();
    changeState(STATE_CONFIRMED);
    beep(10);
  }
}


/* =============================== LOOP ================================== */

void loop() {

  updateBuzzer();
  updateCameraMemory();
  updateBeams();
  checkCrossing();
  checkButton();

  unsigned long now = millis();

  switch (state) {

    /* ---------------------------------------------------------------- */
    case STATE_READY:

      checkForCard();

      if (peopleCounted >= 1) {
        // The beams saw somebody with no card and no button.
        if (cameraAgrees()) {
          Serial.println(F("*** CONFIRMED INTRUDER (beams + camera) ***"));
          lastEventConfirmed = true;
          lockDoor();
          changeState(STATE_CONFIRMED);
          beep(10);
        } else {
          Serial.println(F("ALARM: INTRUDER  (IR beams only)"));
          Serial.println(F("       waiting to see if the camera agrees..."));
          lastEventConfirmed = false;
          lockDoor();
          changeState(STATE_INTRUDER);
          beep(6);
        }
      }
      else if (doorIsOpen()) {
        Serial.println(F("ALARM: door forced open"));
        changeState(STATE_FORCED);
        beep(5);
      }
      else if (cameraAlert()) {
        Serial.println(F("ALARM: CAMERA  (camera only, no crossing yet)"));
        Serial.println(F("       waiting to see if the beams agree..."));
        lastEventConfirmed = false;
        peopleCounted = 0;
        resetCrossing();
        changeState(STATE_CAMERA);
        beep(4);
      }
      break;

    /* ---------------------------------------------------------------- */
    case STATE_UNLOCKED:

      if (peopleCounted >= 2) {
        Serial.println();
        Serial.println(F("*** TAILGATING DETECTED ***"));
        Serial.print(F("    IR beams: "));
        Serial.print(peopleCounted);
        Serial.println(F(" people on ONE authorised entry"));
        if (cameraAgrees()) {
          Serial.println(F("    Camera:   movement confirmed"));
          lastEventConfirmed = true;
        } else {
          Serial.println(F("    Camera:   no confirmation"));
          lastEventConfirmed = false;
        }
        Serial.println();
        lockDoor();
        changeState(STATE_TAILGATE);
        beep(8);
      }
      else if (now - stateStartTime > UNLOCK_TIME_MS) {
        lockDoor();
        peopleCounted = 0;
        resetCrossing();
        Serial.println(F("Relocked (time up)"));
        changeState(STATE_READY);
      }
      break;

    /* ---------------------------------------------------------------- */
    /* Single-sensor alarms keep listening for the other sensor.         */

    case STATE_INTRUDER:
    case STATE_CAMERA:
      lockDoor();
      tryUpgradeToConfirmed();       // <<< the important line
      if (now - stateStartTime > ALARM_TIME_MS) {
        if (!doorIsOpen() && !cameraAlert()) {
          peopleCounted = 0;
          resetCrossing();
          Serial.println(F("Alarm cleared - system armed\n"));
          changeState(STATE_READY);
        }
      }
      break;

    /* ---------------------------------------------------------------- */
    case STATE_TAILGATE:
    case STATE_FORCED:
    case STATE_CONFIRMED:
      lockDoor();
      if (now - stateStartTime > ALARM_TIME_MS) {
        if (!doorIsOpen() && !cameraAlert()) {
          peopleCounted = 0;
          resetCrossing();
          Serial.println(F("Alarm cleared - system armed\n"));
          changeState(STATE_READY);
        }
      }
      break;
  }

  refreshScreenIfNeeded();
}
