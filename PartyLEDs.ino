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
#define DISPLAY_SNAKE         4
#define DISPLAY_SINE          5

// BTLE
Adafruit_BLE_UART BTLEserial = Adafruit_BLE_UART(ADAFRUITBLE_REQ, ADAFRUITBLE_RDY, ADAFRUITBLE_RST);

aci_evt_opcode_t last_status = ACI_EVT_DISCONNECTED;
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
const uint16_t CYAN = matrix.Color(0, 255, 255);
const uint16_t BLUE = matrix.Color(0, 0, 255);
const uint16_t MAGENTA = matrix.Color(255, 0, 255);
const uint16_t WHITE = matrix.Color(255, 255, 255);

// CURRENT DISPLAY
unsigned int current_display = DISPLAY_RAINBOW;
uint16_t color = GREEN; // Color for mono-color displays

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

// RAINBOW
byte rainbow_offset = 0;

void rainbow() {
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      matrix.drawPixel(x, y, wheel((y * MATRIX_WIDTH + x + rainbow_offset) & 0xFF));
    }
  }
  matrix.show();

  rainbow_offset++;
  delay(50);
}

// RAINBOW CYCLE
byte rainbow_cycle_offset = 0;

// Slightly different, this makes the rainbow equally distributed throughout
void rainbow_cycle() {
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      matrix.drawPixel(x, y, wheel((((y * MATRIX_WIDTH + x) * 256 / NUM_PIXELS) + rainbow_cycle_offset) & 0xFF));
    }
  }
  matrix.show();

  rainbow_cycle_offset++;
  delay(50);
}

// SINE
byte sine_offset = 0; // counter for current position of sine waves

byte sin8(int x) {
   return sin(2 * 3.14159265 * x / 256) * 128 + 128; 
}

void sine() { 
  // Draw one frame of the animation into the LED array
  for (byte x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {

      // Calculate "sine" waves with varying periods
      // sin8 is used for speed; cos8, quadwave8, or triwave8 would also work here
      byte r = abs(y * 256 / MATRIX_HEIGHT - sin8(sine_offset * 9 + x * 16));
      byte g = abs(y * 256 / MATRIX_HEIGHT - sin8(sine_offset * 10 + x * 16));
      byte b = abs(y * 256 / MATRIX_HEIGHT - sin8(sine_offset * 11 + x * 16));
  
      matrix.drawPixel(x, y, matrix.Color(255 - r, 255 - g, 255 - b));
    }
  }
  matrix.show();
  
  sine_offset++;
  delay(20);
}

// MESSAGE
char message_text[MESSAGE_BUFFER_SIZE];
byte message_length;
int message_cursor;

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

// SPECTROGRAM
AudioInputAnalog         adc(MIC_IN);
AudioAnalyzeFFT1024      fft;
AudioConnection          patchCord(adc, fft);

int band[8][2] = { // Bin intervals of each frequency band
  {   2,   3 },
  {   3,   5 },
  {   5,  14 },
  {  14,  32 },
  {  32,  45 },
  {  45,  69 },
  {  69,  91 },
  {  91, 116 }
};
byte peak[8];      // Peak level of each column; used for falling dots
byte dotCount = 0; // Frame counter for delaying dot-falling speed
byte colCount = 0; // Frame counter for storing past column data
int col[8][10];    // Column levels for the prior 10 frames
int minLvlAvg[8];  // For dynamic adjustment of low & high ends of graph,
int maxLvlAvg[8];  // Pseudo rolling averages for the prior few frames.

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

// SNAKE
unsigned int snake_length;
unsigned int snake_pos[NUM_PIXELS][2];
byte snake_dx;
byte snake_dy;
unsigned int snake_food_x;
unsigned int snake_food_y;
int snake_dying_count;

