#include <R503Lib.h>
#include <WebSocketsClient_Generic.h>
#include <Base64.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <LiquidCrystal_Software_I2C.h>
#include <vector>
#include <tuple>

// Set to true to enable debug output
#define R503_DEBUG false
LiquidCrystal_I2C lcd(0x27, 16, 2, 4, 5);

const String DEVICE_ID = "SENSOR_1";

//Wifi Config
const char* ssid = "OnePlus 9R";
const char* password = "/Lahiru@28";
const int port = 8090;
const char* hostIP = "192.168.1.227";
const char* SocketAddress = "/fingerprint-websocket?id=SENSOR_1" ;
String lastSourceDevice = "";
String ID = "";

// Set this to the serial port you are using
#define fpsSerial Serial2
R503Lib fps(&fpsSerial, 17, 16, 0xFFFFFFFF);

// inizalize webSocket
WebSocketsClient webSocket;

// Template buffer
uint8_t templateData[1792] = {0};
uint16_t sizeTemplateData;

void enrollFinger();
void searchFinger();
int matchFinger();
void deleteFinger();
void clearLibrary();
void printIndexTable();
void saveTemplateToBuffer();
void restoreTemplateFromBuffer();
void handleTextMessage();
void handleBinaryMessage();
std::vector<std::tuple<uint8_t, int>> runLengthEncode(const uint8_t*, size_t);
void decodeBinaryData(JsonArray& encodedDataArray, uint8_t*, uint16_t&);

void setup()
{
    lcd.init();
    lcd.backlight();
    lcd.cursor_on();
    lcd.blink_on();
    Serial.begin(115200);
    Serial.printf("\n\n=========================================\n");
    Serial.printf("== Fingerprint Sensor R503 Example ======\n");
    Serial.printf("=========================================\n\n");

    // set the data rate for the sensor serial port
    if (fps.begin(57600, 0x0) != R503_OK)
    {
        Serial.println("[X] Sensor not found!");
        while (1)
        {
            delay(1);
        }
    }
    else
    {
        fps.setAuraLED(aLEDBreathing, aLEDBlue, 255, 1);
        // fps.setPacketSize(256); // default is 128, 32 <> 256
        // fps.setBaudrate(57600); // default is 57600, 9600 <> 115200
        Serial.println(" >> Sensor 1 found!");
    }

    //fps.printDeviceInfo();
    //fps.printParameters();
    lcd.print("Connecting to ");
    lcd.print(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
      Serial.println("Connecting to WiFi...");
      lcd.print(".");
    }
    Serial.println("Connected to WiFi");
    lcd.clear();
    lcd.print("Connected");
    lcd.print("Connecting webSocket server");
    webSocket.begin(hostIP, port, SocketAddress);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}

void loop() // run over and over again
{
    webSocket.loop();
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("Disconnected from WebSocket server");
            break;
        case WStype_CONNECTED:
            Serial.println("Connected to WebSocket server");
            lcd.clear();
            lcd.print("OK");
            break;
        case WStype_TEXT:
            handleTextMessage((char*)payload);
            break;
        // case WStype_BIN:
        //     handleBinaryMessage(payload, length);
        //     break;
    }
}

void handleTextMessage(char* payload) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
        Serial.println("Failed to parse JSON");
        return;
    }
    String targetDevice = doc["targetDevice"].as<String>();
    String sourceDevice = doc["sourceDevice"].as<String>();
    String command = doc["message"].as<String>();
    String tempID = doc["applicationId"].as<String>();
    ID = tempID;
    lastSourceDevice = sourceDevice;
    if (targetDevice == DEVICE_ID) {
        if (command == "SCAN") {
            command = "";
            lcd.clear();
            lcd.print("SCAN");
            performScan();
        } else if (command == "MATCH") {
          Serial.println(command);
            if (doc.containsKey("templateData")) {
                JsonArray templateArray = doc["templateData"].as<JsonArray>();
                decodeBinaryData(templateArray, templateData, sizeTemplateData);
                for (int i = 0; i < 1792; i++) {
                  Serial.print(templateData[i]);
                  Serial.print(",");
                }
                restoreTemplateFromBuffer();
                int temp = matchFinger();
                Serial.println(temp);
                if(temp == 1){
                  sendTextMessage("MATCH_FOUND");
                }else{
                  sendTextMessage("MATCH_NOT_FOUND");
                }
            } else {
                Serial.println("No template data found in the message");
            }
        } else if (command == "RESET"){

          Serial.println("Reset");
        }
    }
}


