#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include <AceButton.h>
#include <Preferences.h>   // <<--- ADDED
using namespace ace_button;

// -------- CONFIG --------
#define USE_LATCHED_SWITCH true

// Provisioning details
const char *service_name = NULL;          // <<--- CHANGED
const char *pop = "305roomwow";           // <<--- CHANGED

// Device Names
char deviceName_1[] = "Switch1";
char deviceName_2[] = "Switch2";
char deviceName_3[] = "Switch3";
char deviceName_4[] = "Switch4";
char deviceName_5[] = "Switch5";
char deviceName_6[] = "Switch6";
char deviceName_7[] = "Switch7";
char deviceName_8[] = "Switch8";

// Relay pins (as requested)
static uint8_t RelayPin1 = 21;
static uint8_t RelayPin2 = 19;
static uint8_t RelayPin3 = 18;
static uint8_t RelayPin4 = 5;
static uint8_t RelayPin5 = 17;
static uint8_t RelayPin6 = 16;
static uint8_t RelayPin7 = 4;
static uint8_t RelayPin8 = 2;

// Switch pins (change to match your wiring if needed)
static uint8_t SwitchPin1 = 32;
static uint8_t SwitchPin2 = 33;
static uint8_t SwitchPin3 = 25;
static uint8_t SwitchPin4 = 26;
static uint8_t SwitchPin5 = 27;
static uint8_t SwitchPin6 = 14;
static uint8_t SwitchPin7 = 12;
static uint8_t SwitchPin8 = 13;

// Misc pins
static uint8_t wifiLed = 22;   // Not using 2 because it's used as RelayPin1
static uint8_t gpio_reset = 0;

// States
bool toggleState[8] = {false, false, false, false, false, false, false, false};

// Preferences object (to remember states across power cycles)
Preferences prefs; // <<--- ADDED

// AceButton setup
ButtonConfig configs[8];
AceButton buttons[8] = {
  AceButton(&configs[0]), AceButton(&configs[1]),
  AceButton(&configs[2]), AceButton(&configs[3]),
  AceButton(&configs[4]), AceButton(&configs[5]),
  AceButton(&configs[6]), AceButton(&configs[7])
};

// RainMaker switches
static Switch my_switch[8] = {
  Switch(deviceName_1, &RelayPin1),
  Switch(deviceName_2, &RelayPin2),
  Switch(deviceName_3, &RelayPin3),
  Switch(deviceName_4, &RelayPin4),
  Switch(deviceName_5, &RelayPin5),
  Switch(deviceName_6, &RelayPin6),
  Switch(deviceName_7, &RelayPin7),
  Switch(deviceName_8, &RelayPin8)
};

// Forward declarations
void write_callback(Device *device, Param *param, const param_val_t val, void *priv_data, write_ctx_t *ctx);
void sysProvEvent(arduino_event_t *sys_event);

// ---------------- Helpers ----------------
void setRelay(uint8_t pin, int idx, bool state) {
  // active-low relay assumed (adjust if you use active-high)
  digitalWrite(pin, !state);
}

// Save current toggleState[] into preferences as a bitmask
void saveSwitchStatesToPrefs() { // <<--- ADDED
  uint8_t mask = 0;
  for (int i = 0; i < 8; ++i) {
    if (toggleState[i]) mask |= (1 << i);
  }
  prefs.putUChar("swmask", mask);
}

// Load saved states from preferences into toggleState[] (returns mask)
uint8_t loadSwitchStatesFromPrefs() { // <<--- ADDED
  uint8_t mask = prefs.getUChar("swmask", 0); // default 0 => all OFF
  for (int i = 0; i < 8; ++i) {
    toggleState[i] = ((mask >> i) & 0x01);
  }
  return mask;
}

