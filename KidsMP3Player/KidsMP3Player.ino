#include <Arduino.h>
#include <SoftwareSerial.h>
#include <avr/sleep.h>
#include <avr/eeprom.h>
#include "DFMiniMp3.h"

#define EEPROM_CFG 1
#define EEPROM_FOLDER 2
#define EEPROM_TRACK 4

#define BUTTON_TOLERANCE 25
#define LONG_KEY_PRESS_TIME_MS 2000L
#define VOLUME_CHECK_INTERVAL_MS 200L
#define PLAY_DELAY_MS 500L

#define PIN_KEY A3
#define PIN_VOLUME A2
#define PIN_VOLUME_INTERNAL A1

#define NO_FOLDERS 11

#define BUTTON_SLEEP_TIMER                   11
#define BUTTON_TOGGLE_CONTINUOUS_PLAY         1
#define BUTTON_TOGGLE_LOOP_PLAYLIST           2
#define BUTTON_TOGGLE_RESTART_PLAY_ON_START   3

#if( BUTTON_TOGGLE_CONTINUOUS_PLAY || BUTTON_TOGGLE_LOOP_PLAYLIST || BUTTON_TOGGLE_RESTART_PLAY_ON_START )
  #define USE_TOGGLE_FEATURES
#endif

// Sleep timer timeout is this time factor multiplied by button number
#define SLEEP_TIMER_TIME_FACTOR ( 5L * 60L * 1000L)  // 5 minutes
#define SLEEP_TIMER_FADE_OUT_MS ( 3L * 60L * 1000L ) // 3 minutes

#ifdef AVR_UNO
 #define DEBUG
#endif

#ifdef DEBUG
 #define DEBUG_INIT()      Serial.begin(9600)
 #define DEBUG_PRINT(x)    Serial.print(x)
 #define DEBUG_PRINTLN(x)  Serial.println (x)
#else
 #define DEBUG_INIT()
 #define DEBUG_PRINT(x)
 #define DEBUG_PRINTLN(x)
#endif

#define DEBUG_LINE  DEBUG_PRINT("Line: "); \
                    DEBUG_PRINTLN(__LINE__);

void playFolderOrNextInFolder(int folder, boolean loop);
void writeTrackInfo(int16_t folder, int16_t track);

#ifdef AVR_UNO
 SoftwareSerial softSerial(10, 11); // RX, TX
#else
 SoftwareSerial softSerial(0, 1); // RX, TX
#endif

enum Mode {
  MODE_NORMAL, MODE_SET_TIMER
};

#if( BUTTON_TOGGLE_CONTINUOUS_PLAY != 0 )
  boolean continuousPlayWithinPlaylist = false;
#else
  #define continuousPlayWithinPlaylist true
#endif
#if( BUTTON_TOGGLE_LOOP_PLAYLIST != 0 )
  boolean loopPlaylist = false;
#else
  #define loopPlaylist true
#endif
#if( BUTTON_TOGGLE_RESTART_PLAY_ON_START != 0 )
  boolean restartLastTrackOnStart = false;
#else
  #define restartLastTrackOnStart true
#endif

int vol = -1;
int key = -1;
unsigned long keyPressTimeMs = 0L;
unsigned long volumeHandledLastMs = 0L;

#if( BUTTON_SLEEP_TIMER != 0 )
  unsigned long sleepAtMs = 0L;
  unsigned long offAtMs = 0L;
  float volFade = 1.0;
  Mode mode = MODE_NORMAL;
#else
  #define volFade 1
  #define mode MODE_NORMAL
#endif

unsigned long nowMs;

int16_t curFolder = -1;
int16_t curTrack = -1;

int16_t expectedGlobalTrackToFinish = -1;

unsigned long startTrackAtMs = 0L;

int maxTracks[NO_FOLDERS];

class Mp3Notify
{
  public:
    static void OnError(uint16_t errorCode) {
    }

    static void OnPlayFinished(uint16_t globalTrack) {
      if (expectedGlobalTrackToFinish == globalTrack) {
        expectedGlobalTrackToFinish = -1;

        if (continuousPlayWithinPlaylist && startTrackAtMs == 0 /* no user request pending */) {
          playFolderOrNextInFolder(curFolder, loopPlaylist);
        }

        if (restartLastTrackOnStart && (curTrack == -1 || !continuousPlayWithinPlaylist)) {
          writeTrackInfo(-1, -1);
        }
      }
    }

    static void OnCardOnline(uint16_t code) {
    }

    static void OnCardInserted(uint16_t code) {
    }

    static void OnCardRemoved(uint16_t code) {
    }
};

DFMiniMp3<SoftwareSerial, Mp3Notify> player(softSerial);

#if( BUTTON_SLEEP_TIMER != 0 )
void turnOff() {
  volFade = 0.0;
  player.setVolume(0);
  delay(50);
  player.stop();
  delay(50);
  player.setPlaybackSource(DfMp3_PlaySource_Sleep);
  delay(50);
  player.disableDAC();
  delay(50);
  player.sleep();
  delay(200);

  while (1) {
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_mode();
    delay(5000);
  }
}
#endif

