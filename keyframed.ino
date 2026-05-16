#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <DFRobot_DF1201S.h>
#include <SoftwareSerial.h>

SoftwareSerial DF1201SSerial(2, 3);
DFRobot_DF1201S DF1201S;
Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver();

#define BUTTON_PIN      7
#define DEBOUNCE_MS     20
#define RELEASE_DEBOUNCE 250
#define HOLD_THRESH_MS  500

#define BODY_ROTATION  0
#define BODY_TILT      1
#define HEAD_ROTATION  2
#define JAW            3

#define BODY_ROTATION_MIN     260
#define BODY_ROTATION_MAX     460
#define BODY_ROTATION_DEFAULT 360

#define BODY_TILT_MIN     300
#define BODY_TILT_MAX     420
#define BODY_TILT_DEFAULT 360

#define HEAD_ROTATION_MIN     240
#define HEAD_ROTATION_MAX     460
#define HEAD_ROTATION_DEFAULT 350

#define JAW_MIN     280
#define JAW_MAX     375
#define JAW_DEFAULT 350

#define SERVO_OFFSET  240
#define TOTAL_TRACKS  3

#define BODY_ROTATION_TRIM  25
#define BODY_TILT_TRIM      25
#define HEAD_ROTATION_TRIM  25
#define JAW_TRIM            25

struct Keyframe {
  uint16_t time;
  uint8_t bodyRot;
  uint8_t bodyTilt;
  uint8_t headRot;
  uint8_t jaw;
};

inline Keyframe readKeyframe(const Keyframe* arr, int i) {
  Keyframe kf;
  memcpy_P(&kf, &arr[i], sizeof(Keyframe));
  return kf;
}

#include "track1.h"
#include "track2.h"
#include "track3.h"

bool isPlaying   = false;
bool playAll     = false;
unsigned long trackStartTime = 0;
int currentTrack = 0;
int nextTapTrack = 1;

// Button state
bool lastRawState        = LOW;
bool debouncedState      = LOW;
unsigned long lastDebounceTime = 0;
unsigned long pressStartTime   = 0;
bool holdFired           = false;
bool buttonReady         = false;
unsigned long startupTime = 0;

inline int trimBR(int val) {
  return constrain(val + BODY_ROTATION_TRIM, BODY_ROTATION_MIN, BODY_ROTATION_MAX);
}

inline int trimBT(int val) {
  return constrain(val + BODY_TILT_TRIM, BODY_TILT_MIN, BODY_TILT_MAX);
}

inline int trimHR(int val) {
  return constrain(val + HEAD_ROTATION_TRIM, HEAD_ROTATION_MIN, HEAD_ROTATION_MAX);
}

inline int trimJaw(int val) {
  return constrain(val + JAW_TRIM, JAW_MIN, JAW_MAX);
}

void setup() {
  Serial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  DF1201SSerial.begin(115200);
  while (!DF1201S.begin(DF1201SSerial)) {
    Serial.println(F("Init failed, check wiring!"));
    delay(200);
  }

  Serial.println(F("DFPlayer ready!"));
  DF1201S.setVol(10);
  DF1201S.switchFunction(DF1201S.MUSIC);
  delay(1000);
  DF1201S.setPlayMode(DF1201S.SINGLE);
  DF1201S.setPrompt(false);

  pwm.begin();
  pwm.setPWMFreq(50);
  delay(50);

  for (int i = 0; i < 16; i++) pwm.setPWM(i, 0, 0);
  delay(10);

  pwm.setPWM(BODY_ROTATION, 0, trimBR(BODY_ROTATION_DEFAULT));
  pwm.setPWM(BODY_TILT,     0, trimBT(BODY_TILT_DEFAULT));
  pwm.setPWM(HEAD_ROTATION, 0, trimHR(HEAD_ROTATION_DEFAULT));
  pwm.setPWM(JAW,           0, trimJaw(JAW_MAX));
  delay(200);

  startupTime = millis();
  Serial.println(F("Tap = next track  |  Hold = play all"));
}

void loop() {
  handleButton();
  handleSerial();

  if (isPlaying) {
    unsigned long elapsed = millis() - trackStartTime;

    const Keyframe* frames;
    int len;
    if      (currentTrack == 1) { frames = track1; len = track1Length; }
    else if (currentTrack == 2) { frames = track2; len = track2Length; }
    else                        { frames = track3; len = track3Length; }

    animateKeyframes(frames, len, elapsed);

    if (elapsed >= (unsigned long)readKeyframe(frames, len - 1).time) {
      if (playAll && currentTrack < TOTAL_TRACKS) {
        currentTrack++;
        DF1201S.playFileNum(currentTrack);
        trackStartTime = millis();
        Serial.print(F("Track "));
        Serial.println(currentTrack);
      } else {
        isPlaying = false;
        playAll   = false;
        returnToDefault();
        if (currentTrack < TOTAL_TRACKS) {
          nextTapTrack = currentTrack + 1;
        } else {
          nextTapTrack = 1;
        }
        Serial.print(F("Done. Next tap: track "));
        Serial.println(nextTapTrack);
      }
    }
  }

  delay(20);
}

