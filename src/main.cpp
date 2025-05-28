#include <Arduino.h>
#include "FS.h"
#include "SD.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "LedControl.h"

//AMPLIFIER
#define I2S_DOUT 25
#define I2S_BCLK 26
#define I2S_LRC 27

//CONTROLS
#define SD_CS 5
#define POT_PIN 32
#define BTN_PREV 12
#define BTN_PLAY 13
#define BTN_NEXT 14
#define BTN_ANIMATION 33
#define POT_BRIGHTNESS_PIN 35

//LCD
#define LCD_SDA_PIN 21
#define LCD_SCL_PIN 22
#define LCD_ADDRESS 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

//LED MATRIX
#define LED_MATRIX_DIN_PIN 16
#define LED_MATRIX_CS_PIN 4
#define LED_MATRIX_CLK_PIN 15
#define NUM_LED_MATRICES 4

AudioOutputI2S *out = nullptr;
AudioGeneratorWAV *wav = nullptr;
AudioFileSourceSD *file = nullptr;
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
LedControl lc = LedControl(LED_MATRIX_DIN_PIN, LED_MATRIX_CLK_PIN, LED_MATRIX_CS_PIN, NUM_LED_MATRICES);

byte solid_block[8] = {0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111};
byte music_note_pattern[8] = {0b00100,0b00110,0b00101,0b00101,0b00100,0b01100,0b01100,0b00000};

#define PROGRESS_CHAR_FILLED (byte)0
#define MUSIC_NOTE_CHAR (byte)1
#define PROGRESS_CHAR_PAUSED_FILLED '='
#define PROGRESS_CHAR_EMPTY '_'
#define PROGRESS_CHAR_PAUSED_EMPTY '.'

const char* tracks[] = {
    "/A.R. Rahman, The Pussycat Dolls - Jai Ho (You Are My Destiny).wav",
    "/Avicii - Wake Me Up (Official Video).wav",
    "/Pitbull_-_Give_Me_Everything_ft._Ne-Yo,_Afrojack,_Nayer.wav",
    "/BABASHA_Marae.wav",
    "/Connect-R feat. Chris Mayer - Still.wav",
    "/Sexy Bitch (feat. Akon).wav",
    "/U 96 - Club Bizarre.wav",
    "/Welcome_to_Los_Santos.wav",
    "/will.i.am - Scream & Shout ft. Britney Spears.wav"
};
const int numTracks = sizeof(tracks) / sizeof(tracks[0]);
volatile int currentTrack = 0;
volatile bool isPlaying = false;
volatile bool isPaused = false;

unsigned long lastButtonPress = 0;
unsigned long lastAnimationButtonPress = 0;
const unsigned long debounceDelay = 200;

TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t ledMatrixTaskHandle = NULL;
SemaphoreHandle_t lcdMutex = NULL;

static int titleScrollOffset = 0;
static unsigned long lastScrollTime = 0;
static unsigned long endScrollPauseTime = 0;
static bool isScrollingPaused = false;
const int scrollSpeedDelay = 350;
const int endScrollPauseDuration = 2000;

enum LedAnimation { VERTICAL_SWEEP, HORIZONTAL_SWEEP, MUSIC, LIFELINE, OFF };
volatile LedAnimation selectedLedAnimation = VERTICAL_SWEEP;
LedAnimation currentLedAnimation = VERTICAL_SWEEP;
int ledAnimationStep = 0;

byte lifeline_data[NUM_LED_MATRICES * 8];
int lifeline_blip_counter = 0;
const byte lifeline_base = 0b00010000;
const byte blip_up1 = 0b00110000;
const byte blip_up2 = 0b01111000;

void updateDisplay();
void playWAV(const char* filename);
void ledMatrixAnimate();
void updateBrightness();

