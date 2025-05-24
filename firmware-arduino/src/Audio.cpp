#include "OTA.h"
#include "Print.h"
#include "Config.h"
#include "AudioTools.h"
// #include "AudioTools/Concurrency/RTOS.h"
#include "AudioTools/AudioCodecs/CodecOpus.h"
#include <WebSocketsClient.h>
#include "Audio.h"

// WEBSOCKET
SemaphoreHandle_t wsMutex;
WebSocketsClient webSocket;

// TASK HANDLES
TaskHandle_t speakerTaskHandle = NULL;
TaskHandle_t micTaskHandle = NULL;
TaskHandle_t networkTaskHandle = NULL;

// TIMING REGISTERS
volatile bool scheduleListeningRestart = false;
unsigned long scheduledTime = 0;
unsigned long speakingStartTime = 0;

// AUDIO SETTINGS
int currentVolume = 70;
const int CHANNELS = 1;         // Mono
const int BITS_PER_SAMPLE = 16; // 16-bit audio

// AUDIO OUTPUT
class BufferPrint : public Print {
public:
  BufferPrint(BufferRTOS<uint8_t>& buf) : _buffer(buf) {}

  // networkTask -> webSocket.loop() -> webSocketEvent(WStype_BIN, ...) -> opusDecoder.write() -> bufferPrint.write()
  virtual size_t write(uint8_t data) override {
    if (webSocket.isConnected() && deviceState == SPEAKING) {
        return _buffer.writeArray(&data, 1);
    }
    return 1; //let opusDecoder write, otherwise thread will stuck
  }

  // networkTask -> webSocket.loop() -> webSocketEvent(WStype_BIN, ...) -> opusDecoder.write() -> bufferPrint.write()
  virtual size_t write(const uint8_t *buffer, size_t size) override {
    if (webSocket.isConnected() && deviceState == SPEAKING) {
        return _buffer.writeArray(buffer, size);
    }
    return size; //let opusDecoder write, otherwise thread will stuck
  }

private:
  BufferRTOS<uint8_t>& _buffer;
};

BufferPrint bufferPrint(audioBuffer);
OpusAudioDecoder opusDecoder;  //access guarded by wsmutex
BufferRTOS<uint8_t> audioBuffer(AUDIO_BUFFER_SIZE, AUDIO_CHUNK_SIZE);  //producer: networkTask, consumer: audioStreamTask. Thread safe in single producer->single consumer scenario.
I2SStream i2s; //access from audioStreamTask only
PitchShiftOutput<int16_t, VariableSpeedRingBuffer<int16_t>> pitchShift(i2s);
VolumeStream volume(pitchShift); //access from audioStreamTask only
QueueStream<uint8_t> queue(audioBuffer); //access from audioStreamTask only
StreamCopy copier(volume, queue);
AudioInfo info(SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE);
volatile bool outputFlushScheduled = false;

unsigned long getSpeakingDuration() {
    if (deviceState == SPEAKING && speakingStartTime > 0) {
        return millis() - speakingStartTime;
    }
    return 0;
}

// networkTask -> webSocket.loop() -> webSocketEvent(WStype_TEXT, ...) -> transitionToSpeaking()
void transitionToSpeaking() {
    vTaskDelay(50);

    i2sInputFlushScheduled = true;
    
    deviceState = SPEAKING;
    //digitalWrite(I2S_SD_OUT, HIGH);
    speakingStartTime = millis();
    
    webSocket.enableHeartbeat(30000, 15000, 3);
    
    Serial.println("Transitioned to speaking mode");
}

// networkTask -> transitionToListening()
// ( networkTask -> webSocket.loop() -> webSocketEvent(WStype_TEXT, ...) -> (sets scheduleListeningRestart) -> networkTask -> transitionToListening() )
void transitionToListening() {
    deviceState = PROCESSING;   
    scheduleListeningRestart = false;
    Serial.println("Transitioning to listening mode");

    i2sInputFlushScheduled = true;
    outputFlushScheduled = true;

    Serial.println("Transitioned to listening mode");

    deviceState = LISTENING;
    //digitalWrite(I2S_SD_OUT, LOW);
    webSocket.disableHeartbeat();
}

