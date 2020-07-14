
#define __MAIN_CPP__
#include "odrive_main.h"
#include "nvm_config.hpp"

#include "usart.h"
#include "freertos_vars.h"
#include <communication/interface_usb.h>
#include <communication/interface_uart.h>
#include <communication/interface_i2c.h>
#include <communication/interface_can.hpp>

ODriveCAN::Config_t can_config;
SensorlessEstimator::Config_t sensorless_configs[AXIS_COUNT];
Controller::Config_t controller_configs[AXIS_COUNT];
Axis::Config_t axis_configs[AXIS_COUNT];
TrapezoidalTrajectory::Config_t trap_configs[AXIS_COUNT];
Endstop::Config_t min_endstop_configs[AXIS_COUNT];
Endstop::Config_t max_endstop_configs[AXIS_COUNT];

std::array<Axis*, AXIS_COUNT> axes;
ODriveCAN *odCAN = nullptr;
ODrive odrv{};


ConfigManager config_manager;

static bool config_pop_all() {
    bool success = board_pop_config() &&
           config_manager.pop(&odrv.config_) &&
           config_manager.pop(&can_config);
    for (size_t i = 0; (i < AXIS_COUNT) && success; ++i) {
        success = config_manager.pop(&encoders[i].config_) &&
                  config_manager.pop(&sensorless_configs[i]) &&
                  config_manager.pop(&controller_configs[i]) &&
                  config_manager.pop(&trap_configs[i]) &&
                  config_manager.pop(&min_endstop_configs[i]) &&
                  config_manager.pop(&max_endstop_configs[i]) &&
                  config_manager.pop(&motors[i].config_) &&
                  config_manager.pop(&axis_configs[i]);
    }
    return success;
}

static bool config_push_all() {
    bool success = board_push_config() &&
           config_manager.push(&odrv.config_) &&
           config_manager.push(&can_config);
    for (size_t i = 0; (i < AXIS_COUNT) && success; ++i) {
        success = config_manager.push(&encoders[i].config_) &&
                  config_manager.push(&sensorless_configs[i]) &&
                  config_manager.push(&controller_configs[i]) &&
                  config_manager.push(&trap_configs[i]) &&
                  config_manager.push(&min_endstop_configs[i]) &&
                  config_manager.push(&max_endstop_configs[i]) &&
                  config_manager.push(&motors[i].config_) &&
                  config_manager.push(&axis_configs[i]);
    }
    return success;
}

static void config_clear_all() {
    odrv.config_ = {};
    can_config = {};
    for (size_t i = 0; i < AXIS_COUNT; ++i) {
        encoders[i].config_ = {};
        sensorless_configs[i] = {};
        controller_configs[i] = {};
        trap_configs[i] = {};
        motors[i].config_ = {};
        axis_configs[i] = {};
        // Default step/dir pins are different, so we need to explicitly load them
        Axis::load_default_step_dir_pin_config(hw_configs[i].axis_config, &axis_configs[i]);
        Axis::load_default_can_id(i, axis_configs[i]);
        min_endstop_configs[i] = {};
        max_endstop_configs[i] = {};
        controller_configs[i].load_encoder_axis = i;
    }
}

static bool config_apply_all() {
    bool success = true;
    for (size_t i = 0; (i < AXIS_COUNT) && success; ++i) {
        success = encoders[i].apply_config(motors[i].config_.motor_type)
               && motors[i].apply_config();
    }
    return success;
}



void ODrive::save_configuration(void) {
    bool success = config_manager.prepare_store()
                && config_push_all()
                && config_manager.start_store()
                && config_push_all()
                && config_manager.finish_store();
    if (success) {
        user_config_loaded_ = true;
    } else {
        printf("saving configuration failed\r\n");
        osDelay(5);
    }
}

extern "C" int load_configuration(void) {
    bool success = config_manager.start_load()
                && config_pop_all()
                && config_manager.finish_load()
                && config_apply_all();
    if (success) {
        odrv.user_config_loaded_ = true;
    } else {
        config_clear_all();
        config_apply_all();
    }
    return success ? 0 : -1;
}

void ODrive::erase_configuration(void) {
    NVM_erase();

    // FIXME: this reboot is a workaround because we don't want the next save_configuration
    // to write back the old configuration from RAM to NVM. The proper action would
    // be to reset the values in RAM to default. However right now that's not
    // practical because several startup actions depend on the config. The
    // other problem is that the stack overflows if we reset to default here.
    NVIC_SystemReset();
}

