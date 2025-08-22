/*
 * eSpeak Complete Solution - arduino-espeak-ng + M5Avatar + Advanced Features
 * 
 * 統合機能:
 * - バッファ分離方式による安定動作
 * - リアルタイムリップシンク
 * - シリアルコマンド制御
 * - メモリ使用量監視
 * - 音声パラメータ調整
 * - M5.Display競合回避
 */

#include <M5Unified.h>
#include <Avatar.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>

// Fix macro conflicts BEFORE other includes
#ifdef B110
#undef B110
#endif
#ifdef B1000000
#undef B1000000
#endif

#include "AudioTools.h"
#include "FileSystems.h"

// CRITICAL: Enable Schatzmann's Stack Hack
#define ESPEAK_STACK_HACK 1
#include "espeak.h"
#include "espeak-ng-data.h"

// ===== Configuration =====
#define AUDIO_SAMPLE_RATE 22050
#define MAX_AUDIO_BUFFER_SIZE 160000  // 約7.5秒分のオーディオ（さらに増量）
#define MAX_TEXT_LENGTH 300
#define SERIAL_BUFFER_SIZE 350
#define SPEECH_TIMEOUT_MS 15000

// ===== Debug Logging =====
#define LOG_I(tag, format, ...) Serial.printf("[I][%s] " format "\n", tag, ##__VA_ARGS__)
#define LOG_E(tag, format, ...) Serial.printf("[E][%s] " format "\n", tag, ##__VA_ARGS__)
#define LOG_W(tag, format, ...) Serial.printf("[W][%s] " format "\n", tag, ##__VA_ARGS__)

// ===== Global Variables =====
static bool g_systemReady = false;
static bool g_isSpeaking = false;
static volatile int g_currentLevel = 0;
static uint8_t g_volume = 50;
static bool g_displayEnabled = false; // Display制御フラグ

// Audio buffer (PSRAMに配置)
static int16_t* g_audioBuffer = nullptr;
static size_t g_audioBufferPos = 0;
static size_t g_playbackPos = 0;

// Serial input buffer
static char g_serialBuffer[SERIAL_BUFFER_SIZE];
static int g_serialPos = 0;

// eSpeak parameters
static int g_rate = 150;
static int g_pitch = 70;
static int g_volume_internal = 100;
static int g_pitchRange = 100;

// M5 avatar
using namespace m5avatar;
Avatar avatar;

// ===== Memory Buffer Stream =====
class MemoryBufferStream : public AudioStream {
public:
    size_t readBytes(uint8_t *data, size_t len) override { 
        return 0; 
    }
    
    size_t write(const uint8_t *data, size_t len) override {
        if (!g_systemReady) {
            return 0;
        }
        
        size_t samples = len / sizeof(int16_t);
        const int16_t* audioData = (const int16_t*)data;
        
        size_t samplesWritten = 0;
        for (size_t i = 0; i < samples && g_audioBufferPos < MAX_AUDIO_BUFFER_SIZE && g_audioBuffer; i++) {
            g_audioBuffer[g_audioBufferPos++] = audioData[i];
            samplesWritten++;
        }
        
        // バッファ満杯の警告
        if (g_audioBufferPos >= MAX_AUDIO_BUFFER_SIZE && samplesWritten < samples) {
            LOG_W("BUFFER", "Audio buffer full! Truncated %d samples", samples - samplesWritten);
        }
        
        return samplesWritten * sizeof(int16_t);
    }
    
    bool begin() { 
        LOG_I("STREAM", "MemoryBufferStream begin");
        g_audioBufferPos = 0;
        return true; 
    }
    
    void end() {
        LOG_I("STREAM", "MemoryBufferStream end - buffer ready with %d samples", g_audioBufferPos);
    }
    
    int available() override { 
        return g_systemReady ? 1024 : 0; 
    }
    
