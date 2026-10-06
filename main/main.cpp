#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "LittleFS.h"       // for using littleFS file system on esp32
#include "WiFi.h"  
#include "mqtt_client.h"
#include "ArduinoJson.h"
#include "time.h"           //for connecting to NTP server
#include "Wire.h"           // for I2C
#include "RTClib.h"         // for interacting with RS3231
#include "secrets.h"
static_assert(__cplusplus >= 201703L, "C++17 not enabled"); // confirm the use of c++17

#define SCHEDULE_ARRAY_SIZE 50
#define BUFFER_SIZE 200


typedef enum{
  NOTIFY_SCHEDULE_UPDATED = 0b0001,
  ALARM_TRIGGERED = 0b0010  // important: each value has unique bit to avoid conflict (ex: use 0b0011 activate 0b0001 and 0b0010)
} scheduleTask_enum;

struct Schedule{
  long long id;   // use 64 bit number to avoid overflow in 2038
  time_t startTime;
  uint16_t duration;
  uint16_t interval;
};

TaskHandle_t ScheduleTaskHandle = NULL;
Schedule scheduleArray[SCHEDULE_ARRAY_SIZE];

// WiFi related variables
// ensure WiFi use 2,4 GHz and use WPA2 for compatibility
const char* wifi_ssid = WIFI_SSID;
const char* wifi_password = WIFI_PASSWORD;
char receivedData[BUFFER_SIZE];

// MQTT related variables
esp_mqtt_client_handle_t mqttClient;

// NTP related variables
const char* ntp_server = "pool.ntp.org";
const long gmt_offset = 25200; // GMT+7 (3600 x 7)
const int daylight_offset = 0;

// LittleFS related variables
const char* schedule_file = "/schedule.txt";
const char* temp_file = "/temp.txt";
const char* web_file = "/index.html";

// RS3231 related variables
RTC_DS3231 rtc;
DateTime currentDateTime;
const uint8_t sqw_pin = 7;
const uint8_t sda_pin = 8;
const uint8_t scl_pin = 9;
const uint8_t rtc_addr = 0x68;

// Device related variables
const uint8_t devicePin = 16;
volatile bool deviceOn = false;


//==========================================================================================//


// Device related functions

// Turn on device and notify client via MQTT
void TurnOnDevice()
{
  digitalWrite(devicePin, HIGH);
  deviceOn = true;
  esp_mqtt_client_publish(mqttClient, "ras/status", "true", 4, 1, 1);
}

// Turn off device and notify client via MQTT
void TurnOffDevice()
{
  digitalWrite(devicePin, LOW);
  deviceOn = false;
  esp_mqtt_client_publish(mqttClient, "ras/status", "false", 5, 1, 1);
} 


//==========================================================================================//


// Network related functions

// Initialize Wi-Fi connection
void InitWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.println(String(WiFi.status()));
    delay(1000);
  }
  Serial.println("Connected to : " + String(wifi_ssid));
  delay(1000);
}


//==========================================================================================//


// File system related functions

// Initialize LittleFS
void InitLittleFS()
{
  while (!LittleFS.begin(false))  // do not immedietly format upon mounting fail
  {
    Serial.println("LittleFS Failed To Initialized");
    delay(1000);
  } 
  Serial.println("LittleFS Initialized");
  delay(1000);

  // sanity check
  File testFile = LittleFS.open("/test.txt", "r");
  if (!testFile)
  {
    Serial.println("Test File Missing Or Can't Be Opened");
  }
  Serial.print("Test file : ");
  while (testFile.available())
  {
    Serial.write(testFile.read());  // file.read() get raw byte one at time, serial.write() display the byte (work with byte only)
  }
  Serial.print("\n");
  testFile.close();
  delay(1000);
}

// Add schedule to txt file (0 success, -1 schedule file open error)
int AddSchedule(JsonDocument& scheduleDoc)
{
  File scheduleFile = LittleFS.open(schedule_file, "a");
  if (!scheduleFile)
  {
    return -1;
  }
  serializeJson(scheduleDoc, scheduleFile);
  scheduleFile.close();
  Serial.println("Schedule : " + String(scheduleDoc["id"].as<const char*>()) + " successfully added!");
  return 0;
}

