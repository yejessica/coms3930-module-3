/*
  Module 3: ESP-NOW communication for two ESP32 LILYGO T-Display boards.
  Both boards broadcast their state every 250 milliseconds.
  View received messages in the Serial Monitor at 115200 baud.

  Use the same TEAM_ID on both boards.
  Set PLAYER_ID to 0 on one board and 1 on the other.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <TFT_eSPI.h>

#define TEAM_ID       5      // TODO: Team id = 5 - can be any int just make sure that you and your pair have the same id.
#define PLAYER_ID     0      // TODO: Use 0 on one board and 1 on the other. 0 is x direction, 1 is y direction
#define MAX_PLAYERS   2
#define HEARTBEAT_MS  250
#define BUTTON_A      0      // left button
#define BUTTON_B      35     // right button
#define MOVE_INTERVAL_MS 30 //how often dot moves while button is held
#define STEP 2 //px per move
//maze variables
#define ROWS 16
#define COLS 9
#define CELL 15

static_assert(PLAYER_ID == 0 || PLAYER_ID == 1, "PLAYER_ID must be 0 or 1");

TFT_eSPI tft = TFT_eSPI();

// (0, 0) is bottom-left
// X increases right, Y increases up.
const int DOT_RADIUS = 5;
const int SCREEN_WIDTH = 135;
const int SCREEN_HEIGHT = 240;
bool lastA = HIGH, lastB = HIGH;
const char MAZE[ROWS][COLS + 1]{ //maze array
  "wwwwwwwww",
  "w___w__sw",
  "w_w_w_w_w",
  "w_w___w_w",
  "w_wwwww_w",
  "w_____w_w",
  "wwwww_w_w",
  "w_____w_w",
  "w_wwwww_w",
  "w_w_____w",
  "w_w_www_w",
  "w_w___w_w",
  "w_www_w_w",
  "w___w___w",
  "wwwgw_w_w",
  "wwwwwwwww"
};
int previousDotX, previousDotY;


uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t lastHeartbeat = 0;

volatile uint16_t coords[MAX_PLAYERS] = {SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2};
volatile bool received[MAX_PLAYERS] = {false, false};

void drawDot() {
  tft.fillScreen(TFT_BLACK);

  // this is so that we can handle positive y = up
  int screenY = SCREEN_HEIGHT - 1 - coords[1];
  tft.fillCircle(coords[0], screenY, DOT_RADIUS, TFT_RED);
}

void drawMaze() {
  tft.fillScreen(TFT_BLACK);
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      char ch = MAZE[r][c];
      if (ch == 'w') tft.fillRect(c * CELL, r * CELL, CELL, CELL, TFT_PINK);
      if (ch == 'g') tft.fillRect(c * CELL, r * CELL, CELL, CELL, TFT_GREEN);
    }
  }
}

void findStart() { //set dot coords to center of starting cell
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
    if (MAZE[r][c] == 's') {
      coords[0] = c * CELL + CELL / 2;
      coords[1] = SCREEN_HEIGHT - 1 - (r * CELL + CELL / 2);
    }
}

// message structure between devices
typedef struct {
  uint8_t  teamId;     // which group this message belongs to
  uint8_t  playerId;   // which player in the group sent it
  uint16_t coordinate; // Sender's current X (player 0) or Y (player 1).
} Message;


void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Message)) return;

  Message msg;
  memcpy(&msg, data, sizeof(msg));
  if (msg.teamId != TEAM_ID) return;
  if (msg.playerId >= MAX_PLAYERS) return;
  if (msg.playerId == PLAYER_ID) return;

  if (msg.playerId == 0) {
    coords[0] = constrain((int)msg.coordinate, DOT_RADIUS, SCREEN_WIDTH - DOT_RADIUS - 1);
  } else if (msg.playerId == 1) {
    coords[1] = constrain((int)msg.coordinate, DOT_RADIUS, SCREEN_HEIGHT - DOT_RADIUS - 1);
  }

  received[msg.playerId] = true;
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }

  if (esp_now_register_recv_cb(onReceive) != ESP_OK) {
    Serial.println("ESP-NOW receive callback registration failed");
    while (true) delay(1000);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddress, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW broadcast peer setup failed");
    while (true) delay(1000);
  }
}

void sendState() {
  Message msg = {};
  msg.teamId = TEAM_ID;
  msg.playerId = PLAYER_ID;
  msg.coordinate = coords[PLAYER_ID];

  if (esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(msg)) != ESP_OK) {
    Serial.println("ESP-NOW send failed");
  }
}

bool buttonPressed(uint8_t pin, bool &last) {
  bool now = digitalRead(pin);
  bool pressed = (last == HIGH && now == LOW);
  last = now;
  return pressed;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT);  // GPIO 35 uses the board's external pull-up.
  tft.init();
  tft.setRotation(0);
  setupEspNow();
  Serial.printf("Player %u ready on team %u\n", PLAYER_ID, TEAM_ID);
  findStart();
  previousDotX = coords[0];
  previousDotY = coords[1];
  drawMaze();
  moveDot(coords[0], coords[1], coords[0], coords[1]);
}

void loop() {

  bool decrease = digitalRead(BUTTON_A) == LOW;
  bool increase = digitalRead(BUTTON_B) == LOW;

  static uint32_t lastMove = 0;
  if ((decrease || increase) && millis() - lastMove >= MOVE_INTERVAL_MS) {
    lastMove = millis();

    int c = coords[PLAYER_ID];
    if (increase) c += STEP;
    if (decrease) c -= STEP;

    int axisLimit = PLAYER_ID == 0 ? SCREEN_WIDTH : SCREEN_HEIGHT;
    c = constrain(c, DOT_RADIUS, axisLimit - DOT_RADIUS - 1);

    //block dot from moving through walls
    int testX = PLAYER_ID == 0 ? c: coords[0];
    int testY = PLAYER_ID == 1 ? c : coords[1];
    if (!isWall(testX, testY) && c !=coords[PLAYER_ID]) {
      coords[PLAYER_ID] = c;
      sendState();
    } 
  }

  // redraw when there's a change in coords
  int x = coords[0];
  int y = coords[1];

  if (x != previousDotX || y != previousDotY) {
    moveDot(previousDotX, previousDotY, x, y);
    previousDotX = x;
    previousDotY = y;
  }

  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    sendState();
  }

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (received[i]) {
      received[i] = false;
      Serial.printf("Received from player %d: coordinate %u\n", i, (unsigned int)coords[i]);
    }
  }

}

void moveDot(int oldX, int oldY, int newX, int newY) {
  tft.fillCircle(oldX, SCREEN_HEIGHT - 1 -oldY, DOT_RADIUS, TFT_BLACK);
  tft.fillCircle(newX, SCREEN_HEIGHT - 1 - newY, DOT_RADIUS, TFT_RED);
}


//maze helpers
char cellAt(int x, int y){
  int col = x / CELL;
  int row = (SCREEN_HEIGHT - 1 - y) / CELL;
  if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return 'w'; //if cell is at the edge, draw a wall
  return MAZE[row][col];
}

bool isWall(int x, int y){
  return  cellAt(x - DOT_RADIUS, y - DOT_RADIUS) == 'w' ||
          cellAt(x + DOT_RADIUS, y - DOT_RADIUS) == 'w' ||
          cellAt(x - DOT_RADIUS, y + DOT_RADIUS) == 'w' ||
          cellAt(x + DOT_RADIUS, y + DOT_RADIUS) == 'w';
}




