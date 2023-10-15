#include <WiFi.h>
#include <ESP32Servo.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebSrv.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include "SPIFFS.h"
#include <queue>
#include "AdafruitIO_WiFi.h"

// Adafruit IO credentials
#define IO_USERNAME "<ADAFRUIT USERNAME>"
#define IO_KEY "<ADAFRUIT KEY>"
// WiFi credentials
#define WIFI_SSID "<WIFI SSID>"
#define WIFI_PASS "<WIFI PASSWORD>"

// Connects ESP32 to Adafruit IO service over WiFi
AdafruitIO_WiFi io(IO_USERNAME, IO_KEY, WIFI_SSID, WIFI_PASS);

// Set up Adafruit feeds
AdafruitIO_Feed *temperatureFeed = io.feed("temperature");
AdafruitIO_Feed *motorRunTimeFeed = io.feed("motorRunTime");
AdafruitIO_Feed *raiseMotorFeed = io.feed("raise_motor");
AdafruitIO_Feed *lowerMotorFeed = io.feed("lower_motor");
AdafruitIO_Feed *rainStateFeed = io.feed("rain_state");
AdafruitIO_Feed *coverPositionFeed = io.feed("cover_position");
AdafruitIO_Feed *automaticMotorFeed = io.feed("automatic_motor");

// ESP32 pins
int POWER_PIN = 4;    // provides power to the rain sensor
int RAIN_PIN = 16;    // digital pin connected to TTL pin of the rain sensor
int TEMP_PIN = 34;    // analog pin connected to VOUT pin of the temp sensor
int SERVO1_PIN = 2;   // digital pin connected to motor 1
int SERVO2_PIN = 13;  // digital pin connected to motor 2

// Create servo objects to control servo motors
Servo servo1;  // motor 1 - controls the right side
Servo servo2;  // motor 2 - controls the left side

// Set the raised and lowered positions (in degrees) of each motor
// Right motor
int RAISE_POS_R = 0;
int LOWER_POS_R = 90;
// Left motor
int RAISE_POS_L = 180;
int LOWER_POS_L = 90;

int coverPosition = 0;

unsigned long motorStartTime = 0;  // Stores the time when motor started working
bool motorWasOn = false;           // Indicates if motor was previously ON or OFF
bool manualControl = false;        // Variable to determine whether motors are controlled by the user or automatically responds to rain

int dataSendCounter = 0;    // Increments every 5 seconds
int maxDataSendCount = 12;  // Sends data every 1 minute (can be adjusted)

AsyncWebServer server(80);  // Initialise an asynchronous web server on port 80

WiFiUDP ntpUDP;                // Create a UDP connection object for NTP synchronisation
NTPClient timeClient(ntpUDP);  // Initialize an NTPClient object using the UDP connection

const int MAX_DATA_POINTS = 30;
String recentDates[MAX_DATA_POINTS];
float recentTemperatures[MAX_DATA_POINTS];
int currentDataCount = 0;

// Structure to store temperature statistics
struct TemperatureStatistics {
  String timeRange;
  float maxTemperature;
  float minTemperature;
  float avgTemperature;
  float variance;
  float stdDeviation;
  String recommendation;
};

// Declare functions (function definitions can be found later in the code)
void controlServos(String position);
float readTempSensor();
String getCurrentDate();
int readRainSensor();
void rainControlServo(int rain_state);
void recordSensorData(int rain_state);
void addTemperatureData(float temperature);
void saveTempDataToFile();
void saveRainDataToFile(String data);
String getSavedTempData();
String getSavedRainData();
TemperatureStatistics calculateStatistics(String data);

