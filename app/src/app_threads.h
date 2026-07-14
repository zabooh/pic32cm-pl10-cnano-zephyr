#ifndef APP_THREADS_H_
#define APP_THREADS_H_

/*
 * Central budget/priority table for the application threads.
 *
 * The threads themselves are still defined (K_THREAD_DEFINE) in the module
 * that owns their state - led_ctrl.c, pl10_adc.c, cmd_parser.c - so each
 * module stays self-contained. Only their stack sizes and priorities live
 * here, so every thread budget can be seen and tuned in one place instead of
 * hunting through the modules. For a live view of actual stack usage, run the
 * "threads" console command (see cmd_parser.c).
 */

/*
 * Stack sizes are set with headroom over the measured high-water mark (run
 * the "threads" command to see live usage). The initial 256/320 B for the
 * blink/ADC threads turned out to read 100%/97% used - bumped to 512 so
 * there's real margin, since a Cortex-M0+ overflow corrupts silently (no MPU
 * / no stack sentinel here). Re-check with "threads" after touching a thread's
 * call path (e.g. a deeper driver call, printf format).
 */
#define BLINK_THREAD_STACK_SIZE       352
#define BLINK_THREAD_PRIORITY         7

#define ADC_STREAM_THREAD_STACK_SIZE  352
#define ADC_STREAM_THREAD_PRIORITY    7

#define CMD_THREAD_STACK_SIZE         640
#define CMD_THREAD_PRIORITY           7

#define BUTTON_THREAD_STACK_SIZE      384
#define BUTTON_THREAD_PRIORITY        7

#endif /* APP_THREADS_H_ */
