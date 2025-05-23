/**
 * @file streams-i2s-webserver_wav.ino
 *
 *  This sketch reads sound data from I2S. The result is provided as WAV stream which can be listened to in a Web Browser
 *
 * **ADD THIS**
 * lib_deps =
        https://github.com/pschatzmann/arduino-audio-tools.git
 *
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
#include <WiFi.h>
#include "AudioTools.h"
#include "Config.h"

const char *ssid = "name";
const char *password = "password";
// AudioEncodedServer server(new WAVEncoder(),"ssid","password");
AudioWAVServer server(ssid, password); // the same a above

I2SStream i2sStream;                                    // Access I2S as stream
ConverterFillLeftAndRight<int16_t> filler(LeftIsEmpty); // fill both channels - or change to RightIsEmpty

void setup()
{
    Serial.begin(115200);
    AudioLogger::instance().begin(Serial, AudioLogger::Info);

    //   // Connect to Wi-Fi
    Serial.println("Connecting to WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(1000);
        Serial.println("Connecting...");
    }
    Serial.println("Connected to WiFi");

    // Print the IP address
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // start i2s input with default configuration
    Serial.println("starting I2S...");
    auto config = i2sStream.defaultConfig(RX_MODE);
    // config.i2s_format = I2S_LSB_FORMAT; // if quality is bad change to I2S_LSB_FORMAT https://github.com/pschatzmann/arduino-audio-tools/issues/23
    // config.sample_rate = 22050;
    // config.channels = 2;
    // config.bits_per_sample = 32;

    // working well
    /*
    config.i2s_format = I2S_STD_FORMAT;
    config.sample_rate = 44100;  // INMP441 supports up to 44.1kHz
    config.channels = 1;         // INMP441 is mono
    config.bits_per_sample = 16; // INMP441 is a 24-bit ADC

    config.pin_ws = 19; // Adjust these pins according to your wiring
    config.pin_bck = 18;
    config.pin_data = 21;
    config.use_apll = true; // Try with APLL for better clock stability
    */

    config.bits_per_sample = 16;
    config.sample_rate = 24000;
    config.channels = 1;
    config.signal_type = PDM;
    config.i2s_format = I2S_LEFT_JUSTIFIED_FORMAT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    //i2sConfig.i2s_format = I2S_PCM;
    // Configure your I2S input pins appropriately here:
    //i2sConfig.pin_bck = I2S_PIN_NO_CHANGE;
    config.pin_ws  = I2S_WS;
    config.pin_data = I2S_SD;
    config.port_no = I2S_PORT_IN;


    i2sStream.begin(config);
    Serial.println("I2S started");

    // start data sink
    server.begin(i2sStream, config, &filler);
}

// Arduino loop
void loop()
{
    // Handle new connections
    server.copy();
}