// Setup function, runs once at the start
void setup() {
  Serial.begin(115200);

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to WiFi...");
    delay(5000);
  }
  Serial.println("Connected to WiFi");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // Connect to Adafruit IO
  io.connect();
  while (io.status() < AIO_CONNECTED) {
    Serial.println("Connecting to AdaFruit IO...");
    delay(5000);
  }
  Serial.println("Connected to AdaFruit IO...");

  // Configure GPIO pins
  pinMode(POWER_PIN, OUTPUT);  // configure the power pin pin as an OUTPUT
  pinMode(RAIN_PIN, INPUT);    // configure DO pin as an INPUT

  // Set pulse width of motors to a standard 50hz
  servo1.setPeriodHertz(50);
  servo2.setPeriodHertz(50);

  // Attach each servo on their pins to each servo object
  // using a min/max of 500us and 2400us for an accurate 0 to 180 sweep
  servo1.attach(SERVO1_PIN, 500, 2400);
  servo2.attach(SERVO2_PIN, 500, 2400);

  // Set servo motors to the raised position upon start up
  controlServos("raise");

  // Attach message handlers to AdaFruit IO feeds
  raiseMotorFeed->onMessage(handleRaiseMotor);
  lowerMotorFeed->onMessage(handleLowerMotor);
  automaticMotorFeed->onMessage(handleAutomaticMotor);

  // Initialise SPIFFS for file storage
  timeClient.begin();

  if (!SPIFFS.begin(true)) {
    Serial.println("An error occurred while mounting SPIFFS");
    return;
  }

  // Define routes for the web server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = generateHTML();
    request->send(200, "text/html", html);
  });

  server.on("/raise", HTTP_GET, [](AsyncWebServerRequest *request) {
    controlServos("raise");
    manualControl = true;
    request->send(200, "text/plain", "Cover raised");
  });

  server.on("/lower", HTTP_GET, [](AsyncWebServerRequest *request) {
    controlServos("lower");
    manualControl = true;
    request->send(200, "text/plain", "Cover lowered");
  });

  server.on("/automatic", HTTP_GET, [](AsyncWebServerRequest *request) {
    manualControl = false;
    request->send(200, "text/plain", "Cover will be automatically controlled");
  });

  server.on("/refresh", HTTP_GET, [](AsyncWebServerRequest *request) {
    SPIFFS.remove("/temperature.txt");
    SPIFFS.remove("/rain.txt");
    // File tempFile = SPIFFS.open("/temperature.txt", "w");  // Open temperature.txt in write mode
    // File rainFile = SPIFFS.open("/rain.txt", "w");         // Open rain.txt in write mode

    // tempFile.println("");  // Write an empty string to clear temperature.txt
    // rainFile.println("");  // Write an empty string to clear rain.txt
    // tempFile.close();  // Close the temperature.txt file
    // rainFile.close();  // Close the rain.txt file
    request->send(200, "text/plain", "Data refreshed");
  });

  server.on("/getData", HTTP_GET, [](AsyncWebServerRequest *request) {
    float temperature = readTempSensor();
    String currentDate = getCurrentDate();
    String jsonResponse = "{\"temperature\": " + String(temperature) + ", \"time\": \"" + currentDate + "\"}";
    request->send(200, "application/json", jsonResponse);
  });

  // Start the web server
  server.begin(); 
}

void loop() {
  io.run();
  int rain_state = readRainSensor();
  if (dataSendCounter >= maxDataSendCount) {
    recordSensorData(rain_state, coverPosition);
    dataSendCounter = 0;  // Reset the counter
  }
  if (!manualControl) {
    rainControlServo(rain_state);
  }
  dataSendCounter++;
  delay(1000);
}

float readTempSensor() {
  int tempVal = analogRead(TEMP_PIN);
  float volts = tempVal / 1023.0;
  float temp = (volts - 0.5) * 100;

  addTemperatureData(temp);

  return temp;
}

int readRainSensor() {
  digitalWrite(POWER_PIN, HIGH);  // turn the rain sensor's power  ON
  delay(10);                      // wait 10 milliseconds

  int rain_state = digitalRead(RAIN_PIN);  // HIGH indicates rain, LOW indicates no rain

  digitalWrite(POWER_PIN, LOW);  // turn the rain sensor's power OFF

  return rain_state;
}

// Controls servo motors based on the rain sensor reading
void rainControlServo(int rain_state) {
  // Lower the cover if it is raining
  if (rain_state == HIGH) {
    controlServos("lower");
    // Raise the cover if it is not raining
  } else {
    controlServos("raise");
  }
}