// Remove schedule from txt file (0 success, 1 schedule not found, -1 schedule file open error, -2 temp file open error)
int RemoveSchedule(long long idToDelete)
{
  File original = LittleFS.open(schedule_file, "r");
  if (!original)
  {
    return -1;
  }
  File temp = LittleFS.open(temp_file, "w");
  if (!temp)
  {
    return -2;
  }

  bool isThereMatch = false;
  while (original.available())
  {
    JsonDocument scheduleDoc;
    DeserializationError error = deserializeJson(scheduleDoc, original);
    if (error)
    {
      break;  
    }
    if (strtoll(scheduleDoc["id"].as<const char*>(), NULL, 10) == idToDelete)
    {
      isThereMatch = true;
    }
    else
    {
      String scheduleString;
      serializeJson(scheduleDoc, scheduleString);
      temp.println(scheduleString);
    }
  }
  original.close();
  temp.close();
  if (!isThereMatch)
  {
    LittleFS.remove(temp_file);
    Serial.println("Schedule : " + String(idToDelete) + " not found!");
    return 1;
  }
  LittleFS.remove(schedule_file);
  LittleFS.rename(temp_file, schedule_file);
  Serial.println("Schedule : " + String(idToDelete) + " successfully removed!");
  return 0;
}

// Load schedule from txt file into scheduleArray and apply gmt_offset to startTime
int LoadScheduleToArray()
{
  int c = 0;
  memset(scheduleArray, 0, sizeof(scheduleArray));
  File scheduleFile = LittleFS.open(schedule_file, "r");
  if (!scheduleFile)
  {
    return -1;
  }
  while (scheduleFile.available() && c < SCHEDULE_ARRAY_SIZE)
  {
    JsonDocument scheduleDoc;
    DeserializationError error = deserializeJson(scheduleDoc, scheduleFile);
    if(error)
    {
      break;
    }
    Serial.println("schedule id from file = " + String(scheduleDoc["id"].as<const char*>()));
    scheduleArray[c].id = strtoll(scheduleDoc["id"].as<const char*>(), NULL, 10);
    Serial.println("schedule id in array = " + String(scheduleArray[c].id));
    scheduleArray[c].startTime = scheduleDoc["startTime"].as<time_t>() + gmt_offset;
    scheduleArray[c].duration = scheduleDoc["duration"].as<uint16_t>();
    scheduleArray[c].interval = scheduleDoc["interval"].as<uint16_t>();
    c++;
  }
  scheduleFile.close();
  return 0;
}


//==========================================================================================//


// NTP related functions

// Initialize NTP connection
void InitNTP()
{
  configTime(gmt_offset, daylight_offset, ntp_server);
  Serial.println("NTP Initializdd, Using GMT+" + String(gmt_offset / 3600));
  delay(1000);

  // sanity check
  struct tm dateTime;
  getLocalTime(&dateTime);
  Serial.println(
    String("From NTP : ") + 
    dateTime.tm_year + String(":") + 
    dateTime.tm_mon + String(":") +
    dateTime.tm_mday + String(":") +
    dateTime.tm_wday + String(" ; ") +
    dateTime.tm_hour + String(":") + 
    dateTime.tm_min
  );
  delay(1000);
}


//==========================================================================================//


// DS3231 related functions
void InitRTC()
{
  while (!Wire.begin(sda_pin, scl_pin, 100000))
  {
    Serial.println("I2C Failed To Initialized");
    delay(1000);
  }
  Serial.println("I2C Initialized");

  while (!rtc.begin())
  {
    Serial.println("RTC Not Found");
    delay(1000);
  }
  Serial.println("RTC Initialized");
  rtc.disable32K();

  struct tm dateTime;
  getLocalTime(&dateTime);
  time_t dateTimeEpoch = mktime(&dateTime); // convert to absolute time in seconds
  dateTimeEpoch += gmt_offset;    // applied back offset
  rtc.adjust(DateTime(dateTimeEpoch));
  delay(1000);

  // sanity check
  currentDateTime = rtc.now();
  Serial.println(
    String("from RTC : ") + 
    currentDateTime.year() + String(":") + 
    currentDateTime.month() + String(":") +
    currentDateTime.day() + String(":") +
    currentDateTime.dayOfTheWeek() + String(" ; ") +
    currentDateTime.hour() + String(":") + 
    currentDateTime.minute()
  );
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  rtc.writeSqwPinMode(DS3231_OFF);
  rtc.disable32K();
  delay(1000);
}


//==========================================================================================//


// Utility Functions
bool isScheduleArrayEmpty(const Schedule* scheduleArray)
{
  for (int i = 0; i < SCHEDULE_ARRAY_SIZE; i++)
  {
    if (scheduleArray[i].startTime != 0)
    {
      return false;
    }
  }
  return true;
}

