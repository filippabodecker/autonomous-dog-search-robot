/*!
 * Autonomous Dog Search Robot — External ESP32
 *
 * Receives mission events from the UNIHIKER K10 over UART, accepts
 * captured images over Wi-Fi/HTTP and forwards reports and images to Telegram.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <WebServer.h>

// Public placeholders — keep real credentials out of version control.
const char* WIFI_SSID = "xx";
const char* WIFI_PASSWORD = "yy";


//-------------------------------------------------
// TELEGRAM
//-------------------------------------------------

const char* BOT_TOKEN = "xxx";
const char* CHAT_ID = "yyy";

//-------------------------------------------------
// K10 UART
//-------------------------------------------------

// K10 / Maqueen P0 signal (P) --> ESP32 GPIO 16 (RX2)
// K10 / Maqueen GND           --> ESP32 GND
const int K10_RX_PIN = 16;

HardwareSerial k10Serial(2);

String k10Message = "";


//-------------------------------------------------
// HTTP SERVER AND IMAGE UPLOADS
//-------------------------------------------------

WebServer server(80);

File uploadedFile;

bool uploadOk = false;
bool telegramPending = false;

String uploadPath = "";
String uploadName = "";

String pendingPhotoPath = "";
String pendingPhotoName = "";


//-------------------------------------------------
// FUNCTION DECLARATIONS
//-------------------------------------------------

void connectWiFi();

void readK10Serial();
void handleK10Message(String message);

void handleUpload();

String urlEncode(const String& text);

bool sendTelegramText(
    const String& text
);

bool sendTelegramDocument(
    const String& filePath,
    const String& fileName
);


//-------------------------------------------------
// SYSTEM INITIALIZATION
//-------------------------------------------------

void setup() {

    Serial.begin(115200);

    if (!LittleFS.begin(true)) {

        Serial.println("LITTLEFS ERROR");

        return;
    }

    connectWiFi();


    //---------------------------------
    // UART FROM K10
    //---------------------------------

    k10Serial.setRxBufferSize(1024);

    k10Serial.begin(
        115200,
        SERIAL_8N1,
        K10_RX_PIN,
        -1
    );

    Serial.println("K10 UART READY");


    //---------------------------------
    // WI-FI IMAGE UPLOADS
    //---------------------------------

    server.on(
        "/upload",
        HTTP_POST,

        []() {

            if (uploadOk) {

                server.send(
                    200,
                    "text/plain",
                    "UPLOAD OK"
                );
            }

            else {

                server.send(
                    500,
                    "text/plain",
                    "UPLOAD FAILED"
                );
            }
        },

        handleUpload
    );

    server.begin();

    Serial.println(
        "ESP32 READY FOR WIFI PHOTO"
    );
}


//-------------------------------------------------
// MAIN LOOP
//-------------------------------------------------

void loop() {

    // Receive images from the K10 over Wi-Fi.
    server.handleClient();

    // Receive DOG_FOUND and MISSION_COMPLETE events over UART.
    readK10Serial();

    // Forward each stored image to Telegram.
    if (telegramPending) {

        telegramPending = false;

        bool sent =
            sendTelegramDocument(
                pendingPhotoPath,
                pendingPhotoName
            );

        if (sent) {

            Serial.println(
                "TELEGRAM DOCUMENT SENT"
            );
        }

        else {

            Serial.println(
                "TELEGRAM DOCUMENT FAILED"
            );
        }
    }
}


//-------------------------------------------------
// WI-FI CONNECTION
//-------------------------------------------------

void connectWiFi() {

    WiFi.mode(WIFI_STA);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    Serial.print("CONNECTING WIFI");

    while (WiFi.status() != WL_CONNECTED) {

        Serial.print(".");

        delay(500);
    }

    Serial.println();
    Serial.println("WIFI CONNECTED");
    Serial.println(WiFi.localIP());
}


//-------------------------------------------------
// READ UART MESSAGES FROM K10
//-------------------------------------------------

void readK10Serial() {

    while (k10Serial.available()) {

        char character =
            (char)k10Serial.read();

        if (character == '\n') {

            k10Message.trim();

            if (k10Message.length() > 0) {

                handleK10Message(
                    k10Message
                );
            }

            k10Message = "";
        }

        else if (character != '\r') {

            k10Message += character;
        }
    }
}


//-------------------------------------------------
// HANDLE K10 MESSAGE
//-------------------------------------------------

void handleK10Message(String message) {

    Serial.print("FROM K10: ");
    Serial.println(message);

    // Ignore the K10 startup confirmation.
    if (message == "K10_READY") {

        return;
    }

    if (message.startsWith("DOG_FOUND,")) {

        int dogCount =
            message.substring(10).toInt();

        String telegramText =
            "Hund hittad! Totalt antal hundar: " +
            String(dogCount);

        sendTelegramText(
            telegramText
        );
    }

    else if (message.startsWith(
                 "MISSION_COMPLETE,"
             )) {

        int dogCount =
            message.substring(17).toInt();

        String telegramText =
            "Uppdrag klart! Antal hittade hundar: " +
            String(dogCount);

        sendTelegramText(
            telegramText
        );
    }
}


//-------------------------------------------------
// RECEIVE IMAGE FROM K10 OVER WI-FI
//-------------------------------------------------

void handleUpload() {

    HTTPUpload& upload =
        server.upload();

    if (upload.status ==
        UPLOAD_FILE_START) {

        uploadOk = true;

        uploadName =
            upload.filename;

        // Ensure that the file is stored in the LittleFS root directory.
        if (!uploadName.startsWith("/")) {

            uploadPath =
                "/" + uploadName;
        }

        else {

            uploadPath = uploadName;
        }

        LittleFS.remove(
            uploadPath
        );

        uploadedFile =
            LittleFS.open(
                uploadPath,
                FILE_WRITE
            );

        if (!uploadedFile) {

            uploadOk = false;

            Serial.println(
                "CANNOT CREATE FILE"
            );

            return;
        }

        Serial.print("RECEIVING PHOTO: ");
        Serial.println(uploadName);
    }


    else if (upload.status ==
             UPLOAD_FILE_WRITE) {

        if (uploadedFile) {

            size_t written =
                uploadedFile.write(
                    upload.buf,
                    upload.currentSize
                );

            if (written !=
                upload.currentSize) {

                uploadOk = false;
            }
        }
    }


    else if (upload.status ==
             UPLOAD_FILE_END) {

        if (uploadedFile) {

            uploadedFile.close();
        }

        if (uploadOk) {

            Serial.print(
                "PHOTO FILE SAVED: "
            );

            Serial.println(
                upload.totalSize
            );

            // Preserve the received filename, e.g. dog_002.bmp.
            pendingPhotoPath =
                uploadPath;

            pendingPhotoName =
                uploadName;

            telegramPending = true;
        }
    }
}


//-------------------------------------------------
// URL-ENCODE TELEGRAM TEXT
//-------------------------------------------------

String urlEncode(const String& text) {

    const char* hex =
        "0123456789ABCDEF";

    String encoded = "";

    for (size_t i = 0;
         i < text.length();
         i++) {

        uint8_t character =
            (uint8_t)text[i];

        bool normalCharacter =
            (character >= 'a' &&
             character <= 'z') ||

            (character >= 'A' &&
             character <= 'Z') ||

            (character >= '0' &&
             character <= '9');

        if (normalCharacter) {

            encoded +=
                (char)character;
        }

        else if (character == ' ') {

            encoded += "+";
        }

        else {

            encoded += "%";
            encoded +=
                hex[character >> 4];
            encoded +=
                hex[character & 0x0F];
        }
    }

    return encoded;
}


//-------------------------------------------------
// SEND TELEGRAM TEXT MESSAGE
//-------------------------------------------------

bool sendTelegramText(
    const String& text
) {

    WiFiClientSecure client;

    client.setInsecure();

    if (!client.connect(
            "api.telegram.org",
            443
        )) {

        Serial.println(
            "TELEGRAM TEXT CONNECTION FAILED"
        );

        return false;
    }

    String body =
        "chat_id=" +
        String(CHAT_ID) +
        "&text=" +
        urlEncode(text);

    client.printf(
        "POST /bot%s/sendMessage HTTP/1.1\r\n",
        BOT_TOKEN
    );

    client.println(
        "Host: api.telegram.org"
    );

    client.println(
        "Connection: close"
    );

    client.println(
        "Content-Type: application/x-www-form-urlencoded"
    );

    client.println(
        "Content-Length: " +
        String(body.length())
    );

    client.println();

    client.print(body);

    unsigned long startWait =
        millis();

    while (!client.available() &&
           millis() - startWait <
           20000) {

        delay(10);
    }

    String response =
        client.readString();

    Serial.println(
        "TELEGRAM TEXT RESPONSE:"
    );

    Serial.println(response);

    client.stop();

    return response.indexOf(
               "\"ok\":true"
           ) >= 0;
}


//-------------------------------------------------
// SEND IMAGE TO TELEGRAM
//-------------------------------------------------

bool sendTelegramDocument(
    const String& filePath,
    const String& fileName
) {

    File file =
        LittleFS.open(
            filePath,
            FILE_READ
        );

    if (!file) {

        Serial.println(
            "CANNOT OPEN PHOTO"
        );

        return false;
    }

    WiFiClientSecure client;

    client.setInsecure();

    if (!client.connect(
            "api.telegram.org",
            443
        )) {

        file.close();

        Serial.println(
            "TELEGRAM CONNECTION FAILED"
        );

        return false;
    }

    String boundary =
        "----K10PhotoBoundary";

    String startData =
        "--" + boundary + "\r\n" +
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
        String(CHAT_ID) + "\r\n" +

        "--" + boundary + "\r\n" +
        "Content-Disposition: form-data; name=\"document\"; filename=\"" +
        fileName + "\"\r\n" +

        "Content-Type: application/octet-stream\r\n\r\n";

    String endData =
        "\r\n--" + boundary + "--\r\n";

    size_t contentLength =
        startData.length() +
        file.size() +
        endData.length();

    client.printf(
        "POST /bot%s/sendDocument HTTP/1.1\r\n",
        BOT_TOKEN
    );

    client.println(
        "Host: api.telegram.org"
    );

    client.println(
        "Connection: close"
    );

    client.println(
        "Content-Type: multipart/form-data; boundary=" +
        boundary
    );

    client.println(
        "Content-Length: " +
        String(contentLength)
    );

    client.println();

    client.print(startData);

    uint8_t buffer[512];

    while (file.available()) {

        size_t bytesRead =
            file.read(
                buffer,
                sizeof(buffer)
            );

        client.write(
            buffer,
            bytesRead
        );
    }

    file.close();

    client.print(endData);

    unsigned long startWait =
        millis();

    while (!client.available() &&
           millis() - startWait <
           20000) {

        delay(10);
    }

    String response =
        client.readString();

    Serial.println(
        "TELEGRAM DOCUMENT RESPONSE:"
    );

    Serial.println(response);

    client.stop();

    return response.indexOf(
               "\"ok\":true"
           ) >= 0;
}