void recordSensorData(int rain_state, int coverPosition) {
  float temp = readTempSensor();
  temperatureFeed->save(temp);
  // If it is raining
  if (rain_state == HIGH) {
    rainStateFeed->save(1);
    if (!motorWasOn) {  // If motor was OFF previously, record the start time
      motorStartTime = millis();
      motorWasOn = true;
    }
  }
  // If it is not raining
  else {
    rainStateFeed->save(0);
    if (motorWasOn) {  // If motor was ON previously, calculate runtime and upload
      unsigned long motorRunTime = millis() - motorStartTime;
      motorWasOn = false;

      String timeDuring = String(motorRunTime / 1000.0f);
      // Convert run time from milliseconds to seconds before saving
      // motorRunTimeFeed->save(timeDuring);

      String currentDate = getCurrentDate();

      String rainData = currentDate + ',' + timeDuring;

      saveRainDataToFile(rainData);
    }
  }
  coverPositionFeed->save(coverPosition);
}

void controlServos(String position) {
  if (position == "raise") {
    // set both servo motors to the raised position
    servo1.write(RAISE_POS_R);
    servo2.write(RAISE_POS_L);
    coverPosition = 0;
  } else if (position == "lower") {
    // set both servo motors to the lowered position
    servo1.write(LOWER_POS_R);
    servo2.write(LOWER_POS_L);
    coverPosition = 1;
  }
}

// Raise the cover thorugh AdaFruit IO
void handleRaiseMotor(AdafruitIO_Data *data) {
  String value = data->value();
  if (value == "ON" || value == "1") {
    controlServos("raise");
    manualControl = true;
  }
}

// Lower the cover thorugh AdaFruit IO
void handleLowerMotor(AdafruitIO_Data *data) {
  String value = data->value();
  if (value == "ON" || value == "1") {
    controlServos("lower");
    manualControl = true;
  }
}

// Make the rain cover raise or lower depending on rain thorugh AdaFruit IO
void handleAutomaticMotor(AdafruitIO_Data *data) {
  String value = data->value();
  if (value == "ON" || value == "1") {
    manualControl = false;
  }
}

