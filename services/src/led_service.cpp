/*
 * Copyright (c) 2016 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "led_service.h"

#include "rgbled_hal.h"

#include "debug.h"

// TODO: Move synchronization macros to some header file
#if PLATFORM_ID != 3

#include "spark_wiring_interrupts.h"

#define LED_SERVICE_DECLARE_LOCK(name)
#define LED_SERVICE_WITH_LOCK(name) ATOMIC_BLOCK()

#else // PLATFORM_ID == 3

#if PLATFORM_THREADING

#include "spark_wiring_thread.h"

#define LED_SERVICE_DECLARE_LOCK(name) Mutex name
#define LED_SERVICE_WITH_LOCK(name) WITH_LOCK(name)

#else // PLATFORM_ID == 3 && !PLATFORM_THREADING

#define LED_SERVICE_DECLARE_LOCK(name)
#define LED_SERVICE_WITH_LOCK(name)

#endif

#endif

namespace {

class StatusQueue {
public:
    StatusQueue() :
            front_(nullptr) {
    }

    void add(LEDStatusData* status) {
        SPARK_ASSERT(status);
        LEDStatusData* prev = nullptr;
        LEDStatusData* next = front_;
        while (next) {
            // Elements are sorted by priority in descending order
            if (next->priority <= status->priority) {
                break;
            }
            prev = next;
            next = next->next;
        }
        if (next) {
            next->prev = status;
        }
        if (prev) {
            prev->next = status;
        } else {
            front_ = status;
        }
        status->prev = prev;
        status->next = next;
    }

    void remove(LEDStatusData* status) {
        SPARK_ASSERT(status);
        if (status->next) {
            status->next->prev = status->prev;
        }
        if (status->prev) {
            status->prev->next = status->next;
        } else {
            front_ = status->next;
        }
    }

    LEDStatusData* front() const {
        return front_;
    }

private:
    LEDStatusData* front_;
};

class LEDService {
public:
    LEDService() :
            color_({ 0 }),
            pattern_(0), // Invalid pattern type
            speed_(0),
            period_(0),
            ticks_(0),
            disabled_(0),
            reset_(true) {
    }

    void setStatusActive(LEDStatusData* status, bool active) {
        SPARK_ASSERT(status);
        LED_SERVICE_WITH_LOCK(lock_) {
            if (status->flags & LED_STATUS_FLAG_ACTIVE) {
                queue_.remove(status);
                status->flags &= ~LED_STATUS_FLAG_ACTIVE;
            }
            if (active) {
                queue_.add(status);
                status->flags |= LED_STATUS_FLAG_ACTIVE;
            }
        }
    }

    void setUpdatesEnabled(bool enabled) {
        LED_SERVICE_WITH_LOCK(lock_) {
            if (enabled) {
                --disabled_;
                if (disabled_ == 0) {
                    reset_ = true; // Ignore cached color
                }
            } else {
                ++disabled_;
            }
        }
    }

    void update(system_tick_t ticks) {
        uint32_t color = 0;
        uint8_t pattern = 0; // Invalid pattern type
        uint8_t speed = 0;
        bool enabled = false;
        bool reset = false;
        bool off = false;
        // TODO: Add some flag to avoid locking if the queue has not changed since last update
        LED_SERVICE_WITH_LOCK(lock_) {
            LEDStatusData* s = queue_.front();
            if (s) {
                // Copy status parameters
                color = s->color;
                pattern = s->pattern;
                speed = s->speed;
                off = s->flags & LED_STATUS_FLAG_OFF;
            }
            enabled = (disabled_ == 0);
            reset = reset_;
            reset_ = false;
        }
        if (pattern_ != pattern || speed_ != speed) {
            pattern_ = pattern;
            speed_ = speed;
            period_ = patternPeriod(pattern_, speed_);
            ticks_ = 0; // Restart pattern animation
        } else if (period_ > 0) {
            ticks_ += ticks;
            if (ticks_ >= period_) {
                ticks_ %= period_;
            }
        }
        if (enabled) {
            Color c = { 0 }; // Black
            if (pattern_ != 0 && !off) {
                scaleColor(color, led_rgb_brightness, &c);
                if (period_ > 0) {
                    updatePatternColor(pattern_, ticks_, period_, &c);
                }
            }
            if (reset || color_.r != c.r || color_.g != c.g || color_.b != c.b) {
                setLedColor(c);
                color_ = c;
            }
        }
    }

private:
    struct Color {
        uint16_t r, g, b; // Scaled color components
    };

    StatusQueue queue_;

    Color color_; // Current color

    uint8_t pattern_; // Pattern type
    uint8_t speed_; // Pattern speed
    uint32_t period_; // Pattern period in milliseconds
    uint32_t ticks_; // Number of milliseconds passed within pattern period

    uint16_t disabled_; // LED updates are enabled if this counter is set to 0
    bool reset_; // Flag signaling that cached LED color should be ignored

    LED_SERVICE_DECLARE_LOCK(lock_);

    // Updates color according to specified pattern and timing
    static void updatePatternColor(uint8_t pattern, uint32_t ticks, uint32_t period, Color* color) {
        switch (pattern) {
        case LED_PATTERN_BLINK: {
            if (ticks >= period / 2) { // Turn LED off
                color->r = 0;
                color->g = 0;
                color->b = 0;
            }
            break;
        }
        case LED_PATTERN_FADE: {
            period /= 2;
            if (ticks < period) { // Fade out
                ticks = period - ticks;
            } else { // Fade in
                ticks = ticks - period;
            }
            color->r = color->r * ticks / period;
            color->g = color->g * ticks / period;
            color->b = color->b * ticks / period;
            break;
        }
        default:
            break;
        }
    }

    // Returns pattern period in milliseconds
    static uint32_t patternPeriod(uint8_t pattern, uint8_t speed) {
        switch (pattern) {
        case LED_PATTERN_BLINK:
            // Blinking LED
            if (speed == LED_SPEED_NORMAL) {
                return 200; // Normal
            } else if (speed > LED_SPEED_NORMAL) {
                return 100; // Fast
            } else {
                return 500; // Slow
            }
        case LED_PATTERN_FADE:
            // "Breathing" LED
            if (speed == LED_SPEED_NORMAL) {
                return 4000; // Normal
            } else if (speed > LED_SPEED_NORMAL) {
                return 1000; // Fast
            } else {
                return 8000; // Slow
            }
        default:
            return 0; // Not applicable
        }
    }

    // Splits 32-bit RGB value into 16-bit color components (as expected by HAL) and applies
    // brightness correction
    static void scaleColor(uint32_t color, uint8_t value, Color* scaled) {
        const uint32_t v = (uint32_t)value * Get_RGB_LED_Max_Value();
        scaled->r = (((color >> 16) & 0xff) * v) >> 16;
        scaled->g = (((color >> 8) & 0xff) * v) >> 16;
        scaled->b = ((color & 0xff) * v) >> 16;
    }

    // Sets LED color and invokes user callback
    static void setLedColor(const Color& color) {
        Set_RGB_LED_Values(color.r, color.g, color.b);
        if (led_update_handler) {
            led_update_handler(led_update_handler_data, color.r, color.g, color.b, nullptr /* reserved */);
        }
    }
};

LEDService ledService;

} // namespace

void led_set_status_active(LEDStatusData* status, int active, void* reserved) {
    ledService.setStatusActive(status, active);
}

void led_set_updates_enabled(int enabled, void* reserved) {
    ledService.setUpdatesEnabled(enabled);
}

void led_update(system_tick_t ticks, void* reserved) {
    ledService.update(ticks);
}
