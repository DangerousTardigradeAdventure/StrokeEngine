#include <Arduino.h>
#include <StrokeEngine.h>
#include <FastAccelStepper.h>
#include <pattern.h>


FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *servo = NULL;

void StrokeEngine::begin(machineGeometry *physics, motorProperties *motor) {
    // store the machine geometry and motor properties pointer
    _physics = physics;
    _motor = motor;

    // Derived Machine Geometry & Motor Limits in steps:
    _travel = (_physics->physicalTravel - (2 * _physics->keepoutBoundary));
    _minStep = 0;
    _maxStep = int(0.5 + _travel * _motor->stepPerMillimeter)
    _maxStepPerSecond = int(0.5 + (_motor->maxRPM * _motor->stepsPerRevolution) / 60)
    _maxStepAcceleration = int(0.5 + _motor->maxAcceleration * _motor->stepsPerRevolution);
          
    // Initialize with default values
    _state = SERVO_DISABLED;
    _isHomed = false;
    _patternIndex = 0;
    _index = 0;
    _depth = _maxStep; 
    _stroke = 1/3 * _maxStep;
    _timeOfStroke = 1.0;
    _sensation = 0.0;

    // Setup FastAccelStepper 
    engine.init();
    servo = engine.stepperConnectToPin(_motor->stepPin);
    if (servo) {
        servo->setDirectionPin(_motor->directionPin, _motor->directionPin);
        servo->setEnablePin(_motor->enablePin, _motor->enableActiveLow);
        servo->setAutoEnable(false);
        servo->disableOutputs(); 
    }
    Serial.println("Servo initialized");

#ifdef DEBUG_VERBOSE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif
}

void StrokeEngine::setSpeed(float speed) {
    // Convert FPM into seconds to complete a full stroke
    _timeOfStroke = 60.0 / speed;

    // Constrain stroke time between 10ms and 120 seconds
    _timeOfStroke = constrain(_timeOfStroke, 0.01, 120.0); 

    // Update pattern with new speed, will be used with the next stroke or on update request
    patternTable[_patternIndex]->setTimeOfStroke(_timeOfStroke);

#ifdef DEBUG_VERBOSE
    Serial.println("setTimeOfStroke: " + String(_timeOfStroke, 2));
#endif

}

void StrokeEngine::setDepth(float depth) {
    // Convert depth from mm into steps
    _depth = int(depth * _motor->stepsPerMillimeter);

    // Constrain depth between minStep and maxStep
    _depth = constrain(_depth, _minStep, _maxStep); 

    // Update pattern with new speed, will be used with the next stroke or on update request
    patternTable[_patternIndex]->setDepth(_depth);

#ifdef DEBUG_VERBOSE
    Serial.println("setDepth: " + String(_depth));
#endif
}

void StrokeEngine::setStroke(float stroke) {
    // Convert stroke from mm into steps
    _stroke = int(stroke * _motor->stepsPerMillimeter);

    // Constrain stroke between minStep and maxStep
    _stroke = constrain(_stroke, _minStep, _maxStep); 

    // Update pattern with new speed, will be used with the next stroke or on update request
    patternTable[_patternIndex]->setStroke(_stroke);

#ifdef DEBUG_VERBOSE
    Serial.println("setStroke: " + String(_stroke));
#endif
}

void StrokeEngine::setSensation(float sensation) {
    // Constrain sensation between -100 and 100
    _sensation = constrain(sensation, -100, 100); 

    // Update pattern with new speed, will be used with the next stroke or on update request
    patternTable[_patternIndex]->setSensation(_sensation);

#ifdef DEBUG_VERBOSE
    Serial.println("setSensation: " + String(_sensation));
#endif
}