// AceButton -> Relay handler wrapper
void buttonHandler(AceButton* button, uint8_t eventType, uint8_t,
                   uint8_t relayPin, int idx, Switch &sw, bool &state) {
  bool newState = false;
  if (USE_LATCHED_SWITCH) {
    newState = (eventType == AceButton::kEventPressed);
  } else {
    if (eventType != AceButton::kEventReleased) return;
    newState = !(digitalRead(relayPin) == LOW);
  }

  setRelay(relayPin, idx, newState);
  state = newState;
  sw.updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, state);
  Serial.printf("Manual: Relay pin %d -> %d\n", relayPin, state);

  // persist the change
  saveSwitchStatesToPrefs(); // <<--- ADDED
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(100);

  // Initialize preferences namespace
  prefs.begin("sw", false); // <<--- ADDED (namespace "sw", read/write)

  // Generate per-device service_name with your prefix
  static char service_name_buf[24];                                  // <<--- ADDED
  uint64_t chipid = ESP.getEfuseMac();                               // <<--- ADDED
  snprintf(service_name_buf, sizeof(service_name_buf),               // <<--- ADDED
           "growgreenskull-%04X", (uint16_t)(chipid & 0xFFFF));      // <<--- ADDED
  service_name = service_name_buf;                                   // <<--- ADDED

  // Initialize relay pins and set default OFF (toggleState defaults to false)
  uint8_t relayPins[8] = {RelayPin1, RelayPin2, RelayPin3, RelayPin4, RelayPin5, RelayPin6, RelayPin7, RelayPin8};

  // Load saved states from NVS and apply before setting pins
  loadSwitchStatesFromPrefs(); // <<--- ADDED: populates toggleState[]

  toggleState[0] = true;                     // <<--- LOCK 21 HIGH (logic state always ON)

  for (int i = 0; i < 8; ++i) {
    pinMode(relayPins[i], OUTPUT);
    if (i == 0) {
      // For RelayPin1 (GPIO21), always drive HIGH, ignore saved state
      digitalWrite(relayPins[i], HIGH);      // <<--- LOCK 21 HIGH (electrical level)
    } else {
      setRelay(relayPins[i], i, toggleState[i]); // restored state
    }
  }

  // Misc pins
  pinMode(wifiLed, OUTPUT);
  pinMode(gpio_reset, INPUT);

  // Switch pins and AceButton init
  uint8_t switchPins[8] = {SwitchPin1, SwitchPin2, SwitchPin3, SwitchPin4, SwitchPin5, SwitchPin6, SwitchPin7, SwitchPin8};
  for (int i = 0; i < 8; ++i) {
    pinMode(switchPins[i], INPUT_PULLUP);
    buttons[i].init(switchPins[i]);
  }

  // Attach AceButton handlers (using lambdas to pass per-button params)
  // Switch 1: IGNORE button, keep GPIO21 HIGH
  configs[0].setEventHandler([](AceButton* b, uint8_t e, uint8_t s){
    digitalWrite(RelayPin1, HIGH);                             // <<--- LOCK 21 HIGH
    toggleState[0] = true;                                     // <<--- LOCK 21 HIGH
    my_switch[0].updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, true);
    Serial.println("Switch1 ignored. GPIO21 forced HIGH.");
  });

  configs[1].setEventHandler([](AceButton* b, uint8_t e, uint8_t s){ buttonHandler(b,e,s,RelayPin2,1,my_switch[1],toggleState[1]); });
  configs[2].setEventHandler([](AceButton* b, uint8_t e, uint8_t s){ buttonHandler(b,e,s,RelayPin3,2,my_switch[2],toggleState[2]); });
  configs[3].setEventHandler([](AceButton* b, uint8_t e, uint8_t s){ buttonHandler(b,e,s,RelayPin4,3,my_switch[3],toggleState[3]); });
  configs[4].setEventHandler([](AceButton* b, uint8_t e, uint8_t s){ buttonHandler(b,e,s,RelayPin5,4,my_switch[4],toggleState[4]); });
  configs[5].setEventHandler([](AceButton* b, uint8_t e, uint8_t s){ buttonHandler(b,e,s,RelayPin6,5,my_switch[5],toggleState[5]); });
  configs[6].setEventHandler([](AceButton* b, uint8_t e, uint8_t s){ buttonHandler(b,e,s,RelayPin7,6,my_switch[6],toggleState[6]); });
  configs[7].setEventHandler([](AceButton* b, uint8_t e, uint8_t s){ buttonHandler(b,e,s,RelayPin8,7,my_switch[7],toggleState[7]); });

  // RainMaker node + devices
  Node my_node = RMaker.initNode("ESP32_Relay_8");
  for (int i = 0; i < 8; ++i) {
    my_switch[i].addCb(write_callback);
    my_node.addDevice(my_switch[i]);
    my_switch[i].updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, toggleState[i]);
  }

  // ---- Enable Timezone + Schedule so app's schedule feature works ----
  RMaker.enableTZService();        // <<--- ADDED
  RMaker.enableSchedule();         // <<--- ADDED
  // -------------------------------------------------------------------

  // Minimal RainMaker features: NO OTA (still disabled)
  RMaker.start();
  WiFi.onEvent(sysProvEvent);

  // Provisioning using NETWORK_PROV_... (new macros)
  WiFiProv.beginProvision(NETWORK_PROV_SCHEME_BLE,
                          NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM,
                          NETWORK_PROV_SECURITY_1,
                          pop, service_name);

  Serial.println("Setup complete (EEPROM/TZ/OTA/Schedule removed). Relays start OFF or restored from prefs.");
}

