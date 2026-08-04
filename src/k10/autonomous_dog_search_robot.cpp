/*!
 * MindPlus
 * esp32s3bit
 *
 * Hunddetektering + foto + servoarm + LiDAR på egen CPU-kärna
 * + ESP32 / Telegram
 * + röstkommando: Mission complete
 * + inspelad avslutsfras
 */

#include "unihiker_k10.h"
#include "AIRecognition.h"
#include "asr.h"

#include <DFRobot_MaqueenPlusV2.h>
#include <DFRobot_matrixLidarDistanceSensor.h>
#include <WiFi.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


//-------------------------------------------------
// OBJEKT
//-------------------------------------------------

UNIHIKER_K10 k10;
AIRecognition ai;
ASR asr;
Music music;

DFRobot_MaqueenPlusV2 maqueenPlus;

uint8_t tofAddress = 0x33;
DFRobot_matrixLidarDistanceSensor_I2C tof(tofAddress);


//-------------------------------------------------
// WIFI
//-------------------------------------------------

const char* ssid = "xx";
const char* password = "yy";

uint8_t screen_dir = 2;


//-------------------------------------------------
// EXTERN ESP32: UART
//-------------------------------------------------

// Maqueens vanliga P0-port:
// GND / 3V3 / P
//
// P0 (P) --> ESP32 RX2
// GND    --> ESP32 GND
const uint8_t ESP32_TX_PIN = P0;


//-------------------------------------------------
// SERVOARM
//-------------------------------------------------

const uint8_t SERVO_PIN = P1;
const uint8_t SERVO_CHANNEL = 4;

const int ARM_CLOSED_ANGLE = 120;
const int ARM_RELEASE_ANGLE = 100;

const unsigned long ARM_RELEASE_TIME = 800;
const unsigned long ARM_CLOSE_TIME = 500;


//-------------------------------------------------
// FOTO OCH HUNDDETEKTERING
//-------------------------------------------------

bool photoTaken = false;
bool showingPhoto = false;

unsigned long dogFirstSeenAt = 0;
unsigned long dogLastSeenAt = 0;

const unsigned long DOG_CONFIRM_TIME = 350;
const unsigned long DOG_LOST_TIME = 150;

const int MIN_DOG_SIZE = 25;


//-------------------------------------------------
// HUNDRÄKNARE
//-------------------------------------------------

int dogsFound = 0;


//-------------------------------------------------
// RÖSTSTYRNING
//-------------------------------------------------

const int CMD_MISSION_COMPLETE = 1;

bool missionComplete = false;


//-------------------------------------------------
// LIDAR-VARIABLER
//-------------------------------------------------

volatile float distanceMiddle;
volatile float distanceLeft;
volatile float distanceRight;

int angleCounter = 0;
int searchCounter = 0;
int searchPattern = 0;


//-------------------------------------------------
// DELNING MELLAN CPU-KÄRNOR
//-------------------------------------------------

volatile bool navigationPaused = false;
volatile bool navigationStopped = true;

TaskHandle_t navigationTaskHandle = NULL;


//-------------------------------------------------
// FUNKTIONSDEKLARATIONER
//-------------------------------------------------

void navigationTask(void *parameter);

void updateLidar();
void runOriginalLidarNavigation();

void setupServo();
void setServoAngle(int angle);
void servoArm();

void setupVoiceRecognition();
void checkMissionCompleteCommand();
void completeMission();

void dogDetected();
void finishPhotoDisplay();

void sendDogFoundToEsp32();
void sendMissionCompleteToEsp32();


//-------------------------------------------------
// SETUP
//-------------------------------------------------

