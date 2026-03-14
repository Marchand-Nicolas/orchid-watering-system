#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define USE_SERIAL Serial
#define OLED_SDA 4
#define OLED_SCL 15 
#define OLED_RST 16
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define uS_TO_S_FACTOR 1000000
#define TIME_TO_SLEEP  5

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);

const int outputPin = 16;
const int inputPin = 35;
int threshold = 2000;
int waterScore = 0;
int maxScore = 100000;

const int pumpPin = 4;

int outputPinValue = 0; 
int inputPinValue = 0;

int state = 0;

void setup() {
    pinMode(outputPin, OUTPUT);
    pinMode(inputPin, INPUT_PULLDOWN);
    pinMode(pumpPin, OUTPUT);
    digitalWrite(pumpPin, LOW);
    digitalWrite(outputPin, HIGH);
    USE_SERIAL.begin(115200);
}

void loop() {
  inputPinValue = analogRead(inputPin);
  String p1=";";
  USE_SERIAL.println(waterScore + p1 + state + p1 + inputPinValue);
  delay(100);
  switch (state) {
    case 0:
          threshold = 2000;
          if (waterScore > maxScore - 10) {
            digitalWrite(pumpPin, HIGH);
            state = 1;
          }
    break;
    case 1:
          threshold = 3000;
          if (waterScore < maxScore - 20) {
            digitalWrite(pumpPin, HIGH);
            state = 0;
          }
    break;
  }
  if (inputPinValue < threshold) {
    waterScore = min(waterScore + 1, maxScore);
  }
  else {
    waterScore = max(waterScore - 1, -maxScore);
  }
}
