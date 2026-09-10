#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <Wire.h>

// --- MOTOR PIN CONFIGURATION ---
#define PIN_IN1 14  // D5 (Left Forward)
#define PIN_IN2 12  // D6 (Left Reverse)
#define PIN_IN3 5   // D1 (Right Forward)
#define PIN_IN4 4   // D2 (Right Reverse)

// --- SENSOR CONFIGURATION ---
#define PIN_TRIG 0   // D3
#define PIN_ECHO 13  // D7

// --- SERVO CONFIGURATION ---
#define PIN_SERVO 15  // D8

// --- TUNED HACKATHON DETECTION BOUNDARIES ---
#define DETECT_CENTER 45  // Stops smoothly at 45cm straight ahead
#define DETECT_SIDE 35    // Stops safely at 35cm on the sides

ESP8266WebServer server(80);

volatile int leftDistance = 0;
volatile int centerDistance = 0;
volatile int rightDistance = 0;
bool autoDeliveryMode = false;
bool emergencyBrakeLocked = false;
String robotStateMsg = "SYSTEM ONLINE: STANDBY";

const char HTML_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<title>Zippy Mission Hub</title>
<style>
  body{font-family:sans-serif;text-align:center;background:#121216;color:#fff;padding:10px;margin:0;}
  h1{color:#00ffcc;font-size:24px;margin-bottom:2px;}
  .status-lbl{font-weight:bold;color:#ff3366;font-size:14px;margin-bottom:15px;}
  .btn{display:inline-block;width:95px;height:45px;margin:5px;font-size:14px;font-weight:bold;color:#fff;background:#007BFF;border:none;border-radius:8px;line-height:45px;cursor:pointer;}
  .dispatch-btn{width:220px;background:#28a745;height:50px;line-height:50px;font-size:16px;font-weight:bold;border-radius:10px;border:none;color:white;margin-bottom:10px;}
  .radar-box{background:#1c1c24;margin:10px auto;padding:15px;border-radius:12px;max-width:320px;border:1px solid #00ffcc;}
  .radar-grid{display:flex;justify-content:space-around;margin-top:10px;}
  .data-val{font-size:22px;font-weight:bold;color:#ffcc00;}
</style>
<script>
  function sendCmd(action) { fetch('/' + action).catch(err => {}); }
  setInterval(function() {
    fetch('/data').then(response => response.json()).then(data => {
      document.getElementById('lbl_left').innerText = data.left + ' cm';
      document.getElementById('lbl_center').innerText = data.center + ' cm';
      document.getElementById('lbl_right').innerText = data.right + ' cm';
      document.getElementById('lbl_status').innerText = data.msg;
      if (data.brake) {
         document.getElementById('lbl_status').style.color = "#ff0033";
      } else {
         document.getElementById('lbl_status').style.color = data.mode ? "#28a745" : "#00ffcc";
      }
    }).catch(err => {});
  }, 350);
</script>
</head>
<body>
  <h1>ZIPPY CONTROL INTERFACE</h1>
  <div class='status-lbl' id='lbl_status'>SYSTEM ONLINE: STANDBY</div>
  <button class='dispatch-btn' onclick="sendCmd('toggle_auto')">DISPATCH MISSION (A TO B)</button>
  <div class='radar-box'>
    <h3>📡 REAL-TIME SONAR FEED</h3>
    <div class='radar-grid'>
      <div><div>LEFT</div><div id='lbl_left' class='data-val'>--</div></div>
      <div><div>CENTER</div><div id='lbl_center' class='data-val'>--</div></div>
      <div><div>RIGHT</div><div id='lbl_right' class='data-val'>--</div></div>
    </div>
  </div>
  <h3>MANUAL PILOT DRIVING</h3>
  <button class='btn' onclick="sendCmd('forward')">FORWARD</button><br>
  <button class='btn' onclick="sendCmd('left')">LEFT</button>
  <button class='btn' style='background:#DC3545;' onclick="sendCmd('stop')">STOP</button>
  <button class='btn' onclick="sendCmd('right')">RIGHT</button><br>
  <button class='btn' onclick="sendCmd('backward')">BACKWARD</button>
</body></html>
)=====";

// Library-Free Direct Low-Level OLED Command Engine
void writeOLEDCommand(uint8_t c) {
  Wire.beginTransmission(0x3C);
  Wire.write(0x00);
  Wire.write(c);
  Wire.endTransmission();
}

void initOLEDDirect() {
  writeOLEDCommand(0xAE);  // Display OFF
  writeOLEDCommand(0xD5);
  writeOLEDCommand(0x80);
  writeOLEDCommand(0xA8);
  writeOLEDCommand(0x3F);
  writeOLEDCommand(0xD3);
  writeOLEDCommand(0x00);
  writeOLEDCommand(0x40);
  writeOLEDCommand(0x8D);
  writeOLEDCommand(0x14);  // Enable Charge Pump
  writeOLEDCommand(0x20);
  writeOLEDCommand(0x02);  // Page Addressing Mode
  writeOLEDCommand(0xA1);  // Segment Remap
  writeOLEDCommand(0xC8);  // COM Scan Direction
  writeOLEDCommand(0xDA);
  writeOLEDCommand(0x12);
  writeOLEDCommand(0x81);
  writeOLEDCommand(0xCF);
  writeOLEDCommand(0xD9);
  writeOLEDCommand(0xF1);
  writeOLEDCommand(0xDB);
  writeOLEDCommand(0x40);
  writeOLEDCommand(0xA4);  // Entire Display ON
  writeOLEDCommand(0xA6);  // Normal Display
  writeOLEDCommand(0xAF);  // Display ON
}

void clearOLEDDirect() {
  for (uint8_t page = 0; page < 8; page++) {
    writeOLEDCommand(0xB0 + page);
    writeOLEDCommand(0x00);
    writeOLEDCommand(0x10);
    Wire.beginTransmission(0x3C);
    Wire.write(0x40);
    for (uint8_t i = 0; i < 128; i++) {
      Wire.write(0x00);
      if (i % 16 == 0 && i > 0) {
        Wire.endTransmission();
        Wire.beginTransmission(0x3C);
        Wire.write(0x40);
      }
    }
    Wire.endTransmission();
  }
}

void handleRoot() {
  server.send(200, "text/html", HTML_PAGE);
}
void handleData() {
  String json = "{\"left\":" + String(leftDistance) + ",\"center\":" + String(centerDistance) + ",\"right\":" + String(rightDistance) + ",\"mode\":" + String(autoDeliveryMode) + ",\"brake\":" + String(emergencyBrakeLocked) + ",\"msg\":\"" + robotStateMsg + "\"}";
  server.send(200, "application/json", json);
}

void botForward() {
  if (!emergencyBrakeLocked) {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, HIGH);
    digitalWrite(PIN_IN3, LOW);
    digitalWrite(PIN_IN4, HIGH);
  }
}
void botBackward() {
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, HIGH);
  digitalWrite(PIN_IN4, LOW);
}
void botLeft() {
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, LOW);
  digitalWrite(PIN_IN4, HIGH);
}
void botRight() {
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, HIGH);
  digitalWrite(PIN_IN3, HIGH);
  digitalWrite(PIN_IN4, LOW);
}
void botStop() {
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, LOW);
  digitalWrite(PIN_IN4, LOW);
}

void handleForward() {
  autoDeliveryMode = false;
  if (!emergencyBrakeLocked) {
    robotStateMsg = "MANUAL: DRIVING FORWARD";
    botForward();
  }
  server.send(200, "text/plain", "OK");
}
void handleBackward() {
  autoDeliveryMode = false;
  robotStateMsg = "MANUAL: DRIVING REVERSE";
  botBackward();
  server.send(200, "text/plain", "OK");
}
void handleLeft() {
  autoDeliveryMode = false;
  robotStateMsg = "MANUAL: TURNING LEFT";
  botLeft();
  server.send(200, "text/plain", "OK");
}
void handleRight() {
  autoDeliveryMode = false;
  robotStateMsg = "MANUAL: TURNING RIGHT";
  botRight();
  server.send(200, "text/plain", "OK");
}
void handleStop() {
  autoDeliveryMode = false;
  robotStateMsg = "MANUAL OVERRIDE STOP";
  botStop();
  server.send(200, "text/plain", "OK");
}

void handleToggleAuto() {
  autoDeliveryMode = !autoDeliveryMode;
  if (!autoDeliveryMode) {
    botStop();
    robotStateMsg = "MISSION STOPPED: STANDBY";
  } else {
    emergencyBrakeLocked = false;
    robotStateMsg = "AUTONOMOUS DELIVERY RUN ACTIVE";
  }
  server.send(200, "text/plain", "OK");
}

int getRadarClearance() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long dur = pulseIn(PIN_ECHO, HIGH, 15000);
  int d = dur * 0.034 / 2;
  return (d > 1 && d < 200) ? d : 200;
}