    AudioInfo audioInfo() override {
        AudioInfo info;
        info.sample_rate = AUDIO_SAMPLE_RATE;
        info.channels = 1;
        info.bits_per_sample = 16;
        return info;
    }
};

// ===== Global Objects =====
MemoryBufferStream memoryStream;
ESpeak espeak(memoryStream);

// ===== Level Calculation =====
void updateLevel(const int16_t* samples, size_t count) {
    if (count == 0) {
        g_currentLevel = 0;
        return;
    }
    
    long sum = 0;
    size_t checkSamples = min(count, (size_t)10);
    
    for (size_t i = 0; i < checkSamples; i++) {
        sum += abs(samples[i]);
    }
    
    int avgLevel = sum / checkSamples;
    g_currentLevel = constrain((avgLevel * 100) / 32767, 0, 100);
}

// ===== Memory Monitor =====
namespace MemoryMonitor {
    static void printStatus() {
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t totalSRAM = 520 * 1024; // AtomS3R SRAM capacity
        uint32_t usedSRAM = totalSRAM - freeHeap;
        uint32_t freePsram = ESP.getFreePsram();
        uint32_t totalPsram = ESP.getPsramSize();
        uint32_t usedPsram = totalPsram - freePsram;
        
        Serial.printf("\n[MEMORY] Status Report:\n");
        Serial.printf("  SRAM - Free: %.1f KB, Used: %.1f KB\n", 
                     freeHeap / 1024.0f, usedSRAM / 1024.0f);
        Serial.printf("  PSRAM - Free: %.1f KB, Used: %.1f KB\n", 
                     freePsram / 1024.0f, usedPsram / 1024.0f);
        Serial.printf("  Audio Buffer: %.1f KB (in PSRAM)\n", 
                     (MAX_AUDIO_BUFFER_SIZE * sizeof(int16_t)) / 1024.0f);
        
        UBaseType_t stackRemaining = uxTaskGetStackHighWaterMark(NULL);
        Serial.printf("  Stack remaining: %.1f KB\n", stackRemaining * 4 / 1024.0f);
        
        if (stackRemaining < 1024) {
            Serial.println("  [WARNING] Stack usage high");
        } else {
            Serial.println("  [OK] Memory usage within safe limits");
        }
        Serial.println("=============================\n");
    }
}

