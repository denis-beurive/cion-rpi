#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

// Get the GPIO chip name:
//
//     $ gpiodetect
//     gpiochip0 [pinctrl-bcm2835] (54 lines)
//     gpiochip1 [brcmvirt-gpio] (2 lines)
//     gpiochip2 [raspberrypi-exp-gpio] (8 lines)
//
// => The chip name is "gpiochip0".

#define CHIP_NAME "gpiochip0"

// Get the number of the:
//
//        $ gpioinfo
//        ...
//        line  16:     "GPIO16"       unused   input  active-high
//        line  17:     "GPIO17"       unused   input  active-high
//        line  18:     "GPIO18"       unused   input  active-high
//        ...
//
// => The ID for "GPIO16" is 16.
// => The ID for "GPIO17" is 17.

#define RED_LED_LINE 16
#define GREEN_LED_LINE 17

#define NUMBER_OF_LED 2 // The green LED and the red LED.

/**
 * Data passed to the function that implements a thread used to control a LED.
 */

struct issuer_args {
    /** The number of seconds to wait for a LED to change its state. */
    time_t duration_sec;
    /** The number of nano seconds to wait for a LED to change its state.
     *  This value must be in the range [0, 999999999]
     */
    long duration_nano_sec;
    /** The number of changes of state. */
    long count;
    /** The (GPIO) line ID that controls the state of the LED. */
    int line_id;
    const char *name;
    /** The (GPIO) chip ID. */
    struct gpiod_chip *chip;
};

/**
 * Print an error message and terminate the program.
 * @param message The message to print.
 */

void error(char *message) {
    fprintf(stderr, "ERROR: %s\n", message);
    exit(1);
}

/**
 * Implement the thread that controls a LED.
 * @param in_args Pointer to `struct led_args`.
 */

void* led_thread(void *in_args) {
    struct issuer_args *args = (struct issuer_args*)in_args;
    struct gpiod_line  *led;
    struct timespec duration = { args->duration_sec, args->duration_nano_sec };
    struct timespec remaining;
    struct timespec remaining_again;
    struct timespec *d, *r;

    // Open GPIO lines. Set the line as output.
    led = gpiod_chip_get_line(args->chip, args->line_id);
    if (NULL == led) {
        error("cannot get the line");
    }

    // Open LED lines for output
    if (-1 == gpiod_line_request_output(led, args->name, 0)) {
        gpiod_line_release(led);
        error("cannot request the output");
    }

    for (long cycle=0; cycle<args->count; cycle++) {
        int state = (cycle & 0x1) != 0;
        int done;

        d = &duration;
        r = &remaining;
        printf("G [%4ld] Set %s\n", cycle, state ? "up" : "down");
        if (-1 == gpiod_line_set_value(led, state)) {
            error("cannot change the value of the output");
        }

        do {
            done = 1;
            if (-1 == nanosleep(d, r)) {
                switch (errno) {
                    case EINTR: {
                        d = r;
                        r = &remaining_again;
                        done = 0;
                    }; break;
                    case EFAULT: error("problem with copying information from user space");
                    case EINVAL: error("unexpected period duration");
                    default: error("unexpected errno value");
                }
            }
        } while(! done);
    }

    // Avoid useless current drain.
    if (-1 == gpiod_line_set_value(led, 0)) {
        error("cannot change the value of the output");
    }

    gpiod_line_release(led);
    return NULL;
}



int main()
{
    struct gpiod_chip  *chip;
    struct issuer_args green_args, red_args;
    pthread_t          all_threads[NUMBER_OF_LED];
    int       all_threads_index = 0;

    // Open GPIO chip
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (NULL == chip) {
        error("cannot open the chip");
    }

    green_args.duration_sec      = 0;
    green_args.duration_nano_sec = 999999999 / 2; // 1/2 second
    green_args.count             = 50;
    green_args.line_id           = GREEN_LED_LINE;
    green_args.name              = "green";
    green_args.chip              = chip;

    red_args.duration_sec      = 0;
    red_args.duration_nano_sec = 999999999 / 3; // 1/3 second
    red_args.count             = 50;
    red_args.line_id           = RED_LED_LINE;
    red_args.name              = "red";
    red_args.chip              = chip;

    // Start the green LED.
    if (0 != pthread_create(&all_threads[all_threads_index++], NULL,
                            &led_thread, (void*)&green_args)) {
        error("cannot create the thread for the green LED");
    }

    // Start the green LED.
    if (0 != pthread_create(&all_threads[all_threads_index++], NULL,
                            &led_thread, (void*)&red_args)) {
        error("cannot create the thread for the red LED");
    }

    // Wait for all threads.
    for (int i=0; i<all_threads_index; i++) {
        pthread_join(all_threads[i], NULL);
    }

    // Release the chip.
    gpiod_chip_close(chip);
    return 0;
}