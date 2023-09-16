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
static struct gpiod_chip *CHIP;

/**
 * Print an error message and terminate the program.
 * @param message The message to print.
 */

void error(char *message) {
    fprintf(stderr, "ERROR: %s\n", message);
    exit(1);
}

/**
 * Reset the GPIO.
 */

void reset_gpio() {
    printf("Reset GPIO\n");
    int pins[3] = { GPIO_16, GPIO_17, GPIO_21};
    for (int i=0; i<3; i++) {
        struct gpiod_line *l = gpiod_chip_get_line(CHIP, pins[i]);
        if (NULL == l) {
            fprintf(stderr, "Warning: error while resetting line #%d (gpiod_chip_get_line() - errno: %d)\n", pins[i], errno);
            continue;
        }
        if (-1 == gpiod_line_request_input(l, "line")) {
            fprintf(stderr, "Warning: error while resetting line #%d (gpiod_line_request_input() - errno: %d)\n", pins[i], errno);
            continue;
        }
        gpiod_line_release(l);
    }
    gpiod_chip_close(CHIP);
}

// ---------------------------------------------------------------------------------
// ISSUER
// ---------------------------------------------------------------------------------

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
};

struct issuer_thread_resource {
    struct gpiod_line  *issuer;
};

void issuer_thread_init(struct issuer_thread_resource *resource) {
    resource->issuer = NULL;
}

void issuer_thread_terminate(struct issuer_thread_resource *resource) {
    if (NULL != resource->issuer) {
        gpiod_line_release(resource->issuer);
    }
}

void* issuer_thread(void *in_args) {
    struct issuer_thread_resource resource;
    struct issuer_args *args = (struct issuer_args*)in_args;
    struct gpiod_line  *issuer;
    struct timespec duration = { args->duration_sec, args->duration_nano_sec };
    struct timespec remaining;
    struct timespec remaining_again;
    struct timespec *d, *r;

    issuer_thread_init(&resource);

    // Open GPIO line. Set the line's mode to output.
    resource.issuer = gpiod_chip_get_line(CHIP, args->line_id);
    if (NULL == resource.issuer) {
        issuer_thread_terminate(&resource);
        error("issuer: cannot get the line");

    }

    // Open issuer's line for output
    if (-1 == gpiod_line_request_output(resource.issuer, "issuer", 0)) {
        issuer_thread_terminate(&resource);
        error("issuer: cannot set the line's mode to output");
    }

    for (long cycle=0; cycle<args->count; cycle++) {
        int value = (cycle & 0x1) != 0;
        int done;

        d = &duration;
        r = &remaining;
        printf("I [%4ld] Set %s\n", cycle, value ? "up" : "down");
        if (-1 == gpiod_line_set_value(resource.issuer, value)) {
            issuer_thread_terminate(&resource);
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
                    case EFAULT: {
                        issuer_thread_terminate(&resource);
                        error("issuer: problem with copying information from user space");
                    }
                    case EINVAL: {
                        issuer_thread_terminate(&resource);
                        error("issuer: unexpected period duration");
                    }
                    default: {
                        issuer_thread_terminate(&resource);
                        error("issuer: unexpected errno value");
                    }
                }
            }
        } while(! done);
    }

    issuer_thread_terminate(&resource);
    return NULL;
}

// ---------------------------------------------------------------------------------
// ISSUER
// ---------------------------------------------------------------------------------

struct receiver_args {
    /** The number of changes of state before termination. */
    long count;
    /** The (GPIO) line ID that receives the "message" from the issuer. */
    int  receiver_line_id;
    /** The (GPIO) line ID used to control the LED. */
    int  controller_line_id;
};

struct receiver_thread_resource {
    struct gpiod_line  *receiver;
    struct gpiod_line  *controller;
};

void receiver_thread_init(struct receiver_thread_resource *resource) {
    resource->receiver = NULL;
    resource->controller = NULL;
}

void receiver_thread_terminate(struct receiver_thread_resource *resource) {
    if (NULL != resource->receiver) {
        gpiod_line_release(resource->receiver);
    }
    if (NULL != resource->controller) {
        gpiod_line_release(resource->controller);
    }
}

void* receiver_thread(void *in_args) {
    struct receiver_thread_resource resource;
    struct receiver_args *args = (struct receiver_args*)in_args;
    struct gpiod_line_event event;
    int state;

    receiver_thread_init(&resource);

    // Open GPIO lines.
    resource.receiver = gpiod_chip_get_line(CHIP, args->receiver_line_id);
    if (NULL == resource.receiver) {
        receiver_thread_terminate(&resource);
        error("receiver: cannot get the line used to receive messages from the issuer");
    }
    resource.controller = gpiod_chip_get_line(CHIP, args->controller_line_id);
    if (NULL == resource.receiver) {
        receiver_thread_terminate(&resource);
        error("receiver: cannot get the line used to control the LED");
    }

    if (-1 == gpiod_line_request_output(resource.controller, "controller", 0)) {
        receiver_thread_terminate(&resource);
        error("receiver: cannot set the line's mode to input");
    }

    if (-1 == gpiod_line_request_both_edges_events(resource.receiver, "receiver")) {
        receiver_thread_terminate(&resource);
        error("receiver: cannot set the line's callbacks");
    }

    for (long cycle=0; cycle<args->count; cycle++) {

        // Wait for an event.
        if (-1 == gpiod_line_event_wait(resource.receiver, NULL)) {
            receiver_thread_terminate(&resource);
            error("receiver: error while waiting for an event");
        }

        if (-1 == gpiod_line_event_read(resource.receiver, &event)) {
            receiver_thread_terminate(&resource);
            error("receiver: error while reading the event");
        }

        printf("Get an event!\n");
        state = !state;
        if (-1 == gpiod_line_set_value(resource.controller, state)) {
            receiver_thread_terminate(&resource);
            error("contoller: cannot change the value of the output");
        }
    }

    receiver_thread_terminate(&resource);
    return NULL;
}




int main()
{
    struct issuer_args issuer_arg;
    struct receiver_args receiver_arg;
    pthread_t          all_threads[NUMBER_OF_THREAD];
    int                all_threads_index = 0;

    // Open GPIO chip
    CHIP = gpiod_chip_open_by_name(CHIP_NAME);
    if (NULL == CHIP) {
        error("cannot open the chip");
    }
    atexit(reset_gpio);

    issuer_arg.duration_sec      = 1;
    issuer_arg.duration_nano_sec = 0;
    issuer_arg.count             = 5;
    issuer_arg.line_id           = GPIO_16;

    receiver_arg.count              = 3;
    receiver_arg.receiver_line_id   = GPIO_21;
    receiver_arg.controller_line_id = GPIO_17;

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

    return 0;
}