bool StrokeEngine::setPattern(int patternIndex) {
    // Check wether pattern Index is in range
    if (patternIndex < patternTableSize) {
        _patternIndex = patternIndex;

        // Inject current motion parameters into new pattern
        patternTable[_patternIndex]->setTimeOfStroke(_timeOfStroke);
        patternTable[_patternIndex]->setDepth(_depth);
        patternTable[_patternIndex]->setStroke(_stroke);
        patternTable[_patternIndex]->setSensation(_sensation);

        // Reset index counter
        _index = 0; 

#ifdef DEBUG_VERBOSE
    Serial.println("setPattern: [" + String(_patternIndex) + "] " + patternTable[_patternIndex]->getName());
    Serial.println("setTimeOfStroke: " + String(_timeOfStroke, 2));
    Serial.println("setDepth: " + String(_depth));
    Serial.println("setStroke: " + String(_stroke));
    Serial.println("setSensation: " + String(_sensation));
#endif
        return true;
    }

    // Return false on no match
#ifdef DEBUG_VERBOSE
    Serial.println("Failed to set pattern: " + String(_patternIndex));
#endif
    return false;   
}

bool StrokeEngine::applyNewSettingsNow() {
    motionParameter currentMotion;

#ifdef DEBUG_VERBOSE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

    // Only allowed when in a running state
    if (_state == SERVO_RUNNING) {
        // Ask pattern for update on motion parameters
        currentMotion = patternTable[_patternIndex]->nextTarget(_index);

        // Apply new trapezoidal motion profile to servo
        _applyMotionProfile(&currentMotion);

#ifdef DEBUG_VERBOSE
    Serial.println("Apply New Settings Now");
#endif
        // Success
        return true;
    }

#ifdef DEBUG_VERBOSE
    Serial.println("Failed to apply settings");
#endif

    // Updating not allowed
    return false;
}

bool StrokeEngine::startMotion() {
    // Only valid if state is ready
    if (_state != SERVO_READY) {

#ifdef DEBUG_VERBOSE
    Serial.println("Failed to start motion");
#endif
        return false;
    }

    // Stop current move, should one be pending (moveToMax or moveToMin)
    if (servo->isRunning()) {
        // Stop servo motor as fast as legaly allowed
        servo->setAcceleration(_maxStepAcceleration);
        servo->applySpeedAcceleration();
        servo->stopMove();
    }

    // Set state to RUNNING
    _state = SERVO_RUNNING;

    // Reset Stroke and Motion parameters
    _index = -1;
    patternTable[_patternIndex]->setTimeOfStroke(_timeOfStroke);
    patternTable[_patternIndex]->setDepth(_depth);
    patternTable[_patternIndex]->setStroke(_stroke);
    patternTable[_patternIndex]->setSensation(_sensation);

    // Create Stroke Task
    xTaskCreate(
        this->_strokingImpl,    // Function that should be called
        "Stroking",             // Name of the task (for debugging)
        2048,                   // Stack size (bytes)
        this,                   // Pass reference to this class instance
        24,                     // Pretty high task piority
        &_taskStrokingHandle    // Task handle
    ); 

#ifdef DEBUG_VERBOSE
    Serial.println("Started motion task");
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

    return true;

}

void StrokeEngine::stopMotion() {
    // only valid when 
    if (_state == SERVO_RUNNING) {
        // Stop servo motor as fast as legaly allowed
        servo->setAcceleration(_maxStepAcceleration);
        servo->applySpeedAcceleration();
        servo->stopMove();

        // Set state
        _state = SERVO_READY;

#ifdef DEBUG_VERBOSE
        Serial.println("Motion stopped");
#endif

    }
    
#ifdef DEBUG_VERBOSE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif
}

void StrokeEngine::enableAndHome(int pin, int activeLow, void(*callBackHoming)(bool), float speed = 5.0) {
    // Store callback
    _callBackHomeing = callBackHoming;

    // enable and home
    enableAndHome(pin, activeLow, speed);
}

void StrokeEngine::enableAndHome(int pin, int activeLow, float speed = 5.0) {
    // set homing pin as input
    _homeingPin = pin;
    pinMode(_homeingPin, INPUT);
    _homeingActiveLow = activeLow;
    _homeingSpeed = speed * _motor->stepsPerMillimeter;

    // first stop current motion and delete stroke task
    stopMotion();

    // Enable Servo
    servo->enableOutputs();

    // Create homing task
    xTaskCreate(
        this->_homingProcedureImpl,     // Function that should be called
        "Homing",                       // Name of the task (for debugging)
        2048,                           // Stack size (bytes)
        this,                           // Pass reference to this class instance
        20,                             // Pretty high task piority
        &_taskHomingHandle              // Task handle
    ); 
#ifdef DEBUG_VERBOSE
    Serial.println("Homing task started");
#endif

}