// audioStreamTask -> copier.copy() (conditional on webSocket.isConnected())
void audioStreamTask(void *parameter) {
    Serial.println("Starting I2S stream pipeline...");
    
    //pinMode(I2S_SD_OUT, OUTPUT);

    OpusSettings cfg;
    cfg.sample_rate = SAMPLE_RATE;
    cfg.channels = CHANNELS;
    cfg.bits_per_sample = BITS_PER_SAMPLE;
    cfg.max_buffer_size = 6144;

    xSemaphoreTake(wsMutex, portMAX_DELAY);
    opusDecoder.setOutput(bufferPrint);
    opusDecoder.begin(cfg);
    xSemaphoreGive(wsMutex);

    audioBuffer.setReadMaxWait(0);
    
    queue.begin();

    auto config = i2s.defaultConfig(TX_MODE);
    config.bits_per_sample = BITS_PER_SAMPLE;
    config.sample_rate = SAMPLE_RATE;
    config.channels = CHANNELS;
    config.pin_bck = I2S_BCK_OUT;
    config.pin_ws = I2S_WS_OUT;
    config.pin_data = I2S_DATA_OUT;
    config.port_no = I2S_PORT_OUT;

    config.copyFrom(info);  
    i2s.begin(config);    


    auto pcfg = pitchShift.defaultConfig();
    pcfg.copyFrom(info);
    pcfg.pitch_shift = 1.5;
    pcfg.buffer_size = 512;
    pitchShift.begin(pcfg);

    auto vcfg = volume.defaultConfig();
    vcfg.copyFrom(config);
    vcfg.allow_boost = true;
    volume.begin(vcfg);

    while (1) {
        if ( outputFlushScheduled) {
            outputFlushScheduled = false;
            i2s.flush();
            volume.flush();
            queue.flush();
            //audioBuffer.reset(); todo: read untill empty
        }

        if (webSocket.isConnected() && deviceState == SPEAKING) {
            copier.copy();
        }
        else {
            //we should always read from audioBuffer, otherwise writing thread can stuck
            queue.read();
        }
        vTaskDelay(1); 
    }
}


class WebsocketStream : public Print {
public:
    // micTask -> micToWsCopier.copyBytes() -> wsStream.write()
    virtual size_t write(uint8_t b) override {
        if (!webSocket.isConnected() || deviceState != LISTENING) {
            return 1;
        }
        
        xSemaphoreTake(wsMutex, portMAX_DELAY);
        webSocket.sendBIN(&b, 1);
        xSemaphoreGive(wsMutex);
        return 1;
    }
    
    // micTask -> micToWsCopier.copyBytes() -> wsStream.write()
    virtual size_t write(const uint8_t *buffer, size_t size) override {
        if (size == 0 || !webSocket.isConnected() || deviceState != LISTENING) {
            return size;
        }
        
        xSemaphoreTake(wsMutex, portMAX_DELAY);
        webSocket.sendBIN(buffer, size);
        xSemaphoreGive(wsMutex);
        return size;
    }
};

WebsocketStream wsStream; //guard with wsMutex
I2SStream i2sInput; //access from micTask only
StreamCopy micToWsCopier(wsStream, i2sInput);
volatile bool i2sInputFlushScheduled = false;
const int MIC_COPY_SIZE = 64;

void micTask(void *parameter) {
    // Configure and start I2S input stream.
    auto i2sConfig = i2sInput.defaultConfig(RX_MODE);
    i2sConfig.bits_per_sample = BITS_PER_SAMPLE;
    i2sConfig.sample_rate = SAMPLE_RATE;
    i2sConfig.channels = CHANNELS;
    i2sConfig.signal_type = PDM;
    i2sConfig.i2s_format = I2S_LEFT_JUSTIFIED_FORMAT;
    i2sConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    //i2sConfig.i2s_format = I2S_PCM;
    // Configure your I2S input pins appropriately here:
    i2sConfig.pin_bck = I2S_PIN_NO_CHANGE; //free GPIO6!
    i2sConfig.pin_ws  = I2S_WS;
    i2sConfig.pin_data = I2S_SD;
    i2sConfig.port_no = I2S_PORT_IN;
    i2sInput.begin(i2sConfig);

    micToWsCopier.setDelayOnNoData(0);

    while (1) {
        if ( i2sInputFlushScheduled ) {
            i2sInputFlushScheduled = false;
            i2sInput.flush();
        }

        if (deviceState == LISTENING && webSocket.isConnected()) {
            // Use smaller chunk size to avoid blocking too long
            micToWsCopier.copyBytes(MIC_COPY_SIZE);
            
            // Yield more frequently
            vTaskDelay(1);
        } else {
            vTaskDelay(10);
        }
    }
}

