// Using SDL2

#include <stdio.h>
#include <stdlib.h>

#include "SDL.h"
#include "gba.h"
#include "globals.h"
#include "memory.h"
#include "sound.h"
#include "ui.h"
#if defined(__SWITCH__)
#include <switch.h>
#endif

int showAudioDebug = 1;

int isQuitting = 0;
int autoSaveEnabled = 0;
int frameDrawn = 0;
int audioSent = 0;
uint32_t frameCount = 0;
int emuFPS = 0;
char savFilePath[512];
char osdText[64];
int osdShowCount = 0;

SDL_Texture *texture;
SDL_Renderer *renderer;
SDL_AudioDeviceID audioDevice;
uint32_t lastTime = SDL_GetTicks();
uint8_t lastSaveBuf[LIBRETRO_SAVE_BUF_LEN];
int prevSaveChanged = 0;

#define AUDIO_FIFO_CAP (8192)
volatile int16_t audioFifo[AUDIO_FIFO_CAP];
volatile int audioFifoHead = 0;
volatile int audioFifoLen = 0;
volatile int turboMode = 0;
// "a", "b", "select", "start", "right", "left", "up", "down", "r", "l",
// "turbo", "menu"
volatile int emuKeyState[12][2];
int emuKeyboardMap[12] = {SDLK_x,     SDLK_z,    SDLK_SPACE, SDLK_RETURN,
                          SDLK_RIGHT, SDLK_LEFT, SDLK_UP,    SDLK_DOWN,
                          SDLK_s,     SDLK_a,    SDLK_TAB,   SDLK_ESCAPE};
int emuJoystickMap[12] = {0, 1, 11, 10, 14, 12, 13, 15, 7, 6, 9, 5};
int emuJoystickDeadzone = 10000;

void emuRunAudio() {
  audioSent = 0;
  while (!audioSent) {
    CPULoop();
  }
}

void emuRunFrame() {
  frameDrawn = 0;
  while (!frameDrawn) {
    CPULoop();
  }
}

void emuShowOsd(int cnt, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(osdText, sizeof(osdText), fmt, args);
  va_end(args);
  osdShowCount = cnt;
}

int emuUpdateSaveFile() {
  if (strlen(savFilePath) == 0) {
    return 0;
  }
  FILE *savFile = fopen(savFilePath, "wb");
  if (!savFile) {
    printf("Failed to open save file: %s\n", savFilePath);
    emuShowOsd(10, "Save failed!");
    return -1;
  }
  fwrite(libretro_save_buf, 1, LIBRETRO_SAVE_BUF_LEN, savFile);
  fclose(savFile);
  printf("Saved save file: %s\n", savFilePath);
  emuShowOsd(3, "Auto saved.");
  osdShowCount = 3;
  return 0;
}

void emuCheckSave() {
  if (!autoSaveEnabled) {
    return;
  }
  int changed = memcmp(lastSaveBuf, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN);
  if (changed) {
    memcpy(lastSaveBuf, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN);
  } else {
    if (prevSaveChanged) {
      // Changed in previous check, and not changed now
      emuUpdateSaveFile();
    }
  }
  prevSaveChanged = changed;
}

void emuUpdateFB() {
  SDL_UpdateTexture(texture, NULL, pix, 256 * 2);
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  SDL_RenderPresent(renderer);
}

void systemMessage(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  printf("GBA: %s\n", buf);
}

