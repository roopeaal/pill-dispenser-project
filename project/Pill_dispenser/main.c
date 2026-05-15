#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/i2c.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

// Use the AppKey assigned to the LoRa-E5 module in the course device list.
#define LORA_APP_KEY "00000000000000000000000000000000"

// UART0 is reserved for stdio/debug output, so the LoRa module is handled on UART1.
#define EEPROM_I2C i2c0
#define LORA_UART uart1

// Project board pins from the pill dispenser assignment.
#define SW0_PIN 9u
#define STATUS_LED_PIN 20u
#define PIEZO_PIN 27u
#define OPTO_PIN 28u

#define STEPPER_PIN_COUNT 4u
#define HALF_STEP_COUNT 8u
#define NOMINAL_HALF_STEPS_PER_REVOLUTION 4096u
#define STEP_DELAY_US 1500u

#define DISPENSE_INTERVAL_MS 30000u
#define PILL_FALL_WINDOW_MS 1500u
#define BUTTON_DEBOUNCE_MS 30u
#define ERROR_BLINKS 5u

#define EEPROM_ADDRESS 0x50u
#define EEPROM_SIZE 32768u
#define EEPROM_PAGE_SIZE 64u
#define EEPROM_WRITE_DELAY_MS 10u

#define LORA_RESPONSE_SIZE 192u
#define LORA_MESSAGE_SIZE 80u

#define MODE_WAIT_CALIBRATION 0u
#define MODE_READY_TO_START 1u
#define MODE_RUNNING 2u

#define STATE_MAGIC 0x50494C4Cu
#define STATE_VERSION 6u

typedef struct dispenser_state {
    // Saved to EEPROM so the dispenser can continue correctly after reset.
    uint32_t magic;
    uint8_t version;
    uint8_t mode;
    uint8_t pills_left;
    // This stays set in EEPROM if power is lost while the motor is turning.
    uint8_t turn_in_progress;
    uint16_t steps_per_revolution;
    uint16_t checksum;
} dispenser_state_t;

// Keep the project state in one fixed slot at the end of the AT24C256 EEPROM.
#define STATE_ADDRESS ((uint16_t)(EEPROM_SIZE - sizeof(dispenser_state_t)))

// Shared by the piezo interrupt and the main loop.
static volatile uint32_t piezo_edges = 0;

static uint16_t checksum16(const uint8_t *data, size_t length) {
    uint16_t checksum = 0;

    for (size_t i = 0; i < length; i++) {
        checksum = (uint16_t)(checksum + data[i]);
    }

    return checksum;
}

static void gpio_irq_callback(uint gpio, uint32_t events) {
    if (gpio == PIEZO_PIN && (events & GPIO_IRQ_EDGE_FALL)) {
        // Keep the ISR short: only record that the piezo saw a falling edge.
        piezo_edges++;
    }
}

static bool eeprom_read(uint16_t address, uint8_t *data, size_t length) {
    // AT24C256 uses a two-byte memory address before the actual read.
    uint8_t address_bytes[] = {
        (uint8_t)(address >> 8),
        (uint8_t)address
    };

    if ((uint32_t)address + length > EEPROM_SIZE) {
        return false;
    }

    if (i2c_write_blocking(EEPROM_I2C, EEPROM_ADDRESS, address_bytes, sizeof(address_bytes), true) !=
        (int)sizeof(address_bytes)) {
        return false;
    }

    return i2c_read_blocking(EEPROM_I2C, EEPROM_ADDRESS, data, length, false) == (int)length;
}

static bool eeprom_write(uint16_t address, const uint8_t *data, size_t length) {
    uint8_t write_buffer[2u + EEPROM_PAGE_SIZE];

    if ((uint32_t)address + length > EEPROM_SIZE) {
        return false;
    }

    while (length > 0) {
        // The EEPROM write must not cross a 64-byte page boundary.
        size_t page_space = EEPROM_PAGE_SIZE - (address % EEPROM_PAGE_SIZE);
        size_t chunk = length < page_space ? length : page_space;

        write_buffer[0] = (uint8_t)(address >> 8);
        write_buffer[1] = (uint8_t)address;
        memcpy(&write_buffer[2], data, chunk);

        if (i2c_write_blocking(EEPROM_I2C, EEPROM_ADDRESS, write_buffer, chunk + 2u, false) !=
            (int)(chunk + 2u)) {
            return false;
        }

        sleep_ms(EEPROM_WRITE_DELAY_MS);
        address = (uint16_t)(address + chunk);
        data += chunk;
        length -= chunk;
    }

    return true;
}

