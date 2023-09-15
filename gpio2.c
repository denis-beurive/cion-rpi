#include <gpiod.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <string.h>
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
// => The ID for "GPIO 16" is 16.
// => The ID for "GPIO 17" is 17.
// => The ID for "GPIO 21" is 21.

#define GPIO_16 16
#define GPIO_17 17
#define GPIO_21 21
#define NUMBER_OF_THREAD 2

/**
 * Data passed to the function that implements a thread used to control the issuer.
 */

struct issuer_args {
    /** The number of seconds to wait for the signal to change its level. */
    time_t duration_sec;
    /** The number of nano seconds to wait for the signal to change its level.
     *  This value must be in the range [0, 999999999]
     */
    long duration_nano_sec;
    /** The number of changes of state. */
    long count;
    /** The (GPIO) line ID that controls the state of the LED. */
    int line_id;
    /** An arbitrary name for the line. */
    char *name;
    /** The (GPIO) chip ID. */
    struct gpiod_chip *chip;
};

/**
 * Data passed to the function that implements a thread used to control the receiver.
 */

struct receiver_args {
    /** The number of changes of state before termination. */
    long count;
    /** The (GPIO) line ID that receives the "message" from the issuer. */
    int  receiver_line_id;
    /** The (GPIO) line ID used to control the LED. */
    int  controller_line_id;
    /** An arbitrary name for the line that receives the "message" from the issuer. */
    char *receiver_name;
    /** An arbitrary name for the line that used to control the LED. */
    char *controller_name;
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
 * Implement the thread that controls the issuer.
 * @param in_args Pointer to `struct issuer_args`.
 */

void* issuer_thread(void *in_args) {
    struct issuer_args *args = (struct issuer_args*)in_args;
    struct gpiod_line  *issuer;
    struct timespec duration = { args->duration_sec, args->duration_nano_sec };
    struct timespec remaining;
    struct timespec remaining_again;
    struct timespec *d, *r;

    // Open GPIO line. Set the line's mode to output.
    issuer = gpiod_chip_get_line(args->chip, args->line_id);
    if (NULL == issuer) {
        error("issuer: cannot get the line");
    }

    // Open issuer's line for output
    if (-1 == gpiod_line_request_output(issuer, args->name, 0)) {
        gpiod_line_release(issuer);
        error("issuer: cannot set the line's mode to output");
    }

    for (long cycle=0; cycle<args->count; cycle++) {
        int state = (cycle & 0x1) != 0;
        int done;

        d = &duration;
        r = &remaining;
        printf("I [%4ld] Set %s\n", cycle, state ? "up" : "down");
        if (-1 == gpiod_line_set_value(issuer, state)) {
            error("issuer: cannot change the value of the output");
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
                    case EFAULT: error("issuer: problem with copying information from user space");
                    case EINVAL: error("issuer: unexpected period duration");
                    default: error("issuer: unexpected errno value");
                }
            }
        } while(! done);
    }

    // Avoid useless current drain.
    if (-1 == gpiod_line_set_value(issuer, 0)) {
        error("issuer: cannot change the value of the output");
    }

    // TODO: refactor

    gpiod_line_release(issuer);

    // Set issuer's line for input (this is a safety measure).
    issuer = gpiod_chip_get_line(args->chip, args->line_id);
    if (NULL == issuer) {
        error("issuer: cannot get the line");
    }

    if (-1 == gpiod_line_request_input(issuer, args->name)) {
        gpiod_line_release(issuer);
        printf("errno: %d\n", errno);
        printf("=> %s\n", strerror(errno));
        error("issuer: cannot set the line's mode to input");
    }

    gpiod_line_release(issuer);

    return NULL;
}


/**
 * Implement the thread that controls the receiver.
 * @param in_args Pointer to `struct receiver_args`.
 */

