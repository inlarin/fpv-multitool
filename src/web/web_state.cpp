#include "web_state.h"

namespace WebState {
    SemaphoreHandle_t mutex = nullptr;
    ServoState servo;
    MotorState motor;
    BattServiceState battSvc;
    FlashState flashState;
    CRSFState crsf;

    void initMutex() {
        if (!mutex) mutex = xSemaphoreCreateMutex();
    }
}