void sendServoPulse(int degrees) {
  int pw = 500 + (degrees * 10.55);
  digitalWrite(PIN_SERVO, HIGH);
  delayMicroseconds(pw);
  digitalWrite(PIN_SERVO, LOW);
}

void setup() {
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);
  botStop();

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_SERVO, OUTPUT);

  // Explicitly binds I2C communication engine to Pins D9 and D10
  Wire.begin(3, 1);
  initOLEDDirect();
  clearOLEDDirect();

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Zippy_Robot", "zippy123");

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/toggle_auto", handleToggleAuto);
  server.on("/forward", handleForward);
  server.on("/backward", handleBackward);
  server.on("/left", handleLeft);
  server.on("/right", handleRight);
  server.on("/stop", handleStop);
  server.begin();
}

void loop() {
  server.handleClient();

  static unsigned long lastUpdate = 0;
  static int state = 0;

  if (millis() - lastUpdate > 300) {
    lastUpdate = millis();

    switch (state) {
      case 0:
        sendServoPulse(20);
        state = 1;
        break;
      case 1:
        leftDistance = getRadarClearance();
        state = 2;
        break;
      case 2:
        sendServoPulse(90);
        state = 3;
        break;
      case 3:
        centerDistance = getRadarClearance();
        state = 4;
        break;
      case 4:
        sendServoPulse(160);
        state = 5;
        break;
      case 5:
        rightDistance = getRadarClearance();
        state = 0;
        break;
    }

    if (state == 0) {
      // Safety limits applied dynamically here (Center <= 45cm OR Sides <= 35cm)
      if ((centerDistance > 0 && centerDistance <= DETECT_CENTER) || (leftDistance > 0 && leftDistance <= DETECT_SIDE) || (rightDistance > 0 && rightDistance <= DETECT_SIDE)) {

        botStop();  // AUTOMATIC EMERGENCY STOP
        emergencyBrakeLocked = true;

        if (autoDeliveryMode) {
          delay(400);
          if (rightDistance >= leftDistance && rightDistance > DETECT_SIDE) {
            robotStateMsg = "AUTONOMOUS: DETOUR RIGHT";
            botRight();
            delay(450);
            botStop();
            emergencyBrakeLocked = false;
          } else if (leftDistance > rightDistance && leftDistance > DETECT_SIDE) {
            robotStateMsg = "AUTONOMOUS: DETOUR LEFT";
            botLeft();
            delay(450);
            botStop();
            emergencyBrakeLocked = false;
          } else {
            robotStateMsg = "AUTONOMOUS: ESCAPE REVERSE";
            botBackward();
            delay(600);
            botStop();
            emergencyBrakeLocked = false;
          }
        } else {
          robotStateMsg = "⚠️ BRAKE LOCK: COLLISION THREAT DETECTED";
        }
      } else {
        emergencyBrakeLocked = false;
        if (autoDeliveryMode) {
          botForward();
          robotStateMsg = "AUTONOMOUS DELIVERY RUN: ACTIVE EN ROUTE TO B";
        }
      }
    }
  }
}