void initDFPlayer(boolean reset = false) {
  delay(100);
  player.setEq(DfMp3_Eq_Normal);
  delay(100);
  player.setPlaybackSource(DfMp3_PlaySource_Sd);
  delay(250);
  player.enableDAC();
  delay(250);
}

void readConfig() {
  uint8_t cfg = eeprom_read_byte((uint8_t*) EEPROM_CFG);

#if( BUTTON_TOGGLE_LOOP_PLAYLIST != 0 )
  loopPlaylist = cfg & 1;
#endif
#if( BUTTON_TOGGLE_CONTINUOUS_PLAY != 0 )
  continuousPlayWithinPlaylist = cfg & 2;
#endif
#if( BUTTON_TOGGLE_RESTART_PLAY_ON_START != 0 )
  restartLastTrackOnStart = cfg & 4;
#endif
}

void writeConfig() {
  uint8_t cfg = eeprom_read_byte((uint8_t*) EEPROM_CFG);

#if( BUTTON_TOGGLE_LOOP_PLAYLIST != 0 )
  cfg = (cfg & (0xff ^ 1)) | (loopPlaylist ? 1 : 0);
#endif
#if( BUTTON_TOGGLE_CONTINUOUS_PLAY != 0 )
  cfg = (cfg & (0xff ^ 2)) | (continuousPlayWithinPlaylist ? 2 : 0);
#endif
#if( BUTTON_TOGGLE_RESTART_PLAY_ON_START != 0 )
  cfg = (cfg & (0xff ^ 4)) | (restartLastTrackOnStart ? 4 : 0);
#endif

  eeprom_update_byte((uint8_t*) EEPROM_CFG, cfg);
}

void readTrackInfo() {
  curFolder = (int16_t) eeprom_read_word((uint16_t*) EEPROM_FOLDER);
  if (curFolder < 1 || curFolder > NO_FOLDERS) {
    curFolder = -1;
    curTrack = -1;

    return;
  }

  curTrack = (int16_t) eeprom_read_word((uint16_t*) EEPROM_TRACK);
  if (curTrack < 1 || curTrack > maxTracks[curFolder - 1]) {
    curTrack = -1;
    curFolder = -1;
  }
}

void writeTrackInfo(int16_t folder, int16_t track) {
  eeprom_update_word((uint16_t*) EEPROM_FOLDER, (uint16_t) folder);
  eeprom_update_word((uint16_t*) EEPROM_TRACK, (uint16_t) track);
}

void playOrAdvertise(int fileNo) {
  int state = player.getStatus();
  if ((state & 1) == 1) {
    player.playAdvertisement(fileNo);
  } else {
    player.playMp3FolderTrack(fileNo);
  }
}

void playFolderOrNextInFolder(int folder, boolean loop = true) {
  startTrackAtMs = 0;

  if (curFolder != folder) {
    curFolder = folder;
    curTrack = 1;
  } else {
    if (curTrack == -1) {
      curTrack = 1;
    }
    else if (++curTrack > maxTracks[folder - 1]) {
      if (!loop)
      {
        curTrack = -1;
        return;
      }

      curTrack = 1;
    }
  }

  startTrackAtMs = millis() + PLAY_DELAY_MS;
}

void setup() {
  DEBUG_INIT();
  DEBUG_PRINTLN("Starting setup.");

  pinMode(PIN_VOLUME, INPUT);
  pinMode(PIN_VOLUME_INTERNAL, INPUT);
  pinMode(PIN_KEY, INPUT_PULLUP);

#ifdef USE_TOGGLE_FEATURES
  readConfig();
  delay(50);
#endif

  player.begin();

  initDFPlayer();

  for (int i = 0; i < NO_FOLDERS; ++i) {
    maxTracks[i] = player.getFolderTrackCount(i + 1);
    if (maxTracks[i] == -1) i--;
  }

  if (restartLastTrackOnStart) {
    readTrackInfo();
    if (curFolder != -1 && curTrack != -1) {
      startTrackAtMs = millis() + PLAY_DELAY_MS;
    }
  }

  DEBUG_PRINTLN("Setup done.");
}

#if( BUTTON_SLEEP_TIMER != 0 )
inline void handleSleepTimer() {
  if (sleepAtMs != 0 && nowMs >= sleepAtMs) {
    volFade = 1.0 - (nowMs - sleepAtMs) / (float) SLEEP_TIMER_FADE_OUT_MS;
    if (volFade <= 0.0) {
      turnOff();
    }
  }
}
#else
  #define handleSleepTimer()
#endif

inline void handleVolume() {
  if (nowMs > volumeHandledLastMs + VOLUME_CHECK_INTERVAL_MS) {
    volumeHandledLastMs = nowMs;

    int volCurrent = analogRead(PIN_VOLUME);
    int volInternal = analogRead(PIN_VOLUME_INTERNAL);
    int volNew = (map(volCurrent, 0, 1023, 1,
                      31 - map(volInternal, 1023, 0, 1, 30))) * volFade;
    if (volNew != vol) {
      vol = volNew;
      player.setVolume(vol);

      DEBUG_PRINT("New volume: ");
      DEBUG_PRINTLN(vol);
    }
  }
}