String generateHTML() {
  String savedTempData = getSavedTempData();
  String savedRainData = getSavedRainData();
  String debugInitialTempData = "<pre>" + savedTempData + "</pre>";
  String debugInitialRainData = "<pre>" + savedRainData + "</pre>";
  TemperatureStatistics stats = calculateStatistics(savedTempData);
  String getTimeRange = stats.timeRange;
  String getMaxTemp = String(stats.maxTemperature);
  String getMinTemp = String(stats.minTemperature);
  String getAvgTemp = String(stats.avgTemperature);
  String getVariance = String(stats.variance);
  String getStdDeviation = String(stats.stdDeviation);
  String getRecommendation = stats.recommendation;

  String html = "<html>"
                "<head>"
                "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"
                "<style>"
                ".button {"
                "  display: inline-block;"
                "  padding: .75rem 1.25rem;"
                "  border-radius: 10rem;"
                "  color: #fff;"
                "  text-transform: uppercase;"
                "  font-size: 1rem;"
                "  letter-spacing: .15rem;"
                "  transition: all .3s;"
                "  position: relative;"
                "  overflow: hidden;"
                "  z-index: 1;"
                "}"
                ".button:after {"
                "  content: '';"
                "  position: absolute;"
                "  bottom: 0;"
                "  left: 0;"
                "  width: 100%;"
                "  height: 100%;"
                "  background-color: #007bff;"
                "  border-radius: 10rem;"
                "  z-index: -2;"
                "}"
                ".button:before {"
                "  content: '';"
                "  position: absolute;"
                "  bottom: 0;"
                "  left: 0;"
                "  width: 0%;"
                "  height: 100%;"
                "  background-color: #0056b3;"
                "  transition: all .3s;"
                "  border-radius: 10rem;"
                "  z-index: -1;"
                "}"
                ".button:hover {"
                "  color: #fff;"
                "}"
                ".button:hover:before {"
                "  width: 100%;"
                "}"

                "@keyframes glowingtableheader {"
                "  0% { background-position: 0 0; }"
                "  50% { background-position: 400% 0; }"
                "  100% { background-position: 0 0; }"
                "}"
                "@keyframes combinedAnimation {"
                "  0% { transform: translateX(-100%) scale(0) rotate(-90deg); opacity: 0; }"
                "  100% { transform: translateX(0) scale(1) rotate(0deg); opacity: 1; }"
                "}"
                ".fancy-table {"
                "  width: 60%;"
                "  border-collapse: collapse;"
                "  margin: 25px 0;"
                "  font-size: 18px;"
                "  text-align: left;"
                "  animation: combinedAnimation 1.5s ease-in-out;"
                "}"
                ".fancy-table th, .fancy-table td {"
                "  padding: 10px 20px;"
                "}"
                ".fancy-table th {"
                "  background: linear-gradient(45deg, #ff0000, #ff7300, #fffb00, #48ff00, #00ffd5, #002bff, #7a00ff, #ff00c8, #ff0000);"
                "  background-size: 400%;"
                "  color: white;"
                "  animation: glowingtableheader 30s linear infinite;"
                "}"
                ".fancy-table tr {"
                "  border-bottom: 1px solid #dddddd;"
                "}"
                ".fancy-table tr:last-of-type {"
                "  border-bottom: 2px solid #009879;"
                "}"
                ".fancy-table tr:hover {"
                "  background-color: #f1f1f1;"
                "}"
                "</style>"
                "</head>"
                "<body>"
                "<button class='button' onclick='location.href=\"/raise\"'>Raise</button>"
                "<button class='button' onclick='location.href=\"/lower\"'>Lower</button>"
                "<button class='button' onclick='location.href=\"/automatic\"'>Automatic</button> "
                + debugInitialTempData + "<div style=\"width: 1000px; height: 400px;\">"
                                         "  <canvas id='myChart'></canvas>"
                                         "</div>"
                                         "<table class='fancy-table' border='1'>"
                                         "<tr><th>Metric</th><th>Value</th></tr>"
                                         "<tr><td>Time Range</td><td>"
                + getTimeRange + "</td></tr>"
                                 "<tr><td>Max Temperature</td><td>"
                + getMaxTemp + "</td></tr>"
                               "<tr><td>Min Temperature</td><td>"
                + getMinTemp + "</td></tr>"
                               "<tr><td>Average Temperature</td><td>"
                + getAvgTemp + "</td></tr>"
                               "<tr><td>Variance</td><td>"
                + getVariance + "</td></tr>"
                                "<tr><td>Standard Deviation</td><td>"
                + getStdDeviation + "</td></tr>"
                                    "</table>"
                                    "<button class='button' onclick='location.href=\"/refresh\"'>Refresh Data</button> "
                + getRecommendation + debugInitialRainData + "<div style=\"width: 1000px; height: 400px;\">"
                                                             "  <canvas id='myChart2'></canvas>"
                                                             "</div>"
                                                             "<script>"
                                                             "let initialTempData = "
                + savedTempData + ";"
                                  "let initialRainData = "
                + savedRainData + ";"
                                  "let labels = [];"
                                  "let data = [];"
                                  "let rainLabels = [];"
                                  "let rData = [];"
                                  "for (let i = 0; i < initialTempData.length; i++) {"
                                  "    let tempData = initialTempData[i].split(',');"
                                  "    labels.push(tempData[0]);"
                                  "    data.push(parseFloat(tempData[1]));"
                                  "}"

                                  "for (let i = 0; i < initialRainData.length; i++) {"
                                  "    let rainData = initialRainData[i].split(',');"
                                  "    rainLabels.push(rainData[0]);"
                                  "    rData.push(parseFloat(rainData[1]));"
                                  "}"

                                  "let ctx = document.getElementById('myChart').getContext('2d');"
                                  "let ctx2 = document.getElementById('myChart2').getContext('2d');"
                                  "let chart = new Chart(ctx, {"
                                  "    type: 'line',"
                                  "    data: {"
                                  "        labels: labels, "
                                  "        datasets: [{"
                                  "            label: 'Temperature',"
                                  "            data: data, "
                                  "            borderColor: 'rgba(255, 99, 132, 1)',"
                                  "            fill: false"
                                  "        }]"
                                  "    },"
                                  "});"
                                  "let chart2 = new Chart(ctx2, {"
                                  "    type: 'line',"
                                  "    data: {"
                                  "        labels: rainLabels, "
                                  "        datasets: [{"
                                  "            label: 'Rain Time',"
                                  "            data: rData, "
                                  "            borderColor: 'rgba(255, 99, 132, 1)',"
                                  "            fill: false"
                                  "        }]"
                                  "    },"
                                  "});"
                                  "async function fetchData() {"
                                  "    let response = await fetch('/getData');"
                                  "    let json = await response.json();"
                                  "    if (labels.length >= 30) {"
                                  "        labels.shift();"
                                  "        data.shift();"
                                  "    }"
                                  "    labels.push(json.time);"
                                  "    data.push(json.temperature);"
                                  "    chart.update();"

                                  "}"
                                  "setInterval(fetchData, 5000);"
                                  "</script>"
                                  "</body>"
                                  "</html>";

  return html;
}