void snake_move() {
  // Check if is press button controller message
  if (message_buffer[0] == '!' && message_buffer[1] == 'B') {
    switch (message_buffer[2]) {
      case '5': // Up
        if (snake_dy == 0) {
          snake_dx = 0;
          snake_dy = -1;
        }
        break;
      case '6': // Down
        if (snake_dy == 0) {
          snake_dx = 0;
          snake_dy = 1;
        }
        break;
      case '7': // Left
        if (snake_dx == 0) {
          snake_dx = -1;
          snake_dy = 0;
        }
        break;
      case '8': // Right
        if (snake_dx == 0) {
          snake_dx = 1;
          snake_dy = 0;
        }
        break;
    }
  }
}

void snake_spawn_food() {
  int empty_spaces = NUM_PIXELS - snake_length;
  int next_food = rand() % empty_spaces;
  
  int count = 0;
  for (unsigned int x = 0; x < MATRIX_WIDTH; x++) {
    for (unsigned int y = 0; y < MATRIX_HEIGHT; y++) {
      for (unsigned int i = 0; i < snake_length; i++) {
        unsigned int *part = snake_pos[i];
        if (part[0] == x && part[1] == y) {
          // Skip to next cell as space is occupied by snake
          goto next;
        }
      }
      
      if (count == next_food) {
        snake_food_x = x;
        snake_food_y = y;
        return;
      } else {
        count++;
      }
      
      next: ;
    }
  }
}

void snake_reset() {
  snake_length = 5;
  for (int i = 0; i < 5; i++) {
    unsigned int *part = snake_pos[i];
    part[0] = 4 - i;
    part[1] = MATRIX_HEIGHT  / 2;
  }
  snake_dx = 1;
  snake_dy = 0;
  snake_spawn_food();
  snake_dying_count = 0;
}

void snake_draw() {
  for (unsigned int i = 0; i < snake_length; i++) {
      unsigned int *part = snake_pos[i];
      matrix.drawPixel(part[0], part[1], wheel(i * 256 / snake_length));
  }
}

void snake() {
  matrix.fillScreen(0);
  
  unsigned int *head = snake_pos[0];
  unsigned int next_x = (head[0] + snake_dx) % MATRIX_WIDTH;
  unsigned int next_y = (head[1] + snake_dy) % MATRIX_HEIGHT;

  if (snake_dying_count > 0) {
    // Draw snake at odd frames only to produce flashing effect
    if ((snake_dying_count & 1) == 1) {
      snake_draw();
    }

    snake_dying_count++;
    if (snake_dying_count == 7) {
      snake_reset();
    }
  } else {
    // Check if snake collided with its own body
    for (unsigned int i = 0; i < snake_length - 1; i++) {
      unsigned int *part = snake_pos[i];
      // Collision detected
      if (part[0] == next_x && part[1] == next_y) {
        snake_dying_count++;
        return;
      }
    }

    // If snake ate the food, increase snake length
    if (next_x == snake_food_x && next_y == snake_food_y) {
      snake_length++;
      snake_spawn_food();
    }

    // Move snake in the right direction
    for (unsigned int i = 0; i < snake_length; i++) {
      unsigned int *part = snake_pos[i];
      unsigned int temp_x = part[0];
      unsigned int temp_y = part[1];
      part[0] = next_x;
      part[1] = next_y;
      next_x = temp_x;
      next_y = temp_y;
    }

    matrix.drawPixel(snake_food_x, snake_food_y, WHITE);
    snake_draw();
  }
  matrix.show();

  delay(300);
}

void setup() {
  // Initialize RNG
  srand(millis());
  
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
    if (strcasecmp(token, "snake") == 0) {
      current_display = DISPLAY_SNAKE;
      snake_reset();
      return true;
    }

    if (strcasecmp(token, "sine") == 0) { 
      current_display = DISPLAY_SINE; 
      return true; 
    }
  }
  
  return false;
}

void loop() {
  // Check if we received a new BTLE message
  if (poll_bluetooth() && !change_display()) {
    switch (current_display) {
      case DISPLAY_SNAKE:
        snake_move();
        break;
    }
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
    case DISPLAY_SNAKE:
      snake();
      break;
    case DISPLAY_SINE: 
      sine(); 
      break;
  }
}

