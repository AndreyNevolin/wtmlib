/**
 * Copyright © 2018 Andrey Nevolin, https://github.com/AndreyNevolin
 * Twitter: @Andrey_Nevolin
 * LinkedIn: https://www.linkedin.com/in/andrey-nevolin-76387328
 *
 * Example code demonstrating use of Wall-clock Time Measurement library (wtmlib)
 */

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "src/wtmlib.h"

#define USECS_TO_LOOP_FOR 2547291ul

int calcDeltaInNsecs( const struct timespec *start_time,
                      const struct timespec *end_time,
                      uint64_t *delta)
{
    if ( start_time->tv_sec > end_time->tv_sec )
    {
        fprintf( stdout, "\tSystem error. Start system time has %lu seconds while end "
                 "system time has smaller amount of seconds (%lu)\n", start_time->tv_sec,
                 end_time->tv_sec);

        return -1;
    }

    uint64_t num_nsecs = 0;
    uint64_t num_secs = end_time->tv_sec - start_time->tv_sec;

    if ( !num_secs && start_time->tv_nsec > end_time->tv_nsec )
    {
        fprintf( stdout, "\tSystem error. Start and end system times have equal amounts "
                 "of seconds but the start time has more nanoseconds (%lu > %lu)\n",
                 start_time->tv_nsec, end_time->tv_nsec);

        return -1;
    }

    if ( start_time->tv_nsec > end_time->tv_nsec )
    {
        num_secs--;
        num_nsecs = 1000000000 - start_time->tv_nsec + end_time->tv_nsec;
    } else
    {
        num_nsecs = end_time->tv_nsec - start_time->tv_nsec;
    }

    if ( delta ) *delta = (num_secs * 1000000000) + num_nsecs;

    return 0;
}