int GetClosestScheduleIndex()
{
  if (isScheduleArrayEmpty(scheduleArray) == true)
  {
    return -1;
  }
  int ClosestScheduleIndex = 0;
  for (int i = 0; i < SCHEDULE_ARRAY_SIZE; i++)
  {
    if (scheduleArray[i].startTime != 0 && scheduleArray[i].startTime < scheduleArray[ClosestScheduleIndex].startTime)      
    {
      ClosestScheduleIndex = i;
    }
  }
  Serial.println("Closest Schedule : ");
  Serial.println(String(scheduleArray[ClosestScheduleIndex].id) + "|||" + String(scheduleArray[ClosestScheduleIndex].startTime) + "|||" + String(scheduleArray[ClosestScheduleIndex].duration));
  return ClosestScheduleIndex;
}

void SetAlarmDuration(int ClosestScheduleIndex)
{
  // set duration to alarm1
  time_t finishTimeEpoch = scheduleArray[ClosestScheduleIndex].startTime + scheduleArray[ClosestScheduleIndex].duration;
  DateTime finishTime = DateTime(finishTimeEpoch);
  char buffer2[] = "YYYY/MM/DD hh:mm:ss";
  Serial.println(finishTime.toString(buffer2));
  rtc.setAlarm1(finishTime, DS3231_A1_Date);
  delay(10);
  rtc.clearAlarm(1);
}

void SetAlarm()
{
  if (isScheduleArrayEmpty(scheduleArray) == true)
  {
    rtc.clearAlarm(1);
    rtc.clearAlarm(2);
    Serial.println("No Schedule, Alarm Cleared");
  }
  int ClosestScheduleIndex = GetClosestScheduleIndex();

  // set start time to alarm2
  DateTime startTime = DateTime(scheduleArray[ClosestScheduleIndex].startTime);
  char buffer[] = "YYYY/MM/DD hh:mm:ss";
  Serial.println(startTime.toString(buffer));
  rtc.setAlarm2(startTime, DS3231_A2_Date);
  delay(10);
  rtc.clearAlarm(2);

  SetAlarmDuration(ClosestScheduleIndex);
}

// Decide what to do after schedule is done
void AfterScheduleHandle(int ClosestScheduleIndex)
{
  int removeStatus = RemoveSchedule(scheduleArray[ClosestScheduleIndex].id);
  Serial.println("AfterScheduleHandle->RemoveSchedule status : " + String(removeStatus));
  // interval is already stored in seconds (converted by the web client before sending)
  scheduleArray[ClosestScheduleIndex].startTime += scheduleArray[ClosestScheduleIndex].interval;
  // subtract gmt_offset before storing: LoadScheduleToArray always adds it back, so the file
  // must always contain the raw epoch (same format the web client originally sends)
  Schedule toStore = scheduleArray[ClosestScheduleIndex];
  toStore.startTime -= gmt_offset;
  JsonDocument scheduleDoc;
  scheduleDoc["id"] = String(toStore.id);
  scheduleDoc["startTime"] = String(toStore.startTime);
  scheduleDoc["duration"] = String(toStore.duration);
  scheduleDoc["interval"] = String(toStore.interval);
  int addStatus = AddSchedule(scheduleDoc);
  Serial.println("AfterScheduleHandle->AddSchedule status : " + String(addStatus));
}


//==========================================================================================//


// MQTT related functions
static void MqttEventHandler(void* event_handler_args, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  switch ((esp_mqtt_event_id_t)event_id)
  {
    case MQTT_EVENT_CONNECTED:
      Serial.println("MQTT Connected");
      esp_mqtt_client_subscribe(mqttClient, "ras/schedule/add", 1);
      esp_mqtt_client_subscribe(mqttClient, "ras/schedule/delete", 1);
      esp_mqtt_client_subscribe(mqttClient, "ras/schedule/get", 1);
      break;

    case MQTT_EVENT_DISCONNECTED:
      Serial.println("MQTT Disconnected");
      break;

    case MQTT_EVENT_DATA:
    {
      if (event->data_len >= BUFFER_SIZE)
      {
        Serial.println("MQTT data too large");
        break;
      }
      memcpy(receivedData, event->data, event->data_len);
      receivedData[event->data_len] = '\0';

      if (strncmp(event->topic, "ras/schedule/add", event->topic_len) == 0)
      {
        JsonDocument scheduleDoc;
        DeserializationError error = deserializeJson(scheduleDoc, receivedData);
        if (!error)
        {
          AddSchedule(scheduleDoc);
          xTaskNotify(ScheduleTaskHandle, NOTIFY_SCHEDULE_UPDATED, eSetBits);
        }
      }
      else if (strncmp(event->topic, "ras/schedule/delete", event->topic_len) == 0)
      {
        RemoveSchedule(strtoll(receivedData, NULL, 10));
        xTaskNotify(ScheduleTaskHandle, NOTIFY_SCHEDULE_UPDATED, eSetBits);
      }
      else if (strncmp(event->topic, "ras/schedule/get", event->topic_len) == 0)
      {
        File scheduleFile = LittleFS.open(schedule_file, "r");
        if (scheduleFile)
        {
          String content = scheduleFile.readString();
          scheduleFile.close();
          esp_mqtt_client_publish(mqttClient, "ras/schedule/list", content.c_str(), content.length(), 1, 0);
        }
      }
      break;
    }

    case MQTT_EVENT_PUBLISHED:
      Serial.println("MQTT Published msg_id=" + String(event->msg_id));
      break;

    case MQTT_EVENT_ERROR:
      Serial.println("MQTT Error type=" + String(event->error_handle->error_type));
      break;

    default:
      break;
  }
}

