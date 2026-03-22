#include <SPI.h>
#include <MFRC522.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);
#define SS_PIN 10
#define RST_PIN 9
#define BUZZER_PIN 4
#define NO_ID_BUTTON 5   
#define ENROLL_BUTTON 6  

MFRC522 mfrc522(SS_PIN, RST_PIN);
SoftwareSerial mySerial(2, 3); // Fingerprint (Yellow: 2, Green: 3)
Adafruit_Fingerprint finger(&mySerial);
SoftwareSerial BTSerial(7, 8); // Bluetooth (TX: 7, RX: 8)

const long RE_SCAN_DELAY_MS = 5000;
const unsigned long FINGER_TIMEOUT_MS = 10000;

String currentStudentName = "";
int currentFingerID = -1;
long lastScanTime = 0;
bool systemReady = false; 

void setup() {
  Serial.begin(9600);
  BTSerial.begin(9600); // 9600 is most stable for SoftwareSerial

  SPI.begin();
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(NO_ID_BUTTON, INPUT_PULLUP);
  pinMode(ENROLL_BUTTON, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  
  // Initialize RFID
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max); 

  // --- Fingerprint Hardware Handshake ---
  mySerial.begin(57600); 
  mySerial.listen(); 
  
  displayMessage(F("CHECKING"), F("FINGER SENSOR..."));
  delay(1000); 

  if (finger.verifyPassword()) {
    successBeep();
    displayMessage(F("FINGER SENSOR"), F("WORKING"));
  } else {
    errorBeep();
    displayMessage(F("FINGER ERROR"), F("CHECK WIRING"));
  }
  delay(2000);

  // --- Start Bridge Wait ---
  // Ensure we clear any garbage from the buffer before listening
  while(BTSerial.available() > 0) BTSerial.read(); 
  
  BTSerial.listen(); 
  displayMessage(F("  WAITING FOR  "), F("  BRIDGE...    "));
}

void showIdleMessage() {
  displayMessage(F("  SYSTEM READY  "), F("   SCAN CARD    "));
}

// Optimized to not "hog" the CPU, allowing RFID to still trigger
String readUnifiedSerial() {
  // Ensure BTSerial is the active listener whenever we expect a response
  if (!BTSerial.isListening()) BTSerial.listen();
  
  unsigned long start = millis();
  while (millis() - start < 2000) { 
    if (Serial.available() > 0) return Serial.readStringUntil('\n');
    if (BTSerial.available() > 0) {
      String response = BTSerial.readStringUntil('\n');
      response.trim();
      return response;
    }
    delay(10); 
  }
  return "";
}

bool preCheckTime() {
  displayMessage(F("Verifying"), F("Access Time..."));
  Serial.println(F("CHECK_TIME")); 
  BTSerial.println(F("CHECK_TIME"));
  
  String res = readUnifiedSerial(); 
  res.trim();
  
  if(res == F("TOO_LATE")) {
    errorBeep();
    displayMessage(F("  CLASS ENDED   "), F("     DENIED     "));
    delay(3000); 
    showIdleMessage(); 
    return false;
  }
  return true; 
}

