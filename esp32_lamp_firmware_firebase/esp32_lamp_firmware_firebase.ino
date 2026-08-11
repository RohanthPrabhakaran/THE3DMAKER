/*
 * ============================================================
 *  3DLabX Smart Lamp — ESP32-C3 Firmware (Firebase edition)
 * ============================================================
 *  Talks directly to Firebase: Authentication (anonymous, one
 *  permanent identity per lamp) + Realtime Database (state +
 *  polled commands) over plain HTTPS. No MQTT broker, no custom
 *  backend server — matches web/index.html + database.rules.json.
 *
 *  LIBRARIES REQUIRED (Arduino Library Manager):
 *    - FastLED
 *    - ArduinoJson (v6+)
 *  (WiFi.h, WebServer.h, DNSServer.h, Preferences.h, HTTPClient.h,
 *   WiFiClientSecure.h ship with the ESP32 board package)
 *
 *  BEFORE FLASHING, FILL IN (see FIREBASE_SETUP.md steps 4-5):
 *    - FIREBASE_API_KEY   the "apiKey" from your firebaseConfig
 *    - DATABASE_URL        the "databaseURL" from your firebaseConfig,
 *                           WITHOUT a trailing slash
 *
 *  FIRST BOOT: generates a permanent Device ID + Claim Token and
 *  prints them once to Serial — capture these for the lamp's QR
 *  label. It also creates one permanent anonymous Firebase
 *  identity for this lamp (stored in NVS, never recreated).
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <FastLED.h>
#include <ArduinoJson.h>

// ---------------- USER CONFIG ----------------
#define LED_PIN         5
#define NUM_LEDS        8
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define MAX_BRIGHTNESS  255

#define SETUP_BUTTON_PIN 9
#define LONG_PRESS_MS     5000
#define FACTORY_RESET_MS 10000

const char* FIREBASE_API_KEY = "AIzaSyBfJzAPxQCu7zrPfXXhsN8qzocCjLb7szo";
const char* DATABASE_URL     = "https://rgb-led-c4f12-default-rtdb.asia-southeast1.firebasedatabase.app"; // no trailing slash
const char* FIRMWARE_VERSION = "1.0.0";

const unsigned long COMMAND_POLL_MS = 2500;   // how often to check for new commands
const unsigned long HEARTBEAT_MS    = 30000;  // how often to refresh lastSeen/meta

// NOTE on TLS: every HTTPS call below uses client.setInsecure(), which skips
// certificate validation. This is the common pragmatic choice for talking to
// Google APIs from an ESP32 because Google's root CAs rotate and pinning one
// can break unexpectedly. It means requests are encrypted but not verified
// against a trusted root, so in principle traffic could be intercepted by
// something that can redirect your device's DNS/network. For a hardened
// build later, fetch Google's current root CA (e.g. "GTS Root R1") and use
// client.setCACert() instead — see FIREBASE_SETUP.md "TLS trade-off" note.
// -----------------------------------------------

CRGB leds[NUM_LEDS];
Preferences prefs;
WebServer setupServer(80);
DNSServer dnsServer;

// ---------------- PERSISTENT IDENTITY ----------------
String deviceId, claimToken;
String wifiSsid, wifiPass;
bool wifiConfigured = false;

// Firebase identity for this lamp (separate from any human user)
String firebaseUid, firebaseRefreshToken, firebaseIdToken;
unsigned long idTokenExpiresAt = 0; // millis() timestamp

// ---------------- RUNTIME LED STATE ----------------
bool    powerOn      = false;
uint8_t brightness   = 75;
uint8_t colorR = 0x9C, colorG = 0x27, colorB = 0xB0;
String  modeName     = "static";
uint8_t speedPct      = 50;
unsigned long lastAppliedCommandTs = 0;

// ---------------- STATE MACHINE ----------------
enum SystemState {
  BOOT, CHECK_CONFIGURATION, PROVISIONING, WIFI_CONNECTING, WIFI_CONNECTED,
  CLOUD_CONNECTING, ONLINE, WIFI_ERROR, INTERNET_ERROR, CLOUD_ERROR, AUTH_ERROR
};
SystemState state = BOOT;
unsigned long stateEnteredAt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastCommandPoll = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastLedFrame = 0;
uint16_t frameCounter = 0;

bool buttonWasDown = false;
unsigned long buttonDownAt = 0;

void enterState(SystemState s) { state = s; stateEnteredAt = millis(); }

// ================================================================
//  IDENTITY  (Device ID / Claim Token — generated ONCE, permanent)
// ================================================================
String randomHex(int bytes) {
  String out;
  for (int i = 0; i < bytes; i++) {
    char buf[3]; snprintf(buf, sizeof(buf), "%02X", (uint8_t)(esp_random() & 0xFF));
    out += buf;
  }
  return out;
}

void loadOrCreateIdentity() {
  prefs.begin("3dlabx", false);
  if (prefs.getBool("identity_set", false)) {
    deviceId   = prefs.getString("device_id", "");
    claimToken = prefs.getString("claim_token", "");
  } else {
    uint64_t mac = ESP.getEfuseMac();
    char idBuf[16];
    snprintf(idBuf, sizeof(idBuf), "3DLX-%04X%02X", (uint16_t)(mac >> 16), (uint8_t)(mac >> 8));
    deviceId = String(idBuf);
    claimToken = randomHex(8);
    prefs.putString("device_id", deviceId);
    prefs.putString("claim_token", claimToken);
    prefs.putBool("identity_set", true);
    Serial.println("==================================================");
    Serial.println("FIRST BOOT — permanent identity generated:");
    Serial.println("  Device ID:   " + deviceId);
    Serial.println("  Claim Token: " + claimToken);
    Serial.println("  -> Use these two values to print this lamp's QR label.");
    Serial.println("==================================================");
  }
  wifiSsid = prefs.getString("wifi_ssid", "");
  wifiPass = prefs.getString("wifi_pass", "");
  wifiConfigured = wifiSsid.length() > 0;

  // Firebase anonymous identity, also permanent once created
  firebaseUid = prefs.getString("fb_uid", "");
  firebaseRefreshToken = prefs.getString("fb_refresh", "");
}

void saveWifiCredentials(const String& ssid, const String& pass) {
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  wifiSsid = ssid; wifiPass = pass; wifiConfigured = true;
}

void clearWifiCredentials() {
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  wifiConfigured = false;
  // device_id / claim_token / Firebase identity intentionally NOT cleared —
  // permanent per the requirements doc, survives Wi-Fi resets.
}

void saveFirebaseIdentity(const String& uid, const String& refreshToken) {
  firebaseUid = uid;
  firebaseRefreshToken = refreshToken;
  prefs.putString("fb_uid", uid);
  prefs.putString("fb_refresh", refreshToken);
}

// ================================================================
//  STATUS LED
// ================================================================
void showStatusColor(CRGB c) {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  leds[0] = c;
  FastLED.setBrightness(120);
  FastLED.show();
}

void statusAnimationTick() {
  unsigned long t = millis();
  switch (state) {
    case BOOT:
    case CHECK_CONFIGURATION:
      showStatusColor(CRGB::White); break;
    case PROVISIONING: {
      bool on = (t / 500) % 2 == 0;
      showStatusColor(on ? CRGB::Blue : CRGB::Black); break;
    }
    case WIFI_CONNECTING: {
      float phase = (t % 2000) / 2000.0 * 2 * PI;
      uint8_t lvl = (uint8_t)(((sin(phase) + 1.0) / 2.0) * 255);
      CRGB c = CRGB::Blue; c.nscale8_video(lvl);
      showStatusColor(c); break;
    }
    case WIFI_CONNECTED:
    case CLOUD_CONNECTING:
      showStatusColor(CRGB::Green); break;
    case ONLINE:
      break; // normal effect loop takes over
    case INTERNET_ERROR:
      showStatusColor(CRGB::Yellow); break;
    case WIFI_ERROR: {
      bool on = (t / 300) % 2 == 0;
      showStatusColor(on ? CRGB::Red : CRGB::Black); break;
    }
    case CLOUD_ERROR:
    case AUTH_ERROR:
      showStatusColor(CRGB::OrangeRed); break;
  }
}

// ================================================================
//  WI-FI PROVISIONING  (unchanged from the local-prototype version)
// ================================================================
const char SETUP_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>3DLabX Lamp Setup</title>
<style>
body{font-family:sans-serif;background:#0a0e17;color:#eee;padding:24px;max-width:420px;margin:auto}
h1{font-size:20px} select,input{width:100%;padding:10px;margin:8px 0;border-radius:8px;border:1px solid #333;background:#141a28;color:#eee}
button{width:100%;padding:12px;border-radius:8px;border:none;background:#3b82f6;color:#fff;font-weight:bold;margin-top:10px}
.id{font-family:monospace;background:#141a28;padding:8px;border-radius:6px;margin:10px 0}
</style></head><body>
<h1>3DLabX Lamp Setup</h1>
<p>Device ID</p><div class="id">%DEVICE_ID%</div>
<form action="/save" method="POST">
<label>Wi-Fi network</label>
<select name="ssid">%NETWORKS%</select>
<label>Password</label>
<input type="password" name="pass" placeholder="Wi-Fi password">
<button type="submit">Connect</button>
</form>
</body></html>
)HTML";

void handleSetupRoot() {
  int n = WiFi.scanNetworks();
  String options;
  for (int i = 0; i < n; i++) options += "<option value=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + "</option>";
  String page = String(SETUP_PAGE);
  page.replace("%DEVICE_ID%", deviceId);
  page.replace("%NETWORKS%", options);
  setupServer.send(200, "text/html", page);
}
void handleSetupSave() {
  String ssid = setupServer.arg("ssid"), pass = setupServer.arg("pass");
  if (ssid.length() == 0) { setupServer.send(400, "text/plain", "SSID required"); return; }
  saveWifiCredentials(ssid, pass);
  setupServer.send(200, "text/html", "<html><body style='font-family:sans-serif;background:#0a0e17;color:#eee;padding:24px'>Saved. Rebooting...</body></html>");
  delay(1500);
  ESP.restart();
}
void startProvisioning() {
  enterState(PROVISIONING);
  WiFi.mode(WIFI_AP);
  String apName = "3DLabX-Setup-" + deviceId.substring(deviceId.length() - 6);
  WiFi.softAP(apName.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  setupServer.on("/", handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  setupServer.onNotFound(handleSetupRoot);
  setupServer.begin();
  Serial.println("[provisioning] AP started: " + apName);
}

// ================================================================
//  LOCAL LED EFFECTS  (unchanged — keeps running through outages)
// ================================================================
CRGB currentColor() { return CRGB(colorR, colorG, colorB); }
uint16_t speedDelayMs() { return map(speedPct, 1, 100, 120, 4); }
void effectStatic()   { fill_solid(leds, NUM_LEDS, currentColor()); }
void effectRainbow() {
  static uint8_t hue = 0;
  fill_rainbow(leds, NUM_LEDS, hue, 255 / max(1, NUM_LEDS));
  hue += map(speedPct, 1, 100, 1, 8);
}
void effectBreathing() {
  float t = (millis() % 4000) / 4000.0 * 2 * PI;
  float level = (sin(t) + 1.0) / 2.0;
  CRGB c = currentColor(); c.nscale8_video((uint8_t)(level * 255));
  fill_solid(leds, NUM_LEDS, c);
}
void effectBlink() {
  bool on = (frameCounter / 6) % 2 == 0;
  fill_solid(leds, NUM_LEDS, on ? currentColor() : CRGB::Black);
}
void effectChase() {
  fadeToBlackBy(leds, NUM_LEDS, 40);
  int pos = frameCounter % NUM_LEDS;
  for (int i = 0; i < NUM_LEDS; i += 3) leds[(pos + i) % NUM_LEDS] = currentColor();
}
void effectFire() {
  static byte heat[NUM_LEDS];
  int cooling = 55, sparking = 120;
  for (int i = 0; i < NUM_LEDS; i++) heat[i] = qsub8(heat[i], random8(0, ((cooling * 10) / NUM_LEDS) + 2));
  for (int k = NUM_LEDS - 1; k >= 2; k--) heat[k] = (heat[k-1] + heat[k-2] + heat[k-2]) / 3;
  if (random8() < sparking) { int y = random8(7); heat[y] = qadd8(heat[y], random8(160, 255)); }
  for (int j = 0; j < NUM_LEDS; j++) leds[j] = HeatColor(heat[j]);
}
void effectPolice() {
  bool half = (frameCounter / 8) % 2 == 0;
  for (int i = 0; i < NUM_LEDS; i++) leds[i] = ((i < NUM_LEDS/2) == half) ? CRGB::Red : CRGB::Blue;
}
void effectCandle() {
  for (int i = 0; i < NUM_LEDS; i++) { uint8_t f = random8(180, 255); leds[i] = CRGB(f, f*0.55, 0); }
}
void effectMusic() {
  uint8_t level = beatsin8(60, 40, 255); // placeholder — wire a mic to an ADC pin for real audio reactivity
  CRGB c = currentColor(); c.nscale8_video(level);
  fill_solid(leds, NUM_LEDS, c);
}
void runLedEffect() {
  if (!powerOn) { fill_solid(leds, NUM_LEDS, CRGB::Black); return; }
  if (modeName == "rainbow")        effectRainbow();
  else if (modeName == "breathing") effectBreathing();
  else if (modeName == "blink")     effectBlink();
  else if (modeName == "chase")     effectChase();
  else if (modeName == "fire")      effectFire();
  else if (modeName == "police")    effectPolice();
  else if (modeName == "candle")    effectCandle();
  else if (modeName == "music")     effectMusic();
  else                                effectStatic();
}
void applyBrightness() {
  uint8_t b = map(brightness, 0, 100, 0, MAX_BRIGHTNESS);
  FastLED.setBrightness(powerOn ? b : 0);
}

// ================================================================
//  FIREBASE AUTH  (anonymous sign-up once, refresh forever after)
// ================================================================
bool firebaseSignUpAnonymous() {
  Serial.println("[trace] firebaseSignUpAnonymous: start");
  Serial.printf("[trace] free heap: %u\n", ESP.getFreeHeap());
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") + FIREBASE_API_KEY;
  Serial.println("[trace] url: " + url);
  if (!http.begin(client, url)) {
    Serial.println("[trace] http.begin() FAILED");
    return false;
  }
  Serial.println("[trace] http.begin() ok, sending POST...");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST("{\"returnSecureToken\":true}");
  Serial.printf("[trace] POST returned code: %d\n", code);
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    Serial.println("[trace] body: " + body);
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      Serial.println("[trace] JSON parse failed: " + String(err.c_str()));
    } else {
      saveFirebaseIdentity(doc["localId"].as<String>(), doc["refreshToken"].as<String>());
      firebaseIdToken = doc["idToken"].as<String>();
      idTokenExpiresAt = millis() + (doc["expiresIn"].as<long>() * 1000UL) - 60000UL;
      Serial.println("[firebase] anonymous identity created: " + firebaseUid);
      ok = true;
    }
  } else {
    String body = http.getString();
    Serial.printf("[firebase] signUp failed, HTTP %d\n", code);
    Serial.println("[firebase] response body: " + body);
  }
  http.end();
  Serial.println("[trace] firebaseSignUpAnonymous: end, ok=" + String(ok));
  return ok;
}



bool firebaseRefreshIdToken() {
  Serial.println("[trace] firebaseRefreshIdToken: start");
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
  if (!http.begin(client, url)) {
    Serial.println("[trace] refresh http.begin() FAILED");
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=refresh_token&refresh_token=" + firebaseRefreshToken;
  int code = http.POST(body);
  Serial.printf("[trace] refresh POST returned code: %d\n", code);
  bool ok = false;
  if (code == 200) {
    String respBody = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, respBody);
    if (err) {
      Serial.println("[trace] refresh JSON parse failed: " + String(err.c_str()));
    } else {
      firebaseIdToken = doc["id_token"].as<String>();
      firebaseRefreshToken = doc["refresh_token"].as<String>();
      prefs.putString("fb_refresh", firebaseRefreshToken);
      idTokenExpiresAt = millis() + (doc["expires_in"].as<long>() * 1000UL) - 60000UL;
      Serial.println("[trace] refresh ok, idToken len=" + String(firebaseIdToken.length()));
      ok = true;
    }
  } else {
    String respBody = http.getString();
    Serial.printf("[firebase] token refresh failed, HTTP %d\n", code);
    Serial.println("[firebase] refresh response body: " + respBody);
  }
  http.end();
  Serial.println("[trace] firebaseRefreshIdToken: end, ok=" + String(ok));
  return ok;
}

// Ensures we have a valid idToken, signing up on first-ever boot.
bool ensureFirebaseAuth() {
  // Serial.println("[trace] ensureFirebaseAuth called, refreshToken len=" + String(firebaseRefreshToken.length()));
  if (firebaseRefreshToken.length() == 0) {
    Serial.println("[trace] ensureFirebaseAuth called, sucess=" + String(firebaseRefreshToken.length()));
    return firebaseSignUpAnonymous();
  }
  if (firebaseIdToken.length() == 0 || millis() > idTokenExpiresAt) {
    Serial.println("[trace] ensureFirebaseAuth called, sucess 2=" + String(firebaseRefreshToken.length()));
    return firebaseRefreshIdToken();
  }
  return true;
}

// ================================================================
//  REALTIME DATABASE  (plain HTTPS REST, auth via ?auth=idToken)
// ================================================================
bool rtdbRequest(const String& method, const String& path, const String& jsonBody, JsonDocument* outDoc) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String(DATABASE_URL) + path + ".json?auth=" + firebaseIdToken;
  Serial.println("[trace] rtdb idToken len=" + String(firebaseIdToken.length()));
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");

  int code;
  if (method == "GET")        code = http.GET();
  else if (method == "PUT")   code = http.PUT(jsonBody);
  else if (method == "PATCH") code = http.PATCH(jsonBody);
  else { http.end(); return false; }

  bool ok = (code >= 200 && code < 300);
  String raw = http.getString(); // read once, use for either success parse or error print
  if (ok && outDoc) {
    DeserializationError err = deserializeJson(*outDoc, raw);
    if (err) ok = false;
  }
  if (!ok) {
    Serial.printf("[rtdb] %s %s -> HTTP %d\n", method.c_str(), path.c_str(), code);
    Serial.println("[rtdb] error body: " + raw);
  }
  http.end();
  return ok;
}

bool registerWithFirebase() {
  Serial.println("[trace] registerWithFirebase: start");
  StaticJsonDocument<256> existing;
  bool gotIt = rtdbRequest("GET", "/devices/" + deviceId + "/deviceUid", "", &existing);
  Serial.println("[trace] GET deviceUid ok=" + String(gotIt));

  if (gotIt && !existing.isNull() && existing.as<String>().length() > 0) {
    if (existing.as<String>() != firebaseUid) {
      Serial.println("[register] WARNING: Device ID collision with a different Firebase identity!");
      return false;
    }
    Serial.println("[trace] already registered previously");
    return true;
  }

  StaticJsonDocument<256> doc;
  doc["deviceUid"] = firebaseUid;
  doc["claimToken"] = claimToken;
  String body; serializeJson(doc, body);
  bool ok = rtdbRequest("PATCH", "/devices/" + deviceId, body, nullptr);
  Serial.println("[trace] PATCH register ok=" + String(ok));
  if (ok) Serial.println("[register] device registered in Realtime Database");
  return ok;
}
void publishMeta() {
  StaticJsonDocument<256> doc;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  doc["internetConnected"] = state == ONLINE;
  JsonObject lastSeen = doc.createNestedObject("lastSeen");
  lastSeen[".sv"] = "timestamp";
  String body; serializeJson(doc, body);
  rtdbRequest("PATCH", "/devices/" + deviceId + "/meta", body, nullptr);
}

void publishState() {
  StaticJsonDocument<256> doc;
  doc["power"] = powerOn;
  doc["brightness"] = brightness;
  char hex[8]; snprintf(hex, sizeof(hex), "#%02X%02X%02X", colorR, colorG, colorB);
  doc["color"] = hex;
  doc["mode"] = modeName;
  doc["speed"] = speedPct;
  String body; serializeJson(doc, body);
  rtdbRequest("PATCH", "/devices/" + deviceId + "/state", body, nullptr);
}

void pollCommand() {
  DynamicJsonDocument doc(512);
  bool ok = rtdbRequest("GET", "/devices/" + deviceId + "/command", "", &doc);
  if (!ok || doc.isNull()) return;

  unsigned long ts = doc["ts"] | 0UL;
  if (ts == 0 || ts <= lastAppliedCommandTs) return; // nothing new
  lastAppliedCommandTs = ts;

  bool changed = false;
  if (!doc["power"].isNull())      { powerOn = doc["power"].as<bool>(); changed = true; }
  if (!doc["brightness"].isNull()) { brightness = constrain(doc["brightness"].as<int>(), 0, 100); changed = true; }
  if (!doc["mode"].isNull())       { modeName = doc["mode"].as<String>(); changed = true; }
  if (!doc["speed"].isNull())      { speedPct = constrain(doc["speed"].as<int>(), 1, 100); changed = true; }
  if (!doc["color"].isNull()) {
    String hex = doc["color"].as<String>(); hex.replace("#", "");
    if (hex.length() == 6) {
      long val = strtol(hex.c_str(), NULL, 16);
      colorR = (val >> 16) & 0xFF; colorG = (val >> 8) & 0xFF; colorB = val & 0xFF;
      changed = true;
    }
  }
  if (changed) { applyBrightness(); publishState(); }
}

// ================================================================
//  SETUP / RESET BUTTON
// ================================================================
void handleSetupButton() {
  bool down = digitalRead(SETUP_BUTTON_PIN) == LOW;
  unsigned long now = millis();
  if (down && !buttonWasDown) { buttonDownAt = now; buttonWasDown = true; }
  else if (!down && buttonWasDown) {
    unsigned long held = now - buttonDownAt;
    buttonWasDown = false;
    if (held >= FACTORY_RESET_MS) {
      showStatusColor(CRGB::Red); delay(300);
      showStatusColor(CRGB::White); delay(300);
      showStatusColor(CRGB::Blue); delay(300);
      clearWifiCredentials();
      ESP.restart();
    } else if (held >= LONG_PRESS_MS) {
      clearWifiCredentials();
      ESP.restart();
    }
  }
}

// ================================================================
//  ARDUINO SETUP / LOOP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(0);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  loadOrCreateIdentity();
  enterState(CHECK_CONFIGURATION);
}

void loop() {
  unsigned long now = millis();
  handleSetupButton();

  switch (state) {
    case BOOT:
      enterState(CHECK_CONFIGURATION); break;

    case CHECK_CONFIGURATION:
      if (wifiConfigured) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);   // <-- disable modem sleep, fixes most intermittent drops
        WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
        lastWifiAttempt = now;
        enterState(WIFI_CONNECTING);
      } else {
        startProvisioning();
      }
      break;

    case PROVISIONING:
      dnsServer.processNextRequest();
      setupServer.handleClient();
      break;

    case WIFI_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[wifi] connected, IP: " + WiFi.localIP().toString());
        enterState(WIFI_CONNECTED);
      } else if (now - stateEnteredAt > 20000) {
        enterState(WIFI_ERROR);
      }
      break;

    case WIFI_CONNECTED:
      enterState(CLOUD_CONNECTING);
      break;

    case CLOUD_CONNECTING:
      Serial.println("[trace] entered CLOUD_CONNECTING");
      if (!ensureFirebaseAuth()) { Serial.println("[trace] -> AUTH_ERROR"); enterState(AUTH_ERROR); break; }
      if (!registerWithFirebase()) { Serial.println("[trace] -> INTERNET_ERROR"); enterState(INTERNET_ERROR); break; }
      Serial.println("[trace] -> ONLINE");
      publishMeta();
      publishState();
      lastHeartbeat = now;
      enterState(ONLINE);
      break;
    

    case ONLINE:
      if (WiFi.status() != WL_CONNECTED) { enterState(WIFI_CONNECTING); break; }
      if (!ensureFirebaseAuth()) { enterState(AUTH_ERROR); break; }
      if (now - lastCommandPoll > COMMAND_POLL_MS) { lastCommandPoll = now; pollCommand(); }
      if (now - lastHeartbeat > HEARTBEAT_MS) { lastHeartbeat = now; publishMeta(); }
      break;

    case WIFI_ERROR:
    case INTERNET_ERROR:
    case CLOUD_ERROR:
    case AUTH_ERROR:
      if (now - lastWifiAttempt > 15000) { lastWifiAttempt = now; enterState(CHECK_CONFIGURATION); }
      break;
  }

  if (state == ONLINE) {
    if (now - lastLedFrame >= speedDelayMs()) {
      lastLedFrame = now; frameCounter++;
      applyBrightness(); runLedEffect(); FastLED.show();
    }
  } else {
    statusAnimationTick();
  }
}