void ODrive::enter_dfu_mode() {
    if ((hw_version_major_ == 3) && (hw_version_minor_ >= 5)) {
        __asm volatile ("CPSID I\n\t":::"memory"); // disable interrupts
        _reboot_cookie = 0xDEADBEEF;
        NVIC_SystemReset();
    } else {
        /*
        * DFU mode is only allowed on board version >= 3.5 because it can burn
        * the brake resistor FETs on older boards.
        * If you really want to use it on an older board, add 3.3k pull-down resistors
        * to the AUX_L and AUX_H signals and _only then_ uncomment these lines.
        */
        //__asm volatile ("CPSID I\n\t":::"memory"); // disable interrupts
        //_reboot_cookie = 0xDEADFE75;
        //NVIC_SystemReset();
    }
}

extern "C" int construct_objects(){
#if HW_VERSION_MAJOR == 3 && HW_VERSION_MINOR >= 3
    if (odrv.config_.enable_i2c_instead_of_can) {
        // Set up the direction GPIO as input
        GPIO_InitTypeDef GPIO_InitStruct;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_PULLUP;

        GPIO_InitStruct.Pin = I2C_A0_PIN;
        HAL_GPIO_Init(I2C_A0_PORT, &GPIO_InitStruct);
        GPIO_InitStruct.Pin = I2C_A1_PIN;
        HAL_GPIO_Init(I2C_A1_PORT, &GPIO_InitStruct);
        GPIO_InitStruct.Pin = I2C_A2_PIN;
        HAL_GPIO_Init(I2C_A2_PORT, &GPIO_InitStruct);

        osDelay(1);
        i2c_stats_.addr = (0xD << 3);
        i2c_stats_.addr |= HAL_GPIO_ReadPin(I2C_A0_PORT, I2C_A0_PIN) != GPIO_PIN_RESET ? 0x1 : 0;
        i2c_stats_.addr |= HAL_GPIO_ReadPin(I2C_A1_PORT, I2C_A1_PIN) != GPIO_PIN_RESET ? 0x2 : 0;
        i2c_stats_.addr |= HAL_GPIO_ReadPin(I2C_A2_PORT, I2C_A2_PIN) != GPIO_PIN_RESET ? 0x4 : 0;
        MX_I2C1_Init(i2c_stats_.addr);
    } else
#endif
        MX_CAN1_Init();

    HAL_UART_DeInit(&huart4);
    huart4.Init.BaudRate = odrv.config_.uart_baudrate;
    HAL_UART_Init(&huart4);

    // Init general user ADC on some GPIOs.
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Pin = GPIO_1_Pin;
    HAL_GPIO_Init(GPIO_1_GPIO_Port, &GPIO_InitStruct);
    GPIO_InitStruct.Pin = GPIO_2_Pin;
    HAL_GPIO_Init(GPIO_2_GPIO_Port, &GPIO_InitStruct);
    GPIO_InitStruct.Pin = GPIO_3_Pin;
    HAL_GPIO_Init(GPIO_3_GPIO_Port, &GPIO_InitStruct);
    GPIO_InitStruct.Pin = GPIO_4_Pin;
    HAL_GPIO_Init(GPIO_4_GPIO_Port, &GPIO_InitStruct);
#if HW_VERSION_MAJOR == 3 && HW_VERSION_MINOR >= 5
    GPIO_InitStruct.Pin = GPIO_5_Pin;
    HAL_GPIO_Init(GPIO_5_GPIO_Port, &GPIO_InitStruct);
#endif

    // Construct all objects.
    odCAN = new ODriveCAN(can_config, &hcan1);
    for (size_t i = 0; i < AXIS_COUNT; ++i) {
        SensorlessEstimator *sensorless_estimator = new SensorlessEstimator(sensorless_configs[i]);
        Controller *controller = new Controller(controller_configs[i]);
        TrapezoidalTrajectory *trap = new TrapezoidalTrajectory(trap_configs[i]);
        Endstop *min_endstop = new Endstop(min_endstop_configs[i]);
        Endstop *max_endstop = new Endstop(max_endstop_configs[i]);
        axes[i] = new Axis(i, hw_configs[i].axis_config, axis_configs[i],
                encoders[i], *sensorless_estimator, *controller, motors[i], *trap, *min_endstop, *max_endstop);

        controller_configs[i].parent = controller;
        min_endstop_configs[i].parent = min_endstop;
        max_endstop_configs[i].parent = max_endstop;
        axis_configs[i].parent = axes[i];
    }
    return 0;
}