String getCurrentDate() {
  timeClient.update();
  unsigned long epochTime = timeClient.getEpochTime();

  // Convert epoch time to day/month/year format
  tm *ptm = gmtime((time_t *)&epochTime);
  int day = ptm->tm_mday;
  int month = ptm->tm_mon + 1;
  int year = ptm->tm_year + 1900;

  return String(day) + "/" + String(month) + "/" + String(year);
}

void addTemperatureData(float temperature) {
  for (int i = 0; i < MAX_DATA_POINTS - 1; i++) {
    recentDates[i] = recentDates[i + 1];
    recentTemperatures[i] = recentTemperatures[i + 1];
  }
  recentDates[MAX_DATA_POINTS - 1] = getCurrentDate();
  recentTemperatures[MAX_DATA_POINTS - 1] = temperature;

  currentDataCount++;
  if (currentDataCount >= MAX_DATA_POINTS) {
    saveTempDataToFile();
    currentDataCount = 0;
  }
}

void saveTempDataToFile() {
    File file = SPIFFS.open("/temperature.txt", "a");
    if (file) {
        for (int i = 0; i < MAX_DATA_POINTS; i++) {
            file.println(recentDates[i] + "," + String(recentTemperatures[i]));
        }
        file.close();
        Serial.println("Data written to file successfully!");
    } else {
        Serial.println("Failed to write data to file.");
    }
}

void saveRainDataToFile(String data) {
  File file = SPIFFS.open("/rain.txt", "a");
  if (file) {
    file.println(data);
    file.close();
    Serial.println(data);
    Serial.println("Data written to file successfully!");
  } else {
    Serial.println("Failed to write data to file.");
  }
}

String getSavedTempData() {
  std::queue<String> lines;
  Serial.println("Opening temperature.txt file for reading...");

  if (!SPIFFS.begin()) {
    Serial.println("Failed to initialize SPIFFS");
    return "[]";
  }

  File file = SPIFFS.open("/temperature.txt", "r");
  if (!file) {
    Serial.println("Failed to open temperature.txt for reading");
    return "[]";
  }

  Serial.println("Reading data from temperature.txt file...");
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    lines.push(line);
    if (lines.size() > MAX_DATA_POINTS) {
      lines.pop();
    }
  }
  file.close();

  String result = "[";
  while (!lines.empty()) {
    result += "\"" + lines.front() + "\",";
    lines.pop();
  }
  if (result.length() > 1) {
    result.remove(result.length() - 1);  // Remove trailing comma
  }
  result += "]";

  Serial.print("Generated result: ");
  Serial.println(result);
  Serial.println("Finishing getSavedTempData()");
  return result;
}

