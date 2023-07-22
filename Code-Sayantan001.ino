#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_DHT.h>
#include <LiquidCrystal_I2C.h>

#define DHTPIN D1
#define DHTTYPE DHT11

// Replace with your WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Replace with your ThingSpeak API key
const char* apiKey = "YOUR_THINGSPEAK_API_KEY";
const int THINGSPEAK_CHANNEL_ID = YOUR_CHANNEL_ID;

// Pin connected to the soil moisture sensor's digital output
const int soilMoisturePin = D2;

// Pin connected to the water level switch
const int waterLevelPin = D3;

// Pin connected to the relay to control water pump
const int relayPin = D4;

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  Serial.begin(9600);
  lcd.begin(16, 2);

  pinMode(soilMoisturePin, INPUT);
  pinMode(waterLevelPin, INPUT);
  pinMode(relayPin, OUTPUT);

  digitalWrite(relayPin, LOW); // Turn off the relay initially

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi!");
}

void loop() {
  // Read the soil moisture level from the sensor
  int soilMoisture = digitalRead(soilMoisturePin);

  // Read the temperature and humidity from the DHT11 sensor
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  // Read the water level status from the switch
  int waterLevel = digitalRead(waterLevelPin);

  // Print the sensor readings to the serial monitor
  Serial.print("Soil Moisture: ");
  Serial.println(soilMoisture == HIGH ? "Dry" : "Wet");
  Serial.print("Temperature (°C): ");
  Serial.println(temperature);
  Serial.print("Humidity (%): ");
  Serial.println(humidity);
  Serial.print("Water Level: ");
  Serial.println(waterLevel == HIGH ? "Low" : "High");

  // Display the sensor readings on the LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Moisture:");
  lcd.print(soilMoisture == HIGH ? " Dry" : " Wet");

  lcd.setCursor(0, 1);
  lcd.print("Temp:");
  lcd.print(temperature);
  lcd.print("C Hum:");
  lcd.print(humidity);
  lcd.print("%");

  // Control the relay based on the water level status
  if (waterLevel == HIGH) {
    // Turn on the relay if the water level is low (simulate water pump)
    digitalWrite(relayPin, HIGH);
  } else {
    // Turn off the relay if the water level is high
    digitalWrite(relayPin, LOW);
  }

  // Send data to ThingSpeak
  ThingSpeak.setField(1, soilMoisture);
  ThingSpeak.setField(2, temperature);
  ThingSpeak.setField(3, humidity);
  ThingSpeak.setField(4, waterLevel);
  ThingSpeak.writeFields(THINGSPEAK_CHANNEL_ID, apiKey);

  delay(10000); // Delay for 10 seconds before sending the next reading
}