void StrokeEngine::thisIsHome() {
    if (_state != SERVO_ERROR) {
        // Enable Servo
        servo->enableOutputs();

        // Stet current position as home
        servo->setCurrentPosition(-_motor->stepsPerMillimeter * _physics->keepoutBoundary);
        _isHomed = true;

        _state = SERVO_READY;
    }
}

bool StrokeEngine::moveToMax(float speed = 10.0) {
    motionParameter currentMotion;

#ifdef DEBUG_VERBOSE
    Serial.println("Move to max");
#endif

    if (_isHomed) {
        // Stop motion immideately
        stopMotion();

        // Set feedrate for safe move
        currentMotion.speed = speed * _motor->stepsPerMillimeter;       
        currentMotion.acceleration = _maxStepAcceleration / 10;
        currentMotion.position = _maxStep;

        // Apply new trapezoidal motion profile to servo
        _applyMotionProfile(&currentMotion);

        // Set state
        _state = SERVO_READY;

#ifdef DEBUG_VERBOSE
        Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

        // Return success
        return true;

    } else {
        // Return failure
        return false;
    }
}

bool StrokeEngine::moveToMin(float speed = 10.0) {
    motionParameter currentMotion;

#ifdef DEBUG_VERBOSE
    Serial.println("Move to min");
#endif

    if (_isHomed) {
        // Stop motion immideately
        stopMotion();

        // Set feedrate for safe move
        currentMotion.speed = speed * _motor->stepsPerMillimeter;       
        currentMotion.acceleration = _maxStepAcceleration / 10;
        currentMotion.position = _minStep;

        // Apply new trapezoidal motion profile to servo
        _applyMotionProfile(&currentMotion);

        // Set state
        _state = SERVO_READY;

#ifdef DEBUG_VERBOSE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

        // Return success
        return true;

    } else {
        // Return failure
        return false;
    }
}

ServoState StrokeEngine::getState() {
    return _state;
}

void StrokeEngine::disable() {
    _state = SERVO_DISABLED;
    _isHomed = false;

    // Disable servo motor
    servo->disableOutputs();

    // Delete homing Task
    if (_taskHomingHandle != NULL) {
        vTaskDelete(_taskHomingHandle);
        _taskHomingHandle = NULL;
    }

#ifdef DEBUG_VERBOSE
    Serial.println("Servo disabled. Call home to continue.");
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

}

void StrokeEngine::safeState() {
//TODO: propably not interrupt safe. But does it matter?

    // call disable
    disable();
    
    // Set error state
    _state = SERVO_ERROR;

    // Safe State can only be cleared by removing power from Servo and ESP32 a.k.a. reboot
    Serial.println("Servo entered Safe State. Remove power to clear fault.");
#ifdef DEBUG_VERBOSE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif
}

String StrokeEngine::getPatternJSON() {
    String JSON = "[{\"";
    for (size_t i = 0; i < patternTableSize; i++) {
        JSON += String(patternTable[i]->getName());
        JSON += "\": ";
        JSON += String(i, DEC);
        if (i < patternTableSize - 1) {
            JSON += "},{\"";
        } else {
            JSON += "}]";
        }
    }
    return JSON;
}

