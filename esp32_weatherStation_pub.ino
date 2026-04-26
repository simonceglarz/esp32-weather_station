#include <Wire.h>  //Handles I2C electrical signal timing (SDA/SCL)
#include <WiFi.h>       //Contorls ESP32 internal radio hardware
#include <HTTPClient.h> //Formats gathered data into web language for adafruit


#define SENSOR_POWER 23 //Use ESP32 GPIO 23 as a power switch to the BME280 sensor
#define STATUS_LED 2    //GPIO 2 is connected to blue led on ESP32 

//Hardware addresses (From Bosch BME280 Datasheet)
#define BME280_ADDR 0x76  //The I2C address of the BME280 sensor on the bus
#define ID_REG 0xD0       //Register 0xD0 contains the Chip ID (should be 0x60)
#define CALIB_START 0x88  // Start of Temp/Press calibration data
#define DIG_H1_REG 0xA1   // Humidity constant 1 is at a separate address
#define DIG_H2_REG 0xE1   // Humidity constants 2-6 start here
#define CTRL_HUM 0xF2     // Humidity oversampling control
#define CTRL_MEAS 0xF4    // Mode and Temp/Press oversampling control
#define PRESS_DATA 0xF7   // Pressure data starts first in the burst read

// --- USER CREDENTIALS ---
const char* ssid = "YOUR_WIFI_NAME";        //Wifi name
const char* password = "YOUR_WIFI_PASSWORD"; // Wifi password

const char* aio_user = "YOUR_ADAFRUIT_USERNAME"; // Adafruit Username
const char* aio_key = "YOUR_ADAFRUIT_IO_KEY";  // API Key

// --- Calibration constants Storage ---
uint16_t dig_T1;
int16_t dig_T2, dig_T3;
uint16_t dig_P1;
int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
uint8_t dig_H1, dig_H3;
int8_t dig_H6;
int16_t dig_H2, dig_H4, dig_H5;

// We need t_fine for Humidity and Pressure math
int32_t t_fine;  //shared value used to compensate Humidity and Pressure

const uint64_t uS_TO_S_FACTOR = 1000000ULL; //Conversion factor: Microseconds to Seconds
const uint64_t SECONDS_TO_SLEEP = 1800;        //Desired sleep time in seconds