void setup() {

    //---------------------------------
    // SERVO: STARTA STÄNGD
    //---------------------------------

    setupServo();


    //---------------------------------
    // K10
    //---------------------------------

    k10.begin();

    k10.initScreen(screen_dir);


    //---------------------------------
    // AI OCH KAMERA
    //---------------------------------

    ai.initAi();

    k10.initBgCamerImage();

    k10.setBgCamerImage(false);

    k10.creatCanvas();


    //---------------------------------
    // WIFI
    //---------------------------------

    WiFi.mode(WIFI_STA);

    k10.canvas->canvasText(
        "Connecting WiFi...",
        10,
        20,
        0xFFFFFF,
        k10.canvas->eCNAndENFont16,
        50,
        true
    );

    k10.canvas->updateCanvas();

    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {

        delay(500);
    }

    k10.canvas->canvasClear();

    k10.canvas->canvasText(
        "WiFi Connected!",
        10,
        20,
        0x00FF00,
        k10.canvas->eCNAndENFont16,
        50,
        true
    );

    k10.canvas->updateCanvas();

    delay(2000);


    //---------------------------------
    // SD-KORT
    //---------------------------------

    k10.initSDFile();


    //---------------------------------
    // LIVEKAMERA OCH AI
    //---------------------------------

    ai.switchAiMode(ai.NoMode);

    k10.setBgCamerImage(true);

    ai.switchAiMode(ai.Cat);


    //---------------------------------
    // MAQUEEN
    //---------------------------------

    maqueenPlus.sys_int();

    maqueenPlus.motorStop(
        maqueenPlus.ALL
    );

    maqueenPlus.setRGB(
        maqueenPlus.ALL,
        maqueenPlus.OFF
    );


    //---------------------------------
    // LIDAR
    //---------------------------------

    tof.begin();

    delay(1000);

    tof.getAllDataConfig(eObstacle);

    delay(1000);


    //---------------------------------
    // UART TILL EXTERN ESP32
    //---------------------------------

    Serial1.begin(
        115200,
        SERIAL_8N1,
        -1,
        ESP32_TX_PIN
    );

    delay(100);

    // Värmer upp UART-länken.
    // ESP32 visar detta i Serial Monitor,
    // men skickar ingen Telegram-notis för det.
    Serial1.println(
        "K10_READY"
    );

    Serial1.flush();


    //---------------------------------
    // RÖSTIGENKÄNNING
    //---------------------------------

    setupVoiceRecognition();


    //---------------------------------
    // STARTA LIDAR PÅ KÄRNA 0
    //---------------------------------

    navigationPaused = false;
    navigationStopped = true;

    xTaskCreatePinnedToCore(
        navigationTask,
        "LiDAR_navigation",
        4096,
        NULL,
        3,
        &navigationTaskHandle,
        0
    );
}


//-------------------------------------------------
// HUVUDLOOP: KÄRNA 1
//-------------------------------------------------

void loop() {

    //---------------------------------
    // RÖSTKOMMANDO: AVSLUTA UPPDRAG
    //---------------------------------

    checkMissionCompleteCommand();

    if (missionComplete) {

        delay(50);

        return;
    }


    //---------------------------------
    // FOTO VISAS
    //---------------------------------

    if (showingPhoto) {

        delay(5000);

        finishPhotoDisplay();

        return;
    }


    //---------------------------------
    // LÄS AI-DETEKTERING
    //---------------------------------

    bool dogVisible = ai.isDetectContent(
        AIRecognition::Cat
    );

    int dogWidth = ai.getCatData(
        AIRecognition::Length
    );

    int dogHeight = ai.getCatData(
        AIRecognition::Width
    );

    bool possibleDog =
        dogVisible &&
        dogWidth >= MIN_DOG_SIZE &&
        dogHeight >= MIN_DOG_SIZE;


    //---------------------------------
    // MÖJLIG HUND: STOPPA DIREKT
    //---------------------------------

    if (!photoTaken && possibleDog) {

        navigationPaused = true;

        dogLastSeenAt = millis();

        if (dogFirstSeenAt == 0) {

            dogFirstSeenAt = millis();
        }

        if (millis() - dogFirstSeenAt >=
            DOG_CONFIRM_TIME) {

            dogFirstSeenAt = 0;
            dogLastSeenAt = 0;

            dogDetected();
        }
    }


    //---------------------------------
    // FELDETEKTERING: FORTSÄTT SÖKA
    //---------------------------------

    else if (!photoTaken &&
             dogFirstSeenAt != 0 &&
             millis() - dogLastSeenAt >=
             DOG_LOST_TIME) {

        dogFirstSeenAt = 0;
        dogLastSeenAt = 0;

        navigationPaused = false;
    }

    delay(50);
}


