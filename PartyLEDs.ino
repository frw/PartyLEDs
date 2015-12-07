/* By Frederick Widjaja and Evan Yao */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>

#include <Adafruit_BLE_UART.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_NeoMatrix.h>

#define ADAFRUITBLE_REQ 10
#define ADAFRUITBLE_RST  9
#define ADAFRUITBLE_RDY  2
#define LED_OUT 23
#define MIC_IN A0

#define LED_BRIGHTNESS 25 // Low brightness to limit current draw

#define MATRIX_WIDTH 8
#define MATRIX_HEIGHT 8
#define NUM_PIXELS (MATRIX_WIDTH * MATRIX_HEIGHT)

#define MESSAGE_BUFFER_SIZE 256

#define DISPLAY_RAINBOW       0
#define DISPLAY_RAINBOW_CYCLE 1
#define DISPLAY_MESSAGE       2
#define DISPLAY_SPECTROGRAM   3

// BTLE
Adafruit_BLE_UART BTLEserial = Adafruit_BLE_UART(ADAFRUITBLE_REQ, ADAFRUITBLE_RDY, ADAFRUITBLE_RST);

aci_evt_opcode_t last_status = ACI_EVT_DISCONNECTED;
unsigned int message_size;
char message_buffer[MESSAGE_BUFFER_SIZE + 1];

// NEOPIXEL MATRIX
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(MATRIX_WIDTH, MATRIX_HEIGHT, LED_OUT,
  NEO_MATRIX_TOP     + NEO_MATRIX_RIGHT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_PROGRESSIVE,
  NEO_GRB            + NEO_KHZ800);

// COLORS
const uint16_t OFF = matrix.Color(0, 0, 0);
const uint16_t RED = matrix.Color(255, 0, 0);
const uint16_t YELLOW = matrix.Color(255, 255, 0);
const uint16_t GREEN = matrix.Color(0, 255, 0);

// CURRENT DISPLAY
unsigned int current_display = DISPLAY_RAINBOW;
uint16_t color = GREEN; // Color for mono-color displays

// RAINBOW
int rainbow_state = 0;

// RAINBOW CYCLE
int rainbow_cycle_state = 0;

// MESSAGE
char message_text[MESSAGE_BUFFER_SIZE];
byte message_length;
int message_cursor;

// SPECTROGRAM
AudioInputAnalog         adc(MIC_IN);
AudioAnalyzeFFT1024      fft;
AudioConnection          patchCord(adc, fft);

int band[8][2] = { // Bin intervals of each frequency band
  {2, 3},
  {3, 5},
  {5, 14},
  {14, 32},
  {32, 45},
  {45, 69},
  {69, 91},
  {91, 116}
};
byte peak[8];      // Peak level of each column; used for falling dots
byte dotCount = 0; // Frame counter for delaying dot-falling speed
byte colCount = 0; // Frame counter for storing past column data
int col[8][10];    // Column levels for the prior 10 frames
int minLvlAvg[8];  // For dynamic adjustment of low & high ends of graph,
int maxLvlAvg[8];  // Pseudo rolling averages for the prior few frames.