// ===== Speech Function =====
bool speak(const char* text) {
    if (g_isSpeaking || !g_systemReady) {
        LOG_W("SPEAK", "Cannot speak: speaking=%d, ready=%d", g_isSpeaking, g_systemReady);
        return false;
    }
    
    if (!g_audioBuffer) {
        LOG_E("SPEAK", "Audio buffer not allocated");
        return false;
    }
    
    size_t len = strlen(text);
    if (len > MAX_TEXT_LENGTH) {
        LOG_E("SPEAK", "Text too long: %d chars (max %d)", len, MAX_TEXT_LENGTH);
        return false;
    }
    
    if (len == 0) {
        LOG_E("SPEAK", "Empty text provided");
        return false;
    }
    
    LOG_I("SPEAK", "Starting speech synthesis: '%s' (length: %d)", text, len);
    g_isSpeaking = true;
    g_currentLevel = 0;
    
    // Step 1: Clear buffer
    g_audioBufferPos = 0;
    g_playbackPos = 0;
    
    // Step 2: Synthesize to memory buffer with extended timeout
    LOG_I("SPEAK", "Synthesizing to memory buffer...");
    esp_task_wdt_reset();
    
    // eSpeakの内部タイムアウトを避けるため、少し待機
    delay(10);
    
    bool synthSuccess = espeak.say(text);
    
    // 合成完了まで少し待機（長文の場合）
    delay(50);
    esp_task_wdt_reset();
    
    if (!synthSuccess) {
        LOG_E("SPEAK", "eSpeak.say() returned false");
        g_isSpeaking = false;
        return false;
    }
    
    if (g_audioBufferPos == 0) {
        LOG_E("SPEAK", "No audio data generated (buffer empty)");
        g_isSpeaking = false;
        return false;
    }
    
    // バッファ満杯チェック
    if (g_audioBufferPos >= MAX_AUDIO_BUFFER_SIZE) {
        LOG_W("SPEAK", "Audio buffer reached maximum capacity - speech may be truncated");
        LOG_W("SPEAK", "Consider using shorter text or increasing MAX_AUDIO_BUFFER_SIZE");
    }
    
    float duration = (float)g_audioBufferPos / AUDIO_SAMPLE_RATE;
    LOG_I("SPEAK", "Synthesis complete. Buffer size: %d samples (%.2f seconds)", g_audioBufferPos, duration);
    
    // 期待される文字に対する音声長の妥当性チェック
    float expectedDuration = len * 0.06f; // 約1文字あたり60ms
    if (duration < expectedDuration * 0.7f) {
        LOG_W("SPEAK", "Speech duration seems short (%.2fs vs expected %.2fs) - possible truncation", 
              duration, expectedDuration);
    }
    
    // Step 3: Real-time playback with lip sync
    LOG_I("SPEAK", "Playing audio with M5.Speaker...");


    avatar.setExpression(Expression::Happy);    // M5Avatar
    avatar.setSpeechText(text);                 // M5Avatar
    
    const size_t chunkSize = 512;
    g_playbackPos = 0;
    uint32_t startTime = millis();
    
    while (g_playbackPos < g_audioBufferPos && g_isSpeaking && 
           (millis() - startTime < SPEECH_TIMEOUT_MS)) {
        
        size_t remainingSamples = g_audioBufferPos - g_playbackPos;
        size_t currentChunk = min(remainingSamples, chunkSize);
        
        // Level calculation for lip sync
        updateLevel(&g_audioBuffer[g_playbackPos], currentChunk);
        
        // Update mouth movement
        float mouthOpen = (g_currentLevel > 3) ? constrain(g_currentLevel / 30.0f, 0.0f, 1.0f) : 0.0f;
        avatar.setMouthOpenRatio(mouthOpen);
        
        // Audio playback
        esp_task_wdt_reset();
        bool playResult = M5.Speaker.playRaw(
            &g_audioBuffer[g_playbackPos], 
            currentChunk, 
            AUDIO_SAMPLE_RATE, 
            false, 1, 0
        );
        
        if (!playResult) {
            LOG_W("SPEAK", "playRaw failed at position %d", g_playbackPos);
            break;
        }
        
        g_playbackPos += currentChunk;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    
    // Completion
    avatar.setMouthOpenRatio(0.0f);
    avatar.setExpression(Expression::Neutral);
    avatar.setSpeechText("");
    M5.Speaker.stop();
    g_currentLevel = 0;
    g_isSpeaking = false;
    
    LOG_I("SPEAK", "Speech playback completed. Played %d/%d samples", g_playbackPos, g_audioBufferPos);
    return true;
}

// ===== Serial Command Processor =====
namespace SerialProcessor {
    static void processCommand() {
        if (g_isSpeaking) {
            Serial.println("[BLOCKED] Speech in progress");
            return;
        }
        
        if (strncmp(g_serialBuffer, "text:", 5) == 0) {
            speak(g_serialBuffer + 5);
        }
        else if (strncmp(g_serialBuffer, "volume:", 7) == 0) {
            int vol = atoi(g_serialBuffer + 7);
            if (vol >= 0 && vol <= 100) {
                g_volume = vol;
                M5.Speaker.setVolume(vol);
                Serial.printf("[VOLUME] Set to %d\n", vol);
            }
        }
        else if (strncmp(g_serialBuffer, "rate:", 5) == 0) {
            int rate = atoi(g_serialBuffer + 5);
            if (rate >= 80 && rate <= 450) {
                g_rate = rate;
                espeak.setRate(rate);
                Serial.printf("[RATE] Set to %d wpm\n", rate);
            }
        }
        else if (strncmp(g_serialBuffer, "pitch:", 6) == 0) {
            int pitch = atoi(g_serialBuffer + 6);
            if (pitch >= 0 && pitch <= 99) {
                g_pitch = pitch;
                espeak.setPitch(pitch);
                Serial.printf("[PITCH] Set to %d\n", pitch);
            }
        }
        else if (strncmp(g_serialBuffer, "internal_volume:", 16) == 0) {
            int vol = atoi(g_serialBuffer + 16);
            if (vol >= 0 && vol <= 200) {
                g_volume_internal = vol;
                espeak.setVolume(vol);
                Serial.printf("[INTERNAL_VOLUME] Set to %d\n", vol);
            }
        }
        else if (strncmp(g_serialBuffer, "pitch_range:", 12) == 0) {
            int range = atoi(g_serialBuffer + 12);
            if (range >= 0 && range <= 100) {
                g_pitchRange = range;
                espeak.setPitchRange(range);
                Serial.printf("[PITCH_RANGE] Set to %d\n", range);
            }
        }
        else if (strcmp(g_serialBuffer, "display_on") == 0) {
            g_displayEnabled = true;
            Serial.println("[DISPLAY] Enabled");
        }
        else if (strcmp(g_serialBuffer, "display_off") == 0) {
            g_displayEnabled = false;
            M5.Display.clear();
            Serial.println("[DISPLAY] Disabled");
        }
        else if (strcmp(g_serialBuffer, "demo") == 0) {
            speak("Hello! This is eSpeak with real time lip synchronization working perfectly on M5 Atom S3.");
        }
        else if (strcmp(g_serialBuffer, "memory") == 0) {
            MemoryMonitor::printStatus();
        }
        else if (strcmp(g_serialBuffer, "buffer_info") == 0) {
            float maxDuration = (float)MAX_AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE;
            Serial.printf("\n[BUFFER] Audio Buffer Information:\n");
            Serial.printf("  Maximum capacity: %d samples\n", MAX_AUDIO_BUFFER_SIZE);
            Serial.printf("  Maximum duration: %.2f seconds\n", maxDuration);
            Serial.printf("  Memory size: %.1f KB\n", (MAX_AUDIO_BUFFER_SIZE * sizeof(int16_t)) / 1024.0f);
            Serial.printf("  Current usage: %d samples\n", g_audioBufferPos);
            if (g_audioBufferPos > 0) {
                Serial.printf("  Current duration: %.2f seconds\n", (float)g_audioBufferPos / AUDIO_SAMPLE_RATE);
            }
            Serial.println("==========================\n");
        }
        else if (strcmp(g_serialBuffer, "status") == 0) {
            Serial.printf("\n[STATUS] Current Settings:\n");
            Serial.printf("  Rate: %d wpm\n", g_rate);
            Serial.printf("  Pitch: %d\n", g_pitch);
            Serial.printf("  Internal Volume: %d\n", g_volume_internal);
            Serial.printf("  Pitch Range: %d\n", g_pitchRange);
            Serial.printf("  Speaker Volume: %d\n", g_volume);
            Serial.printf("  Display: %s\n", g_displayEnabled ? "ON" : "OFF");
            Serial.printf("  Speaking: %s\n", g_isSpeaking ? "YES" : "NO");
            Serial.println("========================\n");
        }
        else if (strcmp(g_serialBuffer, "help") == 0) {
            Serial.println("\n[HELP] eSpeak Complete Commands:");
            Serial.println("text:Your message        - Speak text");
            Serial.println("volume:50               - Speaker volume (0-100)");
            Serial.println("rate:150                - Speech rate (80-450 wpm)");
            Serial.println("pitch:70                - Voice pitch (0-99)");
            Serial.println("internal_volume:100     - eSpeak internal volume (0-200)");
            Serial.println("pitch_range:100         - Pitch variation (0-100)");
            Serial.println("display_on/display_off  - Toggle display");
            Serial.println("demo                    - Demo speech");
            Serial.println("memory                  - Memory status");
            Serial.println("buffer_info             - Audio buffer information");
            Serial.println("status                  - Current settings");
            Serial.println("help                    - Show this help");
            Serial.printf("\nMax text length: %d characters\n", MAX_TEXT_LENGTH);
            Serial.printf("Max audio duration: ~%.1f seconds\n\n", (float)MAX_AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE);
        }
        else {
            // Direct speech for unrecognized commands
            if (strlen(g_serialBuffer) <= MAX_TEXT_LENGTH) {
                speak(g_serialBuffer);
            }
        }
    }
    
    static void handleInput() {
        while (Serial.available()) {
            char c = Serial.read();
            
            if (c == '\n' || c == '\r') {
                if (g_serialPos > 0) {
                    g_serialBuffer[g_serialPos] = '\0';
                    processCommand();
                    g_serialPos = 0;
                }
                return;
            }
            
            if (g_serialPos < SERIAL_BUFFER_SIZE - 1) {
                g_serialBuffer[g_serialPos++] = c;
            } else {
                Serial.println("[WARNING] Serial buffer overflow - resetting");
                g_serialPos = 0;
            }
        }
    }
}

// ===== Display Manager =====
namespace DisplayManager {
    static void update() {
        if (!g_displayEnabled) return;
        
        M5.Display.clear();
        M5.Display.setCursor(0, 0);
        M5.Display.setTextColor(WHITE);
        
        if (g_isSpeaking) {
            M5.Display.setTextColor(0x07FF); // CYAN
            M5.Display.println("SPEAKING");
        } else {
            M5.Display.setTextColor(0x07E0); // GREEN
            M5.Display.println("READY");
        }
        
        M5.Display.setTextColor(WHITE);
        M5.Display.printf("Vol: %d\n", g_volume);
        M5.Display.printf("Rate: %d\n", g_rate);
        M5.Display.printf("Pitch: %d\n", g_pitch);
        M5.Display.printf("Level: %d\n", g_currentLevel);
    }
}

// ===== Setup =====
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("=== eSpeak Complete Solution ===");
    
    // PSRAMにオーディオバッファを割り当て
    LOG_I("SETUP", "Allocating audio buffer in PSRAM");
    g_audioBuffer = (int16_t*)ps_malloc(MAX_AUDIO_BUFFER_SIZE * sizeof(int16_t));
    if (!g_audioBuffer) {
        LOG_E("SETUP", "Failed to allocate audio buffer in PSRAM");
        return;
    }
    LOG_I("SETUP", "Audio buffer allocated: %d KB in PSRAM", 
          (MAX_AUDIO_BUFFER_SIZE * sizeof(int16_t)) / 1024);
    
    g_systemReady = false;
    g_isSpeaking = false;
    
    // Initialize watchdog
    esp_task_wdt_init(45, true);
    esp_task_wdt_add(NULL);
    
    // PSRAMの利用可能性チェック
    if (!ESP.getPsramSize()) {
        LOG_E("SETUP", "PSRAM not available - cannot allocate large audio buffer");
        return;
    }
    LOG_I("SETUP", "PSRAM available: %.1f KB", ESP.getPsramSize() / 1024.0f);
    
    // M5 initialization (Display disabled by default to avoid conflicts)
    LOG_I("SETUP", "Initializing M5 with Speaker");
    auto cfg = M5.config();
    cfg.external_speaker.atomic_echo = true;
    M5.begin(cfg);
    M5.Lcd.setRotation(1);
    LOG_I("SETUP", "M5 initialized");
    
    // M5.Speaker configuration
    LOG_I("SETUP", "Configuring M5.Speaker");
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = AUDIO_SAMPLE_RATE;
    spk_cfg.stereo = false;
    spk_cfg.buzzer = false;
    spk_cfg.use_dac = false;
    spk_cfg.magnification = 2;
    spk_cfg.dma_buf_len = 128;
    spk_cfg.dma_buf_count = 8;
    spk_cfg.task_priority = 1;
    spk_cfg.pin_data_out = 5;
    spk_cfg.pin_bck = 8;
    spk_cfg.pin_ws = 6;
    spk_cfg.i2s_port = I2S_NUM_0;
    
    M5.Speaker.config(spk_cfg);
    
    if (!M5.Speaker.begin()) {
        LOG_E("SETUP", "M5.Speaker initialization failed");
        return;
    }
    
    M5.Speaker.setVolume(g_volume);
    LOG_I("SETUP", "M5.Speaker initialized successfully");
    
    // Avatar initialization
    LOG_I("SETUP", "Initializing avatar");
    avatar.setScale(0.45);
    avatar.setPosition(-72, -100);
    avatar.init();
    LOG_I("SETUP", "Avatar initialized");
    
    // eSpeak initialization
    LOG_I("SETUP", "Initializing eSpeak");
    espeak.add("/mem/data/voices/!v/f4", 
               espeak_ng_data_voices__v_f4, 
               espeak_ng_data_voices__v_f4_len);
    
    if (!espeak.begin()) {
        LOG_E("SETUP", "eSpeak initialization failed");
        return;
    }
    
    espeak.setVoice("en+f4");
    espeak.setRate(g_rate);
    espeak.setPitch(g_pitch);
    espeak.setVolume(g_volume_internal);
    espeak.setPitchRange(g_pitchRange);
    LOG_I("SETUP", "eSpeak initialized");
    
    g_systemReady = true;
    LOG_I("SETUP", "System ready");
    
    // Initial memory report
    MemoryMonitor::printStatus();
    
    delay(1000);
    speak("eSpeak complete system ready with advanced features");
    
    Serial.println("\n=== System Ready ===");
    Serial.println("Type 'help' for commands");
}