//-------------------------------------------------
// NAVIGERING: KÄRNA 0
//-------------------------------------------------

void navigationTask(void *parameter) {

    while (true) {

        if (navigationPaused) {

            if (!navigationStopped) {

                maqueenPlus.motorStop(
                    maqueenPlus.ALL
                );

                navigationStopped = true;
            }

            vTaskDelay(
                pdMS_TO_TICKS(20)
            );

            continue;
        }

        navigationStopped = false;

        updateLidar();

        runOriginalLidarNavigation();

        vTaskDelay(
            pdMS_TO_TICKS(50)
        );
    }
}


//-------------------------------------------------
// UPPDATERA LIDAR
//-------------------------------------------------

void updateLidar() {

    tof.requestObstacleSensorData();

    distanceMiddle = tof.getDistance(eMiddle);

    distanceLeft = tof.getDistance(eLeft);

    distanceRight = tof.getDistance(eRight);
}


//-------------------------------------------------
// ORIGINAL LIDAR-NAVIGERING
//-------------------------------------------------

void runOriginalLidarNavigation() {

    searchCounter++;

    if (searchCounter > 80) {

        if (searchPattern == 0) {

            maqueenPlus.Angle_control(15, 0);
            searchPattern = 1;
        }

        else if (searchPattern == 1) {

            maqueenPlus.Angle_control(15, 0);
            searchPattern = 2;
        }

        else if (searchPattern == 2) {

            maqueenPlus.Angle_control(30, 0);
            searchPattern = 3;
        }

        else if (searchPattern == 3) {

            maqueenPlus.Angle_control(45, 0);
            searchPattern = 4;
        }

        else if (searchPattern == 4) {

            maqueenPlus.Angle_control(60, 0);
            searchPattern = 5;
        }

        else {

            maqueenPlus.Angle_control(-165, 0);
            searchPattern = 0;
        }

        searchCounter = 0;
    }


    //---------------------------------
    // PROAKTIV NAVIGERING
    //---------------------------------
   
        //---------------------------------
    // PROAKTIV NAVIGERING
    //---------------------------------

    if ((distanceMiddle < 320) &&
        (distanceMiddle > 280)) {

        if (distanceLeft > distanceRight + 120) {

            maqueenPlus.Angle_control(-25, 0);
        }

        else if (distanceRight > distanceLeft + 120) {

            maqueenPlus.Angle_control(25, 0);
        }

        // Vägg nästan rakt fram:
        // välj den sida som har minst lite mer utrymme.
        else if (distanceLeft >= distanceRight) {

            maqueenPlus.Angle_control(-25, 0);
        }

        else {

            maqueenPlus.Angle_control(25, 0);
        }
    }

    else if ((distanceMiddle < 280) &&
             (distanceMiddle > 250)) {

        if (distanceLeft > distanceRight + 120) {

            maqueenPlus.Angle_control(-45, 0);
        }

        else if (distanceRight > distanceLeft + 120) {

            maqueenPlus.Angle_control(45, 0);
        }

        else if (distanceLeft >= distanceRight) {

            maqueenPlus.Angle_control(-45, 0);
        }

        else {

            maqueenPlus.Angle_control(45, 0);
        }
    }

    else if ((distanceMiddle < 250) &&
             (distanceMiddle > 220)) {

        if (distanceLeft > distanceRight + 120) {

            maqueenPlus.Angle_control(-65, 0);
        }

        else if (distanceRight > distanceLeft + 120) {

            maqueenPlus.Angle_control(65, 0);
        }

        else if (distanceLeft >= distanceRight) {

            maqueenPlus.Angle_control(-65, 0);
        }

        else {

            maqueenPlus.Angle_control(65, 0);
        }
    }

    //---------------------------------
    // HINDER UPPTÄCKT
    //---------------------------------

    else if ((distanceMiddle < 250) &&
             (distanceMiddle > 0)) {

        searchCounter = 0;
        searchPattern = 0;

        maqueenPlus.motorStop(
            maqueenPlus.ALL
        );

        delay(150);

        maqueenPlus.motorRun(
            maqueenPlus.ALL,
            maqueenPlus.CCW,
            60
        );

        delay(350);

        maqueenPlus.motorStop(
            maqueenPlus.ALL
        );

        delay(150);

        if (angleCounter == 0) {

            if (distanceLeft > distanceRight) {

                maqueenPlus.Angle_control(-60, 0);
            }

            else {

                maqueenPlus.Angle_control(60, 0);
            }

            angleCounter = 1;
        }

        else if (angleCounter == 1) {

            if (distanceLeft > distanceRight) {

                maqueenPlus.Angle_control(-100, 0);
            }

            else {

                maqueenPlus.Angle_control(100, 0);
            }

            angleCounter = 2;
        }

        else if (angleCounter == 2) {

            if (distanceLeft > distanceRight) {

                maqueenPlus.Angle_control(-145, 0);
            }

            else {

                maqueenPlus.Angle_control(145, 0);
            }

            angleCounter = 3;
        }

        else {

            maqueenPlus.motorRun(
                maqueenPlus.ALL,
                maqueenPlus.CCW,
                60
            );

            delay(800);

            maqueenPlus.motorStop(
                maqueenPlus.ALL
            );

            delay(150);

            maqueenPlus.Angle_control(180, 0);

            angleCounter = 0;
        }
    }


    //---------------------------------
    // INGA HINDER: LÅG SÖKHASTIGHET
    //---------------------------------

    else {

        angleCounter = 0;

        if ((distanceMiddle > 500) &&
            (distanceLeft > 350) &&
            (distanceRight > 350)) {

            maqueenPlus.motorRun(
                maqueenPlus.ALL,
                maqueenPlus.CW,
                155
            );
        }

        else if ((distanceMiddle > 450) &&
                 (distanceLeft > 300) &&
                 (distanceRight > 300)) {

            maqueenPlus.motorRun(
                maqueenPlus.ALL,
                maqueenPlus.CW,
                130
            );
        }

        else if (distanceMiddle > 400) {

            maqueenPlus.motorRun(
                maqueenPlus.ALL,
                maqueenPlus.CW,
                120
            );
        }

        else if (distanceMiddle > 350) {

            maqueenPlus.motorRun(
                maqueenPlus.ALL,
                maqueenPlus.CW,
                100
            );
        }

        else if (distanceMiddle > 300) {

            maqueenPlus.motorRun(
                maqueenPlus.ALL,
                maqueenPlus.CW,
                90
            );
        }

        else {

            maqueenPlus.motorRun(
                maqueenPlus.ALL,
                maqueenPlus.CW,
                70
            );
        }
    }
}