void setup() {
    Serial.begin(115200);  //Start Serial communication at 115200 bits per second

    //Blink status led to confirm code has booted
    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, HIGH);
    delay(100); 
    digitalWrite(STATUS_LED, LOW);

    //Power up the sensor ---
    //configure SENSOR_OUTPUT(23) pin as low impedence OUTPUT to provide VCC to BME280
    pinMode(SENSOR_POWER, OUTPUT);  
    digitalWrite(SENSOR_POWER, HIGH);   //set GPIO 23 to 3.3V (Logic HIGH) 
    delay(10);  //short delay to let the sensor capacitors power up and stabilize

    Wire.begin(21, 22);    //Initialize I2C: SDA on GPIO 21, SCL on GPIO 22

    //Wake up ESP32 radio before requesting any data from BME280 sensor
    WiFi.begin(ssid, password); //Start handshake with wifi;
    Serial.print("Connecting to WiFi");

    int wifi_timeout = 0; //Create a safety counter to prevent battery waste
    //While we arent connected and number of attempts is less than 20
    while(WiFi.status()!= WL_CONNECTED && wifi_timeout < 20) {
        delay(500);     //Wait 0.5 seconds between attempts
        Serial.print(".");  //Print a dot so we know ESP32 is attempting connection
        wifi_timeout++; //Count attemps
    } 

    //Check if connection was succesfull
    if(WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Online!");
    }else {
        Serial.println("\nWiFi Connection failed");
    }
    
    // Step 1. Verify hardware (The Handshake between devices)
    Wire.beginTransmission(BME280_ADDR);    //Put the address 0x76 on the wires
    Wire.write(ID_REG);                     //Tell the sensor we want to look at registter 0xD0
    byte error = Wire.endTransmission();    //Stop talking and check for ACK (Acknowledge)

    if (error == 0) {  //If the sensor is acknowledged
        Wire.requestFrom(BME280_ADDR, 1);   //Ask the sensor to send 1 byte at 0xD0
        if (Wire.available()){             //Check if that byte actually reached the ESP32 buffer
            byte id = Wire.read();          //Pull the byte out of and into 'id'
            Serial.print("Chip ID found: 0x");
            Serial.println(id, HEX);        //Should print 0x60
        }
    } else {
        //If error is not 0, there is a wiring or power issue
        Serial.print("Hardware Check Failed. Error code");
        Serial.println(error);

        // rapid strobe to visually indicate ESP32/BME280 handshake failure
        for(int i = 0; i < 20; i++) { 
            digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
            delay(100);
        }
        // FAIL-SAFE: Power down the sensor pin and sleep.
        digitalWrite(SENSOR_POWER, LOW); 
        
        // Sleep for 30 mins so we can try again later
        uint64_t retry_sleep = 30ULL * 60 * 1000000;
        esp_sleep_enable_timer_wakeup(retry_sleep);
        
        Serial.println("Retrying when ESP32 exits deep sleep");
        Serial.flush();
        esp_deep_sleep_start(); 
    }
    


    //Step 2: Load Calibration Data
    //We need unique factory-burned constants to get accurate results
    Wire.beginTransmission(BME280_ADDR);    // talk to 0x76 again
    Wire.write(CALIB_START);                // point to the first calibration register
    Wire.endTransmission();                 // send the pointer
    delay(10);

    //We request 24 bytes in a Burst Read (Temp and Pressure constants)
    Wire.requestFrom(BME280_ADDR, 24);

    if (Wire.available() == 24) {
        // We reassemble 8-bit registers into 16-bit integers using bit-shifting
        // The sensor is "Little Endian" (Least Significant Byte comes first)
        //I2C can only read 1 byte (8-bits) at a time, so we must read the first byte
        //shif, then read the second byte and shift it 8 to the left, then using the
        //or (|) operator you can store a 16-bit digit.
        dig_T1 = Wire.read() | (Wire.read() << 8);
        dig_T2 = Wire.read() | (Wire.read() << 8);
        dig_T3 = Wire.read() | (Wire.read() << 8);
        dig_P1 = Wire.read() | (Wire.read() << 8);
        dig_P2 = Wire.read() | (Wire.read() << 8);
        dig_P3 = Wire.read() | (Wire.read() << 8);
        dig_P4 = Wire.read() | (Wire.read() << 8);
        dig_P5 = Wire.read() | (Wire.read() << 8);
        dig_P6 = Wire.read() | (Wire.read() << 8);
        dig_P7 = Wire.read() | (Wire.read() << 8);
        dig_P8 = Wire.read() | (Wire.read() << 8);
        dig_P9 = Wire.read() | (Wire.read() << 8);
        Serial.println("Calibration constants loaded successfully. ");  
    }

    // --- Step 2.5: Load Humidity Calibration Constants ---
    
    // Read H1 (it's in its own lonely register)
    Wire.beginTransmission(BME280_ADDR);
    Wire.write(DIG_H1_REG); // 0xA1
    Wire.endTransmission();
    Wire.requestFrom(BME280_ADDR, 1);
    if(Wire.available()) dig_H1 = Wire.read();

    // Read H2 through H6 (these are in a block starting at 0xE1)
    Wire.beginTransmission(BME280_ADDR);
    Wire.write(DIG_H2_REG); // 0xE1
    Wire.endTransmission();
    Wire.requestFrom(BME280_ADDR, 7); // Request 7 bytes to cover H2-H6

    if (Wire.available() == 7) {
        dig_H2 = (int16_t)Wire.read() | ((int16_t)Wire.read() << 8);
        dig_H3 = (uint8_t)Wire.read();
        
        // This part is tricky! H4 and H5 share a byte in the middle.
        int8_t m1 = Wire.read(); // E4
        int8_t m2 = Wire.read(); // E5
        int8_t m3 = Wire.read(); // E6
        
        dig_H4 = ((int16_t)m1 << 4) | (m2 & 0x0F);
        dig_H5 = ((int16_t)m3 << 4) | (m2 >> 4);
        dig_H6 = (int8_t)Wire.read(); // E7
        Serial.println("Humidity constants loaded.");
    }

    // Step 3: Configure sensor settings (Wake it up) ----
    //Before reading data, we must tell the sensor to wake up and start measuring

    // Set Humidity Oversampling (Register 0xF2) first, otherwise data wont be read
    Wire.beginTransmission(BME280_ADDR);    //open communication with sensor
    Wire.write(CTRL_HUM); // set cursor 0xF2
    Wire.write(0x01);     // Humidity oversampling x1
    Wire.endTransmission();

    //After Humidity sampling is set, move to general data sampling address at 0xF4
    Wire.beginTransmission(BME280_ADDR);  //Start a new "write" conversation
    Wire.write(CTRL_MEAS);                //Point to the Control Measurements register
    //0x3F binary is 001 111 11:
    // 001 = Temp oversampling x1 | 111 = Pressure oversampling x16 | 11 = Normal Mode/Always on
    Wire.write(0x3F);        //Write 0x3F to the register to wake it up in the proper measurement mode
    Wire.endTransmission();  //Finish the configuration command


    //MEASURING DATA - next section would be in a loop() block, however when implementing
    //                 the deep sleep mode we must run the entire setup code each cycle

    //Step 1: It takes about 10-50ms for the heater and sensors inside to finish a reading.
    delay(100);

    // STEP 2: Position the cursor ---
    Wire.beginTransmission(BME280_ADDR);    //Start a new write conversation
    Wire.write(PRESS_DATA);                 //Set cursor to beginning of of data registeer at 0xF7
    Wire.endTransmission();                 //End Transmission

    //Step 2: It takes about 10-50ms for the heater and sensors inside to finish a reading.
    delay(100);

    //Step 3: Data Harvest
    //We request 8 bytes: 3 for Pressure, 3 for Temp, 2 for Humdidity.
    Wire.requestFrom(BME280_ADDR, 8);

    if (Wire.available() >= 8) {  //Ensure we received all 8 bytes
        //Step 4: Reconstruct raw data

        //Pressure data (bytes 1,2,3)
        long p_msb = Wire.read();     //Most significant Byte (8 bits)
        long p_lsb = Wire.read();     //Middle Byte (8 bits)
        long p_xlsb = Wire.read();    //Least Significant Byte (remaining 4 bits)

        //Shift msb 12 bits, LSB 4 bits, and grab the top 4 bits of XLSB to make 20 bits
        long rawP = (p_msb << 12) | (p_lsb << 4) | (p_xlsb >> 4);

        // TEMPERATURE (Bytes 3, 4, 5)
        long t_msb = Wire.read(); 
        long t_lsb = Wire.read(); 
        long t_xlsb = Wire.read();
        long rawT = (t_msb << 12) | (t_lsb << 4) | (t_xlsb >> 4);

        // HUMIDITY (Bytes 6, 7)
        long h_msb = Wire.read(); 
        long h_lsb = Wire.read();
        long rawH = (h_msb << 8) | h_lsb; // Humidity is a simple 16-bit reconstruction

        //STEP 5: Use BOSCH's sensor formulas to calculate accurate measurements
        //for pressure, temperature, and humdity 

        // 5A: Temperature (Must be done first because t_fine is needed for P and H)
        double var1T = (((double)rawT) / 16384.0 - ((double)dig_T1) / 1024.0) * ((double)dig_T2);
        double var2T = ((((double)rawT) / 131072.0 - ((double)dig_T1) / 8192.0) *
                       (((double)rawT) / 131072.0 - ((double)dig_T1) / 8192.0)) * ((double)dig_T3);
        
        t_fine = (int32_t)(var1T + var2T); // t_fine is the calibrated "Global Heat" of the chip
        float tempC = (t_fine / 5120.0);
        float tempF = (tempC * 1.8) + 32.0;

        // 5B: Pressure (Converted to hPa/millibars)
        double var1P = ((double)t_fine / 2.0) - 64000.0;
        double var2P = var1P * var1P * ((double)dig_P6) / 32768.0;
        var2P = var2P + var1P * ((double)dig_P5) * 2.0;
        var2P = (var2P / 4.0) + (((double)dig_P4) * 65536.0);
        var1P = (((double)dig_P3) * var1P * var1P / 524288.0 + ((double)dig_P2) * var1P) / 524288.0;
        var1P = (1.0 + var1P / 32768.0) * ((double)dig_P1);
        
        double p = 1048576.0 - (double)rawP;
        p = (p - (var2P / 4096.0)) * 6250.0 / var1P;
        var1P = ((double)dig_P9) * p * p / 2147483648.0;
        var2P = p * ((double)dig_P8) / 32768.0;
        float pressureHPa = (float)((p + (var1P + var2P + ((double)dig_P7)) / 16.0) / 100.0);

        // 5C: Humidity (%)
        double h = (((double)t_fine) - 76800.0);
        h = (rawH - (((double)dig_H4) * 64.0 + ((double)dig_H5) / 16384.0 * h)) *
            (((double)dig_H2) / 65536.0 * (1.0 + ((double)dig_H6) / 67108864.0 * h *
            (1.0 + ((double)dig_H3) / 67108864.0 * h)));
        h = h * (1.0 - ((double)dig_H1) * h / 524288.0);
        float humidity = (float)(h < 0 ? 0 : (h > 100 ? 100 : h)); // Clamp between 0-100%

        // --- STEP 6: OUTPUT RESULTS ---
        Serial.print("Temp: "); Serial.print(tempF); Serial.print(" F | ");
        Serial.print("Humidity: "); Serial.print(humidity); Serial.print(" % | ");
        Serial.print("Pressure: "); Serial.print(pressureHPa); Serial.println(" hPa");

        //Only upload climate data if WiFi connection was successful
        if(WiFi.status() == WL_CONNECTED) {
            sendToAdafruit("temperature", tempF);
            sendToAdafruit("humidity", humidity);   
            sendToAdafruit("pressure", pressureHPa); 
        }

        //Blink status led 3 times to confirm successful measurement
        for(int i = 0; i < 3; i++) {
            digitalWrite(STATUS_LED, HIGH);
            delay(100);  // Longer blink for success
            digitalWrite(STATUS_LED, LOW);
            delay(400);  // Short gap between blinks
        }
        
    }

    //Ensure radio/wifi is shut off before enetering deep sleep
    WiFi.disconnect(true);  //Disconnect form network
    WiFi.mode(WIFI_OFF);    //Switch off radio hardware
    btStop();               //Ensure blutooth is also off

    //Rather than using delay which keeps the device active and draws ~80-150mA,
    // we use sleep mode which only keeps on the RTC which draws ~80-150microAmps.

    digitalWrite(SENSOR_POWER, LOW);     //manually set Sensor VCC to LOW to cutoff power
    esp_sleep_enable_timer_wakeup(SECONDS_TO_SLEEP * uS_TO_S_FACTOR);
    Serial.flush(); // Ensure the Serial message actually finishes sending
    esp_deep_sleep_start();
}

void loop() {
    //empty, all looped code is run in the setup section
}

void sendToAdafruit (String feedName, float value) {
    HTTPClient http; //Step 1: create client object to handle web connection

    //Step 2: Construct destination address using username and feedname into web address
    String url = "https://io.adafruit.com/api/v2/" + String(aio_user) + "/feeds/" + feedName + "/data";

    http.begin(url);    //Step 3: Begin connection to the url
    //Step 4: Set the identification headers
    //Content-type tells the server we are sending JSON data
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-AIO-Key", aio_key);

    //Step 5: Package the data into JSON format {"value":"____"}
    String httpRequestData = "{\"value\":\"" + String(value) + "\"}";

    //Step 6: Send the POST request  and store the result in "httpResponseCode" to see if it worked
    int httpRequestCode = http.POST(httpRequestData);

    //Step 7: Print result to serial moniter for debugging
    // 200=success, 401=wrong key, 404=feed not found
    Serial.print("Cloud Push[");
    Serial.print(feedName);
    Serial.print("] Result: ");
    Serial.println(httpRequestCode);

    //Close connection to free ESP32 memoery
    http.end();

}


