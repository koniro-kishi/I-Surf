// pins
#define pin_WaterSV 26
#define pin_FertSV 27
#define pin_Pump 25
#define pin_WaterUS_Echo 18
#define pin_WaterUS_Trig 5
#define pin_FertUS_Echo 4
#define pin_FertUS_Trig 19
#define pin_FertPH 36
#define pin_FertTDS 39
#define pin_SoilMoist 33


#define VREF 3.3              // analog reference voltage(Volt) of the ADC
#define SCOUNT  30            // sum of sample point
#define SOUND_SPEED 0.034
#define CM_TO_INCH 0.393701

long duration;
float distanceCm;
float distanceInch;

int sampleCount = 0;
int analogBufferIndex = 0;
int copyIndex = 0;

int analogBuffer[SCOUNT];     // store the analog value in the array, read from ADC
int analogBufferTemp[SCOUNT];
float averageVoltage = 0;
float Value = 0;

int soilMoist_val = 0;
int soilMoist_perc = 0;

long WL_duration;
int WL_distancecm;
long FL_duration;
int FL_distancecm;

// static global var
const int AirValue = 4200;   //you need to replace this value with Value_1
const int WaterValue = 1300;  //you need to replace this value with Value_2
const float temperature = 25;       // current temperature for compensation

// median filtering algorithm
int getMedianNum(int bArray[], int iFilterLen){
  int bTab[iFilterLen];
  for (byte i = 0; i<iFilterLen; i++)
  bTab[i] = bArray[i];
  int i, j, bTemp;
  for (j = 0; j < iFilterLen - 1; j++) {
    for (i = 0; i < iFilterLen - j - 1; i++) {
      if (bTab[i] > bTab[i + 1]) {
        bTemp = bTab[i];
        bTab[i] = bTab[i + 1];
        bTab[i + 1] = bTemp;
      }
    }
  }
  if ((iFilterLen & 1) > 0){
    bTemp = bTab[(iFilterLen - 1) / 2];
  }
  else {
    bTemp = (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
  }
  return bTemp;
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  pinMode(pin_WaterSV, OUTPUT);
  pinMode(pin_FertSV, OUTPUT);
  pinMode(pin_Pump, OUTPUT);
  pinMode(pin_SoilMoist, INPUT);
  pinMode(pin_FertTDS, INPUT);
  pinMode(pin_WaterUS_Trig, OUTPUT); // Sets the pin_WaterUS_Trig as an Output
  pinMode(pin_WaterUS_Echo, INPUT);
  analogReadResolution(12);
}

void loop() {

  // ==============================================
  // === ULTRASONIC, status: search more sensor ===
  // ==============================================

  // // Clears the pin_WaterUS_Trig
  // digitalWrite(pin_WaterUS_Trig, LOW);
  // delayMicroseconds(2);
  // // Sets the pin_WaterUS_Trig on HIGH state for 10 micro seconds
  // digitalWrite(pin_WaterUS_Trig, HIGH);
  // delayMicroseconds(10);
  // digitalWrite(pin_WaterUS_Trig, LOW);
  
  // // Reads the pin_WaterUS_Echo, returns the sound wave travel time in microseconds
  // duration = pulseIn(pin_WaterUS_Echo, HIGH);
  
  // // Calculate the distance
  // distanceCm = duration * SOUND_SPEED/2;
  
  // // Convert to inches
  // distanceInch = distanceCm * CM_TO_INCH;
  
  // // Prints the distance in the Serial Monitor
  // Serial.print("Distance (cm): ");
  // Serial.println(distanceCm);
  // Serial.print("Distance (inch): ");
  // Serial.println(distanceInch);
  
  // delay(1000);






  // =================================
  // === TDS Sensor, status: ready ===
  // =================================

  // static unsigned long analogSampleTimepoint = millis();
  // if(millis()-analogSampleTimepoint > 40){     //every 40 milliseconds,read the analog value from the ADC
  //   analogSampleTimepoint = millis();
  //   analogBuffer[analogBufferIndex] = analogRead(pin_FertTDS);    //read the analog value and store into the buffer

  //   analogBufferIndex++;

  //   if (sampleCount < SCOUNT) {
  //     sampleCount++;
  //   }

  //   if(analogBufferIndex == SCOUNT){ 
  //     analogBufferIndex = 0;
  //   }
  // }   
  
  // static unsigned long printTimepoint = millis();
  // if(sampleCount == SCOUNT && millis()-printTimepoint > 1000){
  //   printTimepoint = millis();
    
  //   for(copyIndex=0; copyIndex<SCOUNT; copyIndex++){
  //     analogBufferTemp[copyIndex] = analogBuffer[copyIndex];
  //   }

  //   // read the analog value more stable by the median filtering algorithm, and convert to voltage value
  //     averageVoltage = getMedianNum(analogBufferTemp,SCOUNT) * (float)VREF / 4096.0;
      
  //     //temperature compensation formula: fFinalResult(25^C) = fFinalResult(current)/(1.0+0.02*(fTP-25.0)); 
  //     float compensationCoefficient = 1.0+0.02*(temperature-25.0);
  //     //temperature compensation
  //     float compensationVoltage=averageVoltage/compensationCoefficient;
      
  //     //convert voltage value to tds value
  //     Value=(133.42*compensationVoltage*compensationVoltage*compensationVoltage - 255.86*compensationVoltage*compensationVoltage + 857.39*compensationVoltage)*0.5;
      
  //     Serial.print("voltage:");
  //     Serial.print(averageVoltage,2);
  //     Serial.print("V   ");
  //     Serial.print("WATER TDS Value:");
  //     Serial.print(Value,0);
  //     Serial.println("ppm");
  // }






  // =====================================================================
  // === Cap. Soil Moisture, status: calibrate AirValue and WaterValue ===
  // =====================================================================

  // static unsigned long printTimepoint = millis();
  // if(millis()-printTimepoint > 2000){
  //   soilMoist_val = analogRead(pin_SoilMoist);  //put Sensor insert into soil
  //   soilMoist_perc = constrain(map(soilMoist_val, AirValue, WaterValue, 0, 100), 0, 100);
  //   Serial.printf("Soil Moisture Value : %d \n", soilMoist_val);
  //   Serial.printf("Soil Moisture %% : %d%%\n", soilMoist_perc);
  //   printTimepoint = millis();
  // }






  // ============================================
  // === 3 total channel relay, status: ready ===
  // ============================================

  // static unsigned long switchTimepoint = millis();
  // static bool isOddSwitch = true;
  // if(millis()-switchTimepoint > 2000){
  //   switchTimepoint = millis();
  //   if (!isOddSwitch){
  //     digitalWrite(pin_WaterSV, LOW);
  //     digitalWrite(pin_FertSV, HIGH);
  //     digitalWrite(pin_Pump, LOW);
  //     Serial.println("Current Flowing");
  //   }
  //   else {
  //     digitalWrite(pin_WaterSV, HIGH);
  //     digitalWrite(pin_FertSV, LOW);
  //     digitalWrite(pin_Pump, HIGH);
  //     Serial.println("Current not Flowing");
  //   }
  //   isOddSwitch = !isOddSwitch;
  // }
}