//-------------------------------------------------
// RÖSTIGENKÄNNING: STARTA
//-------------------------------------------------

void setupVoiceRecognition() {

    asr.asrInit(
        CONTINUOUS,
        EN_MODE,
        6000
    );

    while (asr._asrState == 0) {

        delay(100);
    }

    asr.addASRCommand(
        CMD_MISSION_COMPLETE,
        "mission complete"
    );
}


//-------------------------------------------------
// RÖSTIGENKÄNNING: KONTROLLERA KOMMANDO
//-------------------------------------------------

void checkMissionCompleteCommand() {

    if (missionComplete) {

        return;
    }

    if (asr.isDetectCmdID(
            CMD_MISSION_COMPLETE
        )) {

        completeMission();
    }
}


//-------------------------------------------------
// AVSLUTA UPPDRAG
//-------------------------------------------------

void completeMission() {

    if (missionComplete) {

        return;
    }

    missionComplete = true;

    navigationPaused = true;

    while (!navigationStopped) {

        delay(10);
    }

    maqueenPlus.motorStop(
        maqueenPlus.ALL
    );

    maqueenPlus.setRGB(
        maqueenPlus.ALL,
        maqueenPlus.OFF
    );

    showingPhoto = false;
    photoTaken = false;

    k10.setBgCamerImage(false);

    k10.canvas->canvasClear();

    k10.canvas->canvasText(
        "MISSION COMPLETE!",
        10,
        60,
        0x00FF00,
        k10.canvas->eCNAndENFont16,
        220,
        true
    );

    String dogText =
        "DOGS FOUND: " +
        String(dogsFound);

    k10.canvas->canvasText(
        dogText,
        10,
        130,
        0x0000FF,
        k10.canvas->eCNAndENFont16,
        220,
        true
    );

    k10.canvas->updateCanvas();

    sendMissionCompleteToEsp32();

    music.playTFCardAudio(
        "S:/mission_complete.wav"
    );

    delay(4500);
}