// Converts a 32-bit int to a 16-bit color
uint16_t itoc (uint32_t color) {
  return matrix.Color((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85) {
    return matrix.Color(255 - pos * 3, 0, pos * 3);
  }
  if (pos < 170) {
    pos -= 85;
    return matrix.Color(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return matrix.Color(pos * 3, 255 - pos * 3, 0);
}

void setup() {
  // Initialize BTLE
  BTLEserial.begin();

  // Initialize NeoPixel Matrix
  matrix.begin();
  matrix.setBrightness(LED_BRIGHTNESS);
  matrix.setTextWrap(false);
  matrix.setTextColor(color);

  // Initialize spectrogram
  memset(peak, 0, sizeof(peak));
  memset(col , 0, sizeof(col));

  for(int i = 0; i < 8; i++) {
    minLvlAvg[i] = 0;
    maxLvlAvg[i] = 512;
  }
  
  // Audio connections require memory to work.
  AudioMemory(12);
}

aci_evt_opcode_t get_bluetooth_status() {
  // Tell the nRF8001 to do whatever it should be working on.
  BTLEserial.pollACI();

  // Ask what is our current status
  aci_evt_opcode_t status = BTLEserial.getState();
  // If the status changed....
  if (status != last_status) {
    // Print it out!
    if (status == ACI_EVT_DEVICE_STARTED) {
        Serial.println(F("* Bluetooth advertising started"));
    }
    if (status == ACI_EVT_CONNECTED) {
        Serial.println(F("* Bluetooth connected!"));
    }
    if (status == ACI_EVT_DISCONNECTED) {
        Serial.println(F("* Bluetooth disconnected or advertising timed out"));
    }
    // Set the last status change to this one
    last_status = status;
  }

  return status;
}

bool poll_bluetooth() {
  // Lets see if there's any data for us!
  unsigned int total_bytes = 0;
  unsigned int bytes_available;
  while (get_bluetooth_status() == ACI_EVT_CONNECTED
      && (bytes_available = BTLEserial.available()) > 0) {
    for (unsigned int i = total_bytes; i < total_bytes + bytes_available; i++) {
      char c = BTLEserial.read();
      if (i < MESSAGE_BUFFER_SIZE) {
        message_buffer[i] = c;
      }
    }
    total_bytes += bytes_available;
  }

  if (total_bytes > 0) {
    // We recieved a message!
    Serial.print("* "); Serial.print(total_bytes); Serial.println(F(" bytes available from BTLE"));
    
    // Terminate string
    int total_length = MESSAGE_BUFFER_SIZE < total_bytes ? MESSAGE_BUFFER_SIZE : total_bytes;
    if (message_buffer[total_length - 1] == '\n') {
      message_buffer[total_length - 1] = '\0';
    } else {
      message_buffer[total_length] = '\0';
    }
    
    Serial.println(message_buffer);

    return true;
  }

  return false;
}

bool change_display() {
  // Check the message to see if it is a recognized instruction
  char *token;
  if ((token = strtok(message_buffer, " "))) {
    if (strcasecmp(token, "color") == 0) {
      // Check if a color was passed in as an argument
      if ((token = strtok(NULL, ""))) {
        // If string starts with '#', then skip the first character
        if (token[0] == '#') {
          token += 1;
        }
        color = itoc(strtol(token, NULL, 16));
        matrix.setTextColor(color);
      }
    }
    if (strcasecmp(token, "rainbow") == 0) {
      current_display = DISPLAY_RAINBOW;
      return true;
    }
    if (strcasecmp(token, "rainbowcycle") == 0) {
      current_display = DISPLAY_RAINBOW_CYCLE;
      return true;
    }
    if (strcasecmp(token, "message") == 0) {
      current_display = DISPLAY_MESSAGE;

      char *text;
      // Check if a message was passed in as an argument
      if ((token = strtok(NULL, ""))) {
        text = token;
      } else {
        text = (char *) "Hello!";
      }
      strcpy(message_text, text);
      message_length = strlen(message_text);
      message_cursor = matrix.width();
      
      return true;
    }
    if (strcasecmp(token, "spectrogram") == 0) {
      current_display = DISPLAY_SPECTROGRAM;
      return true;
    }
  }
  
  return false;
}

void rainbow() {
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      matrix.drawPixel(x, y, wheel((y * MATRIX_WIDTH + x + rainbow_state) & 0xFF));
    }
  }
  matrix.show();

  if (++rainbow_state >= 256) {
    rainbow_state = 0;
  }
  delay(50);
}

// Slightly different, this makes the rainbow equally distributed throughout
void rainbow_cycle() {
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      matrix.drawPixel(x, y, wheel((((y * MATRIX_WIDTH + x) * 256 / NUM_PIXELS) + rainbow_cycle_state) & 0xFF));
    }
  }
  matrix.show();

  if (++rainbow_cycle_state >= 256 * 5) { // 5 cycles of all colors on wheel
    rainbow_cycle_state = 0;
  }
  delay(50);
}