int main()
{
    char err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    int64_t tsc_range_length = INT64_MAX;
    bool is_monotonic = false;
    wtmlib_TSCConversionParams_t conv_params = {.mult = 0, .shift = -1,
                                                .nsecs_per_tsc_modulus = 0,
                                                .tsc_remainder_length = 0,
                                                .tsc_remainder_bitmask = 0};
    uint64_t secs_before_wrap = 0;
    struct timespec start_time = {.tv_sec = 0, .tv_nsec = 0};
    struct timespec end_time = {.tv_sec = 0, .tv_nsec = 0};
    uint64_t elapsed_nsecs = 0;
    uint64_t start_tsc_val = 0, end_tsc_val = 0;
    int ret = 0;

    fprintf( stdout, "Evaluating TSC reliability (all needed data is collected using a "
				     "single thread \"jumping\" from one CPU to another)...\n");
    ret = wtmlib_EvalTSCReliabilityCPUSW( &tsc_range_length, &is_monotonic, err_msg,
                                          sizeof( err_msg));

    if ( ret )
    {
        fprintf( stdout, "\tEvaluation failed. ");

        switch ( ret )
        {
            case WTMLIB_RET_TSC_INCONSISTENCY:
                fprintf( stdout, "Major TSC inconsistency detected: %s\n\n", err_msg);

                break;
            case WTMLIB_RET_GENERIC_ERR:
                fprintf( stdout, "%s\n\n", err_msg);

                break;
            default:
                fprintf( stdout, "Unexpected error type. Error message: %s\n\n", err_msg);
        }
    } else
    {
        fprintf( stdout, "\tEstimated maximum shift between TSC counters running on "
                 "different CPUs: %ld\n", tsc_range_length);
        fprintf( stdout, "\tTSC values measured successively on same or different CPUs "
                 "%s monotonically increase\n\n", is_monotonic ? "DO" : "DO NOT");
    }

    fprintf( stdout, "Evaluating TSC reliability (all needed data is collected by "
             "concurrently running threads; one thread per each available CPU. "
             "Measurements taken by the threads are sequentially ordered using CAS)"
						 "...\n");
    ret = wtmlib_EvalTSCReliabilityCOP( &tsc_range_length, &is_monotonic, err_msg,
                                        sizeof( err_msg));

    if ( ret )
    {
        fprintf( stdout, "\tEvaluation failed. ");

        switch ( ret )
        {
            case WTMLIB_RET_TSC_INCONSISTENCY:
                fprintf( stdout, "Major TSC inconsistency detected: %s\n\n", err_msg);

                break;
            case WTMLIB_RET_POOR_STAT:
                fprintf( stdout, "Statistical significance criteria are not met: %s\n\n",
                         err_msg);

                break;
            case WTMLIB_RET_GENERIC_ERR:
                fprintf( stdout, "%s\n\n", err_msg);

                break;
            default:
                fprintf( stdout, "Unexpected error type. Error message: %s\n\n", err_msg);
        }
    } else
    {
        fprintf( stdout, "\tEstimated maximum shift between TSC counters running on "
                 "different CPUs: %ld\n", tsc_range_length);
        fprintf( stdout, "\tTSC values measured successively on same or different CPUs "
                 "%s monotonically increase\n\n", is_monotonic ? "DO" : "DO NOT");
    }

    fprintf( stdout, "Getting TSC-to-nanoseconds conversion parameters...\n");
    ret = wtmlib_GetTSCToNsecConversionParams( &conv_params, &secs_before_wrap, err_msg,
                                               sizeof( err_msg));

    if ( ret )
    {
        fprintf( stdout, "\tFailed. ");

        switch ( ret )
        {
            case WTMLIB_RET_TSC_INCONSISTENCY:
                fprintf( stdout, "Major TSC inconsistency detected: %s\n\n", err_msg);

                break;
            case WTMLIB_RET_GENERIC_ERR:
                fprintf( stdout, "%s\n\n", err_msg);

                break;
            default:
                fprintf( stdout, "Unexpected error type. Error message: %s\n\n", err_msg);
        }
    } else
    {
        fprintf( stdout, "\tNanoseconds per TSC modulus: %lu\n",
                 conv_params.nsecs_per_tsc_modulus);
        fprintf( stdout, "\tLength of TSC remainder in bits: %d\n",
                 conv_params.tsc_remainder_length);
        fprintf( stdout, "\tBitmask used to extract TSC remainder: %016lx\n",
                 conv_params.tsc_remainder_bitmask);
        fprintf( stdout, "\tMultiplicator: %lu\n", conv_params.mult);
        fprintf( stdout, "\tShift: %d\n", conv_params.shift);
        fprintf( stdout, "\tTSC ticks per second: %lu\n", conv_params.tsc_ticks_per_sec);
        fprintf( stdout, "\tSeconds before the earliest TSC wrap: %lu\n\n",
                 secs_before_wrap);
    }

    fprintf( stdout, "Now looping for approximately %lu microseconds and measuring "
             "the elapsed time using both system and WTMLIB means...\n",
             USECS_TO_LOOP_FOR);

    ret = clock_gettime( CLOCK_MONOTONIC_RAW, &start_time);
    start_tsc_val = WTMLIB_GET_TSC();

    if ( ret )
    {
        fprintf( stdout, "\tclock_gettime() failed");

        /* This is a test program. It must always succeed */
        return 0;
    }

    do
    {
        ret = clock_gettime( CLOCK_MONOTONIC_RAW, &end_time);
        end_tsc_val = WTMLIB_GET_TSC();

        if ( ret )
        {
            fprintf( stdout, "\tclock_gettime() failed");

            /* This is a test program. It must always succeed */
            return 0;
        }

        if ( calcDeltaInNsecs( &start_time, &end_time, &elapsed_nsecs) ) return 0;
    } while ( elapsed_nsecs < USECS_TO_LOOP_FOR * 1000 );

    if ( end_tsc_val < start_tsc_val )
    {
        fprintf( stdout, "\tError. End TSC value is smaller than start TSC value\n");

        /* This is a test program. It must always succeed */
        return 0;
    }

    fprintf( stdout, "\t%lu nanoseconds passed according to \"clock_gettime()\"\n",
             elapsed_nsecs);
    fprintf( stdout, "\t%lu nanoseconds passed according to WTMLIB\n",
             WTMLIB_TSC_TO_NSEC( end_tsc_val - start_tsc_val, &conv_params));

    return 0;
}
