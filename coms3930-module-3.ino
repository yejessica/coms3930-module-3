/*
  Module 3: ESP-NOW communication for two ESP32 LILYGO T-Display boards.
  Both boards must be powered on to play together, since player 0 manages the game and player 1 sends buttons and displays updates.
  View communication errors in the Serial Monitor at 115200 baud.

  Use the same TEAM_ID on both boards.
  Set PLAYER_ID to 0 on one board and 1 on the other.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <TFT_eSPI.h>

#define TEAM_ID       5      // TODO: Team id = 5 - can be any int just make sure that you and your pair have the same id.
#define PLAYER_ID     0      // TODO: Use 0 on one board and 1 on the other. 0 is x direction, 1 is y direction
#define SEND_INTERVAL_MS 30
#define BUTTON_A      0      // left button
#define BUTTON_B      35     // right button
#define MOVE_INTERVAL_MS 30 //how often dot moves while button is held
#define STEP 2 //px per move
const uint32_t TIMER_DURATION_MS = 60000;
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

const int COIN_COUNT = 10;
const int COIN_RADIUS = 3;
const uint32_t BUTTON_DEBOUNCE_MS = 20;
const uint32_t INPUT_TIMEOUT_MS = 500;

enum Screen : uint8_t { INSTRUCTIONS, PLAYING, GAME_OVER };

// this is only changed by player 0 - player 1 displays the received copy.
struct GameState {
  uint8_t screen;
  uint8_t secondsLeft;
  uint16_t x, y;
  uint8_t coinCells[COIN_COUNT];
  uint16_t collectedCoins;
  uint8_t score;
};

struct InputMessage {
  uint8_t teamId, playerId;
  uint8_t buttons;             // Bit 0: left button, bit 1: right button.
  uint8_t actionScreen;        // Screen on which the button was pressed.
  uint32_t pressNumber;        // Repeat a press until delivered, handle it once.
};

struct StateMessage {
  uint8_t teamId, playerId;
  GameState game;
};

GameState game = {}, lastDrawn = {};
InputMessage localInput = {}, remoteInput = {}, inputInbox = {};
StateMessage stateInbox = {};
portMUX_TYPE inboxMux = portMUX_INITIALIZER_UNLOCKED;
bool messageReceived = false;
bool screenDrawn = false;
uint8_t rawButtons = 0;
uint32_t buttonsChangedAt = 0;
uint32_t lastRemoteInput = 0;
uint32_t lastHandledPress = 0;
uint32_t timerStartedAt = 0;
uint32_t lastMove = 0, lastSend = 0;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // saving messages
  if (PLAYER_ID == 0 && len == sizeof(InputMessage)) {
    InputMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.teamId != TEAM_ID || msg.playerId != 1) return;
    portENTER_CRITICAL(&inboxMux);
    inputInbox = msg;
    messageReceived = true;
    portEXIT_CRITICAL(&inboxMux);
  } else if (PLAYER_ID == 1 && len == sizeof(StateMessage)) {
    StateMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.teamId != TEAM_ID || msg.playerId != 0) return;
    portENTER_CRITICAL(&inboxMux);
    stateInbox = msg;
    messageReceived = true;
    portEXIT_CRITICAL(&inboxMux);
  }
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


void sendUpdate() {
  int result;
  if (PLAYER_ID == 0) {
    StateMessage msg = {};
    msg.teamId = TEAM_ID;
    msg.playerId = PLAYER_ID;
    msg.game = game;
    result = esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(msg));
  } else {
    result = esp_now_send(broadcastAddress, (uint8_t *)&localInput, sizeof(localInput));
  }
  if (result != ESP_OK) Serial.println("ESP-NOW send failed");
  lastSend = millis();
}

void receiveUpdate() {
  portENTER_CRITICAL(&inboxMux);
  bool received = messageReceived;
  if (received) {
    if (PLAYER_ID == 0) {
      remoteInput = inputInbox;
    }
    else game = stateInbox.game;
    messageReceived = false;
  }
  portEXIT_CRITICAL(&inboxMux);
  if (received && PLAYER_ID == 0) lastRemoteInput = millis();
}

void startGame() {
  game = {};
  game.screen = PLAYING;
  game.secondsLeft = 60;
  // choosing coin locations randomly from empty cells
  uint8_t emptyCells[ROWS * COLS];
  int count = 0;
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      if (MAZE[r][c] == '_') emptyCells[count++] = r * COLS + c;
      if (MAZE[r][c] == 's') {
        game.x = c * CELL + CELL / 2;
        game.y = SCREEN_HEIGHT - 1 - (r * CELL + CELL / 2);
      }
    }
  }
  for (int i = 0; i < COIN_COUNT; i++) {
    game.coinCells[i] = 255;
    if (i >= count) continue;
    int chosen = i + esp_random() % (count - i);
    game.coinCells[i] = emptyCells[chosen];
    emptyCells[chosen] = emptyCells[i];
  }
  timerStartedAt = lastMove = millis();
}

void handlePress(uint8_t pressedScreen) {
  // handling which screen will be next, or the game
  if (pressedScreen != game.screen) return;
  if (game.screen == INSTRUCTIONS) startGame();
  else if (game.screen == GAME_OVER) game.screen = INSTRUCTIONS;
}

void readButtons() {
  uint8_t buttons = (digitalRead(BUTTON_A) == LOW ? 1 : 0) | (digitalRead(BUTTON_B) == LOW ? 2 : 0);
  if (buttons != rawButtons) {
    rawButtons = buttons;
    buttonsChangedAt = millis();
  }
  if (millis() - buttonsChangedAt < BUTTON_DEBOUNCE_MS) return;
  uint8_t pressed = buttons & ~localInput.buttons;
  localInput.buttons = buttons;
  if (pressed) {
    localInput.actionScreen = game.screen;
    localInput.pressNumber++;
    if (PLAYER_ID == 0) handlePress(game.screen);
    else sendUpdate();
  }
}

void movePlayer(uint8_t buttons, int axis) {
  int direction = ((buttons & 2) ? 1 : 0) - ((buttons & 1) ? 1 : 0);
  if (axis == 1) direction = -direction; // Left button = up, right = down.
  int x = game.x + (axis == 0 ? direction * STEP : 0);
  int y = game.y + (axis == 1 ? direction * STEP : 0);
  if (!isWall(x, y)) {
    game.x = x;
    game.y = y;
  }
}

void updateGame() {
  uint32_t elapsed = millis() - timerStartedAt;
  if (game.screen == PLAYING && elapsed >= TIMER_DURATION_MS) {
    game.secondsLeft = 0;
    game.screen = GAME_OVER;
  }
  if (remoteInput.pressNumber != lastHandledPress) {
    lastHandledPress = remoteInput.pressNumber;
    handlePress(remoteInput.actionScreen);
  }
  if (game.screen != PLAYING) return;
  elapsed = millis() - timerStartedAt;
  game.secondsLeft = (TIMER_DURATION_MS - elapsed + 999) / 1000;
  if (millis() - lastMove >= MOVE_INTERVAL_MS) {
    lastMove = millis();
    movePlayer(localInput.buttons, 0);
    if (millis() - lastRemoteInput < INPUT_TIMEOUT_MS) movePlayer(remoteInput.buttons, 1);
  }
  for (int i = 0; i < COIN_COUNT; i++) {
    if (game.coinCells[i] == 255 || (game.collectedCoins & (1U << i))) continue;
    int coinX = (game.coinCells[i] % COLS) * CELL + CELL / 2;
    int coinY = SCREEN_HEIGHT - 1 - ((game.coinCells[i] / COLS) * CELL + CELL / 2);
    int dx = game.x - coinX, dy = game.y - coinY;
    if (dx * dx + dy * dy <= (DOT_RADIUS + COIN_RADIUS) * (DOT_RADIUS + COIN_RADIUS)) {
      game.collectedCoins |= 1U << i;
      game.score++;
    }
  }
}

void drawInstructions() {
  tft.fillScreen(TFT_PINK);
  tft.setTextColor(TFT_BLACK, TFT_PINK);
  tft.setTextSize(1);
  tft.drawCentreString("Collect as many coins", SCREEN_WIDTH / 2, 35, 1);
  tft.drawCentreString("in 60 seconds.", SCREEN_WIDTH / 2, 49, 1);
  tft.drawCentreString("You control the", SCREEN_WIDTH / 2, 97, 1);
  tft.drawCentreString(PLAYER_ID == 0 ? "left/right controls." : "up/down controls.",
                       SCREEN_WIDTH / 2, 111, 1);
  tft.drawCentreString("Press either button", SCREEN_WIDTH / 2, 187, 1);
  tft.drawCentreString("to start", SCREEN_WIDTH / 2, 201, 1);
}

void drawEndScreen() {
  tft.fillScreen(TFT_PINK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_RED);
  tft.drawCentreString("Time's up!", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 40, 1);
  tft.drawCentreString("Time's up!", SCREEN_WIDTH / 2 + 1, SCREEN_HEIGHT / 2 - 40, 1);
  char label[24];
  snprintf(label, sizeof(label), "Score: %d", game.score);
  tft.setTextColor(TFT_BLACK);
  tft.drawCentreString(label, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 8, 1);
  tft.setTextSize(1);
  tft.drawCentreString("Press either button", SCREEN_WIDTH / 2, 187, 1);
  tft.drawCentreString("for instructions", SCREEN_WIDTH / 2, 201, 1);
}

void drawCell(int row, int col) {
  char cell = MAZE[row][col];
  uint16_t color = cell == 'w' ? TFT_PINK : (cell == 'g' ? TFT_GREEN : TFT_BLACK);
  tft.fillRect(col * CELL, row * CELL, CELL, CELL, color);
}

void drawGame() {
  bool newScreen = !screenDrawn || game.screen != lastDrawn.screen || (game.screen == PLAYING && memcmp(game.coinCells, lastDrawn.coinCells, sizeof(game.coinCells)) != 0);
  if (game.screen == INSTRUCTIONS) {
    if (newScreen) drawInstructions();
  } else if (game.screen == GAME_OVER) {
    if (newScreen || game.score != lastDrawn.score) drawEndScreen();
  } else {
    if (newScreen) {
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) drawCell(r, c);
    }
    if (newScreen || game.x != lastDrawn.x || game.y != lastDrawn.y ||
        game.collectedCoins != lastDrawn.collectedCoins) {
      if (!newScreen) {
        int screenY = SCREEN_HEIGHT - 1 - lastDrawn.y;
        for (int r = (screenY - DOT_RADIUS) / CELL; r <= (screenY + DOT_RADIUS) / CELL; r++)
          for (int c = (lastDrawn.x - DOT_RADIUS) / CELL; c <= (lastDrawn.x + DOT_RADIUS) / CELL; c++)
            if (r >= 0 && r < ROWS && c >= 0 && c < COLS) drawCell(r, c);
        for (int i = 0; i < COIN_COUNT; i++) {
          if (game.coinCells[i] != 255 && (game.collectedCoins & ~lastDrawn.collectedCoins & (1U << i)))
            drawCell(game.coinCells[i] / COLS, game.coinCells[i] % COLS);
        }
      }
      for (int i = 0; i < COIN_COUNT; i++) {
        if (game.coinCells[i] == 255 || (game.collectedCoins & (1U << i))) continue;
        tft.fillCircle((game.coinCells[i] % COLS) * CELL + CELL / 2,
                       (game.coinCells[i] / COLS) * CELL + CELL / 2, COIN_RADIUS, TFT_YELLOW);
      }
      tft.fillCircle(game.x, SCREEN_HEIGHT - 1 - game.y, DOT_RADIUS, TFT_RED);
    }
    char label[24];
    tft.setTextColor(TFT_BLACK, TFT_PINK);
    if (newScreen || game.score != lastDrawn.score) {
      tft.fillRect(0, 0, SCREEN_WIDTH, CELL, TFT_PINK);
      tft.setTextSize(1);
      snprintf(label, sizeof(label), "Score: %d", game.score);
      tft.drawString(label, 2, 3, 1);
    }
    if (newScreen || game.secondsLeft != lastDrawn.secondsLeft) {
      tft.fillRect(0, SCREEN_HEIGHT - 16, 28, 16, TFT_PINK);
      tft.setTextSize(2);
      snprintf(label, sizeof(label), "%u", game.secondsLeft);
      tft.drawString(label, 2, SCREEN_HEIGHT - 16, 1);
    }
  }
  lastDrawn = game;
  screenDrawn = true;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT);
  tft.init();
  tft.setRotation(0);
  localInput.teamId = TEAM_ID;
  localInput.playerId = PLAYER_ID;
  game.screen = INSTRUCTIONS;
  game.secondsLeft = 60;
  setupEspNow();
  drawGame();
}

void loop() {
  receiveUpdate();
  readButtons();
  if (PLAYER_ID == 0) updateGame();
  drawGame();
  if (millis() - lastSend >= SEND_INTERVAL_MS) sendUpdate();
  delay(1);
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