static uint16_t state_checksum(const dispenser_state_t *state) {
    dispenser_state_t copy = *state;

    // The checksum field itself is ignored while calculating the checksum.
    copy.checksum = 0;
    return checksum16((const uint8_t *)&copy, sizeof(copy));
}

static void set_default_state(dispenser_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->magic = STATE_MAGIC;
    state->version = STATE_VERSION;
    state->mode = MODE_WAIT_CALIBRATION;
    state->pills_left = 0u;
    state->turn_in_progress = 0u;
    state->steps_per_revolution = NOMINAL_HALF_STEPS_PER_REVOLUTION;
    state->checksum = state_checksum(state);
}

static bool state_is_valid(const dispenser_state_t *state) {
    // Reject old, empty or corrupted EEPROM contents before using them.
    if (state->magic != STATE_MAGIC || state->version != STATE_VERSION) {
        return false;
    }

    if (state->mode > MODE_RUNNING || state->pills_left > 7u || state->turn_in_progress > 1u ||
        state->steps_per_revolution < NOMINAL_HALF_STEPS_PER_REVOLUTION / 2u ||
        state->steps_per_revolution > NOMINAL_HALF_STEPS_PER_REVOLUTION * 2u) {
        return false;
    }

    return state->checksum == state_checksum(state);
}

static bool load_state(dispenser_state_t *state) {
    if (eeprom_read(STATE_ADDRESS, (uint8_t *)state, sizeof(*state)) && state_is_valid(state)) {
        return true;
    }

    set_default_state(state);
    return false;
}

static bool save_state(dispenser_state_t *state) {
    state->checksum = state_checksum(state);
    return eeprom_write(STATE_ADDRESS, (const uint8_t *)state, sizeof(*state));
}