void loop() {
  // 1. Check for Commands (Listen to Bluetooth/USB)
  if (Serial.available() > 0 || BTSerial.available() > 0) {
    String cmd = (Serial.available() > 0) ? Serial.readStringUntil('\n') : BTSerial.readStringUntil('\n');
    cmd.trim();

    // The Bridge sends INIT_SYSTEM to start the handshake
    if (cmd == F("INIT_SYSTEM")) {
      systemReady = false;
      displayMessage(F(" INITIALIZING  "), F("    SYSTEM...   "));
      
      // Clear buffer and send confirmation
      while(BTSerial.available() > 0) BTSerial.read();
      BTSerial.println(F("READY"));
      Serial.println(F("READY")); // Also send to USB for debugging
      return; 
    } 
    else if (cmd == F("SYSTEM_READY")) {
      systemReady = true;
      mfrc522.PCD_Init(); // Refresh RFID on start
      showIdleMessage();
      return;
    }
    else if (cmd == F("ALREADY_DONE")) {
      // 3 rapid beeps for attention
      tone(BUZZER_PIN, 500, 100); delay(150);
      tone(BUZZER_PIN, 500, 100); delay(150);
      tone(BUZZER_PIN, 500, 300); 
      
      lcd.clear(); // Ensure screen is clean
      lcd.setCursor(0, 0); lcd.print(F("    STUDENT     "));
      lcd.setCursor(0, 1); lcd.print(F("ALREADY SCANNED "));
      delay(3000); // Hold for 3 seconds
      showIdleMessage();
      return;
    }
    else if (cmd == F("SHOW_SWEEP")) {
      displayMessage(F("  ABSENT SWEEP  "), F(" IN PROGRESS... "));
      // No delay needed; Python sends SYSTEM_READY when finished
      return;
    }
    else if (cmd.startsWith(F("REMOTE_DELETE"))) {
      int commaIndex = cmd.indexOf(',');
      if (commaIndex > 0) {
        String idStr = cmd.substring(commaIndex + 1);
        int idToDelete = idStr.toInt();
        deleteFingerprint(idToDelete);
      }
      return;
    }
  }

  if (!systemReady) return;

  // 2. Check Buttons
  if (digitalRead(NO_ID_BUTTON) == LOW) {
    if (preCheckTime()) { successBeep(); handleNoIDEntry(); }
    return;
  }
  if (digitalRead(ENROLL_BUTTON) == LOW) {
    successBeep(); enrollMode(); showIdleMessage();
    return;
  }

  // 3. Check RFID (Crucial: SPI communication)
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    if ((millis() - lastScanTime) < RE_SCAN_DELAY_MS) {
      displayMessage(F("Wait"), F("Cooldown"));
      delay(1500);
      showIdleMessage();
      return;
    }
    lastScanTime = millis(); 

    if (!preCheckTime()) {
       mfrc522.PICC_HaltA();
       mfrc522.PCD_StopCrypto1();
       return;
    }

    successBeep();
    String uidStr = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if(mfrc522.uid.uidByte[i] < 0x10) uidStr += "0";
      uidStr += String(mfrc522.uid.uidByte[i], HEX);
    }
    uidStr.toUpperCase();
    
    Serial.print(F("CHECK_UID,")); Serial.println(uidStr);
    BTSerial.print(F("CHECK_UID,")); BTSerial.println(uidStr);

    unsigned long waitStart = millis();
    bool responseReceived = false;
    
    while(millis() - waitStart < 3000) {
        String response = readUnifiedSerial();
        if (response == "") continue;

        if(response.startsWith(F("FOUND"))) {
          int firstComma = response.indexOf(',');
          int secondComma = response.indexOf(',', firstComma + 1);
          int thirdComma = response.indexOf(',', secondComma + 1);
          
          currentStudentName = response.substring(firstComma + 1, secondComma);
          currentFingerID = response.substring(secondComma + 1, thirdComma).toInt();
          String studentNumber = response.substring(thirdComma + 1);
          
          successBeep();
          displayMessage(F("Scan Fingerprint:"), currentStudentName.c_str());
          delay(1000);
          
          if (verifyFingerWithTimer(currentFingerID)) {
            successBeep();
            displayMessage(F("Student:"), currentStudentName.c_str());
            delay(1500);
            doubleBeep();
            displayMessage(F("Recording"), F("Attendance..."));
            logToSerial(currentStudentName.c_str(), studentNumber, "PRESENT");
            
            // --- SMART WAIT for Cloud Response (Prevents "System Ready" flash) ---
            String cloudResp = readUnifiedSerial(); // Waits up to 2s
            if (cloudResp == F("ALREADY_DONE")) {
               // Immediate Duplicate Alert
               tone(BUZZER_PIN, 500, 100); delay(150);
               tone(BUZZER_PIN, 500, 100); delay(150);
               tone(BUZZER_PIN, 500, 300); 
               
               lcd.clear(); 
               lcd.setCursor(0, 0); lcd.print(F("    STUDENT     "));
               lcd.setCursor(0, 1); lcd.print(F("ALREADY SCANNED "));
               delay(3000); 
            }
          } else {
            errorBeep();
            displayMessage(F("Timeout"), F("Try Again"));
            delay(1500);
          }
          responseReceived = true;
          break;
        } else if(response.startsWith(F("NOT_FOUND"))) {
          errorBeep();
          displayMessage(F("Card Error"), F("Unregistered"));
          delay(1500);
          responseReceived = true;
          break;
        }
    }
    
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    showIdleMessage();
  }
}

