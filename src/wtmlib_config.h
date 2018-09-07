/**
 * Copyright © 2018 Andrey Nevolin, https://github.com/AndreyNevolin
 * Twitter: @Andrey_Nevolin
 * LinkedIn: https://www.linkedin.com/in/andrey-nevolin-76387328
 *
 * WTMLIB: a library for taking Wall-clock Time Measurements
 *
 * This file contains configuration parameters for the library
 */

/*
   A number of "round trips" that the current thread should make across
   available CPUs when calculating enclosing TSC range
*/
#define WTMLIB_CALC_TSC_RANGE_ROUND_COUNT 100
/*
   A number of "round trips" that the current thread should make across
   available CPUs when evaluating TSC monotonicity
*/
#define WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT 100
/*
   Time (in seconds) that TSC probe threads are allowed to execute. If they don't
   finish during this time, they will be cancelled
*/
#define WTMLIB_TSC_PROBE_WAIT_TIME 300
/*
   Time period (in seconds) that must pass between successive checks for completion
   of TSC probe threads.
   The period should be significantly smaller than WTMLIB_TSC_PROBE_WAIT_TIME, but
   large enough for the waiting thread doesn't wake up too often (otherwise the
   waiting thread may steal CPU resources from the probe threads)
*/
#define WTMLIB_TSC_PROBE_COMPLETION_CHECK_PERIOD 1
/*
   Maximum time (in seconds) to wait for cancelled TSC probe threads to finish. If
   they don't finish during this time, they will be detached.
   This time must be bigger then (and not equal to!) WTMLIB_TSC_PROBE_CHECK_PERIOD
*/
#define WTMLIB_TSC_PROBE_WAIT_AFTER_CANCEL 10
/*
   A threshold used to verify statistical significance of calculated TSC delta range. Here
   "TSC delta" is a shift between two TSC counters running on different CPUs - some CPU of
   interest and some reference CPU. To calculate this "delta", TSC probes are collected
   concurrently on both CPUs. These probes are sequentially ordered. From statistical
   significance perspective, the best case is when the probes collected on one CPU
   alternate neatly with the probes collected on the other CPU. The worst case is when in
   the sequence of probes first come all the probes collected on one CPU and then come all
   the probes collected on the other CPU. In this case, "TSC delta" cannot be calculated
   at all.
   In general, given a sequence of TSC probes, it may be possible to produce several
   independent estimations of a range of values that "TSC delta" can take. These ranges
   (if any) are then intersected into single final estimation. The more independent
   estimations can be extracted from the sequence of TSC probes, the more accurate and
   statistically significant final result we can get. The value defined below is a number
   of independent "delta" range estimations that a TSC probe sequence must allow for the
   final (combined) estimation to be trusted
*/
#define WTMLIB_TSC_DELTA_RANGE_COUNT_THRESHOLD 10ul
/*
   Number of CAS-ordered TSC probes that must be collected on each CPU (allowed by a
   constraint) when calculating a range of values that a shift between TSC counters
   of two different CPUs may take
*/
#define WTMLIB_CALC_TSC_RANGE_PROBES_COUNT 1000
/*
   Number of CAS-ordered TSC probes that must be collected on each CPU (allowed by
   a constraint) when evaluating TSC monotonicity
*/
#define WTMLIB_EVAL_TSC_MONOTCTY_PROBES_COUNT 1000
/*
   A threshold used to assess reliability (statistical significance) of a result of TSC
   monotonicity evaluation

   Assessment of statistical significance makes sense in case of positive result only.
   In case of negative result it is known for sure that collected one after another
   (possibly on different CPUs) TSC values do can decrease. But if the result is positive,
   it cannot be said with 100% confidence that collected one after another (possibly on
   different CPUs) TSC values will never decrease. It can only be said that "with some
   level of certainty they will not decrease".
   This "level of certainty" depends on how many TSC probes were collected on each CPU and
   how well the probes collected on different CPUs are mixed in the sequence.
   The value defined below deals with the degree of mixing. A criterion for good mixing
   used in this library is the following:
     - it must be possible to find two probes in the sequence that were collected on the
       same CPU (let's call it CPU0). This requirement is always met
     - between these two probes there must be at least one probe collected on each (!)
       other CPU. Probes collected on CPU0 may also be there, but that's not a requirement
   So, for a result of monotonicity evaluation to be trusted, in a complete sequence of
   TSC probes there must exist a sub-sequence of successive (!) probes that satisfies the
   two conditions above. If several non-overlapping sub-sequences of that kind can be
   found, then positive result of monotinicity evaluation is even more trusted. The
   threshold defined below specifies exactly the number of such sub-sequences. A result of
   monotonicity evaluation is considered statistically significant if the specified amount
   of sub-sequences with the indicated properties can be discovered in the complete
   sequence of TSC probes
*/
#define WTMLIB_FULL_LOOP_COUNT_THRESHOLD 10ul
/*
   The number of measurements to do when calculating how many times TSC counter ticks
   during a second-long time period

   Relating TSC changes to system time changes is tricky because TSC and system time
   cannot be measured simultaneously (at least on the same CPU). Some factors that can
   affect the time gap between the measurements are: system call overhead, interrupts,
   and context switches. WTMLIB accounts for these negative effects by doing multiple
   measurements and applying some basic statistics to the measured values
*/
#define WTMLIB_TSC_PER_SEC_SAMPLE_COUNT 30
/*
   System time period (in microseconds) that will be matched with a change of TSC. This
   value is used when calculating how many TSC ticks pass during a second-long time period

   When calculating TSC worth of a second, TSC change is matched not obligatory with a
	 second but with the specified time period. Then tsc-per-this-time-period is converted
	 into tsc-per-second
*/
#define WTMLIB_TIME_PERIOD_TO_MATCH_WITH_TSC 500000
/*
   A time period (measured in seconds) used to calculate TSC-to-nanoseconds conversion
   parameters

   The "modulus" defined below is used to divide TSC ticks to be converted into two parts:
       - a part which (roughly) corresponds to a multiple of "modulus" seconds
       - and the rest
   Then the parts are converted to nanoseconds using different (but simple) procedures.
   The results are summed up to produce the final value.

   The accuracy of the conversion depends on the value of the "modulus"
*/
#define WTMLIB_TIME_CONVERSION_MODULUS 10