static void init_eeprom_i2c(void) {
    // AT24C256 EEPROM is connected to I2C0 on the second-year board.
    const uint eeprom_sda_pin = 16u;
    const uint eeprom_scl_pin = 17u;
    const uint eeprom_baud_rate = 100000u;

    i2c_init(EEPROM_I2C, eeprom_baud_rate);
    gpio_set_function(eeprom_sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(eeprom_scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(eeprom_sda_pin);
    gpio_pull_up(eeprom_scl_pin);
}

static void init_lora_uart(void) {
    // LoRa-E5 uses 9600 baud AT commands on GP4/GP5.
    const uint lora_tx_pin = 4u;
    const uint lora_rx_pin = 5u;
    const uint lora_baud_rate = 9600u;

    uart_init(LORA_UART, lora_baud_rate);
    uart_set_format(LORA_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(LORA_UART, true);
    gpio_set_function(lora_tx_pin, GPIO_FUNC_UART);
    gpio_set_function(lora_rx_pin, GPIO_FUNC_UART);
}

static void init_gpio(const uint stepper_pins[STEPPER_PIN_COUNT]) {
    // Inputs use pull-ups because the board switches/sensors pull the line low.
    gpio_init(SW0_PIN);
    gpio_set_dir(SW0_PIN, GPIO_IN);
    gpio_pull_up(SW0_PIN);

    gpio_init(STATUS_LED_PIN);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_PIN, 0);

    gpio_init(OPTO_PIN);
    gpio_set_dir(OPTO_PIN, GPIO_IN);
    gpio_pull_up(OPTO_PIN);

    gpio_init(PIEZO_PIN);
    gpio_set_dir(PIEZO_PIN, GPIO_IN);
    gpio_pull_up(PIEZO_PIN);
    gpio_set_irq_enabled_with_callback(PIEZO_PIN, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_callback);

    for (uint i = 0; i < STEPPER_PIN_COUNT; i++) {
        gpio_init(stepper_pins[i]);
        gpio_set_dir(stepper_pins[i], GPIO_OUT);
        gpio_put(stepper_pins[i], 0);
    }
}

static bool sw0_pressed(void) {
    // SW0 is active low.
    return gpio_get(SW0_PIN) == 0;
}

static void wait_for_button_release(void) {
    while (sw0_pressed()) {
        sleep_ms(10);
    }

    sleep_ms(BUTTON_DEBOUNCE_MS);
}

static void wait_for_button_with_blink(void) {
    bool led_on = false;
    absolute_time_t next_toggle = make_timeout_time_ms(0);

    // During the initial wait the assignment expects the status LED to blink.
    while (!sw0_pressed()) {
        if (absolute_time_diff_us(get_absolute_time(), next_toggle) <= 0) {
            led_on = !led_on;
            gpio_put(STATUS_LED_PIN, led_on);
            next_toggle = make_timeout_time_ms(250);
        }

        sleep_ms(5);
    }

    wait_for_button_release();
    gpio_put(STATUS_LED_PIN, 0);
}

static void wait_for_button_led_on(void) {
    // After calibration the LED stays on until the user starts the schedule.
    gpio_put(STATUS_LED_PIN, 1);

    while (!sw0_pressed()) {
        sleep_ms(10);
    }

    wait_for_button_release();
}

static void blink_status(uint count) {
    for (uint i = 0; i < count; i++) {
        gpio_put(STATUS_LED_PIN, 0);
        sleep_ms(200);
        gpio_put(STATUS_LED_PIN, 1);
        sleep_ms(200);
    }
}

static void apply_step_outputs(const uint stepper_pins[STEPPER_PIN_COUNT], uint current_step) {
    // Half-step sequence for the 4-wire stepper used by the dispenser wheel.
    static const uint8_t half_step_sequence[HALF_STEP_COUNT][STEPPER_PIN_COUNT] = {
        {1, 0, 0, 0},
        {1, 1, 0, 0},
        {0, 1, 0, 0},
        {0, 1, 1, 0},
        {0, 0, 1, 0},
        {0, 0, 1, 1},
        {0, 0, 0, 1},
        {1, 0, 0, 1}
    };

    for (uint i = 0; i < STEPPER_PIN_COUNT; i++) {
        gpio_put(stepper_pins[i], half_step_sequence[current_step][i]);
    }
}

static void step_forward(const uint stepper_pins[STEPPER_PIN_COUNT], uint *current_step) {
    *current_step = (*current_step + 1u) % HALF_STEP_COUNT;
    apply_step_outputs(stepper_pins, *current_step);
    sleep_us(STEP_DELAY_US);
}

static void step_backward(const uint stepper_pins[STEPPER_PIN_COUNT], uint *current_step) {
    *current_step = *current_step == 0u ? HALF_STEP_COUNT - 1u : *current_step - 1u;
    apply_step_outputs(stepper_pins, *current_step);
    sleep_us(STEP_DELAY_US);
}

static void move_steps_forward(const uint stepper_pins[STEPPER_PIN_COUNT],
                               uint *current_step,
                               uint32_t steps) {
    for (uint32_t step = 0; step < steps; step++) {
        step_forward(stepper_pins, current_step);
    }
}

static void release_stepper(const uint stepper_pins[STEPPER_PIN_COUNT]) {
    for (uint i = 0; i < STEPPER_PIN_COUNT; i++) {
        gpio_put(stepper_pins[i], 0);
    }
}

static bool opto_opening_at_sensor(void) {
    // The opto fork output is low when the calibration opening is at the sensor.
    return gpio_get(OPTO_PIN) == 0;
}

static uint32_t steps_per_compartment(const dispenser_state_t *state) {
    // There are eight pill positions, so one compartment is one eighth of a turn.
    return (state->steps_per_revolution + 4u) / 8u;
}

static int32_t wait_for_opto_falling_edge(const uint stepper_pins[STEPPER_PIN_COUNT],
                                          uint *current_step,
                                          uint32_t max_steps) {
    bool previous_high = !opto_opening_at_sensor();

    // A falling edge marks the moment when the opening reaches the opto fork.
    for (uint32_t steps = 1; steps <= max_steps; steps++) {
        step_forward(stepper_pins, current_step);

        bool current_high = !opto_opening_at_sensor();
        if (previous_high && !current_high) {
            return (int32_t)steps;
        }

        previous_high = current_high;
    }

    return -1;
}

static int32_t wait_for_opto_rising_edge(const uint stepper_pins[STEPPER_PIN_COUNT],
                                         uint *current_step,
                                         uint32_t max_steps) {
    bool previous_open = opto_opening_at_sensor();

    // A rising edge marks the end of the same opto opening.
    for (uint32_t steps = 1; steps <= max_steps; steps++) {
        step_forward(stepper_pins, current_step);

        bool current_open = opto_opening_at_sensor();
        if (previous_open && !current_open) {
            return (int32_t)steps;
        }

        previous_open = current_open;
    }

    return -1;
}

static int32_t wait_for_opto_entry_backward(const uint stepper_pins[STEPPER_PIN_COUNT],
                                            uint *current_step,
                                            uint32_t max_steps) {
    bool previous_open = opto_opening_at_sensor();

    // Moving backwards avoids advancing the wheel through remaining pills.
    for (uint32_t steps = 1; steps <= max_steps; steps++) {
        step_backward(stepper_pins, current_step);

        bool current_open = opto_opening_at_sensor();
        if (!previous_open && current_open) {
            return (int32_t)steps;
        }

        previous_open = current_open;
    }

    return -1;
}

static int32_t wait_for_opto_exit_backward(const uint stepper_pins[STEPPER_PIN_COUNT],
                                           uint *current_step,
                                           uint32_t max_steps) {
    bool previous_open = opto_opening_at_sensor();

    for (uint32_t steps = 1; steps <= max_steps; steps++) {
        step_backward(stepper_pins, current_step);

        bool current_open = opto_opening_at_sensor();
        if (previous_open && !current_open) {
            return (int32_t)steps;
        }

        previous_open = current_open;
    }

    return -1;
}

static bool reverse_to_reference_opening(const dispenser_state_t *state,
                                         const uint stepper_pins[STEPPER_PIN_COUNT],
                                         uint *current_step) {
    uint32_t search_limit = (uint32_t)state->steps_per_revolution * 2u;

    printf("Recovery: reversing to calibration opening\r\n");

    if (!opto_opening_at_sensor() &&
        wait_for_opto_entry_backward(stepper_pins, current_step, search_limit) < 0) {
        release_stepper(stepper_pins);
        return false;
    }

    int32_t opening_width = wait_for_opto_exit_backward(stepper_pins, current_step, search_limit);
    if (opening_width < 0) {
        release_stepper(stepper_pins);
        return false;
    }

    move_steps_forward(stepper_pins, current_step, (uint32_t)opening_width / 2u);
    release_stepper(stepper_pins);
    printf("Recovery: centered on calibration opening\r\n");
    return true;
}

static bool calibrate_dispenser(dispenser_state_t *state,
                                const uint stepper_pins[STEPPER_PIN_COUNT],
                                uint *current_step) {
    uint32_t search_limit = NOMINAL_HALF_STEPS_PER_REVOLUTION * 2u;

    // Find both edges of the opto opening so the wheel can stop in the center.
    printf("Calibration: seeking reference opening\r\n");
    if (wait_for_opto_falling_edge(stepper_pins, current_step, search_limit) < 0) {
        release_stepper(stepper_pins);
        return false;
    }

    int32_t opening_width = wait_for_opto_rising_edge(stepper_pins, current_step, search_limit);
    if (opening_width < 0) {
        release_stepper(stepper_pins);
        return false;
    }

    printf("Calibration: opto opening width %ld half steps\r\n", (long)opening_width);
    printf("Calibration: measuring one full revolution\r\n");
    int32_t steps_to_next_opening = wait_for_opto_falling_edge(stepper_pins, current_step, search_limit);

    if (steps_to_next_opening < 0) {
        release_stepper(stepper_pins);
        return false;
    }

    int32_t measured_steps = opening_width + steps_to_next_opening;

    // Ignore impossible measurements caused by wiring or sensor errors.
    if (measured_steps < (int32_t)(NOMINAL_HALF_STEPS_PER_REVOLUTION / 2u) ||
        measured_steps > (int32_t)(NOMINAL_HALF_STEPS_PER_REVOLUTION * 2u)) {
        release_stepper(stepper_pins);
        return false;
    }

    state->steps_per_revolution = (uint16_t)measured_steps;
    move_steps_forward(stepper_pins, current_step, (uint32_t)opening_width / 2u);

    release_stepper(stepper_pins);
    printf("Calibration: measured %u half steps/revolution, %lu half steps/compartment\r\n",
           state->steps_per_revolution,
           (unsigned long)steps_per_compartment(state));
    printf("Calibration: centered on opto opening\r\n");
    return true;
}

static void reset_piezo_edges(void) {
    // Disable interrupts briefly so the ISR cannot update the counter mid-reset.
    uint32_t irq_state = save_and_disable_interrupts();

    piezo_edges = 0;
    restore_interrupts(irq_state);
}

static uint32_t read_piezo_edges(void) {
    // Read the ISR-updated counter atomically.
    uint32_t irq_state = save_and_disable_interrupts();
    uint32_t edges = piezo_edges;

    restore_interrupts(irq_state);
    return edges;
}

static bool move_compartment(dispenser_state_t *state,
                             const uint stepper_pins[STEPPER_PIN_COUNT],
                             uint *current_step) {
    uint32_t compartment_steps = steps_per_compartment(state);

    // Start listening for a pill hit before the wheel begins to move.
    reset_piezo_edges();
    move_steps_forward(stepper_pins, current_step, compartment_steps);
    release_stepper(stepper_pins);

    // Give the falling pill time to hit the piezo after the motor stops.
    sleep_ms(PILL_FALL_WINDOW_MS);
    return read_piezo_edges() > 0;
}

static void clear_lora_rx(void) {
    // Drop old bytes so the next command is matched against its own response.
    while (uart_is_readable(LORA_UART)) {
        (void)uart_getc(LORA_UART);
    }
}

static void send_lora_command(const char *command) {
    clear_lora_rx();
    uart_puts(LORA_UART, command);
    uart_puts(LORA_UART, "\r\n");
}

static bool read_lora_line(char *buffer, size_t size, uint32_t timeout_ms) {
    size_t length = 0;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    // LoRa-E5 responses are line based and terminated by CR/LF.
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (!uart_is_readable(LORA_UART)) {
            sleep_ms(1);
            continue;
        }

        char ch = uart_getc(LORA_UART);
        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            if (length == 0) {
                continue;
            }

            buffer[length] = '\0';
            return true;
        }

        if (length < size - 1u) {
            buffer[length++] = ch;
        }
    }

    buffer[0] = '\0';
    return false;
}