void* receiver_thread(void *in_args) {
    struct receiver_args *args = (struct receiver_args*)in_args;
    struct gpiod_line  *receiver;
    struct gpiod_line  *controller;
    struct gpiod_line_event event;
    int state;

    // Open GPIO lines.
    receiver = gpiod_chip_get_line(args->chip, args->receiver_line_id);
    if (NULL == receiver) {
        error("receiver: cannot get the line used to receive messages from the issuer");
    }
    controller = gpiod_chip_get_line(args->chip, args->controller_line_id);
    if (NULL == receiver) {
        error("receiver: cannot get the line used to control the LED");
    }

    // Set lines' modes.
//    if (-1 == gpiod_line_request_input(receiver, args->receiver_name)) {
//        gpiod_line_release(receiver);
//        error("receiver: cannot set the line's mode to input");
//    }
    if (-1 == gpiod_line_request_output(controller, args->controller_name, 0)) {
        gpiod_line_release(receiver);
        gpiod_line_release(controller);
        error("receiver: cannot set the line's mode to input");
    }

    if (-1 == gpiod_line_request_both_edges_events(receiver, args->receiver_name)) {
        printf("errno: %d\n", errno);
        printf("=> %s\n", strerror(errno));
        gpiod_line_release(receiver);
        gpiod_line_release(controller);
        error("receiver: cannot set the line's callbacks");
    }

    for (long cycle=0; cycle<args->count; cycle++) {

        // Wait for an event.
        if (-1 == gpiod_line_event_wait(receiver, NULL)) {
            gpiod_line_release(receiver);
            gpiod_line_release(controller);
            error("receiver: error while waiting for an event");
        }

        if (-1 == gpiod_line_event_read(receiver, &event)) {
            gpiod_line_release(receiver);
            gpiod_line_release(controller);
            error("receiver: error while reading the event");
        }

        printf("Get an event!\n");
        state = !state;
        if (-1 == gpiod_line_set_value(controller, state)) {
            gpiod_line_release(receiver);
            gpiod_line_release(controller);
            error("contoller: cannot change the value of the output");
        }
    }

    // Avoid useless current drain.
    if (-1 == gpiod_line_set_value(controller, 0)) {
        error("receiver: cannot change the value of the output for the line that controls the LED");
    }

    // TODO: refactor

    gpiod_line_release(receiver);
    gpiod_line_release(controller);

    controller = gpiod_chip_get_line(args->chip, args->controller_line_id);
    if (NULL == receiver) {
        error("receiver: cannot get the line used to control the LED");
    }

    if (-1 == gpiod_line_request_input(controller, args->controller_name)) {
        gpiod_line_release(controller);
        error("receiver: cannot set the line's mode to input");
    }

    gpiod_line_release(controller);

    return NULL;
}




int main()
{
    struct gpiod_chip  *chip;
    struct issuer_args issuer_arg;
    struct receiver_args receiver_arg;
    pthread_t          all_threads[NUMBER_OF_THREAD];
    int                all_threads_index = 0;

    // Open GPIO chip
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (NULL == chip) {
        error("cannot open the chip");
    }

    issuer_arg.duration_sec      = 1;
    issuer_arg.duration_nano_sec = 0;
    issuer_arg.count             = 20;
    issuer_arg.line_id           = GPIO_16;
    issuer_arg.name              = "issuer";
    issuer_arg.chip              = chip;

    receiver_arg.count              = 10;
    receiver_arg.receiver_line_id   = GPIO_21;
    receiver_arg.controller_line_id = GPIO_17;
    receiver_arg.chip               = chip;
    receiver_arg.receiver_name      = "receiver";
    receiver_arg.controller_name    = "controller";

    // Start the issuer.
    if (0 != pthread_create(&all_threads[all_threads_index++], NULL,
                            &issuer_thread, (void*)&issuer_arg)) {
        error("cannot create the thread for the issuer");
    }

    // Start the receiver.
    if (0 != pthread_create(&all_threads[all_threads_index++], NULL,
                            &receiver_thread, (void*)&receiver_arg)) {
        error("cannot create the thread for the receiver");
    }

    // Wait for all threads.
    for (int i=0; i<all_threads_index; i++) {
        pthread_join(all_threads[i], NULL);
    }

    // Release the chip.
    gpiod_chip_close(chip);
    return 0;
}