inline void handleKeyPress() {
  int keyCurrent = analogRead(PIN_KEY);

  if (keyCurrent > 958 && key > 0) {
    switch (mode) {
      case MODE_NORMAL:
#ifdef USE_TOGGLE_FEATURES
        if( (nowMs - keyPressTimeMs) >= LONG_KEY_PRESS_TIME_MS ) {
          int advertise = 0;
          switch( key ) {
#if( BUTTON_SLEEP_TIMER != 0 )
            case BUTTON_SLEEP_TIMER:
              mode = MODE_SET_TIMER;
              advertise = 100;
              break;
#endif
#if( BUTTON_TOGGLE_CONTINUOUS_PLAY != 0 )
            case BUTTON_TOGGLE_CONTINUOUS_PLAY:
              continuousPlayWithinPlaylist = !continuousPlayWithinPlaylist;
              advertise = (continuousPlayWithinPlaylist ? 200 : 201);
              writeConfig();
              break;
#endif
#if( BUTTON_TOGGLE_LOOP_PLAYLIST != 0 )
            case BUTTON_TOGGLE_LOOP_PLAYLIST:
              loopPlaylist = !loopPlaylist;
              advertise = (loopPlaylist ? 300 : 301);
              writeConfig();
              break;
#endif
#if( BUTTON_TOGGLE_RESTART_PLAY_ON_START != 0 )
            case BUTTON_TOGGLE_RESTART_PLAY_ON_START:
              restartLastTrackOnStart = !restartLastTrackOnStart;
              advertise = (restartLastTrackOnStart ? 400 : 401);
              writeConfig();
              writeTrackInfo(restartLastTrackOnStart ? curFolder : -1, restartLastTrackOnStart ? curTrack : -1);
              break;
#endif
          }
          if( advertise ) {
            playOrAdvertise( advertise );
            delay(1000);
          }
        }
        else
#endif
        {
          playFolderOrNextInFolder(key);
        }
        break;

#if( BUTTON_SLEEP_TIMER != 0 )
      case MODE_SET_TIMER:
        playOrAdvertise(key);

        if (key == 1) {
          // deactivate sleep timer
          sleepAtMs = 0;
          offAtMs = 0;
          volFade = 1.0;
        } else {
          // set timer to multiple of button number
          sleepAtMs = nowMs + (key - 1) * SLEEP_TIMER_TIME_FACTOR;
          offAtMs = sleepAtMs + SLEEP_TIMER_FADE_OUT_MS;
        }

        delay(1000);
        mode = MODE_NORMAL;
        break;
#endif
    }

    key = -1;
  } else if (keyCurrent <= 958) {
    int keyOld = key;

    if (keyCurrent > 933 - BUTTON_TOLERANCE) {
      key = 1;
    } else if (keyCurrent > 846 - BUTTON_TOLERANCE) {
      key = 4;
    } else if (keyCurrent > 760 - BUTTON_TOLERANCE) {
      key = 7;
    } else if (keyCurrent > 676 - BUTTON_TOLERANCE) {
      key = 10;
    } else if (keyCurrent > 590 - BUTTON_TOLERANCE) {
      key = 8;
    } else if (keyCurrent > 504 - BUTTON_TOLERANCE) {
      key = 5;
    } else if (keyCurrent > 414 - BUTTON_TOLERANCE) {
      key = 2;
    } else if (keyCurrent > 321 - BUTTON_TOLERANCE) {
      key = 3;
    } else if (keyCurrent > 222 - BUTTON_TOLERANCE) {
      key = 6;
    } else if (keyCurrent > 115 - BUTTON_TOLERANCE) {
      key = 9;
    } else if (keyCurrent > 0) {
      key = 11;
    }

    if (keyOld != key) {
      keyPressTimeMs = nowMs;
      DEBUG_PRINT("Key pressed: ");
      DEBUG_PRINT(key);
      DEBUG_PRINT(" (ADC ");
      DEBUG_PRINT(keyCurrent);
      DEBUG_PRINTLN(")");
    }
  }
}

void loop() {
  nowMs = millis();

  handleSleepTimer();
  handleVolume();
  handleKeyPress();

  if (startTrackAtMs != 0 and nowMs >= startTrackAtMs) {
    startTrackAtMs = 0;
    player.playFolderTrack(curFolder, curTrack);
    if (restartLastTrackOnStart) {
      writeTrackInfo(curFolder, curTrack);
    }

    // Don't reduce the following delay. Otherwise player might not have started playing
    // the requested track, returning the file number of the previous file, thus breaking
    // continuous play list playing which relies on correct curTrackFileNumber.
    delay(500);
    expectedGlobalTrackToFinish = player.getCurrentTrack();
  }

  player.loop();

  if (softSerial.overflow()) {
    softSerial.flush();
  }

  delay(50);
}