static bool lora_expect_response(const char *command, const char *expected, uint32_t timeout_ms) {
    char response[LORA_RESPONSE_SIZE];

    send_lora_command(command);

    while (read_lora_line(response, sizeof(response), timeout_ms)) {
        // Do not print the AppKey back to the debug console.
        if (strstr(response, "+KEY:") != NULL) {
            printf("LoRa: +KEY: set\r\n");
        } else {
            printf("LoRa: %s\r\n", response);
        }

        if (strstr(response, "ERROR") != NULL) {
            return false;
        }

        if (strstr(response, expected) != NULL) {
            return true;
        }
    }

    return false;
}

static bool lora_join(void) {
    char response[LORA_RESPONSE_SIZE];
    bool joined = false;

    send_lora_command("AT+JOIN");

    // Join can report success before the final "Done" line arrives.
    while (read_lora_line(response, sizeof(response), 25000u)) {
        printf("LoRa: %s\r\n", response);

        if (strstr(response, "failed") != NULL || strstr(response, "ERROR") != NULL) {
            return false;
        }

        if (strstr(response, "Network joined") != NULL || strstr(response, "Joined already") != NULL) {
            joined = true;
        }

        if (joined && strstr(response, "Done") != NULL) {
            return true;
        }
    }

    return joined;
}