void InitMqttClient()
{
  esp_mqtt_client_config_t mqttConfig= {
    .uri = MQTT_BROKER,
    .username = MQTT_USERNAME,
    .password = MQTT_PASSWORD,
    .cert_pem = MQTT_CA_CERT
  };
  mqttClient = esp_mqtt_client_init(&mqttConfig);
  esp_mqtt_client_register_event(mqttClient, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, MqttEventHandler, NULL);
  esp_mqtt_client_start(mqttClient);
}


//==========================================================================================//


void IRAM_ATTR onAlarmISR()
{
  if(digitalRead(sqw_pin) == LOW)
  {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    // important: eSetBits will append multiple bits into one atomic state (bitwise accumulation, ex: 0b0001 and 0b0010 sent back-to-back is the same as 0b0011 sent alone)
    xTaskNotifyFromISR(ScheduleTaskHandle, ALARM_TRIGGERED, eSetBits, &xHigherPriorityTaskWoken); 
  }
}

// Handle RTC alarm setting and time keeping
void ScheduleTask(void* pvParams)
{
  // initial load and set alarm so system can work when restarted
  int initialLoadStatus = LoadScheduleToArray();
  Serial.println("Initial LoadScheduleToArray status : " + String(initialLoadStatus));
  SetAlarm();

  uint32_t notifValue;
  while(true)
  {
    if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &notifValue, portMAX_DELAY) == pdPASS)
    {
      if (notifValue & NOTIFY_SCHEDULE_UPDATED) 
      {
        int loadStatus = LoadScheduleToArray();
        Serial.println("ScheduleTask->NOTIFY_SCHEDULE_UPDATED->LoadScheduleToArray status : " + String(loadStatus));
        SetAlarm();
        Serial.println("Read Schedules");
      }
      
      if (notifValue & ALARM_TRIGGERED)
      {
        int ClosestScheduleIndex = GetClosestScheduleIndex();
        DateTime scheduled = DateTime(scheduleArray[ClosestScheduleIndex].startTime);
        DateTime now = rtc.now();

        if (rtc.alarmFired(2))
        {
          if (now.year() == scheduled.year() && now.month() == scheduled.month())
          {
            TurnOnDevice();
            Serial.println("Device Turned On");
          }
          else
          {
            Serial.println("Alarm 2 fired: month/year mismatch, skipping activation");
          }
          rtc.clearAlarm(2);
        }
        else if (rtc.alarmFired(1))
        {
          if (now.year() == scheduled.year() && now.month() == scheduled.month())
          {
            TurnOffDevice();
            Serial.println("Device Turned Off");
            Serial.println(scheduleArray[ClosestScheduleIndex].id);
            AfterScheduleHandle(ClosestScheduleIndex);
            int loadStatus = LoadScheduleToArray();
            Serial.println("ScheduleTask->ALARM_TRIGGERED->LoadScheduleToArray status : " + String(loadStatus));
            SetAlarm();
          }
          else
          {
            Serial.println("Alarm 1 fired: month/year mismatch, skipping");
          }
          rtc.clearAlarm(1);
        }
      }
    }
  }
}


//==========================================================================================//


void setup() 
{
  Serial.begin(115200);
  InitWifi();
  InitLittleFS();
  InitNTP();
  InitRTC();
  InitMqttClient();
  pinMode(devicePin, OUTPUT);
  pinMode(sqw_pin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(sqw_pin), onAlarmISR, FALLING);

  xTaskCreatePinnedToCore(
    ScheduleTask,
    "Schedule Task",
    4096,
    NULL,
    1,
    &ScheduleTaskHandle,
    1
  );
}

void loop() 
{ 
}