void displayTask(void *pvParameters) {
    Serial.println("Display Task running on Core 0");
    while (1) {
        updateDisplay();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void ledMatrixTask(void *pvParameters) {
    Serial.println("LED Matrix Task running on Core 0");
    for (int i = 0; i < NUM_LED_MATRICES; i++) {
        lc.shutdown(i, false);
        lc.clearDisplay(i);
    }

    while (1) {
        ledMatrixAnimate();
        if (selectedLedAnimation == MUSIC) {
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (selectedLedAnimation == OFF) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    lcdMutex = xSemaphoreCreateMutex();
    Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
    Wire.setClock(100000L);

    if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
        lcd.init();
        lcd.backlight();
        lcd.clear();
        lcd.createChar(PROGRESS_CHAR_FILLED, solid_block);
        lcd.createChar(MUSIC_NOTE_CHAR, music_note_pattern);
        lcd.setCursor(0, 0);
        lcd.print("Audio Player");
        lcd.setCursor(0, 1);
        lcd.print("Initializing...");
        xSemaphoreGive(lcdMutex);
    }
    delay(1000);

    pinMode(BTN_PREV, INPUT_PULLUP);
    pinMode(BTN_PLAY, INPUT_PULLUP);
    pinMode(BTN_NEXT, INPUT_PULLUP);
    pinMode(BTN_ANIMATION, INPUT_PULLUP);
    pinMode(POT_BRIGHTNESS_PIN, INPUT);

    analogReadResolution(12);

    if (!SD.begin(SD_CS, SPI, 4000000)) {
        Serial.println("SD Card Mount Failed");
        if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE) {
            lcd.clear();
            lcd.print("SD Card Error!");
            xSemaphoreGive(lcdMutex);
        }
        while(1);
    }
    Serial.println("SD Card Initialized.");

    out = new AudioOutputI2S();
    out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    out->SetOutputModeMono(true);
    out->SetRate(44100);
    out->SetGain(0.5);
    Serial.println("I2S Output Initialized.");

    xTaskCreatePinnedToCore(displayTask, "DisplayTask", 4096, NULL, 1, &displayTaskHandle, 0);
    xTaskCreatePinnedToCore(ledMatrixTask, "LEDMatrixTask", 2048, NULL, 1, &ledMatrixTaskHandle, 0);

    Serial.println("Setup Complete. Main loop starting on Core 1.");
}

void updateBrightness() {
    static int lastIntensity = -1;
    int potValue = analogRead(POT_BRIGHTNESS_PIN);
    int intensity = map(potValue, 0, 4095, 0, 15);

    if (intensity != lastIntensity) {
        lastIntensity = intensity;
        Serial.printf("Setting Matrix Brightness to: %d\n", intensity);
        for (int i = 0; i < NUM_LED_MATRICES; i++) {
            lc.setIntensity(i, intensity);
        }
    }
}

void playWAV(const char* filename) {
    Serial.printf("Attempting to play: %s\n", filename);
    isPlaying = false;
    if (wav && wav->isRunning()) { wav->stop(); }
    delete wav; wav = nullptr;
    delete file; file = nullptr;
    file = new AudioFileSourceSD(filename);
    if (!file || !file->isOpen()) {
        Serial.printf("Failed to open file: %s\n", filename);
        if (file) delete file; file = nullptr;
        isPaused = false; titleScrollOffset = 0; isScrollingPaused = false;
        return;
    }
    wav = new AudioGeneratorWAV();
    if (wav && wav->begin(file, out)) {
        Serial.printf("Now playing: %s\n", filename);
        isPlaying = true; isPaused = false; titleScrollOffset = 0; isScrollingPaused = false;
    } else {
        Serial.printf("Failed to start WAV generator for: %s\n", filename);
        if(wav) delete wav; wav = nullptr;
        if(file) delete file; file = nullptr;
        isPaused = false; titleScrollOffset = 0; isScrollingPaused = false;
    }
}

void updateVolume() {
    int potValue = analogRead(POT_PIN);
    float volume = (float)potValue / 4095.0;
    if (out) { out->SetGain(volume); }
}

void updateDisplay() {
    char lineBuffer0[LCD_COLS + 1];
    char lineBuffer1[LCD_COLS + 1];
    memset(lineBuffer0, ' ', LCD_COLS); lineBuffer0[LCD_COLS] = '\0';
    memset(lineBuffer1, ' ', LCD_COLS); lineBuffer1[LCD_COLS] = '\0';
    bool tempIsPlaying = isPlaying; bool tempIsPaused = isPaused; int tempCurrentTrack = currentTrack;
    uint32_t currentPos = 0; uint32_t totalSize = 0;
    if ((tempIsPlaying || tempIsPaused) && file && file->isOpen()) {
        currentPos = file->getPos(); totalSize = file->getSize();
    }
    int contentWidthLine0 = LCD_COLS - 1;
    if (tempIsPlaying || tempIsPaused) {
        const char* fullPath = tracks[tempCurrentTrack];
        const char* filenameStart = strrchr(fullPath, '/');
        filenameStart = filenameStart ? filenameStart + 1 : fullPath;
        char tempName[128]; strncpy(tempName, filenameStart, sizeof(tempName) - 1);
        tempName[sizeof(tempName) - 1] = '\0';
        char* extension = strstr(tempName, ".wav");
        if (!extension) extension = strstr(tempName, ".WAV");
        if (extension) *extension = '\0';
        for (int k = 0; tempName[k]; k++) { if (tempName[k] == '_') tempName[k] = ' '; }
        int titleLen = strlen(tempName);
        if (titleLen > contentWidthLine0) {
            if (isScrollingPaused) {
                for (int i = 0; i < contentWidthLine0; ++i) { lineBuffer0[i] = (i < titleLen) ? tempName[i] : ' '; }
                if (millis() - endScrollPauseTime >= endScrollPauseDuration) {
                    isScrollingPaused = false; titleScrollOffset = 0; lastScrollTime = millis();
                }
            } else {
                if (millis() - lastScrollTime >= scrollSpeedDelay) { lastScrollTime = millis(); titleScrollOffset++; }
                if (titleScrollOffset == titleLen) { isScrollingPaused = true; endScrollPauseTime = millis();
                } else if (titleScrollOffset >= titleLen + contentWidthLine0) { titleScrollOffset = 0; }
                for (int i = 0; i < contentWidthLine0; ++i) {
                    int effectiveCharIndex = (titleScrollOffset + i) % (titleLen + contentWidthLine0);
                    lineBuffer0[i] = (effectiveCharIndex < titleLen) ? tempName[effectiveCharIndex] : ' ';
                }
            }
        } else {
            for (int i = 0; i < contentWidthLine0; ++i) { lineBuffer0[i] = (i < titleLen) ? tempName[i] : ' '; }
            titleScrollOffset = 0; isScrollingPaused = false;
        }
    } else {
        const char* readyMsg = "Ready..."; int readyLen = strlen(readyMsg);
        for (int i = 0; i < contentWidthLine0; ++i) { lineBuffer0[i] = (i < readyLen) ? readyMsg[i] : ' '; }
        titleScrollOffset = 0; isScrollingPaused = false;
    }
    lineBuffer0[contentWidthLine0] = MUSIC_NOTE_CHAR;
    if (tempIsPaused) {
        const char* pauseMsg = "---PAUSED---"; int msgLen = strlen(pauseMsg);
        int startCol = (LCD_COLS - msgLen) / 2; if (startCol < 0) startCol = 0;
        for (int i = 0; i < msgLen && (startCol + i) < LCD_COLS; ++i) { lineBuffer1[startCol + i] = pauseMsg[i]; }
    } else if (tempIsPlaying && totalSize > 0) {
        float progress = (float)currentPos / totalSize; int filledWidth = (int)(progress * LCD_COLS);
        for (int i = 0; i < LCD_COLS; i++) { lineBuffer1[i] = (i < filledWidth) ? PROGRESS_CHAR_FILLED : PROGRESS_CHAR_EMPTY; }
    } else {
        for (int i = 0; i < LCD_COLS; i++) { lineBuffer1[i] = PROGRESS_CHAR_EMPTY; }
    }
    if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        lcd.setCursor(0, 0);
        for (int i = 0; i < LCD_COLS; ++i) {
            if (lineBuffer0[i] == MUSIC_NOTE_CHAR && i == (LCD_COLS - 1)) lcd.write(MUSIC_NOTE_CHAR);
            else if (lineBuffer0[i] == PROGRESS_CHAR_FILLED) lcd.write(PROGRESS_CHAR_FILLED);
            else lcd.print(lineBuffer0[i]);
        }
        lcd.setCursor(0, 1);
        for (int i = 0; i < LCD_COLS; ++i) {
            if (lineBuffer1[i] == PROGRESS_CHAR_FILLED) lcd.write(PROGRESS_CHAR_FILLED);
            else lcd.print(lineBuffer1[i]);
        }
        xSemaphoreGive(lcdMutex);
    } else { Serial.println("Display Task couldn't get LCD Mutex for update!"); }
}

void ledMatrixAnimate() {
    for (int i = 0; i < NUM_LED_MATRICES; i++) {
        lc.clearDisplay(i);
    }

    if (currentLedAnimation != selectedLedAnimation) {
        currentLedAnimation = selectedLedAnimation;
        ledAnimationStep = 0;
    }

    if (currentLedAnimation == VERTICAL_SWEEP) {
        int totalColumns = NUM_LED_MATRICES * 8;
        int col_to_light = (totalColumns - 1) - ledAnimationStep;
        int addr_logical = col_to_light / 8;
        int col_in_matrix = col_to_light % 8;
        int addr_physical = (NUM_LED_MATRICES - 1) - addr_logical;
        lc.setColumn(addr_physical, col_in_matrix, 0xFF);
        ledAnimationStep++;
        if (ledAnimationStep >= totalColumns) {
            ledAnimationStep = 0;
        }
    } else if (currentLedAnimation == HORIZONTAL_SWEEP) {
        int totalSteps = NUM_LED_MATRICES * 8;

        int matrix_logical = ledAnimationStep / 8;

        int row_sequence = ledAnimationStep % 8;
        int row_to_light = 7 - row_sequence;

        int matrix_physical = (NUM_LED_MATRICES - 1) - matrix_logical;

        lc.setRow(matrix_physical, row_to_light, 0xFF);

        ledAnimationStep++;
        if (ledAnimationStep >= totalSteps) {
            ledAnimationStep = 0;
        }
    } else if (currentLedAnimation == MUSIC) {
        int totalColumns = NUM_LED_MATRICES * 8;
        for (int col = 0; col < totalColumns; col++) {
            int addr = col / 8;
            int col_in_matrix = col % 8;
            int height = random(0, 9);
            byte col_data = 0;
            for (int h = 0; h < height; h++) {
                col_data |= (1 << h);
            }
            lc.setColumn(addr, col_in_matrix, col_data);
        }
    } else if (currentLedAnimation == LIFELINE) {
        int totalColumns = NUM_LED_MATRICES * 8;
        memmove(&lifeline_data[0], &lifeline_data[1], (totalColumns - 1));

        if (lifeline_blip_counter > 0) {
            if (lifeline_blip_counter == 3) lifeline_data[totalColumns - 1] = blip_up1;
            else if (lifeline_blip_counter == 2) lifeline_data[totalColumns - 1] = blip_up2;
            else if (lifeline_blip_counter == 1) lifeline_data[totalColumns - 1] = blip_up1;
            lifeline_blip_counter--;
        } else {
            lifeline_data[totalColumns - 1] = lifeline_base;
            if (random(0, 10) == 0) {
                lifeline_blip_counter = 3;
            }
        }

        for (int col_logical = 0; col_logical < totalColumns; col_logical++) {
            int addr_logical = col_logical / 8;
            int col_in_matrix = col_logical % 8;
            int addr_physical = (NUM_LED_MATRICES - 1) - addr_logical;
            lc.setColumn(addr_physical, col_in_matrix, lifeline_data[col_logical]);
        }
    } else if (currentLedAnimation == OFF) {
    }
}


void loop() {
    updateVolume();
    updateBrightness();

    if (isPlaying && !isPaused) {
        if (wav && wav->isRunning()) {
            if (!wav->loop()) {
                wav->stop(); isPlaying = false;
                Serial.printf("Track %s finished.\n", tracks[currentTrack]);
            }
        } else if (isPlaying) {
            Serial.println("WAV not running while isPlaying is true. Resetting state.");
            isPlaying = false; isPaused = false;
        }
    }

    if (millis() - lastButtonPress >= debounceDelay) {
        int tempCurrentTrack = currentTrack;
        if (digitalRead(BTN_PREV) == LOW) {
            lastButtonPress = millis(); tempCurrentTrack = (tempCurrentTrack - 1 + numTracks) % numTracks;
            currentTrack = tempCurrentTrack; playWAV(tracks[currentTrack]);
        } else if (digitalRead(BTN_NEXT) == LOW) {
            lastButtonPress = millis(); tempCurrentTrack = (tempCurrentTrack + 1) % numTracks;
            currentTrack = tempCurrentTrack; playWAV(tracks[currentTrack]);
        } else if (digitalRead(BTN_PLAY) == LOW) {
            lastButtonPress = millis();
            if (!isPlaying) { playWAV(tracks[currentTrack]); }
            else { isPaused = !isPaused; Serial.println(isPaused ? "Paused." : "Resumed."); }
        }
    }

    if (millis() - lastAnimationButtonPress >= debounceDelay) {
        if (digitalRead(BTN_ANIMATION) == LOW) {
            lastAnimationButtonPress = millis();
            int currentMode = (int)selectedLedAnimation;
            currentMode = (currentMode + 1) % 5;
            selectedLedAnimation = (LedAnimation)currentMode;

            switch (selectedLedAnimation) {
                case VERTICAL_SWEEP:
                    Serial.println("Animation mode set to: Vertical Sweep");
                    break;
                case HORIZONTAL_SWEEP:
                    Serial.println("Animation mode set to: Horizontal Sweep");
                    break;
                case MUSIC:
                    Serial.println("Animation mode set to: Music");
                    break;
                case OFF:
                    Serial.println("Animation mode set to: Off");
                    break;
                case LIFELINE:
                    Serial.println("Animation mode set to: Lifeline");
                    break;
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}