void systemDrawScreen(void) {
  frameDrawn = 1;
  frameCount++;
  if (frameCount % 120 == 0) {
    uint32_t currentTime = SDL_GetTicks();
    uint32_t delta = currentTime - lastTime;
    if (delta <= 0) {
      delta = 1;
    }
    emuFPS = 120 * 1000 / delta;
    printf("FPS: %d\n", emuFPS);
    lastTime = currentTime;
  }
  if (frameCount % 60 == 0) {
    if (!turboMode) {
      emuCheckSave();
      if (osdShowCount > 0) {
        osdShowCount--;
      }
    }
  }
  if (turboMode) {
    if (frameCount % 20 != 0) {
      return;
    }
  }
  char buf[64];
  if (osdShowCount > 0) {
    // Draw osd
    uiDrawBoxDim(0, 0, 240, 10);
    uiDrawText(0, 0, osdText, COLOR_WHITE);
  } else if (showAudioDebug) {
    uiDrawBoxDim(0, 0, 240, 10);
    snprintf(buf, sizeof(buf), "FPS: %d, fifo: %d", emuFPS, audioFifoLen);
    uiDrawText(0, 0, buf, COLOR_WHITE);
  } else {
    if (turboMode) {
      // Show FPS
      uiDrawBoxDim(0, 0, 240, 10);
      snprintf(buf, sizeof(buf), "FPS: %d", emuFPS);
      uiDrawText(0, 0, buf, COLOR_WHITE);
    }
  }
  emuUpdateFB();
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {
  audioSent = 1;
  if (turboMode) {
    // Do not send audio in turbo mode
    return;
  }
  SDL_LockAudioDevice(audioDevice);
  int wpos = (audioFifoHead + audioFifoLen) % AUDIO_FIFO_CAP;
  if (audioFifoLen + length >= AUDIO_FIFO_CAP) {
    printf("audio fifo overflow: %d\n", audioFifoLen);
    goto bed;
  }
  length = (length / 2) * 2;
  for (int i = 0; i < length; i++) {
    if (audioFifoLen >= AUDIO_FIFO_CAP) {
      break;
    }
    audioFifo[wpos] = finalWave[i];
    wpos = (wpos + 1) % AUDIO_FIFO_CAP;
    audioFifoLen++;
  }
bed:
  SDL_UnlockAudioDevice(audioDevice);
}

void audioCallback(void *userdata, Uint8 *stream, int len) {
  memset(stream, 0, len);
  if (turboMode) {
    return;
  }
  uint16_t *wptr = (uint16_t *)stream;
  int samples = len / 2;
  if (audioFifoLen < samples) {
    printf("audio underrun: %d < %d\n", audioFifoLen, samples);
  } else {
    for (int i = 0; i < samples; i++) {
      int16_t sample = audioFifo[audioFifoHead];
      audioFifoHead = (audioFifoHead + 1) % AUDIO_FIFO_CAP;
      audioFifoLen--;
      *wptr = sample;
      wptr++;
    }
  }
}

int emuLoadROM(const char *path) {
  memset(rom, 0, 32 * 1024 * 1024);
  memset(libretro_save_buf, 0, LIBRETRO_SAVE_BUF_LEN);
  FILE *f = fopen(path, "rb");
  if (!f) {
    printf("Failed to open ROM: %s\n", path);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  int size = ftell(f);
  fseek(f, 0, SEEK_SET);
  int bytesRead = fread(rom, 1, size, f);
  fclose(f);
  printf("Loaded %d bytes\n", bytesRead);

  cpuSaveType = 0;
  flashSize = 0x10000;
  enableRtc = false;
  mirroringEnable = false;
  CPUSetupBuffers();
  CPUInit(NULL, false);
  // gba_init();
  void load_image_preferences(void);
  load_image_preferences();
  CPUReset();
  soundSetSampleRate(47782);
  soundReset();
  rtcEnable(true);
  // Load Save File
  snprintf(savFilePath, sizeof(savFilePath), "%s.4gs", path);
  FILE *savFile = fopen(savFilePath, "rb");
  if (savFile) {
    printf("Loading save file: %s\n", savFilePath);
    fread(libretro_save_buf, 1, LIBRETRO_SAVE_BUF_LEN, savFile);
    fclose(savFile);
  }
  memcpy(lastSaveBuf, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN);
  prevSaveChanged = 0;
  return 0;
}

void emuHandleKey(int key, int down) {
  for (int i = 0; i < 12; i++) {
    if (emuKeyboardMap[i] == key) {
      emuKeyState[i][0] = down;
      return;
    }
  }
}

int main(int argc, char *argv[]) {
  int windowWidth = 240 * 4;
  int windowHeight = 160 * 4;
#ifdef _WIN32
  printf("We are on windows! Using opengl...\n");
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  // Init SDL2
  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK);
#ifdef __SWITCH__
  windowWidth = 1280;
  windowHeight = 720;
  if (appletGetOperationMode() == AppletOperationMode_Console) {
    windowWidth = 1920;
    windowHeight = 1080;
  }
#endif
  // Init video to RGB565
  SDL_Window *window = SDL_CreateWindow(
      "GBA", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, windowWidth,
      windowHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, 240, 160);

  SDL_Joystick *joystick = SDL_JoystickOpen(0);
  if (!joystick) {
    printf("Failed to open joystick\n");
  }
  SDL_JoystickEventState(SDL_ENABLE);
  const char *romPath = uiChooseFileMenu();
  if (romPath == NULL) {
    printf("No ROM selected\n");
    return 0;
  }
  if (emuLoadROM(romPath) != 0) {
    return 0;
  }
  // Init audio
  SDL_AudioSpec desiredSpec = {
      .freq = 48000,
      .format = AUDIO_S16SYS,
      .channels = 2,
      .samples = 1024,
      .callback = audioCallback,
  };
  SDL_AudioSpec obtainedSpec;
  audioDevice = SDL_OpenAudioDevice(NULL, 0, &desiredSpec, &obtainedSpec, 0);
  if (audioDevice == 0) {
    printf("Could not open audio device\n");
    return 0;
  }
  printf("Audio device opened\n");
  // Play audio
  SDL_PauseAudioDevice(audioDevice, 0);
#ifdef __SWITCH__
  appletLockExit();
#endif

  while (1) {
    if (isQuitting) {
      goto bed;
    }
    frameDrawn = 0;
    emuRunFrame();
    // Handle event when frame is drawn
    if (frameDrawn) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
          isQuitting = 1;
          goto bed;
        }
        if (event.type == SDL_KEYDOWN) {
          int keyCode = event.key.keysym.sym;
          emuHandleKey(keyCode, 1);
        }
        if (event.type == SDL_KEYUP) {
          int keyCode = event.key.keysym.sym;
          emuHandleKey(keyCode, 0);
        }
      }
      // Poll joysticks
      if (joystick) {
        // SDL_JoystickUpdate(); // Already called in SDL_PollEvent
        for (int i = 0; i < 12; i++) {
          if (emuJoystickMap[i] == -1) {
            continue;
          }
          int value = SDL_JoystickGetButton(joystick, emuJoystickMap[i]);
          emuKeyState[i][1] = value;
        }
        int xaxis = SDL_JoystickGetAxis(joystick, 0);
        int yaxis = SDL_JoystickGetAxis(joystick, 1);
        emuKeyState[4][1] |= xaxis > emuJoystickDeadzone;
        emuKeyState[5][1] |= xaxis < -emuJoystickDeadzone;
        emuKeyState[6][1] |= yaxis < -emuJoystickDeadzone;
        emuKeyState[7][1] |= yaxis > emuJoystickDeadzone;
      }
      joy = 0;
      for (int i = 0; i < 10; i++) {
        if (emuKeyState[i][0] || emuKeyState[i][1]) {
          joy |= (1 << i);
        }
      }
      turboMode = emuKeyState[10][0] || emuKeyState[10][1];
      UpdateJoypad();
    }
  }
bed:
  emuUpdateSaveFile();
#ifdef __SWITCH__
  appletUnlockExit();
#endif
  return 0;
}
