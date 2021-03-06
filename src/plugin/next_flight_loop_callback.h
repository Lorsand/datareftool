#pragma once

#include <functional>

#include "XPLMProcessing.h"

// The callback function returns a bool- true if the callback should repeat, or false if it should only happen once
class NextFlightLoopCallback {
    std::function<void()> callback_function;
    XPLMFlightLoopID flight_loop = nullptr;
    float reschedule_time = 0;

    static float callback(float /* elapsed_since_last_call */, float /* elapsed_since_last_flight_loop */, int /* counter */, void * refcon) {
        NextFlightLoopCallback * pthis = static_cast<NextFlightLoopCallback *>(refcon);
        pthis->callback_function();
        return pthis->reschedule_time;
    }

public:
    NextFlightLoopCallback(std::function<void()> f) : callback_function(f) {
        XPLMCreateFlightLoop_t flight_loop_details;
        flight_loop_details.structSize = sizeof(flight_loop_details);
        flight_loop_details.phase = xplm_FlightLoop_Phase_BeforeFlightModel;
        flight_loop_details.callbackFunc = &NextFlightLoopCallback::callback;
        flight_loop_details.refcon = static_cast<void *>(this);

        flight_loop = XPLMCreateFlightLoop(&flight_loop_details);
    }

    ~NextFlightLoopCallback() {
        XPLMDestroyFlightLoop(flight_loop);
    }

    void scheduleNextFlightLoop() {
        reschedule_time = 0;
        XPLMScheduleFlightLoop(flight_loop, -1, 1);
    }

    void scheduleEveryFlightLoop() {
        reschedule_time = -1;
        XPLMScheduleFlightLoop(flight_loop, -1, 1);
    }

    void schedule(float this_call, float next_call) {
        XPLMScheduleFlightLoop(flight_loop, this_call, 1);
        reschedule_time = next_call;
    }

    void unschedule() {
        reschedule_time = 0;
    }
};