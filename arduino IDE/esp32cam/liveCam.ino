#include <WiFi.h>
#include <WebServer.h>
#include <esp32cam.h>
#include <Preferences.h>

Preferences preferences;

const char* PREF_NAMESPACE = "wifi_config";
WebServer server(80);

// Camera resolutions

// static auto loRes = esp32cam::Resolution::find(320, 240);
// static auto midRes = esp32cam::Resolution::find(350, 530);
// static auto hiRes = esp32cam::Resolution::find(800, 600);
static auto hiRes = esp32cam::Resolution::find(1280, 1024);

// Capture and serve JPEG
void serveJpg() {
  auto frame = esp32cam::capture();
  if (frame == nullptr) {
    Serial.println("CAPTURE FAIL");
    server.send(503, "", "");
    return;
  }
  server.setContentLength(frame->size());
  server.send(200, "image/jpeg");
  frame->writeTo(server.client());
}

// void handleJpgLo() {
//   esp32cam::Camera.changeResolution(loRes);
//   serveJpg();
// }
// void handleJpgMid() {
//   esp32cam::Camera.changeResolution(midRes);
//   serveJpg();
// }
void handleJpgHi() {
  esp32cam::Camera.changeResolution(hiRes);
  serveJpg();
}

void connectToWiFi(String& ssid, String& password) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  Serial.print("Connecting to Wi-Fi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
}

void requestAndStoreCredentials() {
  String ssid = "", password = "";
  Serial.println("Enter Wi-Fi SSID:");
  while (ssid == "") {
    if (Serial.available()) {
      ssid = Serial.readStringUntil('\n');
      ssid.trim();
    }
  }

  Serial.println("Enter Wi-Fi Password:");
  while (password == "") {
    if (Serial.available()) {
      password = Serial.readStringUntil('\n');
      password.trim();
    }
  }

  preferences.begin(PREF_NAMESPACE, false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.end();

  connectToWiFi(ssid, password);
}

void setup() {
  Serial.begin(115200);
  while (!Serial);  // Wait for serial monitor
  delay(100);

  // Initialize camera
  using namespace esp32cam;
  Config cfg;
  cfg.setPins(pins::AiThinker);
  cfg.setResolution(hiRes);
  cfg.setBufferCount(2);
  cfg.setJpeg(80);

  bool ok = Camera.begin(cfg);
  Serial.println(ok ? "CAMERA OK" : "CAMERA FAIL");

  // Read stored credentials
  preferences.begin(PREF_NAMESPACE, true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("pass", "");
  preferences.end();

  // Connect using stored or prompt if not present or invalid
  if (ssid == "" || password == "") {
    requestAndStoreCredentials();
  } else {
    connectToWiFi(ssid, password);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Stored Wi-Fi failed. Re-enter credentials.");
      requestAndStoreCredentials();
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Failed to connect to Wi-Fi.");
  }

  // Setup web server
  // server.on("/cam-lo.jpg", handleJpgLo);
  // server.on("/cam-mid.jpg", handleJpgMid);
  server.on("/cam-hi.jpg", handleJpgHi);
  server.begin();
}

void loop() {
  server.handleClient();
}