String getSavedRainData() {
  std::queue<String> lines;
  Serial.println("Opening rain.txt file for reading...");

  if (!SPIFFS.begin()) {
    Serial.println("Failed to initialize SPIFFS");
    return "[]";
  }

  File file = SPIFFS.open("/rain.txt", "r");
  if (!file) {
    Serial.println("Failed to open rain.txt for reading");
    return "[]";
  }

  Serial.println("Reading data from rain.txt file...");
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    lines.push(line);
    if (lines.size() > MAX_DATA_POINTS) {
      lines.pop();
    }
  }
  file.close();

  String result = "[";
  while (!lines.empty()) {
    result += "\"" + lines.front() + "\",";
    lines.pop();
  }
  if (result.length() > 1) {
    result.remove(result.length() - 1);  // Remove trailing comma
  }
  result += "]";

  Serial.print("Generated result: ");
  Serial.println(result);
  Serial.println("Finishing getSavedRainData()");
  return result;
}

TemperatureStatistics calculateStatistics(String data) {
  TemperatureStatistics stats;

  // Remove surrounding []
  data = data.substring(1, data.length() - 1);

  // Initialize variables
  float maxTemperature = -9999;
  float minTemperature = 9999;
  float totalTemp = 0;
  float varSum = 0;
  int count = 0;

  String firstDate = "";
  String lastDate = "";
  int start = 0;

  // Loop through each data entry
  while (start < data.length()) {
    int commaPos = data.indexOf(',', start);
    int endPos = data.indexOf('\"', commaPos);

    // Extracting date and temperature
    String dateEntry = data.substring(start + 1, commaPos);  // Adjusting to include entire date
    float temp = data.substring(commaPos + 1, endPos).toFloat();

    // Set first and last date
    if (firstDate == "") {
      firstDate = dateEntry;
    }
    lastDate = dateEntry;

    // Check max and min temperature
    if (temp > maxTemperature) {
      maxTemperature = temp;
    }
    if (temp < minTemperature) {
      minTemperature = temp;
    }

    totalTemp += temp;
    count++;
    start = endPos + 2;  // Skip the ending quote and comma for the next iteration
  }

  // Calculate average
  float avgTemperature = totalTemp / count;

  // Recalculate variance in another loop
  start = 0;
  while (start < data.length()) {
    int commaPos = data.indexOf(',', start);
    int endPos = data.indexOf('\"', commaPos);
    float temp = data.substring(commaPos + 1, endPos).toFloat();
    varSum += (temp - avgTemperature) * (temp - avgTemperature);
    start = endPos + 2;  // Skip the ending quote and comma for the next iteration
  }

  stats.timeRange = (firstDate == lastDate) ? firstDate : firstDate + "-" + lastDate;
  stats.maxTemperature = maxTemperature;
  stats.minTemperature = minTemperature;
  stats.avgTemperature = avgTemperature;
  stats.variance = varSum / count;
  stats.stdDeviation = sqrt(stats.variance);

  // Autogenerate reminders
  String recommendation;

  recommendation += "<ul>";  // Start HTML list

  if (stats.avgTemperature > 30) {
    recommendation += "<li>The temperature is quite high, which is ideal for drying your laundry outside. Your laundry will dry in no time.</li>";
  } else if (stats.avgTemperature <= 30 && stats.avgTemperature > 20) {
    recommendation += "<li>The temperature seems moderately warm. Your laundry will dry in average time.</li>";
  } else {
    recommendation += "<li>The temperature seems quite cool. Your laundry may take a little longer to dry.</li>";
  }

  // if (stats.minTemperature < 15)
  // {
  //     recommendation += "<li>The minimum temperature drops below 10 degrees Celsius. Consider carrying a jacket or sweater if you're out early in the morning or late at night.</li>";
  // }
  // else if (stats.minTemperature >= 10 && stats.minTemperature < 20)
  // {
  //     recommendation += "<li>Evenings and mornings might be a bit chilly. A light jacket could be beneficial.</li>";
  // }

  if (stats.stdDeviation > 2) {
    recommendation += "<li>The temperature is fluctuating significantly. Drying time may be unpredictable.</li>";
  } else {
    recommendation += "<li>The temperature is stable without much fluctuation. Drying time is predictable.</li>";
  }

  recommendation += "</ul>";  // End HTML list

  stats.recommendation = recommendation;

  return stats;
}