static bool configure_lora(void) {
    char command[LORA_RESPONSE_SIZE];

    // These commands follow the project handout: OTAA, Class A, port 8 and DR5.
    if (!lora_expect_response("AT", "+AT: OK", 1000u)) {
        printf("LoRa module not responding\r\n");
        return false;
    }

    if (!lora_expect_response("AT+MODE=LWOTAA", "+MODE:", 1000u)) {
        return false;
    }

    snprintf(command, sizeof(command), "AT+KEY=APPKEY,\"%s\"", LORA_APP_KEY);
    if (!lora_expect_response(command, "+KEY:", 1000u)) {
        return false;
    }

    if (!lora_expect_response("AT+CLASS=A", "+CLASS:", 1000u) ||
        !lora_expect_response("AT+PORT=8", "+PORT:", 1000u) ||
        !lora_expect_response("AT+DR=5", "+DR:", 1000u)) {
        return false;
    }

    printf("LoRa join\r\n");
    if (lora_join()) {
        printf("LoRa joined\r\n");
        return true;
    }

    printf("LoRa join failed\r\n");
    return false;
}

static void lora_send_status(bool *lora_joined, const char *event, uint8_t pills_left) {
    char command[LORA_MESSAGE_SIZE + 16u];
    char response[LORA_RESPONSE_SIZE];
    char message[LORA_MESSAGE_SIZE];

    // Dispensing must continue even if LoRa is not available.
    if (!*lora_joined) {
        return;
    }

    snprintf(message, sizeof(message), "%s left=%u", event, pills_left);
    snprintf(command, sizeof(command), "AT+MSG=\"%s\"", message);
    send_lora_command(command);

    while (read_lora_line(response, sizeof(response), 20000u)) {
        printf("LoRa: %s\r\n", response);

        if (strstr(response, "ERROR") != NULL) {
            *lora_joined = false;
            return;
        }

        if (strstr(response, "+MSG: Done") != NULL) {
            return;
        }
    }

    *lora_joined = false;
}

