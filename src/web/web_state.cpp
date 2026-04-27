#include "web_state.h"

namespace WebState {
    SemaphoreHandle_t mutex = nullptr;
    ServoState servo;
    MotorState motor;
    BattServiceState battSvc;
    FlashState flashState;
    CRSFState crsf;

    void initMutex() {
        // Recursive mutex so future typed Facade methods (which acquire
        // internally) don't deadlock when called from a context that already
        // holds WebState::Lock. Binary mutex would deadlock on the second
        // take. xSemaphoreTakeRecursive must be paired with xSemaphoreGiveRecursive.
        if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
    }
}