// Prints a scrolling message
void message() {
  matrix.fillScreen(0);
  matrix.setCursor(message_cursor, 0);
  matrix.print(message_text);
  if (--message_cursor < message_length * -6) {
    message_cursor = matrix.width();
  }
  matrix.show();
  
  delay(100);
}

// Performs FFT on audio input to display a spectrogram of different frequency bands
void spectrogram() {
  if (fft.available()) {
    // Fill background with colors, then idle parts of columns will erase
    matrix.fillRect(0, 0, 8, 3, RED);    // Upper section
    matrix.fillRect(0, 3, 8, 2, YELLOW); // Mid
    matrix.fillRect(0, 5, 8, 3, GREEN);  // Lower section
  
    // Downsample spectrum output to 8 columns:
    for(int x = 0; x < 8; x++) {
      int *b = band[x];
      int bandLvl = (int) (fft.read(b[0], b[1]) * 256);
      
      int *history = col[x];
      history[colCount] = bandLvl;
      
      int minLvl, maxLvl;
      minLvl = maxLvl = history[0];
      for(int i = 1; i < 10; i++) { // Get range of prior 10 frames
        int lvl = history[i];
        if(lvl < minLvl) {
          minLvl = lvl;
        } else if(lvl > maxLvl) {
          maxLvl = lvl;
        }
      }
      // minLvl and maxLvl indicate the extents of the FFT output, used
      // for vertically scaling the output graph (so it looks interesting
      // regardless of volume level).  If they're too close together though
      // (e.g. at very low volume levels) the graph becomes super coarse
      // and 'jumpy'...so keep some minimum distance between them (this
      // also lets the graph go to zero when no sound is playing):
      maxLvl = max(maxLvl, minLvl + 8);
      
      int curMinLvlAvg = (minLvlAvg[x] * 7 + minLvl) >> 3; // Dampen min/max levels
      int curMaxLvlAvg = (maxLvlAvg[x] * 7 + maxLvl) >> 3; // (fake rolling average)
      minLvlAvg[x] = curMinLvlAvg;
      maxLvlAvg[x] = curMaxLvlAvg;
  
      // Second fixed-point scale based on dynamic min/max levels:
      int level = 10L * (bandLvl - curMinLvlAvg) / (long) (curMaxLvlAvg - curMinLvlAvg);
  
      // Clip output and convert to byte:
      int c;
      if(level < 0) {
        c = 0;
      } else if(level > 10) {
        c = 10; // Allow dot to go a couple pixels off top
      } else {
        c = level;
      }
  
      if (c > peak[x]) {
        peak[x] = c; // Keep dot on top
      }
  
      if (peak[x] <= 0) { // Empty column
        matrix.drawLine(x, 0, x, 7, OFF);
        continue;
      } else if (c < 8) { // Partial column
        matrix.drawLine(x, 0, x, 7 - c, OFF);
      }
  
      // The 'peak' dot color varies, but doesn't necessarily match
      // the three screen regions...yellow has a little extra influence.
      int y = 8 - peak[x];
      if (y < 2) {
        matrix.drawPixel(x, y, RED);
      } else if (y < 6) {
        matrix.drawPixel(x, y, YELLOW);
      } else {
        matrix.drawPixel(x, y, GREEN);
      }
    }
  
    matrix.show();
  
    // Every fifth frame, make the peak pixels drop by 1:
    if (++dotCount >= 5) {
      dotCount = 0;
      for (int x = 0; x < 8; x++) {
        if (peak[x] > 0) {
          peak[x]--;
        }
      }
    }
  
    if (++colCount >= 10) {
      colCount = 0;
    }
  }
}

void loop() {
  // Check if we received a new BTLE message
  if (poll_bluetooth()) {
    // Change display accordingly
    change_display();
  }

  switch (current_display) {
    case DISPLAY_RAINBOW:
      rainbow();
      break;
    case DISPLAY_RAINBOW_CYCLE:
      rainbow_cycle();
      break;
    case DISPLAY_MESSAGE:
      message();
      break;
    case DISPLAY_SPECTROGRAM:
      spectrogram();
      break;
  }
}