void enrollMode() {
  if (!checkBridgeAlive()) return;
  displayMessage(F("  ENROLLMENT   "), F("      MODE      "));
  delay(2000);

  String scannedUID = "";
  bool cardAccepted = false;

  for (int secondsLeft = 15; secondsLeft > 0; secondsLeft--) {
    displayMessage(F("  Place Card   "), (String(F("Time: ")) + String(secondsLeft) + "s").c_str());
    for (int check = 0; check < 10; check++) {
      if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        scannedUID = "";
        for (byte i = 0; i < mfrc522.uid.size; i++) {
          if (mfrc522.uid.uidByte[i] < 0x10) scannedUID += "0";
          scannedUID += String(mfrc522.uid.uidByte[i], HEX);
        }
        scannedUID.toUpperCase();
        
        Serial.print(F("CHECK_UID,")); Serial.println(scannedUID);
        BTSerial.print(F("CHECK_UID,")); BTSerial.println(scannedUID);
        
        String response = readUnifiedSerial();
        if (response.startsWith(F("FOUND"))) {
          errorBeep(); displayMessage(F("  Card Already  "), F("   Registered   "));
          delay(1500); scannedUID = ""; 
        } else {
          successBeep(); displayMessage(F("    CARD NOW    "), F("   REGISTERED   "));
          delay(1500); cardAccepted = true; break;
        }
      }
      delay(100);
    }
    if (cardAccepted) break;
  }

  if (!cardAccepted) { errorBeep(); displayMessage(F("  ENROLLMENT   "), F("     FAILED     ")); delay(2000); return; }

  Serial.println(F("GET_NEW_ID"));
  BTSerial.println(F("GET_NEW_ID"));
  
  String idResponse = readUnifiedSerial();
  int newID = idResponse.toInt();
  
  if (newID <= 0) { errorBeep(); displayMessage(F(" Bridge Error "), F(" Invalid ID ")); delay(2000); return; }

  mySerial.listen();
  bool enrollmentStep1Done = false;
  unsigned long enrollStartTime = millis();

  while ((millis() - enrollStartTime < 10000) && !enrollmentStep1Done) {
    int timeLeft = 10 - (millis() - enrollStartTime) / 1000;
    displayMessage(F("  Scan Finger   "), (String(F("Time: ")) + String(timeLeft) + "s").c_str());
    mySerial.listen();
    if (finger.getImage() == FINGERPRINT_OK) {
      if (finger.image2Tz(1) == FINGERPRINT_OK) {
        mySerial.listen();
        if (finger.fingerFastSearch() == FINGERPRINT_OK) {
          errorBeep(); displayMessage(F(" Finger Already "), F("   Registered   ")); delay(2000);
        } else {
          successBeep(); enrollmentStep1Done = true;  
          displayMessage(F(" Remove Finger  "), F(""));
          mySerial.listen();
          while (finger.getImage() != FINGERPRINT_NOFINGER);
          delay(500);
        }
      }
    }
    delay(100);
  }

  if (!enrollmentStep1Done) { errorBeep(); displayMessage(F("  ENROLLMENT   "), F("     FAILED     ")); delay(2000); return; }

  bool enrollmentStep2Done = false;
  while ((millis() - enrollStartTime < 20000) && !enrollmentStep2Done) {
    int timeLeft = (20000 - (millis() - enrollStartTime)) / 1000;
    if (timeLeft < 0) timeLeft = 0;

    lcd.setCursor(0, 0);
    lcd.print(F(" Verify Finger  "));
    lcd.setCursor(0, 1);
    lcd.print(F("Time: ")); lcd.print(timeLeft); lcd.print(F("s    "));

    mySerial.listen();
    if (finger.getImage() == FINGERPRINT_OK) {
      if (finger.image2Tz(2) == FINGERPRINT_OK && finger.createModel() == FINGERPRINT_OK) {
        successBeep(); enrollmentStep2Done = true;
      } else {
        errorBeep(); displayMessage(F("Finger Not Match"), F(" Try Again...    ")); delay(1500);
      }
    }
    delay(100);
  }

  if (enrollmentStep2Done && finger.storeModel(newID) == FINGERPRINT_OK) {
    String finalMsg = String(F("NEW_ENROLL,")) + String(newID) + "," + scannedUID;
    
    Serial.println(finalMsg);
    BTSerial.println(finalMsg);
    
    doubleBeep(); displayMessage(F("  ENROLLMENT   "), F("    SUCCESS!    ")); delay(3000);
  } else {
    errorBeep(); displayMessage(F("  ENROLLMENT   "), F("     FAILED     ")); delay(2000);
  }
  BTSerial.listen();
}