void loop() {
  // Reset handling (press gpio_reset for >3s for WiFi reset, >10s for factory reset)
  if (digitalRead(gpio_reset) == LOW) {
    delay(100);
    unsigned long startTime = millis();
    while (digitalRead(gpio_reset) == LOW) delay(50);
    unsigned long duration = millis() - startTime;
    if (duration > 10000) {
      Serial.println("Factory reset triggered.");
      prefs.clear(); // <<--- ADDED: clear stored switch states on factory reset
      RMakerFactoryReset(2);
    } else if (duration > 3000) {
      Serial.println("WiFi reset triggered.");
      RMakerWiFiReset(2);
    }
  }

  digitalWrite(wifiLed, WiFi.status() == WL_CONNECTED);

  // Poll buttons
  for (int i = 0; i < 8; ++i) buttons[i].check();
}

// ---------------- RainMaker write callback ----------------
void write_callback(Device *device, Param *param, const param_val_t val, void *priv_data, write_ctx_t *ctx) {
  if (strcmp(param->getParamName(), "Power") != 0) return;
  bool newState = val.val.b;

  const char *dname = device->getDeviceName();

  if (strcmp(dname, deviceName_1) == 0) {
    // For Switch1 / GPIO21: ignore requested state, always keep HIGH
    digitalWrite(RelayPin1, HIGH);                            // <<--- LOCK 21 HIGH
    toggleState[0] = true;                                    // <<--- LOCK 21 HIGH
    my_switch[0].updateAndReportParam(param->getParamName(), true);
    Serial.println("Write ignored for Switch1. GPIO21 forced HIGH.");
  } else if (strcmp(dname, deviceName_2) == 0) {
    setRelay(RelayPin2, 1, newState); toggleState[1] = newState; my_switch[1].updateAndReportParam(param->getParamName(), newState);
  } else if (strcmp(dname, deviceName_3) == 0) {
    setRelay(RelayPin3, 2, newState); toggleState[2] = newState; my_switch[2].updateAndReportParam(param->getParamName(), newState);
  } else if (strcmp(dname, deviceName_4) == 0) {
    setRelay(RelayPin4, 3, newState); toggleState[3] = newState; my_switch[3].updateAndReportParam(param->getParamName(), newState);
  } else if (strcmp(dname, deviceName_5) == 0) {
    setRelay(RelayPin5, 4, newState); toggleState[4] = newState; my_switch[4].updateAndReportParam(param->getParamName(), newState);
  } else if (strcmp(dname, deviceName_6) == 0) {
    setRelay(RelayPin6, 5, newState); toggleState[5] = newState; my_switch[5].updateAndReportParam(param->getParamName(), newState);
  } else if (strcmp(dname, deviceName_7) == 0) {
    setRelay(RelayPin7, 6, newState); toggleState[6] = newState; my_switch[6].updateAndReportParam(param->getParamName(), newState);
  } else if (strcmp(dname, deviceName_8) == 0) {
    setRelay(RelayPin8, 7, newState); toggleState[7] = newState; my_switch[7].updateAndReportParam(param->getParamName(), newState);
  }

  // persist the change
  saveSwitchStatesToPrefs(); // <<--- ADDED

  Serial.printf("Write callback: %s -> %d\n", dname, newState);
}

// ---------------- Provisioning / WiFi events ----------------
void sysProvEvent(arduino_event_t *sys_event) {
  switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
      Serial.printf("Provisioning started: %s\n", service_name);
      printQR(service_name, pop, "ble");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("Connected to Wi-Fi");
      digitalWrite(wifiLed, HIGH);
      break;
  }
}