extern "C" {
int odrive_main(void);
void vApplicationStackOverflowHook(xTaskHandle *pxTask, signed portCHAR *pcTaskName) {
    for(auto& axis : axes){
        safety_critical_disarm_motor_pwm(axis->motor_);
    }
        safety_critical_disarm_brake_resistor();
    for (;;); // TODO: safe action
}
void vApplicationIdleHook(void) {
    if (odrv.system_stats_.fully_booted) {
        odrv.system_stats_.uptime = xTaskGetTickCount();
        odrv.system_stats_.min_heap_space = xPortGetMinimumEverFreeHeapSize();
        odrv.system_stats_.min_stack_space_comms = uxTaskGetStackHighWaterMark(comm_thread) * sizeof(StackType_t);
        uint32_t min_stack_space[AXIS_COUNT];
        std::transform(axes.begin(), axes.end(), std::begin(min_stack_space), [](auto& axis) { return uxTaskGetStackHighWaterMark(axes[1]->thread_id_) * sizeof(StackType_t); });
        odrv.system_stats_.min_stack_space_axis = *std::min_element(std::begin(min_stack_space), std::end(min_stack_space));
        odrv.system_stats_.min_stack_space_usb = uxTaskGetStackHighWaterMark(usb_thread) * sizeof(StackType_t);
        odrv.system_stats_.min_stack_space_uart = uxTaskGetStackHighWaterMark(uart_thread) * sizeof(StackType_t);
        odrv.system_stats_.min_stack_space_usb_irq = uxTaskGetStackHighWaterMark(usb_irq_thread) * sizeof(StackType_t);
        odrv.system_stats_.min_stack_space_startup = uxTaskGetStackHighWaterMark(defaultTaskHandle) * sizeof(StackType_t);
        odrv.system_stats_.min_stack_space_can = uxTaskGetStackHighWaterMark(odCAN->thread_id_) * sizeof(StackType_t);

        // Actual usage, in bytes, so we don't have to math
        odrv.system_stats_.stack_usage_axis = axes[0]->stack_size_ - odrv.system_stats_.min_stack_space_axis;
        odrv.system_stats_.stack_usage_comms = stack_size_comm_thread - odrv.system_stats_.min_stack_space_comms;
        odrv.system_stats_.stack_usage_usb = stack_size_usb_thread - odrv.system_stats_.min_stack_space_usb;
        odrv.system_stats_.stack_usage_uart = stack_size_uart_thread - odrv.system_stats_.min_stack_space_uart;
        odrv.system_stats_.stack_usage_usb_irq = stack_size_usb_irq_thread - odrv.system_stats_.min_stack_space_usb_irq;
        odrv.system_stats_.stack_usage_startup = stack_size_default_task - odrv.system_stats_.min_stack_space_startup;
        odrv.system_stats_.stack_usage_can = odCAN->stack_size_ - odrv.system_stats_.min_stack_space_can;
    }
}
}

int odrive_main(void) {
    // Init timers
    board_init();

    // Start ADC for temperature measurements and user measurements
    start_general_purpose_adc();

    // TODO: make dynamically reconfigurable
#if HW_VERSION_MAJOR == 3 && HW_VERSION_MINOR >= 3
    if (odrv.config_.enable_uart) {
        GPIO_InitTypeDef GPIO_InitStruct;

        // make sure nothing is hogging the GPIO's
        Stm32Gpio{GPIO_1_GPIO_Port, GPIO_1_Pin}.unsubscribe();
        Stm32Gpio{GPIO_2_GPIO_Port, GPIO_2_Pin}.unsubscribe();

        GPIO_InitStruct.Pin = GPIO_1_Pin;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_PULLDOWN;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF8_UART4;
        HAL_GPIO_Init(GPIO_1_GPIO_Port, &GPIO_InitStruct);

        GPIO_InitStruct.Pin = GPIO_2_Pin;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF8_UART4;
        HAL_GPIO_Init(GPIO_2_GPIO_Port, &GPIO_InitStruct);
    }
#endif
    //osDelay(100);
    // Init communications (this requires the axis objects to be constructed)
    init_communication();

    // Start pwm-in compare modules
    // must happen after communication is initialized
    pwm_in_init();

    // Setup hardware for all components
    for (size_t i = 0; i < AXIS_COUNT; ++i) {
        if (!axes[i]->setup()) {
            for (;;); // TODO: proper error handling
        }
    }

    for(auto& axis : axes){
        axis->encoder_.setup();
    }

    // Start PWM and enable adc interrupts/callbacks
    start_adc_pwm();

    // This delay serves two purposes:
    //  - Let the current sense calibration converge (the current
    //    sense interrupts are firing in background by now)
    //  - Allow a user to interrupt the code, e.g. by flashing a new code,
    //    before it does anything crazy
    // TODO make timing a function of calibration filter tau
    osDelay(1500);

    // Start state machine threads. Each thread will go through various calibration
    // procedures and then run the actual controller loops.
    // TODO: generalize for AXIS_COUNT != 2
    for (size_t i = 0; i < AXIS_COUNT; ++i) {
        axes[i]->start_thread();
    }

    start_analog_thread();

    odrv.system_stats_.fully_booted = true;
    return 0;
}