void handleButton() {
  if (!buttonReady) {
    if (millis() - startupTime < 100) return;
    buttonReady = true;
    Serial.println(F("Button ready"));
    DF1201S.playFileNum(4);
  }

  bool rawState = digitalRead(BUTTON_PIN);

  if (rawState != lastRawState) {
    lastDebounceTime = millis();
  }
  lastRawState = rawState;

  unsigned long debounce = (rawState == LOW) ? RELEASE_DEBOUNCE : DEBOUNCE_MS;
  if ((millis() - lastDebounceTime) < debounce) return;

  if (rawState != debouncedState) {
    debouncedState = rawState;

    if (debouncedState == HIGH) {
      // Only start a fresh press if we weren't already tracking one
      if (!holdFired && pressStartTime == 0) {
        pressStartTime = millis();
        Serial.print(F("Press at: ")); Serial.println(pressStartTime);
      }

    } else {
      // Genuine debounced release
      Serial.print(F("Release at: ")); Serial.println(millis());

      if (!holdFired) {
        onTap();
      }
      holdFired      = false;
      pressStartTime = 0;  // reset only on confirmed release
    }
  }

  // Hold detection — uses original pressStartTime, immune to mid-hold bounces
  if (debouncedState == HIGH && !holdFired && pressStartTime != 0) {
    if (millis() - pressStartTime >= HOLD_THRESH_MS) {
      holdFired = true;
      onHold();
    }
  }
}

void onTap() {
  Serial.print(F("onTap isPlaying="));
  Serial.println(isPlaying);
  if (isPlaying) return;
  Serial.print(F("Playing track "));
  Serial.println(nextTapTrack);
  playAll = false;
  startTrack(nextTapTrack);
}

void onHold() {
  Serial.println(F("Playing all tracks..."));
  playAll = true;
  nextTapTrack = 1;
  startTrack(1);
}

void startTrack(int track) {
  currentTrack = track;
  DF1201S.playFileNum(track);
  isPlaying    = true;
  trackStartTime = millis();
}

void handleSerial() {
  if (!Serial.available()) return;
  char input = Serial.read();
  while (Serial.available()) Serial.read();

  switch (input) {
    case '1': playAll = false; startTrack(1); break;
    case '2': playAll = false; startTrack(2); break;
    case '3': playAll = false; startTrack(3); break;
    case 'p': case 'P': playAll = true; startTrack(1); break;
    case 's': case 'S':
      DF1201S.pause();
      isPlaying = false;
      playAll   = false;
      returnToDefault();
      Serial.println(F("Stopped."));
      break;
    case '+':
      DF1201S.setVol(DF1201S.getVol() + 1);
      Serial.print(F("Vol: ")); Serial.println(DF1201S.getVol());
      break;
    case '-':
      DF1201S.setVol(DF1201S.getVol() - 1);
      Serial.print(F("Vol: ")); Serial.println(DF1201S.getVol());
      break;
    case 'm': case 'M':
      Serial.println(F("Tap=next track  Hold=play all"));
      Serial.println(F("1/2/3=track  p=all  s=stop  +/-=vol"));
      break;
  }
}

void animateKeyframes(const Keyframe* frames, int count, unsigned long elapsed) {
  Keyframe last = readKeyframe(frames, count - 1);
  if (elapsed >= last.time) {
    pwm.setPWM(BODY_ROTATION, 0, trimBR(last.bodyRot   + SERVO_OFFSET));
    pwm.setPWM(BODY_TILT,     0, trimBT(last.bodyTilt  + SERVO_OFFSET));
    pwm.setPWM(HEAD_ROTATION, 0, trimHR(last.headRot   + SERVO_OFFSET));
    pwm.setPWM(JAW,           0, trimJaw(last.jaw      + SERVO_OFFSET));
    return;
  }

  int next = 1;
  Keyframe kfNext = readKeyframe(frames, next);
  while (next < count && kfNext.time <= elapsed) {
    next++;
    kfNext = readKeyframe(frames, next);
  }
  Keyframe kfPrev = readKeyframe(frames, next - 1);

  float t = (float)(elapsed - kfPrev.time) / (float)(kfNext.time - kfPrev.time);
  t = t * t * (3.0f - 2.0f * t);

  int prevBR  = kfPrev.bodyRot  + SERVO_OFFSET;
  int prevBT  = kfPrev.bodyTilt + SERVO_OFFSET;
  int prevHR  = kfPrev.headRot  + SERVO_OFFSET;
  int prevJaw = kfPrev.jaw      + SERVO_OFFSET;
  int nextBR  = kfNext.bodyRot  + SERVO_OFFSET;
  int nextBT  = kfNext.bodyTilt + SERVO_OFFSET;
  int nextHR  = kfNext.headRot  + SERVO_OFFSET;
  int nextJaw = kfNext.jaw      + SERVO_OFFSET;

  pwm.setPWM(BODY_ROTATION, 0, trimBR((int)(prevBR   + t*(nextBR   - prevBR))));
  pwm.setPWM(BODY_TILT,     0, trimBT((int)(prevBT   + t*(nextBT   - prevBT))));
  pwm.setPWM(HEAD_ROTATION, 0, trimHR((int)(prevHR   + t*(nextHR   - prevHR))));
  pwm.setPWM(JAW,           0, trimJaw((int)(prevJaw + t*(nextJaw  - prevJaw))));
}

void returnToDefault() {
  pwm.setPWM(BODY_ROTATION, 0, trimBR(BODY_ROTATION_DEFAULT));
  pwm.setPWM(BODY_TILT,     0, trimBT(BODY_TILT_DEFAULT));
  pwm.setPWM(HEAD_ROTATION, 0, trimHR(HEAD_ROTATION_DEFAULT));
  pwm.setPWM(JAW,           0, trimJaw(JAW_DEFAULT));
}