void StrokeEngine::_homingProcedure() {
    // Set feedrate for homing
    servo->setSpeedInHz(_homeingSpeed);       
    servo->setAcceleration(_maxStepAcceleration / 10);    

    // Check if we are aleady at the homing switch
    if (digitalRead(SERVO_ENDSTOP) == !_homeingActiveLow) {
        //back off 5 mm from switch
        servo->move(_motor->stepsPerMillimeter * 2 * _physics->keepoutBoundary);

        // wait for move to complete
        while (servo->isRunning()) {
            // Pause the task for 100ms while waiting for move to complete
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        // move back towards endstop
        servo->move(-_motor->stepsPerMillimeter * 4 * _physics->keepoutBoundary);

    } else {
        // Move MAX_TRAVEL towards the homing switch
        servo->move(-_motor->stepsPerMillimeter * _physics->physicalTravel);
    }

    // Poll homing switch
    while (servo->isRunning()) {

        // Switch is active low
        if (digitalRead(SERVO_ENDSTOP) == LOW) {

            //Switch is at -KEEPOUT_BOUNDARY
            servo->forceStopAndNewPosition(-_motor->stepsPerMillimeter * _physics->keepoutBoundary);
            _isHomed = true;

            // drive free of switch and set axis to 0
            servo->moveTo(0);
            
            // Break loop, home was found
            break;
        }

        // Pause the task for 20ms to allow other tasks
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    
    // disable Servo if homing has not found the homing switch
    if (!_isHomed) {
        servo->disableOutputs();
        _state = SERVO_DISABLED;

#ifdef DEBUG_VERBOSE
        Serial.println("Homing failed");
#endif

    } else {
        // Set state to ready
        _state = SERVO_READY;

#ifdef DEBUG_VERBOSE
        Serial.println("Homing succeded");
#endif
    }

    // Call notification callback, if it was defined.
    if (_callBackHomeing != NULL) {
        _callBackHomeing(_isHomed);
    }

#ifdef DEBUG_VERBOSE
    Serial.println("Stroke Engine State: " + verboseState[_state]);
#endif

    // delete one-time task
    _taskHomingHandle = NULL;
    vTaskDelete(NULL);
}

void StrokeEngine::_stroking() {
    motionParameter currentMotion;
    int distanceToCrash = 0;
    int currentSpeed = 0;
    int minimumDeceleration = 0;
    while(1) { // infinite loop

        // Delete task, if not in RUNNING state
        if (_state != SERVO_RUNNING) {
            _taskStrokingHandle = NULL;
            vTaskDelete(NULL);
        }

        // If motor has stopped issue moveTo command to next position
        if (servo->isRunning() == false) {
            // Increment index for pattern
            _index++;

#ifdef DEBUG_STROKE
            Serial.println("Stroking Index: " + String(_index));
#endif

            // Ask pattern for update on motion parameters
            currentMotion = patternTable[_patternIndex]->nextTarget(_index);
            
            // current speed in Steps/s
            currentSpeed = int((1000000.0/servo->getCurrentSpeedInUs()) + 0.5);

            // Calculate distance to boundary in current motion direction
            if (currentSpeed > 0) {
                // forward move
                distanceToCrash = _maxStep - servo->getCurrentPosition();
            } else {
                // backward move
                distanceToCrash = servo->getCurrentPosition() - _minStep;
            }

            // Calculate minimal required deceleration to avoid crash
            minimumDeceleration = int(sq(currentSpeed) / (2.0 * distanceToCrash) + 0.5);

            // Modify acceleration value, if it is lower then minimally required deceleration
            currentMotion.acceleration = max(currentMotion.acceleration, minimumDeceleration);

            // Apply new trapezoidal motion profile to servo
            _applyMotionProfile(&currentMotion);

#ifdef DEBUG_STROKE
            Serial.println("Minimum Deceleration: " + String(minimumDeceleration));
            Serial.println("Current Speed: " + String(currentSpeed));
            Serial.println("Distance to Crash: " + String(distanceToCrash));
#endif
        }
        
        // Delay 10ms 
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void StrokeEngine::_applyMotionProfile(motionParameter* motion) {
    // Apply new trapezoidal motion profile to servo
    // Constrain speed between 1 step/sec and _maxStepPerSecond
    servo->setSpeedInHz(constrain(motion->speed, 1, _maxStepPerSecond);

    // Constrain acceleration between 1 step/sec^2 and _maxStepAcceleration
    servo->setAcceleration(constrain(motion->acceleration, 1, _maxStepAcceleration));

    // Constrain position between 0 and _maxStep
    servo->moveTo(constrain(motion->position, _minStep, _maxStep));

#ifdef DEBUG_STROKE
    Serial.println("motion.position: " + String(motion->position));
    Serial.println("motion.speed: " + String(motion->speed));
    Serial.println("motion.acceleration: " + String(motion->acceleration));
#endif

}