void handleNoIDEntry() {
  if (!checkBridgeAlive()) return; 
  displayMessage(F("  No ID Mode   "), F("")); 
  delay(1000);
  
  unsigned long startTime = millis();
  while (millis() - startTime < FINGER_TIMEOUT_MS) {
    int secondsLeft = (FINGER_TIMEOUT_MS - (millis() - startTime)) / 1000;
    lcd.setCursor(0, 1);
    lcd.print(F("Time: ")); lcd.print(secondsLeft); lcd.print(F("s    "));

    mySerial.listen();
    if (finger.getImage() == FINGERPRINT_OK) {
      if (finger.image2Tz() == FINGERPRINT_OK && finger.fingerFastSearch() == FINGERPRINT_OK) {
          Serial.print(F("GET_NAME_BY_FINGER,")); Serial.println(finger.fingerID);
          BTSerial.print(F("GET_NAME_BY_FINGER,")); BTSerial.println(finger.fingerID);
          
          displayMessage(F("Verifying"), F("Database..."));
          
          unsigned long waitStart = millis();
          while(millis() - waitStart < 3000) {
            String resp = readUnifiedSerial();
            if(resp.startsWith(F("FOUND"))) {
                int firstComma = resp.indexOf(',');
                int secondComma = resp.indexOf(',', firstComma + 1);
                String name = resp.substring(firstComma + 1, secondComma);
                String num = resp.substring(resp.lastIndexOf(',') + 1);
                successBeep();
                displayMessage(F("Student:"), name.c_str());
                delay(1500);
                doubleBeep();
                displayMessage(F("Recording"), F("Attendance..."));
                logToSerial(name.c_str(), num, "NO ID");
                
                String cloudResp = readUnifiedSerial(); 
                if (cloudResp == F("ALREADY_DONE")) {
                   tone(BUZZER_PIN, 500, 100); delay(150);
                   tone(BUZZER_PIN, 500, 100); delay(150);
                   tone(BUZZER_PIN, 500, 300); 
                   lcd.clear(); 
                   lcd.setCursor(0, 0); lcd.print(F("    STUDENT     "));
                   lcd.setCursor(0, 1); lcd.print(F("ALREADY SCANNED "));
                   delay(3000); 
                }
                
                showIdleMessage();
                BTSerial.listen(); 
                return; 
            }
          }
      } else {
          errorBeep(); displayMessage(F("Finger"), F("Unregistered")); delay(2000);
          mySerial.listen();
      }
    }
    delay(50);
  }
  errorBeep(); displayMessage(F("Timeout"), F("Try Again")); delay(1500); showIdleMessage();
  BTSerial.listen();
}

bool verifyFingerWithTimer(int id) {
  mySerial.listen();
  unsigned long startTime = millis();
  while (millis() - startTime < FINGER_TIMEOUT_MS) {
    int secondsLeft = (FINGER_TIMEOUT_MS - (millis() - startTime)) / 1000;
    lcd.setCursor(0, 1);
    lcd.print(F("Time: ")); lcd.print(secondsLeft); lcd.print(F("s    "));

    if (finger.getImage() == FINGERPRINT_OK) {
      if (finger.image2Tz() == FINGERPRINT_OK && finger.fingerFastSearch() == FINGERPRINT_OK) {
        if (finger.fingerID == id) {
          BTSerial.listen(); 
          return true; 
        }
        errorBeep(); displayMessage(F("Finger"), F("Wrong ID")); delay(2000);
        displayMessage(F("Scan Fingerprint"), currentStudentName.c_str());
      } else {
        errorBeep(); displayMessage(F("Finger"), F("Unregistered")); delay(2000);
        displayMessage(F("Scan Fingerprint"), currentStudentName.c_str());
      }
      mySerial.listen();
    }
    delay(100);
  }
  BTSerial.listen();
  return false; 
}

bool checkBridgeAlive() {
  Serial.println(F("PING"));
  BTSerial.println(F("PING"));
  
  unsigned long pingWait = millis();
  while(millis() - pingWait < 2000) { 
    String res = readUnifiedSerial();
    if(res == F("PONG") || res == F("READY")) return true;
  }
  errorBeep(); displayMessage(F("Server"), F("Error")); delay(1500); return false;
}

void logToSerial(const char* name, String num, const char* mode) {
  String logData = String(F("CLOUD_LOG,")) + name + "," + num + "," + mode;
  BTSerial.println(logData);
  Serial.println(logData);
}

void displayMessage(const char* l1, const char* l2) { lcd.clear(); lcd.print(l1); lcd.setCursor(0, 1); lcd.print(l2); }
void displayMessage(const __FlashStringHelper* l1, const __FlashStringHelper* l2) { lcd.clear(); lcd.print(l1); lcd.setCursor(0, 1); lcd.print(l2); }
void displayMessage(const __FlashStringHelper* l1, const char* l2) { lcd.clear(); lcd.print(l1); lcd.setCursor(0, 1); lcd.print(l2); }
void successBeep() { tone(BUZZER_PIN, 2000, 150); }
void doubleBeep() { tone(BUZZER_PIN, 2500, 100); delay(150); tone(BUZZER_PIN, 2500, 100); }
void errorBeep() { tone(BUZZER_PIN, 500, 500); }

void deleteFingerprint(int id) {
  mySerial.listen(); 
  if (finger.deleteModel(id) == FINGERPRINT_OK) {
    successBeep(); displayMessage(F("  ID DELETED   "), F(" FROM DATABASE  "));
  } else {
    errorBeep(); displayMessage(F(" DELETE ERROR  "), F(" ID NOT FOUND  "));
  }
  BTSerial.listen(); 
  delay(2000); showIdleMessage(); 
}