// ===== Main Loop =====
void loop() {
    M5.update();
    esp_task_wdt_reset();
    
    // Handle serial input
    SerialProcessor::handleInput();
    
    // Button handling
    if (M5.BtnA.wasPressed()) {
        // speak("Button A pressed. System working perfectly.");
        speak("Button A pressed. I am Stack-chan minimal voice of English!");
    }
    
    // Update display every 2 seconds (if enabled)
    static uint32_t lastDisplayUpdate = 0;
    if (g_displayEnabled && millis() - lastDisplayUpdate > 2000) {
        DisplayManager::update();
        lastDisplayUpdate = millis();
    }
    
    delay(50);
}

/*
 * 完全統合版の特徴:
 * 
 * 1. 安定性:
 *    - バッファ分離方式でライブラリ競合回避
 *    - ウォッチドッグタイマー対応
 *    - メモリ使用量監視
 * 
 * 2. M5Avatar統合:
 *    - リアルタイムリップシンク
 *    - 音声レベル連動の口の動き
 *    - 安定したアバター表示
 * 
 * 3. 高度な制御機能:
 *    - シリアルコマンド制御
 *    - 音声パラメータ調整（rate, pitch, volume等）
 *    - Display on/off制御（競合回避）
 *    - メモリ状況監視
 * 
 * 4. 使用可能コマンド:
 *    - text:メッセージ - テキスト音声出力
 *    - volume:値 - スピーカー音量 (0-100)
 *    - rate:値 - 話速 (80-450 wpm)
 *    - pitch:値 - 音程 (0-99)
 *    - internal_volume:値 - 内部音量 (0-200)
 *    - pitch_range:値 - 音程変化幅 (0-100)
 *    - display_on/off - 画面表示制御
 *    - demo - デモ音声
 *    - memory - メモリ状況
 *    - status - 現在の設定
 *    - help - ヘルプ表示
 * 
 * 5. 制限事項:
 *    - M5.Display使用時は競合リスク有り（制御可能）
 *    - 英語音声のみ対応
 *    - 最大4秒程度の音声長
 */