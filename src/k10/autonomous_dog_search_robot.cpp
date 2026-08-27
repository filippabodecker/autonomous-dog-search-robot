/*!
 * Autonomous Dog Search Robot — UNIHIKER K10
 *
 * Main firmware for dog detection, image capture, LiDAR navigation,
 * voice-controlled mission completion, treat dispensing and communication
 * with the external ESP32.
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
// GLOBAL OBJECTS
//-------------------------------------------------

UNIHIKER_K10 k10;
AIRecognition ai;
ASR asr;
Music music;

DFRobot_MaqueenPlusV2 maqueenPlus;

uint8_t tofAddress = 0x33;
DFRobot_matrixLidarDistanceSensor_I2C tof(tofAddress);


//-------------------------------------------------
// WI-FI CONFIGURATION
//-------------------------------------------------

const char* ssid = "xx";
const char* password = "yy";

uint8_t screen_dir = 2;


//-------------------------------------------------
// EXTERNAL ESP32: UART
//-------------------------------------------------

// Maqueen P0 connector:
// GND / 3V3 / P
//
// P0 signal (P) --> ESP32 RX2
// GND           --> ESP32 GND
const uint8_t ESP32_TX_PIN = P0;


//-------------------------------------------------
// TREAT-DISPENSING SERVO
//-------------------------------------------------

const uint8_t SERVO_PIN = P1;
const uint8_t SERVO_CHANNEL = 4;

const int ARM_CLOSED_ANGLE = 120;
const int ARM_RELEASE_ANGLE = 100;

const unsigned long ARM_RELEASE_TIME = 800;
const unsigned long ARM_CLOSE_TIME = 500;


//-------------------------------------------------
// IMAGE CAPTURE AND DOG DETECTION
//-------------------------------------------------

bool photoTaken = false;
bool showingPhoto = false;

unsigned long dogFirstSeenAt = 0;
unsigned long dogLastSeenAt = 0;

const unsigned long DOG_CONFIRM_TIME = 350;
const unsigned long DOG_LOST_TIME = 150;

const int MIN_DOG_SIZE = 25;


//-------------------------------------------------
// DETECTION COUNTER
//-------------------------------------------------

int dogsFound = 0;


//-------------------------------------------------
// VOICE CONTROL
//-------------------------------------------------

const int CMD_MISSION_COMPLETE = 1;

bool missionComplete = false;


//-------------------------------------------------
// LIDAR STATE
//-------------------------------------------------

volatile float distanceMiddle;
volatile float distanceLeft;
volatile float distanceRight;

int angleCounter = 0;
int searchCounter = 0;
int searchPattern = 0;


//-------------------------------------------------
// SHARED STATE BETWEEN CPU CORES
//-------------------------------------------------

volatile bool navigationPaused = false;
volatile bool navigationStopped = true;

TaskHandle_t navigationTaskHandle = NULL;


//-------------------------------------------------
// FUNCTION DECLARATIONS
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
// SYSTEM INITIALIZATION
//-------------------------------------------------

void setup() {

    //---------------------------------
    // INITIALIZE SERVO IN CLOSED POSITION
    //---------------------------------

    setupServo();


    //---------------------------------
    // K10
    //---------------------------------

    k10.begin();

    k10.initScreen(screen_dir);


    //---------------------------------
    // AI AND CAMERA
    //---------------------------------

    ai.initAi();

    k10.initBgCamerImage();

    k10.setBgCamerImage(false);

    k10.creatCanvas();


    //---------------------------------
    // WI-FI CONNECTION
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
    // SD CARD
    //---------------------------------

    k10.initSDFile();


    //---------------------------------
    // LIVE CAMERA AND AI
    //---------------------------------

    ai.switchAiMode(ai.NoMode);

    k10.setBgCamerImage(true);

    ai.switchAiMode(ai.Cat);


    //---------------------------------
    // MAQUEEN PLUS V3
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
    // MATRIX LIDAR
    //---------------------------------

    tof.begin();

    delay(1000);

    tof.getAllDataConfig(eObstacle);

    delay(1000);


    //---------------------------------
    // UART TO EXTERNAL ESP32
    //---------------------------------

    Serial1.begin(
        115200,
        SERIAL_8N1,
        -1,
        ESP32_TX_PIN
    );

    delay(100);

    // Warm up the UART link.
    // The ESP32 prints this message to its Serial Monitor
    // without sending a Telegram notification.
    
    Serial1.println(
        "K10_READY"
    );

    Serial1.flush();


    //---------------------------------
    // VOICE RECOGNITION
    //---------------------------------

    setupVoiceRecognition();


    //---------------------------------
    // START LIDAR TASK ON CORE 0
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
// MAIN LOOP: CORE 1
//-------------------------------------------------

void loop() {

    //---------------------------------
    // VOICE COMMAND: COMPLETE MISSION
    //---------------------------------

    checkMissionCompleteCommand();

    if (missionComplete) {

        delay(50);

        return;
    }


    //---------------------------------
    // DISPLAY CAPTURED IMAGE
    //---------------------------------

    if (showingPhoto) {

        delay(5000);

        finishPhotoDisplay();

        return;
    }


    //---------------------------------
    // READ AI DETECTION
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
    // POTENTIAL DOG: PAUSE NAVIGATION
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
    // UNCONFIRMED DETECTION: RESUME SEARCH
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
// NAVIGATION TASK: CORE 0
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
// UPDATE LIDAR MEASUREMENTS
//-------------------------------------------------

void updateLidar() {

    tof.requestObstacleSensorData();

    distanceMiddle = tof.getDistance(eMiddle);

    distanceLeft = tof.getDistance(eLeft);

    distanceRight = tof.getDistance(eRight);
}


//-------------------------------------------------
// LIDAR NAVIGATION LOGIC
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
    // PROACTIVE NAVIGATION
    //---------------------------------
   

    if ((distanceMiddle < 320) &&
        (distanceMiddle > 280)) {

        if (distanceLeft > distanceRight + 120) {

            maqueenPlus.Angle_control(-25, 0);
        }

        else if (distanceRight > distanceLeft + 120) {

            maqueenPlus.Angle_control(25, 0);
        }

        // If a wall is almost directly ahead,
        // turn toward the side with slightly more free space.    
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
    // OBSTACLE DETECTED
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
    // CLEAR PATH: LOW SEARCH SPEED
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
// INITIALIZE VOICE RECOGNITION
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
// CHECK MISSION-COMPLETE COMMAND
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
// COMPLETE MISSION
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
// HANDLE CONFIRMED DOG DETECTION
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
// SEND DOG-DETECTION EVENT TO ESP32
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
// SEND MISSION SUMMARY TO ESP32
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
// ACTUATE TREAT DISPENSER
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
// INITIALIZE SERVO IN CLOSED POSITION
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
// SET SERVO ANGLE
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
// RETURN TO LIVE CAMERA
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