// WEBSOCKET EVENTS
// networkTask -> webSocket.loop() -> webSocketEvent()
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        Serial.printf("[WSc] Disconnected!\n");
        deviceState = IDLE;
        break;
    case WStype_CONNECTED:
        Serial.printf("[WSc] Connected to url: %s\n", payload);
        deviceState = PROCESSING;
        break;
    case WStype_TEXT:
    {
        Serial.printf("[WSc] get text: %s\n", payload);

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (char *)payload);

        if (error)
        {
            Serial.println("Error deserializing JSON");
            deviceState = IDLE;
            return;
        }

        String type = doc["type"];

        // auth messages
        if (strcmp((char*)type.c_str(), "auth") == 0) {
            currentVolume = doc["volume_control"].as<int>();
            bool is_ota = doc["is_ota"].as<bool>();
            bool is_reset = doc["is_reset"].as<bool>();
            volume.setVolume(currentVolume / 100.0f);  // Set initial volume (e.g., 70/100 = 0.7)

            if (is_ota) {
                Serial.println("OTA update received");
                setOTAStatusInNVS(OTA_IN_PROGRESS);
                ESP.restart();
            }

            if (is_reset) {
                Serial.println("Factory reset received");
                // setFactoryResetStatusInNVS(true);
                ESP.restart();
            }
        }

        // oai messages
        if (strcmp((char*)type.c_str(), "server") == 0) {
            String msg = doc["msg"];
            Serial.println(msg);

            if (strcmp((char*)msg.c_str(), "RESPONSE.COMPLETE") == 0 || strcmp((char*)msg.c_str(), "RESPONSE.ERROR") == 0) {
                Serial.println("Received RESPONSE.COMPLETE or RESPONSE.ERROR, starting listening again");

                // Check if volume_control is included in the message
                if (doc.containsKey("volume_control")) {
                    int newVolume = doc["volume_control"].as<int>();
                    volume.setVolume(newVolume / 100.0f * 1.4f);
                }

                scheduleListeningRestart = true;
                scheduledTime = millis() + 1000; // 1 second delay
            } else if (strcmp((char*)msg.c_str(), "AUDIO.COMMITTED") == 0) {
                deviceState = PROCESSING; 
            } else if (strcmp((char*)msg.c_str(), "RESPONSE.CREATED") == 0) {
                Serial.println("Received RESPONSE.CREATED, transitioning to speaking");
                transitionToSpeaking();
            } 
        }
    }
        break;
    case WStype_BIN:
    {
        if (scheduleListeningRestart || deviceState != SPEAKING) {
            Serial.println("Skipping audio data due to touch interrupt.");
            break;
        }

        // Otherwise process the audio data normally
        size_t processed = opusDecoder.write(payload, length);
        if (processed != length) {
            Serial.printf("Warning: Only processed %d/%d bytes\n", processed, length);
        }
        break;
      }
    case WStype_ERROR:
        Serial.printf("[WSc] Error: %s\n", payload);    
        break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_PONG:
    case WStype_PING:
    case WStype_FRAGMENT_FIN:
        break;
    }
}

// wifiTask -> WIFIMANAGER::loop() -> WIFIMANAGER::tryConnect() -> connectCb() -> websocketSetup()
void websocketSetup(String server_domain, int port, String path)
{
    String headers = "Authorization: Bearer " + String(authTokenGlobal);

    xSemaphoreTake(wsMutex, portMAX_DELAY);

    webSocket.setExtraHeaders(headers.c_str());
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(1000);

    webSocket.enableHeartbeat(30000, 15000, 3); // 30s ping interval, 15s timeout, 3 retries

    #ifdef DEV_MODE
    webSocket.begin(server_domain.c_str(), port, path.c_str());
    #else
    webSocket.beginSslWithCA(server_domain.c_str(), port, path.c_str(), CA_cert);
    #endif

    xSemaphoreGive(wsMutex);
}

// networkTask -> webSocket.loop()
void networkTask(void *parameter) {
    while (1) {
        xSemaphoreTake(wsMutex, portMAX_DELAY);

        // Check to see if a transition to listening mode is scheduled.
        if (scheduleListeningRestart && millis() >= scheduledTime) {
            transitionToListening();
        }

        webSocket.loop();
        xSemaphoreGive(wsMutex);

        vTaskDelay(1);
    }
}