static void dispense_once(dispenser_state_t *state,
                          const uint stepper_pins[STEPPER_PIN_COUNT],
                          uint *current_step,
                          bool *lora_joined) {
    printf("Dispensing, pills left before turn: %u\r\n", state->pills_left);
    // Mark the turn before moving so a reset can be handled on the next boot.
    state->turn_in_progress = 1u;
    (void)save_state(state);
    lora_send_status(lora_joined, "turn_start", state->pills_left);

    bool detected = move_compartment(state, stepper_pins, current_step);

    if (state->pills_left > 0u) {
        state->pills_left--;
    }

    state->turn_in_progress = 0u;

    // The piezo result decides whether the turn counted as a successful dispense.
    if (detected) {
        printf("Pill detected, pills left: %u\r\n", state->pills_left);
        lora_send_status(lora_joined, "dispensed", state->pills_left);
    } else {
        printf("No pill detected, pills left: %u\r\n", state->pills_left);
        lora_send_status(lora_joined, "no_pill", state->pills_left);
        blink_status(ERROR_BLINKS);
    }

    if (state->pills_left == 0u) {
        printf("Dispenser empty, returning to calibration wait\r\n");
        // After seven compartments the user must refill and calibrate again.
        state->mode = MODE_WAIT_CALIBRATION;
        lora_send_status(lora_joined, "empty", state->pills_left);
    }

    (void)save_state(state);
}

static bool recover_interrupted_turn(dispenser_state_t *state,
                                     const uint stepper_pins[STEPPER_PIN_COUNT],
                                     uint *current_step,
                                     bool *lora_joined) {
    printf("Power was lost during motor turn\r\n");
    lora_send_status(lora_joined, "power_loss_turn", state->pills_left);

    if (!reverse_to_reference_opening(state, stepper_pins, current_step)) {
        printf("Recovery failed, manual calibration needed\r\n");
        state->turn_in_progress = 0u;
        state->mode = MODE_WAIT_CALIBRATION;
        (void)save_state(state);
        lora_send_status(lora_joined, "recovery_failed", state->pills_left);
        return false;
    }

    // Empty compartments already dispensed can pass the drop tube safely.
    uint32_t completed_dispenses = 7u - state->pills_left;
    uint32_t restore_steps = completed_dispenses * steps_per_compartment(state);

    if (restore_steps > 0u) {
        printf("Recovery: returning to last completed position\r\n");
        move_steps_forward(stepper_pins, current_step, restore_steps);
        release_stepper(stepper_pins);
    }

    state->turn_in_progress = 0u;
    (void)save_state(state);
    printf("Recovery complete, completing interrupted turn now\r\n");
    lora_send_status(lora_joined, "recovered_position", state->pills_left);
    return true;
}