void sendTextMessage(const char* message) {
    DynamicJsonDocument doc(1024);
    doc["applicationId"] = ID;
    doc["sourceDevice"] = DEVICE_ID;
    doc["targetDevice"] = lastSourceDevice;
    doc["message"] = message;

    String jsonString;
    serializeJson(doc, jsonString);
    Serial.println(jsonString);
    webSocket.sendTXT(jsonString.c_str());
}

void sendBinaryData(uint8_t* data, size_t length) {
    // Perform Run-Length Encoding on the data
    auto encodedData = runLengthEncode(data, length);

    // Use a small JSON document size to minimize memory usage
    DynamicJsonDocument doc(1024);

    // Setup JSON fields
    doc["sourceDevice"] = DEVICE_ID;
    doc["targetDevice"] = lastSourceDevice;  // This can be updated as needed
    doc["applicationId"] = ID;
    doc["message"] = "SCAN_COMPLETE";

    JsonArray templateArray = doc.createNestedArray("templateData");

    // Add RLE encoded pairs to the JSON
    for (const auto& pair : encodedData) {
        uint8_t value = std::get<0>(pair);
        int count = std::get<1>(pair);
        JsonArray encodedPair = templateArray.createNestedArray();
        encodedPair.add(value);
        encodedPair.add(count);
    }

    // Serialize JSON and send it via WebSocket
    String jsonString;
    serializeJson(doc, jsonString);
    Serial.println(jsonString);
    // Send the JSON string via WebSocket (non-blocking call)
    webSocket.sendTXT(jsonString.c_str());
}

std::vector<std::tuple<uint8_t, int>> runLengthEncode(const uint8_t* data, size_t length) {
    std::vector<std::tuple<uint8_t, int>> encoded;

    if (length == 0) return encoded;

    uint8_t current_value = data[0];
    int count = 1;

    for (size_t i = 1; i < length; ++i) {
        if (data[i] == current_value) {
            ++count;
        } else {
            encoded.push_back(std::make_tuple(current_value, count));
            current_value = data[i];
            count = 1;
        }
    }

    // Add the last value
    encoded.push_back(std::make_tuple(current_value, count));

    return encoded;
}

void decodeBinaryData(JsonArray& encodedDataArray, uint8_t* outputData, uint16_t& decodedLength) {
    decodedLength = 0;

    // Loop through the RLE encoded pairs in the JSON array
    for (JsonArray::iterator it = encodedDataArray.begin(); it != encodedDataArray.end(); ++it) {
        JsonArray pair = *it;
        uint8_t value = pair[0].as<uint8_t>();
        int count = pair[1].as<int>();

        // Write the decoded data to the output buffer
        for (int i = 0; i < count; ++i) {
            templateData[decodedLength++] = value;
        }
    }
}

void performScan() {
    Serial.println(ID);
    enrollFinger();
    saveTemplateToBuffer();
    sendBinaryData(templateData, sizeTemplateData);
    lcd.clear();
    lcd.print("Done");
}