//-------------------------------------------------
// HUND UPPTÄCKT
//-------------------------------------------------

void dogDetected() {

    if (photoTaken ||
        missionComplete) {

        return;
    }

    navigationPaused = true;

    while (!navigationStopped) {

        delay(10);
    }

    maqueenPlus.setRGB(
        maqueenPlus.ALL,
        maqueenPlus.ON
    );

    k10.canvas->canvasClear();

    k10.canvas->canvasText(
        "DOG DETECTED!",
        10,
        10,
        0xFF0000,
        k10.canvas->eCNAndENFont16,
        50,
        true
    );

    k10.photoSaveToTFCard(
        "S:/photo.bmp"
    );

    sendDogFoundToEsp32();

    servoArm();

    k10.canvas->canvasDrawImage(
        0,
        0,
        "S:/photo.bmp"
    );

    k10.canvas->canvasText(
        "READY TO SEND",
        10,
        200,
        0x0000FF,
        k10.canvas->eCNAndENFont16,
        50,
        true
    );

    k10.canvas->updateCanvas();

    photoTaken = true;
    showingPhoto = true;
}


//-------------------------------------------------
// SKICKA HUNDHÄNDELSE TILL ESP32
//-------------------------------------------------

void sendDogFoundToEsp32() {

    dogsFound++;

    Serial1.print(
        "DOG_FOUND,"
    );

    Serial1.println(
        dogsFound
    );

    Serial1.flush();
}


//-------------------------------------------------
// SKICKA SLUTRAPPORT TILL ESP32
//-------------------------------------------------

void sendMissionCompleteToEsp32() {

    Serial1.print(
        "MISSION_COMPLETE,"
    );

    Serial1.println(
        dogsFound
    );

    Serial1.flush();
}


//-------------------------------------------------
// SERVOARM: ÖPPNA LITE OCH STÄNG IGEN
//-------------------------------------------------

void servoArm() {

    setServoAngle(
        ARM_RELEASE_ANGLE
    );

    delay(ARM_RELEASE_TIME);

    setServoAngle(
        ARM_CLOSED_ANGLE
    );

    delay(ARM_CLOSE_TIME);
}


//-------------------------------------------------
// STARTA SERVO STÄNGD
//-------------------------------------------------

void setupServo() {

    ledcSetup(
        SERVO_CHANNEL,
        50,
        10
    );

    setServoAngle(
        ARM_CLOSED_ANGLE
    );

    ledcAttachPin(
        SERVO_PIN,
        SERVO_CHANNEL
    );

    setServoAngle(
        ARM_CLOSED_ANGLE
    );

    delay(500);
}


//-------------------------------------------------
// ÄNDRA SERVOVINKEL
//-------------------------------------------------

void setServoAngle(int angle) {

    angle = constrain(
        angle,
        0,
        180
    );

    unsigned long pulseWidth = map(
        angle,
        0,
        180,
        544,
        2400
    );

    unsigned long dutyCycle =
        (pulseWidth * 1024UL) / 20000UL;

    ledcWrite(
        SERVO_CHANNEL,
        dutyCycle
    );
}


//-------------------------------------------------
// ÅTERGÅ TILL LIVEKAMERA
//-------------------------------------------------

void finishPhotoDisplay() {

    if (missionComplete) {

        return;
    }

    k10.canvas->canvasClear();

    k10.canvas->updateCanvas();

    k10.setBgCamerImage(true);

    maqueenPlus.setRGB(
        maqueenPlus.ALL,
        maqueenPlus.OFF
    );

    photoTaken = false;
    showingPhoto = false;

    dogFirstSeenAt = 0;
    dogLastSeenAt = 0;

    navigationPaused = false;
}