int main(void) {
    // The stepper pin order matches the wiring order used by the half-step table.
    const uint stepper_pins[STEPPER_PIN_COUNT] = {2u, 3u, 6u, 13u};
    uint current_step = 0;
    dispenser_state_t state;
    bool lora_joined = false;
    absolute_time_t next_dispense_time = make_timeout_time_ms(DISPENSE_INTERVAL_MS);

    stdio_init_all();
    init_eeprom_i2c();
    init_lora_uart();
    init_gpio(stepper_pins);
    sleep_ms(500);

    bool restored = load_state(&state);
    bool cleared_saved_state = false;

    // Holding SW0 during boot starts from calibration instead of resuming EEPROM state.
    if (sw0_pressed()) {
        set_default_state(&state);
        (void)save_state(&state);
        wait_for_button_release();
        restored = false;
        cleared_saved_state = true;
    } else {
        // Save once after loading so a default state is written to an empty EEPROM.
        (void)save_state(&state);
    }

    printf("\r\nPill dispenser project\r\n");
    printf("state: %s, mode=%u, pills_left=%u, previous_turn_in_progress=%u\r\n",
           restored ? "restored" : "default",
           state.mode,
           state.pills_left,
           state.turn_in_progress);
    if (cleared_saved_state) {
        printf("Saved state cleared by SW0 at boot\r\n");
    }

    lora_joined = configure_lora();
    lora_send_status(&lora_joined, state.turn_in_progress ? "boot_recover" : "boot", state.pills_left);

    bool complete_interrupted_turn = false;

    // If reset happened during motor movement, recover position by reversing.
    if (state.turn_in_progress) {
        printf("Recovering from reset during motor turn\r\n");
        complete_interrupted_turn = recover_interrupted_turn(&state, stepper_pins, &current_step, &lora_joined);
    }

    if (complete_interrupted_turn && state.mode == MODE_RUNNING && state.pills_left > 0u) {
        printf("Completing interrupted dispense before returning to schedule\r\n");
        lora_send_status(&lora_joined, "retry_interrupted_turn", state.pills_left);
        dispense_once(&state, stepper_pins, &current_step, &lora_joined);
    }

    if (state.mode == MODE_RUNNING) {
        // Start a fresh 30 second interval after boot and LoRa setup.
        next_dispense_time = make_timeout_time_ms(DISPENSE_INTERVAL_MS);
        printf("Resuming schedule, next dispense in 30 seconds\r\n");
    }

    while (true) {
        if (state.mode == MODE_WAIT_CALIBRATION) {
            printf("Press SW0 to calibrate\r\n");
            wait_for_button_with_blink();
            printf("Calibrating dispenser\r\n");
            lora_send_status(&lora_joined, "calibration_start", state.pills_left);

            if (calibrate_dispenser(&state, stepper_pins, &current_step)) {
                // A fresh calibration means the wheel is full again.
                state.mode = MODE_READY_TO_START;
                state.pills_left = 7u;
                state.turn_in_progress = 0u;
                (void)save_state(&state);
                gpio_put(STATUS_LED_PIN, 1);
                printf("Calibration complete, press SW0 to start 30 s schedule\r\n");
                lora_send_status(&lora_joined, "calibrated", state.pills_left);
            } else {
                printf("Calibration failed, opto opening not found\r\n");
                blink_status(ERROR_BLINKS);
                lora_send_status(&lora_joined, "calibration_failed", state.pills_left);
            }
        } else if (state.mode == MODE_READY_TO_START) {
            printf("Press SW0 to start dispensing schedule\r\n");
            wait_for_button_led_on();
            state.mode = MODE_RUNNING;
            (void)save_state(&state);
            // The assignment starts the 30 second interval from this button press.
            next_dispense_time = make_timeout_time_ms(DISPENSE_INTERVAL_MS);
            printf("Schedule started, first dispense in 30 seconds\r\n");
            lora_send_status(&lora_joined, "started", state.pills_left);
        } else if (state.mode == MODE_RUNNING) {
            gpio_put(STATUS_LED_PIN, 1);

            if (absolute_time_diff_us(get_absolute_time(), next_dispense_time) <= 0) {
                dispense_once(&state, stepper_pins, &current_step, &lora_joined);
                next_dispense_time = make_timeout_time_ms(DISPENSE_INTERVAL_MS);
            }

            sleep_ms(10);
        } else {
            // Unknown saved mode: return to the documented start state.
            set_default_state(&state);
            (void)save_state(&state);
        }
    }
}