void enrollFinger()
{
    R503Lib* fp = &fps;
    int ret;
    String str;

    Serial.flush(); // flush the buffer

    uint8_t featureCount = 6;
    Serial.printf(" << %d\n\n", featureCount);

    Serial.flush(); // flush the buffer

    uint16_t location = 1;
    Serial.printf(" << %d\n\n", location);

    Serial.print("We are all set, follow the steps below to enroll a new finger");

    for (int i = 1; i <= featureCount; i++)
    {
        fp->setAuraLED(aLEDBreathing, aLEDBlue, 50, 255);
        Serial.println("\n\n >> Place finger on sensor...");
        sendTextMessage("Place finger on sensor...");
        lcd.clear();
        lcd.print("Place the finger");
        while (true)
        {
            ret = fp->takeImage();
            if (ret == R503_NO_FINGER)
            {
                delay(100);
                continue; // try again
            }
            else if (ret == R503_OK)
            {
                Serial.printf(" >> Image %d of %d taken \n", i, featureCount);
                fp->setAuraLED(aLEDBreathing, aLEDYellow, 255, 255);
                // Go for feature extraction
            }
            else
            {
                Serial.printf("[X] Could not take image (code: 0x%02X)\n", ret);
                fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
                delay(1000);
                continue; // try again
            }

            ret = fp->extractFeatures(i);
            if (ret != R503_OK)
            {
                Serial.printf("[X] Failed to extract features, trying again (code: 0x%02X)\n", ret);
                fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
                delay(1000);
                continue;
            }

            fp->setAuraLED(aLEDBreathing, aLEDGreen, 255, 255);
            Serial.printf(" >> Features %d of %d extracted\n", i, featureCount);
            delay(250);

            break;
        }

        Serial.println("\n\n >> Lift your finger from the sensor!");
        sendTextMessage("Lift your finger from the sensor!");
        lcd.clear();
        lcd.print("Lift");
        while (fp->takeImage() != R503_NO_FINGER)
        {
            delay(100);
        }
    }

    Serial.println(" >> Creating template...");
    fp->setAuraLED(aLEDBreathing, aLEDPurple, 100, 255);
    delay(100);
    ret = fp->createTemplate();

    if (ret != R503_OK)
    {
        Serial.printf("[X] Failed to create a template (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }

    Serial.println(" >> Template created");
    ret = fp->storeTemplate(1, location);
    if (ret != R503_OK)
    {
        Serial.printf("[X] Failed to store the template (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }

    delay(250);

    fp->setAuraLED(aLEDBreathing, aLEDGreen, 255, 1);
    fp->setAuraLED(aLEDBreathing, aLEDGreen, 0, 1);
    Serial.printf(" >> Template stored at location: %d\n", location);
}

void searchFinger()
{
    unsigned long start = millis();
    R503Lib* fp = &fps;
    int ret;
    String str;

    Serial.flush(); // flush the buffer

    // If there is a second sensor, ask which one to use
    #ifdef R503_SECOND_SENSOR
        Serial.println("Which fringerprint sensor do you want to use (1 or 2) ?");
        do
        {
            str = Serial.readStringUntil('\n');
        } while (str.length() < 1);

        uint8_t sensorID = str.toInt();
        Serial.printf(" << %d\n\n", sensorID);

        if(sensorID == 2)
        {
            fp = &fps2;
        }

        Serial.flush();
    #endif

    Serial.printf(" >> Place your finger on the sensor...\n\n");

    fp->setAuraLED(aLEDBreathing, aLEDBlue, 50, 255);

    while (millis() - start < 30000)
    {
        ret = fp->takeImage();

        if (ret == R503_NO_FINGER)
        {
            delay(250);
            continue;
        }
        else if (ret == R503_OK)
        {
            Serial.printf(" >> Image taken \n");
            fp->setAuraLED(aLEDBreathing, aLEDYellow, 150, 255);
            break;
        }
        else
        {
            Serial.printf("[X] Could not take image (code: 0x%02X)\n", ret);
            fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
            delay(1000);
            continue;
        }
    }

    if (millis() - start >= 10000)
    {
        Serial.printf("[X] Could not take image (timeout)\n");
        fp->setAuraLED(aLEDFlash, aLEDRed, 100, 2);
        return;
    }

    ret = fp->extractFeatures(1);

    if (ret != R503_OK)
    {
        Serial.printf("[X] Could not extract features (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }

    uint16_t location, confidence;
    ret = fp->searchFinger(1, location, confidence);

    if (ret == R503_NO_MATCH_IN_LIBRARY)
    {
        Serial.printf(" >> No matching finger found\n");
        fp->setAuraLED(aLEDBreathing, aLEDRed, 255, 1);
    }
    else if (ret == R503_OK)
    {
        Serial.println(" >> Found finger");
        Serial.printf("    Finger ID: %d\n", location);
        Serial.printf("    Confidence: %d\n", confidence);
        fp->setAuraLED(aLEDBreathing, aLEDGreen, 255, 1);
    }
    else
    {
        Serial.printf("[X] Could not search library (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
    }
}

int matchFinger() {
    unsigned long start = millis();
    R503Lib* fp = &fps;
    int ret;
    String str;

    Serial.flush(); // flush the buffer

    //150 fingerprint location
    ret = fp->getTemplate(1, 150);

    if (ret != R503_OK)
    {
        Serial.printf("[X] Could not get template (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return 0;
    }

    Serial.printf(" >> Place your finger on the sensor...\n");

    fp->setAuraLED(aLEDBreathing, aLEDBlue, 50, 255);

    while (millis() - start < 10000)
    {
        ret = fp->takeImage();

        if (ret == R503_NO_FINGER)
        {
            delay(250);
            continue;
        }
        else if (ret == R503_OK)
        {
            Serial.printf(" >> Image taken \n");
            fp->setAuraLED(aLEDBreathing, aLEDYellow, 150, 255);
            break;
        }
        else
        {
            Serial.printf("[X] Could not take image (code: 0x%02X)\n", ret);
            fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
            delay(1000);
            continue;
        }
    }

    ret = fp->extractFeatures(3); // extract features from buffer 3

    if (ret != R503_OK)
    {
        Serial.printf("[X] Could not extract features (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return 0;
    }

    uint16_t confidence;
    ret = fp->matchFinger(confidence);

    if (ret == R503_NO_MATCH)
    {
        Serial.printf(" >> No matching finger found\n");
        fp->setAuraLED(aLEDBreathing, aLEDRed, 255, 1);
        return 0;
    }
    else if (ret == R503_OK)
    {
        Serial.println(" >> Found finger");
        fp->setAuraLED(aLEDBreathing, aLEDGreen, 255, 1);
        return 1;
    }
    else
    {
        Serial.printf("[X] Could not search library (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return 0;
    }
}

void deleteFinger()
{
    R503Lib* fp = &fps;
    int ret;
    String str;

    Serial.flush(); // flush the buffer

    // If there is a second sensor, ask which one to use
    #ifdef R503_SECOND_SENSOR
        Serial.println("Which fringerprint sensor do you want to use (1 or 2) ?");
        do
        {
            str = Serial.readStringUntil('\n');
        } while (str.length() < 1);

        uint8_t sensorID = str.toInt();
        Serial.printf(" << %d\n\n", sensorID);

        if(sensorID == 2)
        {
            fp = &fps2;
        }

        Serial.flush();
    #endif

    Serial.println("Enter Finger ID to be deleted:");

    do
    {
        str = Serial.readStringUntil('\n');
    } while (str.length() < 1);

    uint16_t location = str.toInt();
    Serial.printf(" << %d\n\n", location);

    ret = fp->deleteTemplate(location);

    if (ret == R503_OK)
    {
        fp->setAuraLED(aLEDBreathing, aLEDGreen, 50, 2);
        Serial.printf("Finger with ID %d deleted\n", location);
    }
    else
    {
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        Serial.printf("\n[X] Failed to delete finger (code: 0x%02X)\n", ret);
    }
}

void clearLibrary()
{
    R503Lib* fp = &fps;
    int ret;
    String str;

    Serial.flush(); // flush the buffer

    // If there is a second sensor, ask which one to use
    #ifdef R503_SECOND_SENSOR
        Serial.println("Which fringerprint sensor do you want to use (1 or 2) ?");
        do
        {
            str = Serial.readStringUntil('\n');
        } while (str.length() < 1);

        uint8_t sensorID = str.toInt();
        Serial.printf(" << %d\n\n", sensorID);

        if(sensorID == 2)
        {
            fp = &fps2;
        }

        Serial.flush();
    #endif

    Serial.println("Do you really want to clear library Yes [y] / No [n]");
    fp->setAuraLED(aLEDON, aLEDRed, 255, 1);

    do
    {
        str = Serial.readStringUntil('\n');
    } while (str.length() < 1);

    Serial.printf(" << %c\n\n", str[0]);

    if (str[0] == 'y')
    {
        int ret = fp->emptyLibrary();
        if (ret == R503_OK)
        {
            Serial.printf("The fingerprint library has been cleared!\n");
            fp->setAuraLED(aLEDBreathing, aLEDGreen, 50, 2);
        }
        else
        {
            Serial.printf("[X] Failed to clear library (code: 0x%02X)\n", ret);
            fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        }
    }
    else
    {
        Serial.printf("The operation has been cancelled.\n");

        fp->setAuraLED(aLEDFadeOut, aLEDRed, 100, 0);
    }
}

void printIndexTable()
{
    R503Lib* fp = &fps;
    int ret;
    String str;

    Serial.flush(); // flush the buffer

    // If there is a second sensor, ask which one to use
    #ifdef R503_SECOND_SENSOR
        Serial.println("Which fringerprint sensor do you want to use (1 or 2) ?");
        do
        {
            str = Serial.readStringUntil('\n');
        } while (str.length() < 1);

        uint8_t sensorID = str.toInt();
        Serial.printf(" << %d\n\n", sensorID);

        if(sensorID == 2)
        {
            fp = &fps2;
        }

        Serial.flush();
    #endif

    Serial.printf("\nReading index table...\n");

    uint16_t count;
    ret = fp->getTemplateCount(count);
    if (ret == R503_OK)
    {
        Serial.printf("- Amount of templates: %d\n", count);
    }
    else
    {
        Serial.printf(" - Amount of templates: could not read (code: 0x%02X)\n", ret);
    }

    R503Parameters params;
    ret = fp->readParameters(params);
    if (ret == OK)
    {
        Serial.printf("- Fingerprint library capacity: %d\n", params.fingerLibrarySize);
    }
    else
    {
        Serial.printf("- Fingerprint library capacity: could not read (code: 0x%02X)\n", ret);
    }

    // only print first page, since we know library size is less than 256
    uint8_t table[32];
    ret = fp->readIndexTable(table);

    if (ret == R503_OK)
    {
        Serial.printf("- Fingerprints stored at locations (ID):\n  ");

        for (int i = 0; i < 32; i++)
        {
            for (int b = 0; b < 8; b++)
            {
                if (table[i] >> b & 0x01)
                {
                    Serial.printf("%d  ", i * 8 + b);
                }
            }
        }

        Serial.println();
    }
    else
    {
        Serial.printf("- Fingerprints stored at locations (ID): could not read (code: 0x%02X)\n", ret);
    }
}

void saveTemplateToBuffer()
{
    R503Lib* fp = &fps;
    int ret;
    String str;

    Serial.flush(); // flush the buffer

    uint8_t fingerID = 1;
    Serial.printf(" << %d\n\n", fingerID);

    Serial.println();

    fp->setAuraLED(aLEDBreathing, aLEDYellow, 50, 255);
    Serial.printf(" >> Getting template from finger ID: %d\n", fingerID);

    ret = fp->getTemplate(1, fingerID);

    if (ret != R503_OK)
    {
        Serial.printf("Err: getting template failed: %d \n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }

    Serial.printf(" >> Template placed in buffer\n");
    Serial.printf("    Downloading template to MCU\n");

    ret = fp->downloadTemplate(2, templateData, sizeTemplateData);

    if (ret != R503_OK)
    {
        Serial.printf("Err: downloading template failed (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }


    if (ret != R503_OK)
    {
        Serial.printf("[X] Failed to upload template (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }

    Serial.printf(" >> Template uploaded to sensor buffer\n");

    ret = fp->storeTemplate(1, fingerID);

    if (ret != R503_OK)
    {
        Serial.printf("[X] Failed to store template at location %d (code: 0x%02X)\n", fingerID, ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }

    fp->setAuraLED(aLEDBreathing, aLEDGreen, 50, 2);
    Serial.printf(" >> Template stored at location %d\n", fingerID);
}

void restoreTemplateFromBuffer()
{
    R503Lib* fp = &fps;
    int ret;
    String str;

    Serial.flush(); // flush the buffer

    // If there is a second sensor, ask which one to use
    #ifdef R503_SECOND_SENSOR
        Serial.println("Which fringerprint sensor do you want to use (1 or 2) ?");
        do
        {
            str = Serial.readStringUntil('\n');
        } while (str.length() < 1);

        uint8_t sensorID = str.toInt();
        Serial.printf(" << %d\n\n", sensorID);

        if(sensorID == 2)
        {
            fp = &fps2;
        }

        Serial.flush();
    #endif


    Serial.println("Getting the fingerprint\n");
    for (int i = 0; i < sizeTemplateData; i++) {
      Serial.print(templateData[i]);
      Serial.print(" ");
    }
    Serial.println();
    Serial.println(sizeTemplateData);

    uint8_t fingerID = 150;
    Serial.printf(" << %d\n\n", fingerID);

    Serial.println();

    fp->setAuraLED(aLEDBreathing, aLEDYellow, 50, 255);
    Serial.println(" >> Uploading template to sensor buffer");

    ret = fp->uploadTemplate(1, templateData, sizeTemplateData);

    if (ret != R503_OK)
    {
        Serial.printf("[X] Failed to upload template (code: 0x%02X)\n", ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }

    Serial.printf(" >> Template uploaded to sensor buffer\n");

    ret = fp->storeTemplate(1, fingerID);

    if (ret != R503_OK)
    {
        Serial.printf("[X] Failed to store template at location %d (code: 0x%02X)\n", fingerID, ret);
        fp->setAuraLED(aLEDFlash, aLEDRed, 50, 3);
        return;
    }

    fp->setAuraLED(aLEDBreathing, aLEDGreen, 50, 2);
    Serial.printf(" >> Template stored at location %d\n", fingerID);
}
