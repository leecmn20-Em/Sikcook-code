    #include <Arduino.h>
    #include <SoftwareSerial.h>

    const int BT_RX_PIN = 5; // HC-06 TX
    const int BT_TX_PIN = 6; // HC-06 RX
    const int LED_PIN = 13;   // UNO LED
    const int MOTOR_PIN = 3;  // Motor Control Pin
    const int INDUCTION_FORE_PIN = 9;
    const int INDUCTION_BACK_PIN = 10;
    const int FG_PIN = 2; // Motor Speed Receive Pin
    SoftwareSerial BT(BT_RX_PIN, BT_TX_PIN);

    #pragma region Constants and Variables
    String serialbuffer="";
    String btbuffer="";
    struct Time {
        int year;
        int month;
        int day;
        int hour;
        int minute;
        int second;
    };
    unsigned long now_millis=0;
    Time currentTime={0,0,0,0,0,0};
    String currentRecipe="";
    Time nextreserveTime={9999,0,0,0,0,0};
    Time nextstartTime={9999,0,0,0,0,0};
    const uint16_t MOTOR_TOP=99;
    volatile uint8_t motorDutyNow = 0;
    volatile uint8_t motorDutyTargetInt=0;
    volatile float motorDutyTargetFrac=0.0f;
    float dither_acc=0.0f;
    const uint32_t DITHER_INTERVAL_US=1000;
    uint32_t lastDitherTimeUs=0;
    uint16_t motorRampStep = 1;
    uint32_t motorRampIntervalMs = 10;
    uint32_t lastMotorRampTimeMs = 0;
    const uint16_t INDUCTION_TOP = 419;
    const uint16_t INDUCTION_DUTY = 209;
    bool isInductionEnabled = false;
    const int PULSE_PER_REVOLUTION = 44;
    const uint32_t WINDOW_US=200000;
    const uint32_t MIN_PULSE_DT_US=100;
    const uint32_t TIMEOUT_US=2000000;
    volatile uint32_t isr_pulsecount=0;
    volatile uint32_t isr_lastpulseus=0;
    volatile uint32_t isr_lastdtus=0;
    float rpm_est=0.0f;
    float rpm_ins=0.0f;
    float rpm_tgt=0.0f;
    float rpm_Kp=0.0006f;
    float rpm_Ki=0.0005f;
    float rpm_Kd=0.0f;
    float i_term=0.0f;
    float last_rpm_est=0.0f;
    const float DUTY_MIN=0.0f;
    const float DUTY_MAX=1.0f;
    const float I_MIN=-0.2f;
    const float I_MAX=0.3f;
    const float D_MIN=-0.2f;
    const float D_MAX=0.2f;
    uint32_t lastcontrolms=0;
    const uint32_t CONTROL_INTERVAL_MS=100;
    float duty_base=0.0f;
    float duty_cmd=0.0f;
    bool printflag=false;
    bool iscooking=false;
    #pragma endregion

    #pragma region Recipes
    const char* names[]={
        "oatmeal",
        "curry",
        "salad",
        "stew",
        "soup"
    };
    int mapRecipeNameToIndex(const String &name){
        for(int i=0; i<sizeof(names)/sizeof(names[0]); i++){
            if(name == names[i]){
                return i;
            }
        }
        return -1;
    }
    const int speedLevels[] = {
        50, // oatmeal
        30, // curry
        60,  // salad
        45, // stew
        45  // soup
    };
    const bool heatLevels[] = {
        true, // oatmeal
        true,  // curry
        false, // salad
        true,  // stew
        true   // soup
    };
    const int cookingTimes[] = {
        300, // oatmeal
        1200, // curry
        300,  // salad
        1800, // stew
        600,  // soup
    };
    #pragma endregion

    void setup() {
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(LED_PIN, LOW);
        initMotor();
        initInduction();
        pinMode(FG_PIN, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(FG_PIN), motorPulseISR, RISING);
        Serial.begin(9600);
        BT.begin(9600);
        Serial.println("Bluetooth Cooking Controller Ready");
        BT.println("BT Ready");
    }

    void loop() {
        updateTime();
        updateLED();
        updateprint();
        updateRecipe();
        updateMotorRPM();
        updateMotorControl();
        updateMotorRamp();
        updateMotorDither();
        checkEmergencyStop();
        while(Serial.available()>0) {
            char c = Serial.read();
            if(c == '\n' || c == '\r') {
                if(serialbuffer.length() > 0){
                    Serial.print("Received Serial Command: ");
                    Serial.println(serialbuffer);
                    handleSerialcommand(serialbuffer);
                    serialbuffer = "";
                }
            } else {
                serialbuffer += c;
            }
        }
        while(BT.available()>0) {
            char c = BT.read();
            if(c == '\n' || c == '\r') {
                if(btbuffer.length() > 0){
                    Serial.print("Received BT Command: ");
                    Serial.println(btbuffer);
                    handleBTcommand(btbuffer);
                    btbuffer = "";
                }
            } else {
                btbuffer += c;
            }
        }
    }

    void updateLED(){
    }
    void updateprint(){
        if(printflag){
            printflag=false;
        }
    }
    void checkEmergencyStop(){
        static int failcount=0;
        if(rpm_est >= 180.0f) failcount++;
        else failcount=0;
        if (failcount>=100){
            rpm_tgt=0.0f;
            setMotorTargetDuty(0.0f);
            applyMotorDuty(0);
            setInduction(false);
            nextstartTime={9999,0,0,0,0,0};
            nextreserveTime={9999,0,0,0,0,0};
            iscooking=false;
            Serial.println("Emergency Stop Activated!");
        }
    }

    #pragma region Command Handlers
    void handleSerialcommand(const String &raw) {
        String command = raw;
        command.trim();
        String parts[10];
        int partCount = splitCommand(command, ':', parts, 10);
        // printCommandPartsSerial(parts, partCount);
        if(parts[0] == "STATUS") {
            Serial.println("Status: OK");
            Serial.println("Current Time: " + convertFromTime(currentTime));
            Serial.println("Current Recipe: " + currentRecipe);
            Serial.println("Reserve Time: " + convertFromTime(nextreserveTime));
            Serial.println("Motor Speed Target Duty: " + String((float)motorDutyTargetInt+motorDutyTargetFrac));
            Serial.println("Motor Speed Current Duty: " + String(motorDutyNow));
            Serial.println("Induction Heating: " + String(isInductionEnabled ? "ON" : "OFF"));
        }
        else if(parts[0] == "PING"){
            Serial.println("COOK");
        }
        else if (parts[0] == "UPDATETIME") {
            requestTimeBT();
        }
        else if (parts[0] == "SETTIME") {
            Time t = convertToTime(parts[1]);
            setTime(t);
            BT.print("Time Set To: ");
            printTimeBT();
            Serial.print("Time Set To: ");
            printTimeSerial();
        }
        else if (parts[0] == "SETSPEED") {
            float speed=parts[1].toFloat();
            setMotorSpeed(speed);
            Serial.print("Target Motor Speed Set To: ");
            Serial.println(parts[1]);
        }
        else if (parts[0] == "SETHEAT") {
            if(parts[1] == "on") {
                setInduction(true);
                Serial.println("Induction Heating ON");
            } else {
                setInduction(false);
                Serial.println("Induction Heating OFF");
            }
        }
        else if (parts[0] == "SETPID") {
            rpm_Kp = parts[1].toFloat();
            rpm_Ki = parts[2].toFloat();
            rpm_Kd = parts[3].toFloat();
            Serial.println("PID Parameters Set");
        }
        else {
            Serial.println("Unknown Command");
        }
    }
    void handleBTcommand(const String &raw) {
        String command = raw;
        command.trim();
        String parts[10];
        int partCount = splitCommand(command, ':', parts, 10);
        printCommandPartsSerial(parts, partCount);
        if(parts[0] == "SETUP") {
            BT.println("Setting up cooking...");
            CookingSetup(parts);
        }
        else if(parts[0] == "STATUS") {
            BT.println("Status: OK");
            BT.println("Current Time: " + convertFromTime(currentTime));
            BT.println("Current Recipe: " + currentRecipe);
            BT.println("Reserve Time: " + convertFromTime(nextreserveTime));
            BT.println("Motor Speed Target Duty: " + String((float)motorDutyTargetInt+motorDutyTargetFrac));
            BT.println("Motor Speed Current Duty: " + String(motorDutyNow));
            BT.println("Induction Heating: " + String(isInductionEnabled ? "ON" : "OFF"));
        }
        else if(parts[0] == "PING"){
            BT.println("COOK");
        }
        else if (parts[0] == "UPDATETIME") {
            requestTimeBT();
        }
        else if (parts[0] == "SETTIME") {
            Time t = convertToTime(parts[1]);
            setTime(t);
            BT.print("Time Set To: ");
            printTimeBT();
            Serial.print("Time Set To: ");
            printTimeSerial();
        }
        else if (parts[0] == "SETSPEED") {
            float speed=parts[1].toFloat();
            setMotorSpeed(speed);
            BT.print("Target Motor Speed Set To: ");
            BT.println(parts[1]);
        }
        else if (parts[0] == "SETHEAT") {
            if(parts[1] == "on") {
                setInduction(true);
                BT.println("Induction Heating ON");
            } else {
                setInduction(false);
                BT.println("Induction Heating OFF");
            }
        }
        else if (parts[0] == "SETPID") {
            rpm_Kp = parts[1].toFloat();
            rpm_Ki = parts[2].toFloat();
            rpm_Kd = parts[3].toFloat();
            BT.println("PID Parameters Set");
        }
        else {
            BT.println("Unknown Command");
        }
    }
    int splitCommand(const String &input, char delimiter, String parts[], int maxParts) {
        int partIndex = 0;
        int startIndex = 0;
        int delimIndex = input.indexOf(delimiter);
        while (delimIndex != -1 && partIndex < maxParts - 1) {
            parts[partIndex++] = input.substring(startIndex, delimIndex);
            startIndex = delimIndex + 1;
            delimIndex = input.indexOf(delimiter, startIndex);
        }
        if (partIndex < maxParts) {
            parts[partIndex++] = input.substring(startIndex);
        }
        return partIndex;
    }
    void printCommandPartsSerial(const String parts[], int count) {
        for (int i = 0; i < count; i++) {
            Serial.print("Part ");
            Serial.print(i);
            Serial.print(": ");
            Serial.println(parts[i]);
        }
    }
    void printCommandPartsBT(const String parts[], int count) {
        for (int i = 0; i < count; i++) {
            BT.print("Part ");
            BT.print(i);
            BT.print(": ");
            BT.println(parts[i]);
        }
    }
    #pragma endregion

    #pragma region Time Function
    void setTime(const Time &t){
        currentTime = t;
        now_millis=millis();
    }
    void updateTime(){
        uint32_t now=millis();
        uint32_t diff=now - now_millis;
        if (diff>=1000){
            unsigned long elapsed=diff/1000;
            now_millis+=elapsed*1000;
            currentTime.second+=elapsed;
            while (currentTime.second>=60){
                currentTime.second-=60;
                currentTime.minute+=1;
            }
            while (currentTime.minute>=60){
                currentTime.minute-=60;
                currentTime.hour+=1;
            }
            while (currentTime.hour>=24){
                currentTime.hour-=24;
                currentTime.day+=1;
            }
            // Simplified month/day rollover, not accounting for different month lengths or leap years
            while (currentTime.day>30){
                currentTime.day-=30;
                currentTime.month+=1;
            }
            while (currentTime.month>12){
                currentTime.month-=12;
                currentTime.year+=1;
            }
        }
    }
    void printTimeSerial(){
        Serial.print(currentTime.year); Serial.print("-");
        Serial.print(currentTime.month); Serial.print("-");
        Serial.print(currentTime.day); Serial.print(" ");
        Serial.print(currentTime.hour); Serial.print(":");
        Serial.print(currentTime.minute); Serial.print(":");
        Serial.println(currentTime.second);
    }
    void printTimeBT(){
        BT.print(currentTime.year); BT.print("-");
        BT.print(currentTime.month); BT.print("-");
        BT.print(currentTime.day); BT.print(" ");
        BT.print(currentTime.hour); BT.print(":");
        BT.print(currentTime.minute); BT.print(":");
        BT.println(currentTime.second);
    }
    void requestTimeSerial(){
        Serial.println("REQTIME");
    }
    void requestTimeBT(){
        BT.println("REQTIME");
    }
    int compareTime(const Time &t1, const Time &t2){
        if(t1.year != t2.year) return t1.year > t2.year ? 1 : -1;
        if(t1.month != t2.month) return t1.month > t2.month ? 1 : -1;
        if(t1.day != t2.day) return t1.day > t2.day ? 1 : -1;
        if(t1.hour != t2.hour) return t1.hour > t2.hour ? 1 : -1;
        if(t1.minute != t2.minute) return t1.minute > t2.minute ? 1 : -1;
        if(t1.second != t2.second) return t1.second > t2.second ? 1 : -1;
        return 0;
    }
    Time convertToTime(const String &timeStr){
        Time t={0,0,0,0,0,0};
        String parts[6];
        int partCount = splitCommand(timeStr, '-', parts, 6);
        if(partCount == 6) {
            t.year = parts[0].toInt();
            t.month = parts[1].toInt();
            t.day = parts[2].toInt();
            t.hour = parts[3].toInt();
            t.minute = parts[4].toInt();
            t.second = parts[5].toInt();
        }
        return t;
    }
    String convertFromTime(const Time &t){
        String timeStr = String(t.year) + "-" + String(t.month) + "-" + String(t.day) + "-" +
                        String(t.hour) + "-" + String(t.minute) + "-" + String(t.second);
        return timeStr;
    }
    Time addTime(const Time &t, int secondsToAdd){
        Time result = t;
        result.second += secondsToAdd;
        while (result.second >= 60) {
            result.second -= 60;
            result.minute += 1;
        }
        while (result.minute >= 60) {
            result.minute -= 60;
            result.hour += 1;
        }
        while (result.hour >= 24) {
            result.hour -= 24;
            result.day += 1;
        }
        // Simplified month/day rollover, not accounting for different month lengths or leap years
        while (result.day > 30) {
            result.day -= 30;
            result.month += 1;
        }
        while (result.month > 12) {
            result.month -= 12;
            result.year += 1;
        }
        return result;
    }
    Time subtractTime(const Time &t, int secondsToSubtract){
        Time result = t;
        result.second -= secondsToSubtract;
        while (result.second < 0) {
            result.second += 60;
            result.minute -= 1;
        }
        while (result.minute < 0) {
            result.minute += 60;
            result.hour -= 1;
        }
        while (result.hour < 0) {
            result.hour += 24;
            result.day -= 1;
        }
        // Simplified month/day rollover, not accounting for different month lengths or leap years
        while (result.day < 1) {
            result.day += 30;
            result.month -= 1;
        }
        while (result.month < 1) {
            result.month += 12;
            result.year -= 1;
        }
        return result;
    }
    #pragma endregion

    #pragma region Cooking Control Functions
    void showrecipeBT(const String &recipe, const String &reserve_time){
        BT.println("Current Recipe: " + recipe);
        BT.println("Reserve Time: " + reserve_time);
    }
    void showrecipeSerial(const String &recipe, const String &reserve_time){
        Serial.println("Current Recipe: " + recipe);
        Serial.println("Reserve Time: " + reserve_time);
    }
    void CookingSetup(const String parts[]){
        String reserve_time=parts[2];
        String recipe=parts[1];
        currentRecipe=recipe;
        int recipeindex=mapRecipeNameToIndex(recipe);
        if(recipeindex==-1){
            BT.println("Unknown Recipe: " + recipe);
            return;
        }
        nextreserveTime=convertToTime(reserve_time);
        nextstartTime=subtractTime(nextreserveTime, cookingTimes[recipeindex]);
        showrecipeBT(recipe, reserve_time);
        BT.println("Synchronizing Time...");
        requestTimeBT();
    }
    void updateRecipe(){
        bool isstart=false;
        bool isend=false;
        isstart=compareTime(currentTime, nextstartTime)>=0;
        isend=compareTime(currentTime, nextreserveTime)>=0;
        if(isstart){
            if(isend){
                if(iscooking){
                    stopCooking();
                }
                nextstartTime={9999,0,0,0,0,0};
                nextreserveTime={9999,0,0,0,0,0};
                currentRecipe="";
            }
            else if(!iscooking){
                startCooking(currentRecipe);
            }
        }
    }
    void startCooking(const String &recipe){
        int recipeindex=mapRecipeNameToIndex(recipe);
        if(recipeindex==-1){
            BT.println("Unknown Recipe: " + recipe);
            return;
        }
        float speed=speedLevels[recipeindex];
        bool heat=heatLevels[recipeindex];
        setMotorSpeed(speed);
        setInduction(heat);
        BT.println("Starting Cooking: " + recipe);
        BT.println("Motor Speed: " + String(speed));
        BT.println("Induction Heat: " + String(heat ? "ON" : "OFF"));
        iscooking=true;
    }
    void stopCooking(){
        BT.println("Stopping Cooking");
        setMotorSpeed(0.0f);
        setInduction(false);
        iscooking=false;
    }
    #pragma endregion

    #pragma region Motor register control
    void initMotor(){
        pinMode(MOTOR_PIN, OUTPUT);
        digitalWrite(MOTOR_PIN, LOW);

        TCCR2A=0;
        TCCR2B=0;
        TCNT2=0;

        TCCR2A |= (1 << WGM21) | (1 << WGM20);
        TCCR2B |= (1 << WGM22);
        TCCR2A |= (1 << COM2B1);
        TCCR2B |= (1 << CS21);

        OCR2A = MOTOR_TOP;
        OCR2B = 0;

        motorDutyNow = 0;
        motorDutyTargetInt = 0;
        motorDutyTargetFrac = 0.0f;
    }
    inline void applyMotorDuty(uint8_t duty){
        if(duty > MOTOR_TOP) duty = MOTOR_TOP;
        if(duty < 0) duty = 0;
        if(duty == 0){
            OCR2B = 0;
            motorDutyNow = 0;
            motorPWMEnable(false);
            return;
        }
        motorPWMEnable(true);
        OCR2B = duty;
        motorDutyNow = duty;
    }
    inline void writeMotorPWM(uint8_t duty){
        if(duty > MOTOR_TOP) duty = MOTOR_TOP;
        if(duty < 0) duty = 0;
        if(duty == 0){
            OCR2B = 0;
            motorPWMEnable(false);
            return;
        }
        motorPWMEnable(true);
        OCR2B = duty;
    }
    inline void motorPWMEnable(bool en){
        if(en){
            TCCR2A |= (1 << COM2B1);
        } else {
            TCCR2A &= ~(1 << COM2B1);
            digitalWrite(MOTOR_PIN, LOW);
        }
    }
    void setMotorTargetDuty(float duty){
        if (duty < 0.0f) duty = 0.0f;
        if (duty > 1.0f) duty = 1.0f;

        float steps=duty * MOTOR_TOP;
        uint8_t base=(uint8_t)floor(steps);
        float frac=steps - static_cast<float>(base);

        motorDutyTargetInt=base;
        motorDutyTargetFrac=frac;
        dither_acc=0.0f;
    }
    void setMotorRamp(uint16_t step, uint32_t intervalMs){
        motorRampStep = (step == 0) ? 1 : step;
        motorRampIntervalMs = (intervalMs == 0) ? 1 : intervalMs;
    }
    void updateMotorRamp(){
        uint32_t now = millis();
        if (now - lastMotorRampTimeMs >= motorRampIntervalMs) {
            lastMotorRampTimeMs = now;
            if (motorDutyNow < motorDutyTargetInt) {
                motorDutyNow += motorRampStep;
                if (motorDutyNow > motorDutyTargetInt) {
                    motorDutyNow = motorDutyTargetInt;
                }
                applyMotorDuty(motorDutyNow);
            } else if (motorDutyNow > motorDutyTargetInt) {
                if(motorDutyNow < motorRampStep) {
                    motorDutyNow = 0;
                } else {
                    motorDutyNow -= motorRampStep;
                }
                if (motorDutyNow < motorDutyTargetInt) {
                    motorDutyNow = motorDutyTargetInt;
                }
                applyMotorDuty(motorDutyNow);
            }
        }
    }
    void updateMotorDither(){
        if(motorDutyTargetInt == 0 && motorDutyTargetFrac <= 0) return;
        if(motorDutyNow != motorDutyTargetInt) return;
        if(motorDutyTargetFrac <= 0.0f) return;
        uint32_t now=micros();
        if(now - lastDitherTimeUs < DITHER_INTERVAL_US) return;
        lastDitherTimeUs=now;

        uint8_t out= motorDutyTargetInt;
        dither_acc += motorDutyTargetFrac;
        if(dither_acc >= 1.0f && out < MOTOR_TOP){
            out += 1;
            dither_acc -= 1.0f;
        }
        writeMotorPWM(out);
    }
    void motorPulseISR(){
        uint32_t now=micros();
        uint32_t dt=now - isr_lastpulseus;

        if(isr_lastpulseus != 0 && dt <MIN_PULSE_DT_US) return;

        isr_lastpulseus=now;
        isr_lastdtus=dt;
        isr_pulsecount++;
    }
    float updateMotorRPM(){
        uint32_t lastpulseus, lastdtus;
        noInterrupts();
        lastpulseus = isr_lastpulseus;
        lastdtus = isr_lastdtus;
        interrupts();

        uint32_t now=micros();

        if(lastpulseus == 0 || (now - lastpulseus) > TIMEOUT_US){
            rpm_ins=0.0f;
        }
        else if (lastdtus > 0){
            rpm_ins = 60000000.0f / (PULSE_PER_REVOLUTION * static_cast<float>(lastdtus));
        }

        const float alpha = 0.1f;
        rpm_est = alpha * rpm_ins + (1.0f - alpha) * rpm_est;

        return rpm_est;
    }
    #pragma endregion

    #pragma region Motor RPM Control
    void updateMotorControl(){
        uint32_t now=millis();
        uint32_t diff=now - lastcontrolms;
        if(diff < CONTROL_INTERVAL_MS) return;
        lastcontrolms=now;

        if(rpm_tgt <= 0.0f){
            duty_cmd=0.0f;
            i_term=0.0f;
            if(rpm_est <= 10.0f) last_rpm_est=0.0f;
            setMotorTargetDuty(0.0f);
            return;
        }

        float rpm_error = rpm_tgt - rpm_est;
        float dt=diff/1000.0f;

        float p_term = rpm_Kp * rpm_error;

        float d_term = rpm_Kd * (last_rpm_est - rpm_est) / dt;
        last_rpm_est = rpm_est;
        if(d_term > D_MAX) d_term = D_MAX;
        if(d_term < D_MIN) d_term = D_MIN;

        float duty_before_i = duty_base + p_term + i_term + d_term;
        bool sat_high = (duty_before_i >= DUTY_MAX);
        bool sat_low = (duty_before_i <= DUTY_MIN);
        bool integrator_active = !( (sat_high && rpm_error > 0.0f) || (sat_low && rpm_error < 0.0f) );
        if(integrator_active){
            i_term += rpm_Ki * rpm_error * dt;
            if(i_term > I_MAX) i_term = I_MAX;
            if(i_term < I_MIN) i_term = I_MIN;
        }

        duty_cmd= duty_base + p_term + i_term + d_term;
        if(duty_cmd > DUTY_MAX) duty_cmd = DUTY_MAX;
        if(duty_cmd < DUTY_MIN) duty_cmd = DUTY_MIN;

        setMotorTargetDuty(duty_cmd);
    }
    void setMotorSpeed(float speed){
        rpm_tgt=speed;
    }
    #pragma endregion

    #pragma region Induction Control
    void initInduction(){
        pinMode(INDUCTION_FORE_PIN, OUTPUT);
        pinMode(INDUCTION_BACK_PIN, OUTPUT);
        digitalWrite(INDUCTION_FORE_PIN, LOW);
        digitalWrite(INDUCTION_BACK_PIN, LOW);

        TCCR1A = 0;
        TCCR1B = 0;
        TCNT1 = 0;

        TCCR1A |= (1 << WGM11);
        TCCR1B |= (1 << WGM13) | (1 << WGM12);
        TCCR1B |= (1 << CS10);

        ICR1 = INDUCTION_TOP;
        OCR1A = INDUCTION_DUTY;
        OCR1B = INDUCTION_DUTY;
    }
    void setInduction(bool heat){
        if(heat){
            TCCR1A &= ~((1 << COM1A0)|(1 << COM1B0));
            TCCR1A |= (1 << COM1A1)|(1 << COM1B1)|(1 << COM1B0);

            OCR1A = INDUCTION_DUTY;
            OCR1B = INDUCTION_DUTY;

            isInductionEnabled = true;
        } else {
            TCCR1A &= ~((1 << COM1A1)|(1 << COM1A0)|(1 << COM1B1)|(1 << COM1B0));
            digitalWrite(INDUCTION_FORE_PIN, LOW);
            digitalWrite(INDUCTION_BACK_PIN, LOW);

            isInductionEnabled = false;
        }
    }
    #pragma endregion