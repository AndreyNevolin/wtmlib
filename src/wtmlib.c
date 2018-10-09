/*
 * Copyright © 2018 Andrey Nevolin, https://github.com/AndreyNevolin
 * Twitter: @Andrey_Nevolin
 * LinkedIn: https://www.linkedin.com/in/andrey-nevolin-76387328
 *
 * WTMLIB: a library for taking Wall-clock Time Measurements
 *
 * Main source file of the library
 */

/* Standard lib headers */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <math.h>

/* System headers */
#include <sys/sysinfo.h>
#include <unistd.h>

#ifdef WTMLIB_DEBUG
#include <execinfo.h>
#endif

#include "wtmlib.h"
#include "wtmlib_config.h"

#ifdef WTMLIB_DEBUG
/**
 * Print stack dump
 */
static inline void wtmlib_PrintStack()
{
    void *frames[100];
    size_t stack_depth;
    char **calls;

    fprintf( stderr, "Stack trace: \n");
    stack_depth = backtrace( frames, 100);
    calls = backtrace_symbols( frames, stack_depth);

    for ( size_t i = 0; i < stack_depth; i++ )
    {
        printf ("\t[%2lu] %s\n", stack_depth - i - 1, calls[i]);
    }

    free( calls);
}

#define WTMLIB_ABORT                                                                \
    fprintf( stderr, "\n"),                                                         \
    fprintf( stderr, "Internal error: file \"%s\", line %u\n", __FILE__, __LINE__), \
    fprintf( stderr, "\n"),                                                         \
    fflush( NULL),                                                                  \
    wtmlib_PrintStack(),                                                            \
    abort()
#endif /* WTMLIB_DEBUG */

#ifdef WTMLIB_DEBUG
#    define WTMLIB_ASSERT( condition_) ((condition_) || (WTMLIB_ABORT, 0))
#else
#    define WTMLIB_ASSERT( condition_)
#endif /* WTMLIB_DEBUG */

#ifdef WTMLIB_LOG
#    define WTMLIB_OUT( format_, ...)             \
         fprintf( stdout, format_, ##__VA_ARGS__)
#else
#    define WTMLIB_OUT( format_, ...)
#endif /* WTMLIB_DEBUG */

/**
 * Print formatted error message to the specified buffer
 * Just a shortcut to make error-processing code more compact
 */
#define WTMLIB_BUFF_MSG( buff_, buff_size_, format_, ...)     \
    if ( buff_ )                                              \
    {                                                         \
        snprintf( buff_, buff_size_, format_, ##__VA_ARGS__); \
    }

/**
 * Wrapper around "strerror_r()" aimed at making calls to
 * "strerror_r()" more compact and manageable
 */
static inline char *WTMLIB_STRERROR_R( char *buff, size_t size)
{
    WTMLIB_ASSERT( buff);

    return strerror_r( errno, buff, size);
}

/**
 * Helper macro to compute absolute value of difference between two integer values
 */
#define ABS_DIFF( a_, b_) \
    ((a_) > (b_) ? (a_) - (b_) : (b_) - (a_))

/**
 * Structure to keep values of selected parameters that describe
 *  - hardware state
 *  - operating system state
 *  - process state
 */
typedef struct
{
    /* Number of configured logical CPUs in the system */
    int num_cpus;
    /* Initial CPU (a CPU that the current thread is executing on when WTM library is
       called) */
    int initial_cpu;
    /* Initial CPU set (a CPU set that the current thread is confined to when WTM
       library is called) */
    cpu_set_t *initial_cpu_set;
    /* Cache line size */
    int cline_size;
} wtmlib_ProcAndSysState_t;

/**
 * Initialize wtmlib_ProcAndSysState_t structure
 */
static void wtmlib_InitProcAndSysState( wtmlib_ProcAndSysState_t *state)
{
    if ( !state ) return;

    state->num_cpus = -1;
    state->initial_cpu = -1;
    state->initial_cpu_set = 0;
    state->cline_size = -1;

    return;
}

/**
 * Deallocate memory used to keep parts of the process/OS/hardware state
 */
static void wtmlib_DeallocProcAndSysState( wtmlib_ProcAndSysState_t *state)
{
    if ( !state ) return;

    if ( state->initial_cpu_set ) CPU_FREE( state->initial_cpu_set);

    return;
}

/**
 * Get cache line size
 *
 * The implementation of this function is platform-specific. But it's expected to
 * work seamlessly on Linux
 *
 * It's expected that WTMLIB will be executed on a system with homogenous CPUs (or
 * on a system with a single CPU). Currently there is no mechanism in WTMLIB to
 * resolve false memory sharing issues on systems with heterogenous CPUs
 *
 * The function returns either the cache line size or some negative value (in case
 * of error)
 */
static int wtmlib_GetCacheLineSize( char *err_msg,
                                    int err_msg_size)
{
    /* Get cache line size using "sysconf" */
    long cline_size = sysconf( _SC_LEVEL1_DCACHE_LINESIZE);

    if ( cline_size == -1 )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "\"sysconf()\" returned an error");

        return -1;
    }

    return cline_size;
}

/**
 * Allocate memory for data structures required by TSC sampling routines
 * "wtmlib_CollectTSCInCPUCarousel()" and "wtmlib_TSCProbeThread()"
 *
 * The function returns:
 *     1) an array of pointers to CPU set structures
 *     2) an array of pointers to arrays of TSC samples
 *
 * The function doesn't change "cpu_sets_ret" and "tsc_vals_ret" pointers if fails.
 * If the function succeeds, then allocated memory should be deallocated after use
 * by means of calling "wtmlib_DeallocMemForTSCSampling()"
 */
static int wtmlib_AllocMemForTSCSampling( int cline_size,
                                          int num_cpus,
                                          int num_cpu_sets,
                                          int num_samples,
                                          int sample_size,
                                          cpu_set_t ***cpu_sets_ret,
                                          void ***tsc_samples_ret,
                                          char *err_msg,
                                          int err_msg_size)
{
    WTMLIB_ASSERT( cpu_sets_ret && tsc_samples_ret);

    int ret = 0;
    cpu_set_t **cpu_sets = 0;
    void **tsc_samples = 0;
    int num_clines_tsc_samples = 0;

    /* Allocate an array to keep pointers to CPU set structures.
       We don't align the array or individual CPU sets to the cache line size. Still,
       they will stay protected from false sharing, because:
           1) they are not modified inside performance-critical functions
              (namely "wtmlib_CollectTSCInCPUCarousel()" and "wtmlib_TSCProbeThread()")
           2) when performance-critical functions execute, there is no concurrently
              executing functions that modify the array or individual CPU sets
           3) we ensure that all data structures that ARE modified inside performance-
              critical functions are aligned to the cache line size (and thus don't
              share cache lines with the array and individual CPU sets)
       In other words, the array and CPU set structures are "read-only" inside
       "wtmlib_CollectTSCInCPUCarousel()" and "wtmlib_TSCProbeThread()"; also they don't
       share cache lines with modifiable data. Thus, even if different CPU set structures
       share the same cache line, there will be no cache line "ping pong" between CPUs.
       Each CPU will just have its own "read-only" cache line copy. The same is true in
       case when CPU set structures share cache lines with other "read-only" data */
    cpu_sets = (cpu_set_t**)calloc( sizeof( cpu_set_t*), num_cpu_sets);

    if ( !cpu_sets )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory to keep "
                         "an array of pointers to CPU sets");
        ret = WTMLIB_RET_GENERIC_ERR;

        goto alloc_mem_for_tsc_sampling_out;
    }

    /* Allocate memory for CPU sets. Store pointers to them in the array */
    for ( int i = 0; i < num_cpu_sets; i++ )
    {
        cpu_sets[i] = CPU_ALLOC( num_cpus);

        if ( !cpu_sets[i] )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for a "
                             "CPU set");
            ret = WTMLIB_RET_GENERIC_ERR;

            goto alloc_mem_for_tsc_sampling_out;
        }
    }

    /* Allocate an array to keep pointers to arrays of TSC samples. This array is
       "read-only" inside "wtmlib_CollectTSCInCPUCarousel()" function. And it's not
       accessed at all inside "wtmlib_TSCProbeThread()" function. Hence, we don't
       align it to the cache-line size */
    tsc_samples = (void**)calloc( sizeof( void*), num_cpu_sets);

    if ( !tsc_samples )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory to store "
                         "pointers to arrays of TSC samples");
        ret = WTMLIB_RET_GENERIC_ERR;

        goto alloc_mem_for_tsc_sampling_out;
    }

    /* Number of cache lines required to store a single array of TSC samples */
    num_clines_tsc_samples = (sample_size * num_samples) / cline_size;

    if ( (sample_size * num_samples) % cline_size ) num_clines_tsc_samples++;

    /* Allocate memory for arrays of TSC samples. Store pointers to these arrays in
       the array allocated above. We DO align these arrays to the cache line size,
       since they ARE modified by "wtmlib_CollectTSCInCPUCarousel()" and
       "wtmlib_TSCProbeThread()" functions */
    for ( int i = 0; i < num_cpu_sets; i++ )
    {
        tsc_samples[i] = (void*)aligned_alloc( cline_size,
                                               num_clines_tsc_samples * cline_size);

        if ( !tsc_samples[i] )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for an "
                             "array of TSC samples");
            ret = WTMLIB_RET_GENERIC_ERR;

            goto alloc_mem_for_tsc_sampling_out;
        }
    }

    WTMLIB_ASSERT( !ret);

alloc_mem_for_tsc_sampling_out:
    if ( !ret )
    {
        *cpu_sets_ret = cpu_sets;
        *tsc_samples_ret = tsc_samples;

        return 0;
    }

    if ( cpu_sets )
    {
        for ( int i = 0; i < num_cpu_sets; i++ )
        {
            /* Array "cpu_sets" was initialized by zeros during allocation. So, this
               check is safe (i.e. it cannot be false-positive) */
            if ( cpu_sets[i] ) CPU_FREE( cpu_sets[i]);
        }

        free( cpu_sets);
    }

    if ( tsc_samples )
    {
        for ( int i = 0; i < num_cpu_sets; i++ )
        {
            /* Array "tsc_samples" was initialized by zeros during allocation. So, this
               check is safe (i.e. it cannot be false-positive) */
            if ( tsc_samples[i] ) free( tsc_samples[i]);
        }

        free( tsc_samples);
    }

    return ret; 
}

/**
 * Deallocate memory allocated by "wtmlib_AllocMemForTSCSampling()" function
 */
static void wtmlib_DeallocMemForTSCSampling( int num_cpu_sets,
                                             cpu_set_t **cpu_sets,
                                             void **tsc_samples)
{
    if ( cpu_sets )
    {
        for ( int i = 0; i < num_cpu_sets; i++ )
        {
            if ( cpu_sets[i] ) CPU_FREE( cpu_sets[i]);
        }

        free( cpu_sets);
    }

    if ( tsc_samples )
    {
        for ( int i = 0; i < num_cpu_sets; i++ )
        {
            if ( tsc_samples[i] ) free( tsc_samples[i]);
        }

        free( tsc_samples);
    }
}

/**
 * Allocate memory needed to run CPU carousel
 */
static int wtmlib_AllocMemForCPUCarousel( int cline_size,
                                          int num_cpus,
                                          int num_cpu_sets,
                                          int num_samples,
                                          cpu_set_t ***cpu_sets_ret,
                                          uint64_t ***tsc_vals_ret,
                                          char *err_msg,
                                          int err_msg_size)
{
    return wtmlib_AllocMemForTSCSampling( cline_size, num_cpus, num_cpu_sets,
                                          num_samples, sizeof( uint64_t),
                                          cpu_sets_ret, (void***)tsc_vals_ret,
                                          err_msg, err_msg_size);
}

/**
 * Deallocate memory allocated previously by "wtmlib_AllocMemForCPUCarousel()" routine
 */
static void wtmlib_DeallocMemForCPUCarousel( int num_cpu_sets,
                                             cpu_set_t **cpu_sets,
                                             uint64_t **tsc_vals)
{
    wtmlib_DeallocMemForTSCSampling( num_cpu_sets, cpu_sets, (void**)tsc_vals);

    return;
}

/**
 * Collect TSC values on all available CPUs in a carousel manner
 *
 * The algorithm is the following:
 *      - the current thread is first moved to the first CPU, and TSC is measured on that
 *        CPU
 *      - then the same thread is moved to the second CPU, and TSC is measured there
 *      - so on, until all available CPUs are visited (and thus the first round of the
 *        carousel is completed)
 *      - further rounds (if num_rounds > 1) are then undertaken in exactly the same way
 *      - after the last round completes, the thread is moved to the first CPU again, and
 *        TSC is measured there again. Thus, first and last measurements taken by the
 *        function are taken on the same CPU
 *
 * CPU affinity of the current thread may change as a result of calling this function
 */
static int wtmlib_CollectTSCInCPUCarousel( cpu_set_t **cpu_sets,
                                           int64_t num_cpu_sets,
                                           uint64_t **tsc_vals,
                                           int num_cpus,
                                           int64_t num_rounds,
                                           char *err_msg,
                                           int err_msg_size)
{
#ifdef WTMLIB_DEBUG
    WTMLIB_ASSERT( cpu_sets);

    for ( int i = 0; i < num_cpu_sets; i++ )
    {
        WTMLIB_ASSERT( cpu_sets[i]);
    }
#endif

    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    const char *change_cpu_err = "Coulnd't change CPU affinity of the current thread";
    pthread_t thread_self = pthread_self();

     /* Move current thread across all CPUs described by CPU masks referenced from
        "cpu_sets" array (each element of the array references a CPU mask that represents
        a single CPU). The thread is moved from one CPU to another in a carousel fashion.
        And the carousel is run the specified number of times */
    for ( int i = 0; i < num_rounds; i++ )
    {
        for ( int ind = 0; ind < num_cpu_sets; ind++ )
        {
            int ret = pthread_setaffinity_np( thread_self, CPU_ALLOC_SIZE( num_cpus),
                                              cpu_sets[ind]);

            if ( ret )
            {
                WTMLIB_BUFF_MSG( err_msg, err_msg_size, "%s: %s", change_cpu_err,
                                 WTMLIB_STRERROR_R( local_err_msg,
                                 sizeof( local_err_msg)));

                return WTMLIB_RET_GENERIC_ERR;
            }

            tsc_vals[ind][i] = WTMLIB_GET_TSC();
        }
    }

    /* Move the thread to the first CPU in the sequence, so that the first and last
       TSC values are measured on the same CPU */
    if ( pthread_setaffinity_np( thread_self, sizeof( cpu_set_t), cpu_sets[0]) )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "%s: %s", change_cpu_err,
                         WTMLIB_STRERROR_R( local_err_msg, sizeof( local_err_msg)));

        return WTMLIB_RET_GENERIC_ERR;
    }

    tsc_vals[0][num_rounds] = WTMLIB_GET_TSC();

    return 0;
}

/**
 * Generic evaluation of consistency of TSC values collected in a CPU carousel
 */
static int wtmlib_CheckCarouselValsConsistency( uint64_t **tsc_vals,
                                                int num_avail_cpus,
                                                uint64_t num_rounds,
                                                char *err_msg,
                                                int err_msg_size)
{
    WTMLIB_ASSERT( tsc_vals);

    /* Make sure that collected TSC values do vary on each of the CPUs. That may not be
       true, for example, in case when some CPUs consistently return "zero" for every TSC
       test.
       This check is really important. If all TSC values are equal, then both TSC "delta"
       ranges and TSC monotonicity will be perfect, but at the same time TSC would be
       absolutely inappropriate for measuring wall-clock time. (Global TSC monotonicity
       evaluation and some other monotonicity checks existing in the library will give the
       positive result because they don't require successively measured TSC values to
       strictly grow. Overall, WTMLIB's requirements with respect to TSC monotonicity are
       the following: TSC values must grow on a global scale and not decrease locally.
       I.e. the library allows some successively measured TSC values to be equal to each
       other) */
    const char *equal_vals_err_msg = "First and last TSC values collected on a CPU with "
                                     "index %d are equal";

    /* Separate check for a CPU with index "zero". Because there must be an extra value
       measured on this CPU */
    WTMLIB_ASSERT( tsc_vals[0]);

    if ( tsc_vals[0][0] == tsc_vals[0][num_rounds] )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, equal_vals_err_msg, 0);

        return WTMLIB_RET_TSC_INCONSISTENCY;
    }

    /* Check TSC values collected on all other CPUs */
    for ( int i = 1; i < num_avail_cpus; i++ )
    {
        WTMLIB_ASSERT( tsc_vals[i]);

        if ( tsc_vals[i][0] == tsc_vals[i][num_rounds - 1] )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, equal_vals_err_msg, i);

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }
    }

    return 0;
}

/*
 * Calculate bounds of a shift between TSC on the given CPU and TSC on the base CPU
 * (assuming that TSC values were collected using CPU carousel method)
 *
 * It's done in the following way:
 *   1) for each TSC value T measured on the given CPU there are TSC values t1 and t2
 *      mesured right before and right after T, but on the base CPU
 *   2) so, we know that when T was measured, TSC on the base CPU was somewhere between
 *      t1 and t2. Let's denote that value of base TSC "t"
 *   3) we're interested in a difference "delta = T - t"
 *   4) since "t belongs to [t1, t2]", "delta belongs to [T - t2; T - t1]"
 *   5) that's how we find a range for "delta" based on a single round of CPU carousel
 *   6) but we (possibly) had multiple rounds of CPU carousel. Based on that, we can
 *      narrow the range of possible "delta" values. To do that, we calculate the
 *      intersection of all "delta" ranges calculated in different carousel rounds
 *
 * Input: tsc_vals[0] - array of base TSC values collected during CPU carousel
 *        tsc_vals[1] - array of TSC values collected on some other CPU during
 *                      the same carousel
 */
static int wtmlib_CalcTSCDeltaRangeCPUSW( uint64_t **tsc_vals,
                                          int64_t num_rounds,
                                          int64_t *delta_min,
                                          int64_t *delta_max,
                                          char *err_msg,
                                          int err_msg_size)
{
    WTMLIB_ASSERT( tsc_vals && tsc_vals[0] && tsc_vals[1]);

    int64_t d_min = INT64_MIN, d_max = INT64_MAX;

    WTMLIB_OUT( "\t\tCalculating shift between TSC counters of the two given CPUs...\n");

    if ( wtmlib_CheckCarouselValsConsistency( tsc_vals, 2, num_rounds,
                                              err_msg, err_msg_size) )
    {
        return WTMLIB_RET_TSC_INCONSISTENCY;
    }

    for ( int i = 0; i < num_rounds; i++ )
    {
        /* Consistency check. Successive TSC values measured on the same CPU must
           not decrease (unless TSC counter wraps) */
        if ( tsc_vals[0][i + 1] < tsc_vals[0][i]
             || (i > 0 && tsc_vals[1][i] < tsc_vals[1][i - 1]) )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Detected decreasing successive TSC "
                             "values (measured on the same CPU). That may be a result of"
                             " TSC wrap");

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }

        /* Check that we will not get overflow while subtracting TSC values. If we do get
           overflow, that means that we cannot rely on TSC while measuring wall-clock
           time. Big difference between TSC values measured on different CPUs may be a
           result of TSC wrap. This possibility is not ruled out by the monotonicity check
           done right above this comment. That check ensures that TSC wrap didn't occur
           DURING the CPU carousel. But the wrap could occur on one of the CPUs right
           BEFORE the carousel was started. For example, one CPU might be close to the
           point of TSC wrap. But the wrap hasn't happened yet (and hasn't happened during
           the CPU carousel). And the other CPU might has passed this point just before
           the carousel. So, during the carousel both CPUs are close to the point of TSC
           wrap, but from different sides. TSC values may grow monotonically on each of
           the CPUs while the carousel runs but cross-CPU difference will be big (and it
           will stay big until the first CPU also experiences TSC wrap) */
        uint64_t diff1 = ABS_DIFF( tsc_vals[1][i], tsc_vals[0][i]);
        uint64_t diff2 = ABS_DIFF( tsc_vals[1][i], tsc_vals[0][i + 1]);

        if ( diff1 > (uint64_t)INT64_MAX || diff2 > (uint64_t)INT64_MAX )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Difference between TSC values "
                             "measured on different CPUs is too big (bigger than %ld). "
                             "May be a result of TSC wrap", INT64_MAX);

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }

        int64_t bound_min = tsc_vals[1][i] - tsc_vals[0][i + 1];
        int64_t bound_max = tsc_vals[1][i] - tsc_vals[0][i];

        WTMLIB_ASSERT( bound_min <= bound_max);

        /* "delta" ranges calculated for different carousel rounds must overlap.
           Otherwise, we have a major TSC inconsistency and cannot rely on TSC while
           measuring wall-clock time */
        if ( bound_min > d_max || bound_max < d_min )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "TSC delta ranges calculated for "
                             "different carousel rounds don't overlap. May be a result "
                             "of some major inconsistency");

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }

        WTMLIB_OUT( "\t\t\tThe shift belongs to range: %ld [%ld, %ld]\n",
                    bound_max - bound_min, bound_min, bound_max);

        d_min = bound_min > d_min ? bound_min : d_min;

        d_max = bound_max < d_max ? bound_max : d_max;
    }

    WTMLIB_ASSERT( d_min <= d_max);
    WTMLIB_OUT( "\t\t\tCombined range (intersection of all the above): %ld [%ld, %ld]\n",
                d_max - d_min, d_min, d_max);

    if ( delta_min ) *delta_min = d_min;

    if ( delta_max ) *delta_max = d_max;

    return 0;
}

#ifdef WTMLIB_LOG
/**
 * Print TSC values collected in a CPU carousel
 */
static void wtmlib_PrintCarouselSamples( int num_cpus,
                                         uint64_t **tsc_vals,
                                         uint64_t num_rounds,
                                         const char* const indent)
{
    WTMLIB_OUT( "%sTSC samples collected in the CPU carousel\n", indent);
    WTMLIB_OUT( "%s(CPU index may not be equal to CPU ID)\n", indent);
    WTMLIB_OUT( "%s(CPU index <-> CPU ID mapping should be printed above)\n", indent);

    for ( uint64_t round = 0; round < num_rounds; round++ )
    {
        for ( int cpu_ind = 0; cpu_ind < num_cpus; cpu_ind++ )
        {
            WTMLIB_OUT( "%s\tRound %lu, CPU index %d: %lu\n", indent, round, cpu_ind,
                        tsc_vals[cpu_ind][round]);
        }
    }

    WTMLIB_OUT( "%s\tRound %lu, CPU index %d: %lu\n", indent, num_rounds, 0,
                tsc_vals[0][num_rounds]);

    return;
}
#endif

/**
 * Calculate size of enclosing TSC range (using data collected by means of "CPU
 * Switching" method)
 *
 * "Size of enclosing TSC range" is a non-negative integer value such that if TSC
 * values are measured simultaneously on all the available CPUs, then difference
 * between the largest and the smallest will be not bigger than this value. In other
 * words, it's an estimated upper bound for shifts between TSC counters running on
 * different CPUs
 *
 * To calculate "enclosing TSC range" we do the following:
 *   1) for each CPU we calculate bounds that enclose a shift between this CPU's TSC
 *      and TSC of some base CPU
 *   2) we calculate the smallest range that encloses all ranges caclulated during
 *      the previous step
 *
 * When "enclosing TSC range" is found, its size is calculated as a difference
 * between its upper and lower bounds
 */
static int wtmlib_CalcTSCEnclosingRangeCPUSW( int num_cpus,
                                              int base_cpu,
                                              const cpu_set_t* const cpu_constraint,
                                              int cline_size,
                                              int64_t *range_size,
                                              char *err_msg,
                                              int err_msg_size)
{
    WTMLIB_ASSERT( cpu_constraint);

    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    cpu_set_t **cpu_sets = 0;
    uint64_t **tsc_vals = 0;
    int cpu_set_size = CPU_ALLOC_SIZE( num_cpus);
    int64_t delta_min = INT64_MAX, delta_max = INT64_MIN;
    int64_t l_bound = INT64_MAX, u_bound = INT64_MIN;
    int ret = 0;

    WTMLIB_OUT( "\tCalculating an upper bound for shifts between TSC counters running "
                "on different CPUs...\n");
    WTMLIB_OUT( "\t\tBase CPU ID: %d\n", base_cpu);
    /* We add 1 to "WTMLIB_CALC_TSC_RANGE_ROUND_COUNT", because
       "wtmlib_AllocMemForCPUCarousel()" function produces "num_rounds + 1" samples for
       the first CPU. The last sample is always taken on the first CPU in the carousel */
    ret = wtmlib_AllocMemForCPUCarousel( cline_size, num_cpus, 2,
                                         WTMLIB_CALC_TSC_RANGE_ROUND_COUNT + 1,
                                         &cpu_sets, &tsc_vals, local_err_msg,
                                         sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for CPU "
                         "carousel: %s", local_err_msg);
        ret = WTMLIB_RET_GENERIC_ERR;

        goto calc_tsc_enclosing_range_cpusw_out;
    }

    CPU_ZERO_S( cpu_set_size, cpu_sets[0]);
    CPU_SET_S( base_cpu, cpu_set_size, cpu_sets[0]);
    CPU_ZERO_S( cpu_set_size, cpu_sets[1]);

    for ( int cpu_id = 0; cpu_id < num_cpus; cpu_id++ )
    {
        if ( !CPU_ISSET_S( cpu_id, cpu_set_size, cpu_constraint) || cpu_id == base_cpu )
        {
            continue;
        }

        CPU_SET_S( cpu_id, cpu_set_size, cpu_sets[1]);
        WTMLIB_OUT( "\n\t\tRunning carousel for CPUs %d and %d...\n", base_cpu, cpu_id);
        ret = wtmlib_CollectTSCInCPUCarousel( cpu_sets, 2, tsc_vals, num_cpus,
                                              WTMLIB_CALC_TSC_RANGE_ROUND_COUNT,
                                              local_err_msg, sizeof( local_err_msg));

        if ( ret )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "CPU carousel failed: %s",
                             local_err_msg);

            goto calc_tsc_enclosing_range_cpusw_out;
        }

#ifdef WTMLIB_LOG
        WTMLIB_OUT( "\t\tCPU ID %d maps to CPU index %d\n", base_cpu, 0);
        WTMLIB_OUT( "\t\tCPU ID %d maps to CPU index %d\n", cpu_id, 1);
        wtmlib_PrintCarouselSamples( 2, tsc_vals, WTMLIB_CALC_TSC_RANGE_ROUND_COUNT,
                                     "\t\t");
#endif
        ret = wtmlib_CalcTSCDeltaRangeCPUSW( tsc_vals, WTMLIB_CALC_TSC_RANGE_ROUND_COUNT,
                                             &delta_min, &delta_max, local_err_msg,
                                             sizeof( local_err_msg));

        if ( ret )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Calculation of TSC delta range "
                             "failed: %s", local_err_msg);

            goto calc_tsc_enclosing_range_cpusw_out;
        }

        /* Update bounds of the enclosing TSC range */
        l_bound = l_bound > delta_min ? delta_min : l_bound;

        u_bound = u_bound < delta_max ? delta_max : u_bound;

        WTMLIB_ASSERT( delta_max >= delta_min && u_bound >= l_bound);
        /* Return CPU mask to the "clean" state */
        CPU_CLR_S( cpu_id, cpu_set_size, cpu_sets[1]);
    }

    WTMLIB_OUT( "\n\t\tShift between TSC on any of the available CPUs and TSC on the "
                "base CPU belongs to range: [%ld, %ld]\n", l_bound, u_bound);
    WTMLIB_OUT( "\t\tUpper bound for shifts between TSCs is: %ld\n", u_bound - l_bound);

    if ( range_size ) *range_size = u_bound - l_bound;

calc_tsc_enclosing_range_cpusw_out:
    wtmlib_DeallocMemForCPUCarousel( 2, cpu_sets, tsc_vals);

    return ret;
}

/**
 * Get values of selected parameters that describe:
 *   - hardware state
 *   - OS state
 *   - current process state
 *
 * Memory allocated for the "initial CPU set" should be deallocated after
 * use by calling CPU_FREE()
 */
static int wtmlib_GetProcAndSystemState( wtmlib_ProcAndSysState_t *state,
                                         char *err_msg,
                                         int err_msg_size)
{
    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    int cline_size = -1;
    int ret = 0;
    /* Get the number of configured logical CPUs in the system (not all of them
       may be availabe at the moment; e.g. some of them may be offline) */
    int num_cpus = get_nprocs_conf();
    /* Get ID of the current CPU */
    int initial_cpu = sched_getcpu();
    cpu_set_t *initial_cpu_set = 0;
    pthread_t thread_self = pthread_self();

    if ( initial_cpu < 0 )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't get ID of the current CPU: %s",
                         WTMLIB_STRERROR_R( local_err_msg, sizeof( local_err_msg)));
        ret = WTMLIB_RET_GENERIC_ERR;

        goto get_proc_and_system_state_out;
    }

    /* Get thread's affinity mask. We're going to test TSC values only on the CPUs allowed
       by the current thread's affinity mask */
    initial_cpu_set = CPU_ALLOC( num_cpus);

    if ( !initial_cpu_set )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for a "
                         "CPU set");
        ret = WTMLIB_RET_GENERIC_ERR;

        goto get_proc_and_system_state_out;
    }

    ret = pthread_getaffinity_np( thread_self, CPU_ALLOC_SIZE( num_cpus),
                                  initial_cpu_set);

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't get CPU affinity of the "
                         "current thread: %s",
                         WTMLIB_STRERROR_R( local_err_msg, sizeof( local_err_msg)));
        ret = WTMLIB_RET_GENERIC_ERR;

        goto get_proc_and_system_state_out;
    }

    cline_size = wtmlib_GetCacheLineSize( local_err_msg, sizeof( local_err_msg));

    if ( cline_size < 0 )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while obtaining cache line "
                         "size: %s", local_err_msg);
        ret = WTMLIB_RET_GENERIC_ERR;

        goto get_proc_and_system_state_out;
    }

get_proc_and_system_state_out:
    if ( ret )
    {
        if ( initial_cpu_set ) CPU_FREE( initial_cpu_set);
    } else if ( state )
    {
        state->num_cpus = num_cpus;
        state->initial_cpu = initial_cpu;
        state->initial_cpu_set = initial_cpu_set;
        state->cline_size = cline_size;
    }

    return ret;
}

/**
 * Restore initial state of the current process
 *
 * Some WTM library functions substantially affect the state of the current process. Such
 * functions should save the process' state at the very beginning (using a call to
 * wtmlib_GetProcAndSystemState() function) and recover the initial state at the very end
 * (using this function)
 */
static int wtmlib_RestoreInitialProcState( wtmlib_ProcAndSysState_t *state,
                                           char *err_msg,
                                           int err_msg_size)
{
    WTMLIB_ASSERT( state);

    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    /* Restore CPU affinity of the current thread. We do that in two steps:
         1) first we move the current thread to the initial CPU
         2) then we confine the thread to the specified set of CPUs
       The second step alone is not enough to recover the initial thread state. While the
       specified CPU set indeed must include the initial CPU, it may also include some
       other CPUs. Hence, if we do the second step only and omit the first one, the thread
       may end up on a CPU that is different from the initial one. But we do want to
       return it to the initial CPU, because before calling WTMLIB the application could
       store some data in the cache of that CPU. Other side effects may also exist.
       Technically, it's not guaranteed that if we do the first step, then after the
       second step the thread will remain on the initial CPU. But we believe the
       probability of that is pretty high */
    pthread_t thread_self = pthread_self();
    int cpu_set_size = CPU_ALLOC_SIZE( state->num_cpus);
    cpu_set_t *cpu_set = CPU_ALLOC( state->num_cpus);

    if ( !cpu_set )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for a "
                         "CPU set");

        return WTMLIB_RET_GENERIC_ERR;
    }

    CPU_ZERO_S( cpu_set_size, cpu_set);
    CPU_SET_S( state->initial_cpu, cpu_set_size, cpu_set);

    if ( pthread_setaffinity_np( thread_self, cpu_set_size, cpu_set) )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't return the current thread to "
                         "the initial CPU: %s",
                         WTMLIB_STRERROR_R( local_err_msg, sizeof( local_err_msg)));
        CPU_FREE( cpu_set);

        return WTMLIB_RET_GENERIC_ERR;
    }

    if ( pthread_setaffinity_np( thread_self, cpu_set_size, state->initial_cpu_set) )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't restore CPU affinity of the "
                         "current thread: %s",
                         WTMLIB_STRERROR_R( local_err_msg, sizeof( local_err_msg)));
        CPU_FREE( cpu_set);

        return WTMLIB_RET_GENERIC_ERR;
    }

    CPU_FREE( cpu_set);

    return 0;
}

/**
 * Check whether TSC values measured on different CPUs one after another monotonically
 * increase
 *
 * The algorithm is the following:
 *   1) move the current thread across all available CPUs in a carousel manner and measure
 *      TSC value after each migration
 *   2) check whether collected TSC values monotonically increase
 *
 * NOTE: if the function reports that collected TSC values do not monotonically increase,
 *       that doesn't necessarily imply that TSCs are unreliable. In some cases the
 *       observed decrease may be a result of TSC wrap
 */
static int wtmlib_EvalTSCMonotonicityCPUSW( int num_cpus,
                                            const cpu_set_t* const cpu_constraint,
                                            int cline_size,
                                            bool *is_monotonic_ret,
                                            char *err_msg,
                                            size_t err_msg_size)
{
    WTMLIB_ASSERT( cpu_constraint);

    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    cpu_set_t **cpu_sets = 0;
    uint64_t **tsc_vals = 0;
    int cpu_set_size = CPU_ALLOC_SIZE( num_cpus);
    /* Number of CPUs available to the current thread */
    int num_cpus_avail = 0;
    bool is_monotonic = true;
    uint64_t prev_tsc_val = 0;
    int round = 0, tsc_series = 0;
    int ret = 0;

    WTMLIB_OUT( "\tEvaluating TSC monotonicity...\n");

    /* Calculate the number of CPUs available to the current thread */
    for ( int cpu_id = 0; cpu_id  < num_cpus; cpu_id++ )
    {
        if ( CPU_ISSET_S( cpu_id, cpu_set_size, cpu_constraint) ) num_cpus_avail++;
    }

    /* We add 1 to "WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT", because
       "wtmlib_AllocMemForCPUCarousel()" function produces "num_rounds + 1" samples for
       the first CPU. The last sample is always taken on the first CPU in the carousel */
    ret = wtmlib_AllocMemForCPUCarousel( cline_size, num_cpus, num_cpus_avail,
                                         WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT + 1,
                                         &cpu_sets, &tsc_vals, local_err_msg,
                                         sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for CPU "
                         "carousel: %s", local_err_msg);
        ret = WTMLIB_RET_GENERIC_ERR;

        goto eval_tsc_monotonicity_cpusw_out;
    }

    /* Initialize CPU sets */
    for ( int cpu_id = 0, set_inx = 0; cpu_id < num_cpus; cpu_id++ )
    {
        if ( !CPU_ISSET_S( cpu_id, cpu_set_size, cpu_constraint) ) continue;

        WTMLIB_ASSERT( set_inx < num_cpus_avail);
        CPU_ZERO_S( cpu_set_size, cpu_sets[set_inx]);
        CPU_SET_S( cpu_id, cpu_set_size, cpu_sets[set_inx]);
        WTMLIB_OUT( "\t\tCPU index %d maps to CPU ID %d\n", set_inx, cpu_id);
        set_inx++;
    }

    ret = wtmlib_CollectTSCInCPUCarousel( cpu_sets, num_cpus_avail, tsc_vals, num_cpus,
                                          WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT,
                                          local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "CPU carousel failed: %s", local_err_msg);

        goto eval_tsc_monotonicity_cpusw_out;
    }

#ifdef WTMLIB_LOG
    wtmlib_PrintCarouselSamples( num_cpus_avail, tsc_vals,
                                 WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT, "\t\t");
#endif
    ret = wtmlib_CheckCarouselValsConsistency( tsc_vals, num_cpus_avail,
                                               WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT,
                                               err_msg, err_msg_size);

    if ( ret ) goto eval_tsc_monotonicity_cpusw_out;

    /* Check whether collected TSC values monotonically increase */
    prev_tsc_val = tsc_vals[0][0];

    for ( round = 0; round < WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT; round++ )
    {
        /* tsc_series may not be equal to CPU IDs. Refer to the initialization of CPU
           sets above to understand how tsc_series relate to CPU IDs */
        for ( tsc_series = 0; tsc_series < num_cpus_avail; tsc_series++ )
        {
            if ( tsc_vals[tsc_series][round] < prev_tsc_val )
            {
                /* This condition doesn't necessarily imply that TSCs are unreliable.
                   Non-monotonic TSC sequence may be a result of TSC wrap */
                is_monotonic = false;
                
                break;
            }

            prev_tsc_val = tsc_vals[tsc_series][round];
        } 
    }

    /* Check the last TSC value which is always measured on the same CPU as the first
       value. This last check is insignificant if WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT
       if large. But it's critical if WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT == 1 */
    if ( is_monotonic )
    {
        if ( tsc_vals[0][WTMLIB_EVAL_TSC_MONOTCTY_ROUND_COUNT] < prev_tsc_val )
        {
            /* This condition doesn't necessarily imply that TSCs are unreliable.
               Non-monotonic TSC sequence may be a result of TSC wrap */
            is_monotonic = false;
        }
    }

#ifdef WTMLIB_LOG
    if ( !is_monotonic )
    {
        tsc_series = (tsc_series == num_cpus_avail) ? 0 : tsc_series;

        WTMLIB_OUT( "\t\tMonotonic increase broke at carousel round %d, CPU index %d\n",
                    round, tsc_series);
    } else
    {
        WTMLIB_OUT( "\t\tThe collected TSC values DO monotonically increase\n");
    }
#endif

eval_tsc_monotonicity_cpusw_out:
    wtmlib_DeallocMemForCPUCarousel( num_cpus_avail, cpu_sets, tsc_vals);

    if ( !ret && is_monotonic_ret ) *is_monotonic_ret = is_monotonic;

    return ret;
}

/**
 * Check whether time-stamp counters can be reliably used for measuring wall-clock time
 *
 * Data required by the calculations is collected using "CPU Switching" method - a single
 * thread jumps from one CPU to another and takes all the needed measurements
 */
int wtmlib_EvalTSCReliabilityCPUSW( int64_t *tsc_range_length_ret,
                                    bool *is_monotonic_ret,
                                    char *err_msg,
                                    int err_msg_size)
{
    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    /* Process and system state */
    wtmlib_ProcAndSysState_t ps_state;
    int64_t tsc_range_length = -1;
    bool is_monotonic = false;
    int ret = 0;

    WTMLIB_OUT( "Evaluating TSC reliability (the required data is collected using "
                "\"CPU Switching\" method)...\n");
    wtmlib_InitProcAndSysState( &ps_state);
    ret = wtmlib_GetProcAndSystemState( &ps_state, local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't obtain details of the "
                         "system and process state: %s", local_err_msg);
        ret = WTMLIB_RET_GENERIC_ERR;

        goto eval_tsc_reliability_cpusw_out;
    }

    ret = wtmlib_CalcTSCEnclosingRangeCPUSW( ps_state.num_cpus, ps_state.initial_cpu,
                                             ps_state.initial_cpu_set,
                                             ps_state.cline_size, &tsc_range_length,
                                             local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while calculating enclosing "
                         "TSC range: %s", local_err_msg);

        goto eval_tsc_reliability_cpusw_out;
    }

    ret = wtmlib_EvalTSCMonotonicityCPUSW( ps_state.num_cpus, ps_state.initial_cpu_set,
                                           ps_state.cline_size, &is_monotonic,
                                           local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while evaluating TSC monotonicity"
                         ": %s", local_err_msg);

        goto eval_tsc_reliability_cpusw_out;
    }

    ret = wtmlib_RestoreInitialProcState( &ps_state, local_err_msg,
                                          sizeof( local_err_msg));

    if ( ret )
    {
        /* This error is not critical. At this point we've already successfully calculated
           all the required information. Just couldn't recover the initial process state.
           In this situation we could NOT report the error but silently return the
           calculated results. And the calling application could use that data because -
           maybe - it must proceed further anyway (regardless of unrecovered initial
           state). But for now we choose a different option. We report the error and don't
           return the calculated results. In this way, the calling application knows that
           something goes as not expected, but it doesn't have TSC reliability estimates
           (and should decide whether it wants to proceed further anyway and - maybe - use
           non-TSC-based time measuring method, or - for example - terminate).
           Another way of dealing with this situation would be to introduce "soft" error
           states that have the following meaning: "ok, we calculated the data and you can
           use it, but something unexpected has happened". We don't do that for now */
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't restore initial state of "
                         "the current process: %s", local_err_msg);
        ret = WTMLIB_RET_GENERIC_ERR;

        goto eval_tsc_reliability_cpusw_out;
    }

    if ( tsc_range_length_ret ) *tsc_range_length_ret = tsc_range_length;

    if ( is_monotonic_ret ) *is_monotonic_ret = is_monotonic;

eval_tsc_reliability_cpusw_out:
    wtmlib_DeallocProcAndSysState( &ps_state);

    return ret;
}

/**
 * A single TSC probe
 */
typedef struct
{
    /* TSC value */
    uint64_t tsc_val;
    /* Position in a globally-ordered sequence on TSC probes */
    uint64_t seq_num;
} wtmlib_TSCProbe_t;

/**
 * Type that describes an argument of TSC probe thread
 */
typedef struct
{
    /* CPU set that represents a CPU that the thread must be executed on */
    cpu_set_t *cpu_set;
    /* Number of CPUs in the system */
    int num_cpus;
    /* Array of TSC probes collected by the thread */
    wtmlib_TSCProbe_t *tsc_probes;
    /* The number of probes to collect */
    uint64_t probes_count;
    /* Global TSC probe sequence counter */
    uint64_t *seq_counter;
    /* A reference to a variable shared by all TSC probe threads. The variable plays a
       role of semaphore. Each thread increments it atomically only once to signal that
       it's ready to collect probes. The threads don't start collecting probes until
       they notice that all the threads incremented the counter. Use of the counter
       ensures that threads start collecting TSC probes more or less simultaneously */
    int *ready_counter;
    /* The number of TSC probe threads. Serves as a target value for the "ready counter".
       The threads start collecting probes only when they notice that the counter is
       equal to the number of threads */
    int num_threads;
    /* A buffer for storing error message generated by the thread (if any) */
    char err_msg[WTMLIB_MAX_ERR_MSG_SIZE];
} wtmlib_TSCProbeThreadArg_t;

/**
 * Initialize an argument for a TSC probe thread
 */
static void wtmlib_InitTSCProbeThreadArg( wtmlib_TSCProbeThreadArg_t* arg)
{
    WTMLIB_ASSERT( arg);
    arg->cpu_set = 0;
    arg->num_cpus = -1;
    arg->tsc_probes = 0;
    arg->probes_count = 0;
    arg->seq_counter = 0;
    arg->ready_counter = 0;
    arg->num_threads = -1;
    arg->err_msg[0] = '\0';

    return;
}

/**
 * Allocate memory required to start and track TSC probe threads:
 *   - memory for thread arguments
 *   - memory for thread descriptors
 *
 * If the function succeeds, then the allocated memory must be deallocated after
 * use by calling "wtmlib_DeallocMemForTSCProbeThreads()" function
 */
static int wtmlib_AllocMemForTSCProbeThreads( int num_threads,
                                              wtmlib_TSCProbeThreadArg_t
                                                  **thread_args_ret,
                                              pthread_t **thread_descs_ret,
                                              char *err_msg,
                                              int err_msg_size)
{
    wtmlib_TSCProbeThreadArg_t *thread_args = 0;
    pthread_t *thread_descs = 0;
    int thread_arg_size = sizeof( wtmlib_TSCProbeThreadArg_t);
    int ret = 0;

    /* Allocate memory for the threads' arguments. The arguments are not modified by the
       threads, and - thus - there is no need to align each argument to the cache line
       size (we align only mutable structures; by doing that we isolate mutable structures
       not only from each other but also from non-mutable data) */
    thread_args = (wtmlib_TSCProbeThreadArg_t*)calloc( thread_arg_size, num_threads);

    if ( !thread_args )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory to keep an "
                         "array of arguments for TSC probe threads");
        ret = WTMLIB_RET_GENERIC_ERR;

        goto alloc_mem_for_tsc_probe_threads_out;
    }

    /* Initialize allocated arguments */
    for ( int i = 0; i < num_threads; i++ )
    {
        wtmlib_InitTSCProbeThreadArg( &thread_args[i]);
    }

    /* Allocate memory for thread descriptors */
    thread_descs = (pthread_t*)calloc( sizeof( pthread_t), num_threads);

    if ( !thread_descs )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for "
                         "descriptors of TSC probe threads");
        ret = WTMLIB_RET_GENERIC_ERR;

        goto alloc_mem_for_tsc_probe_threads_out;
    }

alloc_mem_for_tsc_probe_threads_out:
    if ( ret || !thread_args_ret ) free( thread_args);

    if ( ret || !thread_descs_ret ) free( thread_descs);

    if ( !ret )
    {
        if ( thread_args_ret ) *thread_args_ret = thread_args;

        if ( thread_descs_ret ) *thread_descs_ret = thread_descs;
    }

    return ret;
}

/**
 * Deallocate memory allocated by "wtmlib_AllocMemForTSCProbeThreads()" function
 */
static void wtmlib_DeallocMemForTSCProbeThreads( wtmlib_TSCProbeThreadArg_t *thread_args,
                                                 pthread_t *thread_descs)

{
    if ( thread_args ) free( thread_args);

    if ( thread_descs ) free( thread_descs);

    return;
}

/**
 * Thread that collects TSC probes
 *
 * NOTE: TSC probe threads must allow asynchronous cancelability at any time.
 *       Explicit memory allocation is not allowed inside these threads.
 *       Synchronization methods should be thought through carefully.
 */
static void *wtmlib_TSCProbeThread( void *thread_arg)
{
    /* Make sure the thread can be cancelled at any time. Using "zero" as a second
       argument in the two function calls below is not POSIX-friendly. But there is some
       other non-portable stuff in this library which is hard to get rid of. So, for now
       "zeros" is not a priority problem */
    pthread_setcancelstate( PTHREAD_CANCEL_ENABLE, 0);
    pthread_setcanceltype( PTHREAD_CANCEL_ASYNCHRONOUS, 0);
    WTMLIB_ASSERT( thread_arg);
    WTMLIB_ASSERT( ((wtmlib_TSCProbeThreadArg_t*)thread_arg)->cpu_set);

    wtmlib_TSCProbeThreadArg_t *arg = (wtmlib_TSCProbeThreadArg_t*)thread_arg;
    pthread_t thread_self = pthread_self();
    int cpu_set_size = CPU_ALLOC_SIZE( arg->num_cpus);

    /* Switch to a CPU designated for this thread */
    if ( pthread_setaffinity_np( thread_self, cpu_set_size, arg->cpu_set) )
    {
        WTMLIB_BUFF_MSG( arg->err_msg, sizeof( arg->err_msg), "Couldn't bind itself "
                         "to a designated CPU");

        return (void*)(long int)WTMLIB_RET_GENERIC_ERR;
    }

    /* At this point the thread is ready to collect TSC probes. But it doesn't start
       doing that until all other threads are also ready. We use a shared counter to
       ensure that all threads start collecting probes more or less simultaneously.
       Each thread increments the counter when it is ready to collect probes. Then
       the thread waits until the counter reaches its target value (which is equal to
       the number of threads) */
    __atomic_add_fetch( arg->ready_counter, 1, __ATOMIC_ACQ_REL);

    /* Just spin (and burn CPU cycles, but hopefully for not so long) */
    while ( __atomic_load_n( arg->ready_counter, __ATOMIC_ACQUIRE) < arg->num_threads )
    {
        ;
    }

    uint64_t *seq_counter = arg->seq_counter;

    /* Well, can collect TSC probes finally. This loop should be as tight as
       possible. The less operations inside the better */
    for ( uint64_t i = 0; i < arg->probes_count; i++ )
    {
        uint64_t seq_num = 0;
        uint64_t tsc_val = 0;

        do
        {
            __atomic_load( seq_counter, &seq_num, __ATOMIC_ACQUIRE);
            /* Mixing old-school __sync* built-in function with new-style __atomic* built-
               in functions doesn't look really nice. But unfortunately we need here a
               type of semantics that is not explicitly advertised by any of the __atomic*
               intrinsics. What we strive to achieve here is to prevent reordering of an
               asm statement hidden inside WTMLIB_GET_TSC() with the above
              __atomic_load(). Seems, explicit full memory barrier is the only way to go
              for now */
            __sync_synchronize();
            tsc_val = WTMLIB_GET_TSC();
        } while ( !__atomic_compare_exchange_n( seq_counter, &seq_num, seq_num + 1, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));

        arg->tsc_probes[i].seq_num = seq_num;
        arg->tsc_probes[i].tsc_val = tsc_val;
    }

    return 0;
}

/**
 * Wait for completion of remaining TSC probe threads until they finish or timeout occurs.
 * The set of remaining threads is described by a range of their indexes:
 * [start_ind, num_started).
 *
 * The function returns a new lower bound for the range of indexes of still-running
 * threads. Also it may increase "detach_attempted", "detach_failed", and "thread_failed"
 * variables if the corresponding events occur
 */
static int wtmlib_WaitWithTimeout( pthread_t *thread_descs,
                                   int start_ind,
                                   int num_started,
                                   uint64_t time_to_wait,
                                   int *new_start_ind,
                                   int *detach_attempted,
                                   int *detach_failed,
                                   int *thread_failed)
{
    WTMLIB_ASSERT( thread_descs);

    uint64_t time_passed = 0;
    int detach_attempted_lcl = 0, detach_failed_lcl = 0;
    int thread_failed_lcl = 0;
    int ind = 0;

    for ( ind = start_ind; ind < num_started; ind++ )
    {
        do
        {
            void *thread_ret = 0;
            int join_ret = pthread_tryjoin_np( thread_descs[ind], &thread_ret);

            if ( !join_ret )
            {
                /* The condition below is true not only for threads that returned errors,
                   but also for threads that were cancelled */
                if ( thread_ret )
                {
                    thread_failed_lcl++;
                    WTMLIB_OUT( "\t\t\tThread %d exited with non-zero error code %ld\n",
                                ind, (long)thread_ret);
                }

                break;
            }

            if ( join_ret != EBUSY )
            {
                /* An error that cannot be handled for now. Try to detach the thread */
                detach_attempted_lcl++;

                if ( pthread_detach( thread_descs[ind]) ) detach_failed_lcl++;

                break;
            }

            sleep( WTMLIB_TSC_PROBE_COMPLETION_CHECK_PERIOD);
            time_passed += WTMLIB_TSC_PROBE_COMPLETION_CHECK_PERIOD;
        } while ( time_passed < time_to_wait);

        if ( time_passed >= time_to_wait ) break;
    }

    WTMLIB_ASSERT( (ind == num_started) || (time_passed >= time_to_wait));

    if ( new_start_ind ) *new_start_ind = ind;

    if ( detach_attempted ) (*detach_attempted) += detach_attempted_lcl;

    if ( detach_failed ) (*detach_failed) += detach_failed_lcl;

    if ( thread_failed ) (*thread_failed) += thread_failed_lcl;

    /* The function returns "zero" only if all the threads were successfully joined
       and returned "zeros" */
    return detach_attempted_lcl || thread_failed_lcl || (time_passed >= time_to_wait);
}

/**
 * Wait for completion of TSC probe threads
 */
static int wtmlib_WaitForTSCProbeThreads( pthread_t *thread_descs,
                                          int num_started,
                                          bool is_cancelled,
                                          char *err_msg,
                                          int err_msg_size)
{
    WTMLIB_ASSERT( thread_descs);

    uint64_t time_to_wait = 0;
    int thread_failed = 0;
    int detach_attempted = 0, detach_failed = 0;
    int cancel_failed = 0;
    bool is_timeout = false;
    int ind = 0;
#ifdef WTMLIB_DEBUG
    int ret = 0;
#endif

    if ( is_cancelled )
    {
        time_to_wait = WTMLIB_TSC_PROBE_WAIT_AFTER_CANCEL;
    } else
    {
        time_to_wait = WTMLIB_TSC_PROBE_WAIT_TIME;
    }

#ifdef WTMLIB_DEBUG
    ret = 
#endif
          wtmlib_WaitWithTimeout( thread_descs, 0, num_started, time_to_wait, &ind,
                                  &detach_attempted, &detach_failed, &thread_failed);

    /* Wait time is out. Need to cancel still-running threads. If the threads were
       already cancelled (or at least attempted to be cancelled) outside of this
       function (is_cancelled == true), then we don't try to cancel them again. We
       have already given them a chance to join. Now we can only detach them. And
       we do that later in this function */
    if ( ind < num_started && !is_cancelled )
    {
        WTMLIB_ASSERT( ret);
        is_timeout = true;

        for ( int i = ind; i < num_started; i++ )
        {
            if ( pthread_cancel( thread_descs[i]) ) cancel_failed++;
        }

        /* Now give a chance to cancelled threads to join. We don't care of thread
           return values here. The timeout was triggered. So, what we do here is
           just taking care of releasing the resources */
#ifdef WTMLIB_DEBUG
        ret = 
#endif
              wtmlib_WaitWithTimeout( thread_descs, ind, num_started,
                                      WTMLIB_TSC_PROBE_WAIT_AFTER_CANCEL, &ind,
                                      &detach_attempted, &detach_failed, 0);
    }

    if ( ind < num_started )
    {
        /* Well, if execution came here, then one (and only one) of the two is true:
             1) the function was called to join already cancelled threads but thread
                cancellation timout was triggered
             2) the function was called to join successfully started threads but thread
                execution timeout was triggered. After that an attempt was made to cancel
                the remaining threads. And then thread cancellation timeout was triggered
           So, some threads remain running at this point and we can do nothing about them
           but detach */
        WTMLIB_ASSERT( ret);
        is_timeout = true;

        for ( int i = ind; i < num_started; i++ )
        {
            detach_attempted++;

            if ( pthread_detach( thread_descs[i]) ) detach_failed++;
        }
    }

    if ( is_cancelled )
    {
        if ( !detach_attempted ) return 0;

        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "<timeout: %s>, <non-joined threads: %d "
                         "(%d of them left undetached)>", is_timeout ? "yes" : "no",
                         detach_attempted, detach_failed);

        return WTMLIB_RET_GENERIC_ERR;
    }

    if ( !is_timeout && !detach_attempted && !thread_failed ) return 0;

    WTMLIB_BUFF_MSG( err_msg, err_msg_size, "<timeout: %s>, <cancel errors: %d>, "
                     "<non-joined threads: %d (%d of them left undetached)>, "
                     "<failed threads: %d>", is_timeout ? "yes" : "no", cancel_failed,
                     detach_attempted, detach_failed, thread_failed);

    return WTMLIB_RET_GENERIC_ERR;
}

#ifdef WTMLIB_LOG
/**
 * Print sequence of collected TSC probes
 */
static void wtmlib_PrintTSCProbeSequence( int num_threads,
                                          wtmlib_TSCProbe_t **tsc_probes,
                                          uint64_t probes_count,
                                          const char* const indent)
{
    WTMLIB_OUT( "%sSequence of TSC probes\n", indent);
    WTMLIB_OUT( "%s(CPU index may not be equal to CPU ID)\n", indent);
    WTMLIB_OUT( "%s(CPU index <-> CPU ID mapping should be printed above)\n", indent);
    WTMLIB_ASSERT( UINT64_MAX / probes_count > (uint64_t)num_threads);

    uint64_t *indexes = (uint64_t*)calloc( sizeof( uint64_t), num_threads);

    if ( !indexes )
    {
        WTMLIB_OUT( "%s\tSTOPPED PRINTING. Couldn't allocate temporary memory\n", indent);

        goto print_tsc_probe_sequence_out;
    }

    for ( uint64_t seq_num = 0; seq_num < num_threads * probes_count; seq_num++ )
    {
        int i = 0;

        for ( ; i < num_threads; i++ )
        {
            if ( indexes[i] == probes_count ) continue;

            if ( tsc_probes[i][indexes[i]].seq_num == seq_num )
            {
                WTMLIB_OUT( "%s\tSequence number: %lu CPU index: %d TSC value: %lu\n",
                            indent, seq_num, i, tsc_probes[i][indexes[i]].tsc_val);
                indexes[i]++;

                break;
            }
        }

        if ( i == num_threads )
        {
            WTMLIB_OUT( "%s\tSTOPPED PRINTING. Data inconsistency detected\n", indent);

            break;
        }
    }

print_tsc_probe_sequence_out:
    if ( indexes ) free( indexes);

    return;
}
#endif

/**
 * Collect TSC probes
 *
 *   - probes_count probes is collected on each available CPU
 *   - the probes are collected by concurrently running threads (1 thread per each
 *     available CPU)
 *   - the probes are sequentially ordered. The order is ensured by means of compare-and-
 *     swap operation
 */
static int wtmlib_CollectCASOrderedTSCProbes( int num_threads,
                                              cpu_set_t **cpu_sets,
                                              int num_cpus,
                                              wtmlib_TSCProbe_t **tsc_probes,
                                              uint64_t probes_count,
                                              char *err_msg,
                                              int err_msg_size)
{
    wtmlib_TSCProbeThreadArg_t *thread_args = 0;
    pthread_t *thread_descs = 0;
    int ret = 0;
    uint64_t seq_counter = 0;
    int ready_counter = 0;
    /* The number of threads that were actually started */
    int num_started = num_threads;
    /* Number of times that thread cancellation failed */
    int cancel_fails = false;
    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    char create_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    char cancel_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";

    WTMLIB_ASSERT( num_threads && cpu_sets && tsc_probes);

    /* We're going to assign a global sequence number to each TSC probe. The number is
       of uint64_t type. Hence, the total number of probes cannot exceed UINT64_MAX */
    if ( UINT64_MAX / num_threads < probes_count )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "The number of probes per thread must "
                         "not be bigger than %lu (%lu requested)",
                         UINT64_MAX / num_threads, probes_count);

        return WTMLIB_RET_GENERIC_ERR;
    }

    ret = wtmlib_AllocMemForTSCProbeThreads( num_threads, &thread_args, &thread_descs,
                                             local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while allocating memory for "
                         "TSC probe threads");

        goto make_cas_ordered_tsc_probes_out;
    }

    /* Initialize thread arguments and start threads */
    for ( int i = 0; i < num_threads; i++ )
    {
        thread_args[i].cpu_set = cpu_sets[i];
        thread_args[i].num_cpus = num_cpus;
        thread_args[i].tsc_probes = tsc_probes[i];
        thread_args[i].probes_count = probes_count;
        thread_args[i].seq_counter = &seq_counter;
        thread_args[i].ready_counter = &ready_counter;
        thread_args[i].num_threads = num_threads;
        thread_args[i].err_msg[0] = '\0';
        ret = pthread_create( &thread_descs[i], 0, wtmlib_TSCProbeThread,
                              &thread_args[i]);

        if ( ret )
        {
            num_started = i;
            
            break;
        }
    }

    if ( num_started != num_threads )
    {
        WTMLIB_ASSERT( num_started < num_threads);

        /* Cancel threads that were started. If we don't do that, they will hang
           forever waiting for the target value of the "ready counter" */
        for ( int i = 0; i < num_started; num_started++ )
        {
            if ( pthread_cancel( thread_descs[i]) ) cancel_fails++;
        }
    }

    ret = wtmlib_WaitForTSCProbeThreads( thread_descs, num_started,
                                         num_started != num_threads, local_err_msg,
                                         sizeof( local_err_msg));

    if ( num_started != num_threads )
    {
        snprintf( create_err_msg, sizeof( create_err_msg), "Couldn't start all "
                  "TSC probe threads; only %d were started", num_started);

        if ( cancel_fails )
        {
            snprintf( cancel_err_msg, sizeof( cancel_err_msg), "Cancel request "
                      "failed for %d started threads", cancel_fails);
        } else
        {
            snprintf( cancel_err_msg, sizeof( cancel_err_msg), "Cancel request "
                      "successfully submitted to all started threads");
        }

        if ( ret )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "%s; %s; The following error occured "
                             "while joining started threads: %s", create_err_msg,
                             cancel_err_msg, local_err_msg);
        } else
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "%s; %s; All started threads "
                             "successfully joined", create_err_msg, cancel_err_msg);
        }
        
        ret = WTMLIB_RET_GENERIC_ERR;

        goto make_cas_ordered_tsc_probes_out;
    }

#ifdef WTMLIB_LOG
    for ( int i = 0; i < num_threads; i++ )
    {
        if ( strcmp( thread_args[i].err_msg, "\0") )
        {
            WTMLIB_OUT( "\t\t\tThread %d returned error message: %s\n", i,
                        thread_args[i].err_msg);
        }
    }
#endif

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error encountered while joining "
                         "TSC probe threads: %s", local_err_msg);

        goto make_cas_ordered_tsc_probes_out;
    }

make_cas_ordered_tsc_probes_out:
    wtmlib_DeallocMemForTSCProbeThreads( thread_args, thread_descs);

    return ret;
}

/**
 * Allocate memory required to collect CAS-ordered TSC probes
 */
static int wtmlib_AllocMemForCASOrderedProbes( int cline_size,
                                               int num_cpus,
                                               int num_cpu_sets,
                                               int num_probes,
                                               cpu_set_t ***cpu_sets_ret,
                                               wtmlib_TSCProbe_t ***tsc_probes_ret,
                                               char *err_msg,
                                               int err_msg_size)
{
    return wtmlib_AllocMemForTSCSampling( cline_size, num_cpus, num_cpu_sets,
                                          num_probes, sizeof( wtmlib_TSCProbe_t),
                                          cpu_sets_ret, (void***)tsc_probes_ret,
                                          err_msg, err_msg_size);
}

/**
 * Deallocate memory allocated previously by "wtmlib_AllocMemForCASOrderedProbes()"
 * routine
 */
static void wtmlib_DeallocMemForCASOrderedProbes( int num_cpu_sets,
                                                  cpu_set_t **cpu_sets,
                                                  wtmlib_TSCProbe_t **tsc_probes)
{
    wtmlib_DeallocMemForTSCSampling( num_cpu_sets, cpu_sets, (void**)tsc_probes);

    return;
}

/**
 * Generic evaluation of consistency of TSC probes
 */
static int wtmlib_CheckTSCProbesConsistency( wtmlib_TSCProbe_t **tsc_probes,
                                             int num_avail_cpus,
                                             uint64_t num_probes,
                                             char *err_msg,
                                             int err_msg_size)
{
    WTMLIB_ASSERT( tsc_probes);

    /* Make sure that collected TSC values do vary on each of the CPUs. That may not be
       true, for example, in case when some CPUs consistently return "zero" for every TSC
       test.
       This check is really important. If all TSC values are equal, then both TSC "delta"
       ranges and TSC monotonicity will be perfect, but at the same time TSC would be
       absolutely inappropriate for measuring wall-clock time. (Global TSC monotonicity
       evaluation and some other monotonicity checks existing in the library will give the
       positive result because they don't require successively measured TSC values to
       strictly grow. Overall, WTMLIB's requirements with respect to TSC monotonicity are
       the following: TSC values must grow on a global scale and not decrease locally.
       I.e. the library allows some successively measured TSC values to be equal to each
       other) */
    for ( int i = 0; i < num_avail_cpus; i++ )
    {
        WTMLIB_ASSERT( tsc_probes[i]);

        if ( tsc_probes[i][0].tsc_val == tsc_probes[i][num_probes - 1].tsc_val )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "First and last TSC probes collected "
                             "on a CPU with index %d have equal TSC values", i);

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }
    }

    return 0;
}

/*
 * Calculate bounds of a shift between TSC on a given CPU and TSC on the base CPU
 * (assuming that TSC values were collected using the method of CAS-ordered probes)
 *
 * We do that in the following way:
 *   1) let's take from a globally ordered sequence of TSC probes two probes that were
 *      collected successively on the base CPU. Let's denote these probes p1 and p2
 *   2) in the globally ordered sequence of probes there may or may not be probes that
 *      were collected between p1 and p2 (on the other CPU of course). Assume that a
 *      sequence of such probes does exist. There may be just one probe in this sequence.
 *      Let's denote the first probe in this sequence P1 and the last one P2 (P1 and P2
 *      may refer to the same probe)
 *   3) let's now denote TSC values corresponding to p1 and p2 t1 and t2. Also we denote
 *      TSC values corresponding to P1 and P2 T1 and T2
 *   4) let's choose some value T such that T1 <= T <= T2
 *   5) when TSC on the given CPU was equal to T, TSC on the base CPU was somewhere
 *      between t1 and t2. Let's denote that value of the base TSC "t"
 *   6) we're interested in a difference "delta = T - t". Let's find bounds for this
 *      difference
 *   7) this difference is the smallest when t is the biggest. t is the biggest when it
 *      is closest to t2. But t cannot be closer to t2 than "T2 - T". That's because t
 *      cannot be closer to t2 than T to T2 (assuming that time runs at the same pace on
 *      both CPUs). Thus, the maximum value for t is "t2 - (T2 - T)". And the minimum
 *      value for "delta" is "T - (t2 - (T2 - T)) = T2 - t2"
 *   8) similarly we find the upper bound for delta. delta is the biggest when t is the
 *      smallest. t is the smallest when it is closest to t1. But t cannot be closer to t1
 *      than T to T1. So, the minumim value for t is "t1 + (T - T1)". And the maximum
 *      value for delta is "T - (t1 + (T - T1)) = T1 - t1"
 *   9) that's how we find a range for "delta" based on a single sequence of TSC probes
 *      collected on the given CPU and enclosed between a pair of TSC probes collected
 *      successively on the base CPU
 *  10) but in a globally ordered sequence of TSC probes there may exist multiple
 *      sub-sequences with that property. Based on that, we can narrow the range of
 *      possible "delta" values. To do that, we calculate an intersection of multiple
 *      "delta" ranges calculated for different TSC probe sub-sequences of a globally
 *      ordered TSC sequence
 *
 * Input: tsc_probes[0] - array of TSC probes collected on the base CPU
 *        tsc_probes[1] - array of TSC probes collected on some other CPU
 */
static int wtmlib_CalcTSCDeltaRangeCOP( wtmlib_TSCProbe_t **tsc_probes,
                                        uint64_t num_probes,
                                        int64_t *delta_min,
                                        int64_t *delta_max,
                                        char *err_msg,
                                        int err_msg_size)
{
    WTMLIB_ASSERT( tsc_probes && tsc_probes[0] && tsc_probes[1]);

    int64_t d_min = INT64_MIN, d_max = INT64_MAX;
    /* Indexes into arrays of TSC probes collected on the given and base CPUs */
    uint64_t ig = 0, ib = 0;
    uint64_t seq_num = 0;
    /* The number of produced independent "delta" range estimations */
    uint64_t num_ranges = 0;

    WTMLIB_OUT( "\t\tCalculating shift between TSC counters of the two CPUs...\n");

    if ( wtmlib_CheckTSCProbesConsistency( tsc_probes, 2, num_probes,
                                           err_msg, err_msg_size) )
    {
        return WTMLIB_RET_TSC_INCONSISTENCY;
    }

    /* Consistency check. Successive TSC values measured on the same CPU must
       not decrease (unless TSC counter wraps) */
    for ( uint64_t i = 1; i < num_probes; i++ )
    {
        if ( tsc_probes[0][i].tsc_val < tsc_probes[0][i - 1].tsc_val
             || tsc_probes[1][i].tsc_val < tsc_probes[1][i - 1].tsc_val )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Detected decreasing successive TSC "
                             "values (measured on the same CPU). Can be a result of TSC "
                             "wrap");

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }
    }

    /* Skip those TSC probes collected on the given CPU that are not enclosed between
       any probes collected on the base CPU */
    if ( tsc_probes[1][0].seq_num == 0 )
    {
        for ( ; tsc_probes[1][ig].seq_num == ig; ig++, seq_num++ )
        {
            WTMLIB_ASSERT( ig < num_probes);
        }
    }

    /* Sequence numbers must be sequential */
    WTMLIB_ASSERT( tsc_probes[0][0].seq_num == seq_num);

    /* The loop counter starts from "one" */
    for ( ib = 1; ib < num_probes; ib++, seq_num++ )
    {
        /* Check whether between the current and previous TSC probes collected on the base
           CPU there are probes collected on the other CPU */
        if ( tsc_probes[0][ib].seq_num == seq_num + 1 ) continue;

        WTMLIB_ASSERT( tsc_probes[0][ib].seq_num > seq_num + 1);
        num_ranges++;

        uint64_t tsc_base_prev = tsc_probes[0][ib - 1].tsc_val;
        uint64_t tsc_base_curr = tsc_probes[0][ib].tsc_val;
        /* Fist and last indexes of TSC probes that were collected on the given CPU
           between successive TSC probes collected on the base CPU */
        uint64_t sub_seq_first = ig, sub_seq_last = 0;

        WTMLIB_ASSERT( tsc_probes[1][ig].seq_num == tsc_probes[0][ib - 1].seq_num + 1);

        for ( ; ig < num_probes && tsc_probes[1][ig].seq_num < tsc_probes[0][ib].seq_num;
              ig++, seq_num++ )
        {
            WTMLIB_ASSERT( tsc_probes[1][ig].seq_num == seq_num + 1);
        }

        sub_seq_last = ig - 1;
        WTMLIB_ASSERT( sub_seq_last >= sub_seq_first);
        WTMLIB_ASSERT( tsc_probes[0][ib].seq_num == seq_num + 1);

        uint64_t tsc_given_min = tsc_probes[1][sub_seq_first].tsc_val;
        uint64_t tsc_given_max = tsc_probes[1][sub_seq_last].tsc_val;
        /* Check that we will not get overflow while subtracting TSC values. If we do get
           overflow, that means that we cannot rely on TSC while measuring wall-clock
           time. Big difference between TSC values measured on different CPUs may be a
           result of TSC wrap. This possibility is not ruled out by the monotonicity check
           done at the beginning of the function. That check ensures that TSC wrap didn't
           occur while TSC probes were collected. But the wrap could occur on one of the
           CPUs right before the probe collection was started. For example, one CPU might
           be close to the point of TSC wrap. But the wrap hasn't happened yet (and hasn't
           happened while TSC probes were collected). But the other CPU might has passed
           this point just before WTMLIB started collecting TSC probes. So, both CPUs are
           close to the point of TSC wrap, but from different sides. TSC values may grow
           monotonically on each of the CPUs but cross-CPU difference will be big (until
           the first CPU also experiences TSC wrap) */
        uint64_t diff1 = ABS_DIFF( tsc_given_min, tsc_base_prev);
        uint64_t diff2 = ABS_DIFF( tsc_given_max, tsc_base_curr);

        if ( diff1 > (uint64_t)INT64_MAX || diff2 > (uint64_t)INT64_MAX )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Difference between TSC values "
                             "measured on different CPUs is too big (bigger than %ld). "
                             "May be a result of TSC wrap", INT64_MAX);

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }

        /* Time interval between enclosing base probes must be bigger than time
           interval between enclosed probes collected on the other CPU. The opposite
           means that time runs at different pace on different CPUs (the case of TSC
           wrap is ruled out here because of monotonicity check at the beginning of
           this function) */
        if ( tsc_base_curr - tsc_base_prev < tsc_given_max - tsc_given_min )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "TSC interval between enclosing "
                             "TSC probes collected on the base CPU is shorter than "
                             "TSC interval between enclosed probes collected on the "
                             "given CPU. It appears that time runs at different pace "
                             "on different CPUs");

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }

        int64_t bound_min = tsc_given_max - tsc_base_curr;
        int64_t bound_max = tsc_given_min - tsc_base_prev;

        WTMLIB_ASSERT( bound_min <= bound_max);

        /* "delta" ranges calculated for different sub-sequences must intersect.
           Otherwise, we have a major TSC inconsistency and cannot rely on TSC while
           measuring wall-clock time */
        if ( bound_min > d_max || bound_max < d_min )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "TSC delta ranges calculated for "
                             "different sub-sequences don't intersect. May be a result "
                             "of some major inconsistency");

            return WTMLIB_RET_TSC_INCONSISTENCY;
        }

        WTMLIB_OUT( "\t\t\tThe shift belongs to range: %ld [%ld, %ld]\n",
                    bound_max - bound_min, bound_min, bound_max);

        d_min = bound_min > d_min ? bound_min : d_min;

        d_max = bound_max < d_max ? bound_max : d_max;
    }

    /* Check whether the result is statistically significant */
    if ( num_ranges < WTMLIB_TSC_DELTA_RANGE_COUNT_THRESHOLD )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't observe the required amount of "
                         "TSC probe sub-sequences with desired properties (%lu required, "
                         "%lu found)", WTMLIB_TSC_DELTA_RANGE_COUNT_THRESHOLD,
                         num_ranges);

        return WTMLIB_RET_POOR_STAT;
    }

    WTMLIB_ASSERT( d_min <= d_max);
    WTMLIB_OUT( "\t\t\tCombined range (intersection of all the above): %ld [%ld, %ld]\n",
                d_max - d_min, d_min, d_max);

    if ( delta_min ) *delta_min = d_min;

    if ( delta_max ) *delta_max = d_max;

    return 0;
}

/**
 * Calculate size of enclosing TSC range (using a sequence of CAS-ordered probes)
 *
 * "Size of enclosing TSC range" is a non-negative integer value such that if TSC
 * values are measured simultaneously on all the available CPUs, then difference
 * between the largest and the smallest will be not bigger than this value. In other
 * words, it's an estimated upper bound for shifts between TSC counters running on
 * different CPUs
 *
 * To calculate "enclosing TSC range" we do the following:
 *   1) for each CPU we calculate bounds that enclose a shift between this CPU's TSC
 *      and TSC of some base CPU
 *   2) we calculate the smallest range that encloses all ranges caclulated during
 *      the previous step
 *
 * When "enclosing TSC range" is found, its size is calculated as a difference
 * between its upper and lower bounds
 */
static int wtmlib_CalcTSCEnclosingRangeCOP( int num_cpus,
                                            int base_cpu,
                                            const cpu_set_t* const cpu_constraint,
                                            int cline_size,
                                            int64_t *range_size,
                                            char *err_msg,
                                            int err_msg_size)
{
    WTMLIB_ASSERT( cpu_constraint);

    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    cpu_set_t **cpu_sets = 0;
    wtmlib_TSCProbe_t **tsc_probes = 0;
    int cpu_set_size = CPU_ALLOC_SIZE( num_cpus);
    int64_t delta_min = INT64_MAX, delta_max = INT64_MIN;
    int64_t l_bound = INT64_MAX, u_bound = INT64_MIN;
    int ret = 0;

    WTMLIB_OUT( "\tCalculating an upper bound for shifts between TSC counters running "
                "on different CPUs...\n");
    WTMLIB_OUT( "\t\tBase CPU ID: %d\n", base_cpu);
    ret = wtmlib_AllocMemForCASOrderedProbes( cline_size, num_cpus, 2,
                                              WTMLIB_CALC_TSC_RANGE_PROBES_COUNT,
                                              &cpu_sets, &tsc_probes, local_err_msg,
                                              sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for CAS-"
                         "ordered probes: %s", local_err_msg);

        goto calc_tsc_enclosing_range_cop_out;
    }

    CPU_ZERO_S( cpu_set_size, cpu_sets[0]);
    CPU_SET_S( base_cpu, cpu_set_size, cpu_sets[0]);
    CPU_ZERO_S( cpu_set_size, cpu_sets[1]);

    for ( int cpu_id = 0; cpu_id < num_cpus; cpu_id++ )
    {
        if ( !CPU_ISSET_S( cpu_id, cpu_set_size, cpu_constraint) || cpu_id == base_cpu )
        {
            continue;
        }

        CPU_SET_S( cpu_id, cpu_set_size, cpu_sets[1]);
        WTMLIB_OUT( "\n\t\tCollecting TSC probes on CPUs %d and %d...\n", base_cpu,
                    cpu_id);
        ret = wtmlib_CollectCASOrderedTSCProbes( 2, cpu_sets, num_cpus, tsc_probes,
                                                 WTMLIB_CALC_TSC_RANGE_PROBES_COUNT,
                                                 local_err_msg, sizeof( local_err_msg));

        if ( ret )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while collecting CAS-ordered "
                             "TSC probes: %s", local_err_msg);

            goto calc_tsc_enclosing_range_cop_out;
        }

#ifdef WTMLIB_LOG
        WTMLIB_OUT( "\t\tCPU ID %d maps to CPU index %d\n", base_cpu, 0);
        WTMLIB_OUT( "\t\tCPU ID %d maps to CPU index %d\n", cpu_id, 1);
        wtmlib_PrintTSCProbeSequence( 2, tsc_probes, WTMLIB_CALC_TSC_RANGE_PROBES_COUNT,
                                      "\t\t");
#endif
        ret = wtmlib_CalcTSCDeltaRangeCOP( tsc_probes, WTMLIB_CALC_TSC_RANGE_PROBES_COUNT,
                                           &delta_min, &delta_max, local_err_msg,
                                           sizeof( local_err_msg));

        if ( ret )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Calculation of TSC delta range "
                             "failed: %s", local_err_msg);

            goto calc_tsc_enclosing_range_cop_out;
        }

        /* Update bounds of the enclosing TSC range */
        l_bound = l_bound > delta_min ? delta_min : l_bound;

        u_bound = u_bound < delta_max ? delta_max : u_bound;

        WTMLIB_ASSERT( delta_max >= delta_min && u_bound >= l_bound);
        /* Return CPU mask to the "clean" state */
        CPU_CLR_S( cpu_id, cpu_set_size, cpu_sets[1]);
    }

    WTMLIB_OUT( "\n\t\tShift between TSC on any of the available CPUs and TSC on the "
                "base CPU belongs to range: [%ld, %ld]\n", l_bound, u_bound);
    WTMLIB_OUT( "\t\tUpper bound for shifts between TSCs is: %ld\n", u_bound - l_bound);

    if ( range_size ) *range_size = u_bound - l_bound;

calc_tsc_enclosing_range_cop_out:
    wtmlib_DeallocMemForCASOrderedProbes( 2, cpu_sets, tsc_probes);

    return ret;
}

/**
 * Check whether TSC values monotonically increase along an ordered sequence of TSC
 * probes
 *
 * Monotonicity is evaluated in a straightforward way:
 *   - TSC probes have sequence numbers that reflect an order in which the probes were
 *     collected
 *   - the function traverses the probes in order of increasing sequence numbers and
 *     examines whether TSC values also increase
 *
 * Along with the examination described above the function also assess statistical
 * significance of the result. Let us use graph theory terms to explain how the
 * assessement is made (graph terminology is not important here; it just makes the
 * explanation simpler):
 *   - assume a graph such that allowed CPUs are its vertices. The graph is fully
 *     connected by undirected unweighted edges
 *   - we treat the sequence of TSC probes as a path in this graph. (The sequence of
 *     probes naturally transforms to a sequence of CPUs on which these probes were
 *     collected. And the sequence of CPUs - which is a sequence of vertices of the
 *     graph - represents some path in the graph)
 *   - we introduce the notion of "full loop". "Full loop" is a sub-sequence consisting
 *     of successive (!) points on the path such that:
 *        1) first and last CPUs in the sub-sequence are the same
 *        2) each of the available CPUs can be found at least once in the sub-sequence
 *        3) there is no shorter sub-sequence of successive points that has the same
 *           starting point and satisfies conditions 1) and 2)
 *     In other words, "full loop" is a sub-path of the path such that it starts at
 *     some CPU, passes through all other CPUs (all or some of them may be visited
 *     several times), and returns to the starting CPU (which also might be visited
 *     several times before the finish). And there should be no shorter sub-path with
 *     the same properties which starts at the same starting point.
 *   - positive result of monotonicity evaluation is considered more or less reliable
 *     (or statistically significant) if a "full loop" exists on the path. (If all
 *     available CPUs were visited on the path but no "full loop" exists, the result
 *     cannot be considered reliable. It's easy to understand why).
 *   - the more "full loops" exist the more reliable is the result
 *   - so, to assess statistical significance of monotinicity evaluation we count the
 *     number of "full loops" on the provided CPU path
 *   - there can exist overlapping "full loops" on the path. We don't take them into
 *     account. Taking them into account would complicate the algorithm significantly
 *     while not improving reliability assessment too much. So, we count only "full
 *     loops" that are located on the path strictly one after another
 *   - and we introduce one more simplification. We require that all "full loops" start
 *     with the same CPU (which is actually the first CPU in the TSC probe sequence).
 *     This simplification allows us to build a very simple and fast algorithm. Its
 *     complexity is O(num_probes) and additional memory is O(num_CPUs). But this
 *     algorithm is less precise than algorithm that allows "full loops" to start with
 *     arbitrary CPU. Consider an example. Assume that we have 4 CPUs and the following
 *     path: 3 2 1 3 4 2. There is no "full loop" here that starts with CPU 3. So,
 *     the algorithm implemented here will not find any "full loop" at all. But in the
 *     sequence above there exists a "full loop" that starts with CPU 2.
 *     An algorithm that doesn't impose constraint on the starting CPU can be easily
 *     implemented in a way that is very similar to what is seen in the function below.
 *     The complexity would be O(num_probes * num_CPUs) and additional memory would be
 *     O(num_CPUs ^ 2). But currently the simplified version works well. There is no
 *     need in higher precision. If higher precision will become a requirement, simple
 *     modifications of the below code will allow to have it.
 *
 * The algorithm we use to find "full loops" is the following:
 *   - we iterate over TSC probes from the first to the last
 *   - we have a counter of already found "full loops"
 *   - for each available CPU we have a flag indicating whether this CPU was seen since
 *     we have found the previous "full loop"
 *   - also we a have a variable that tracks the number of different CPUs seen since
 *     the last "full loop" was found
 *   - if this variable becomes equal to the number of available CPUs, and if we
 *     encounter the starting CPU after that, then we conclude that we have one more
 *     "full loop"
 */
static int wtmlib_IsProbeSequenceMonotonic( wtmlib_TSCProbe_t **tsc_probes,
                                            uint64_t probes_num,
                                            int num_avail_cpus,
                                            bool *is_monotonic_ret,
                                            char *err_msg,
                                            int err_msg_size)
{
    WTMLIB_ASSERT( tsc_probes);

    int ret = 0;
    uint64_t prev_tsc_val = 0;
    bool is_monotonic = true;
    /* Index of the first CPU in the TSC probes sequence */
    int first_cpu_ind = -1;
    /* Number of "full loops" there were already found */
    uint64_t num_loops = 0;
    /* Number of different CPUs seen while trying to find a new "full loop" */
    int cpus_seen = 0;
    /* indexes[cpu_ind] stores current index to an array of TSC probes collected on a CPU
       that has index "cpu_ind" */
    uint64_t *indexes = 0;

    WTMLIB_OUT( "\t\tTesting monotonicity of the TSC probes sequence...\n");

    if ( wtmlib_CheckTSCProbesConsistency( tsc_probes, num_avail_cpus, probes_num,
                                           err_msg, err_msg_size) )
    {
        return WTMLIB_RET_TSC_INCONSISTENCY;
    }

    /* If (cpu_seen_num[ind] == num_loops + 1) then it means that we've already seen CPU
       with index "ind" while trying to find a new "full loop". CPU index may not be equal
       to CPU ID. See the caller function to understand the difference */
    uint64_t *cpu_seen_num = (uint64_t*)calloc( sizeof( uint64_t), num_avail_cpus);

    if ( !cpu_seen_num )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory to keep track "
                         "of CPUs visited since the last full loop was found");
        ret = WTMLIB_RET_GENERIC_ERR;

        goto is_probe_sequence_monotonic_out;
    }

    indexes = (uint64_t*)calloc( sizeof( uint64_t), num_avail_cpus);

    if ( !indexes )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory to store "
                         "indexes into arrays of TSC probes");
        ret = WTMLIB_RET_GENERIC_ERR;

        goto is_probe_sequence_monotonic_out;
    }

    /* Get index of the first CPU in the sequence */
    for ( int i = 0; i < num_avail_cpus; i++ )
    {
        if ( tsc_probes[i][0].seq_num == 0 )
        {
            first_cpu_ind = i;

            break;
        }
    }

    WTMLIB_ASSERT( first_cpu_ind != -1);
    WTMLIB_ASSERT( UINT64_MAX / probes_num >= (uint64_t)num_avail_cpus);

    for ( uint64_t i = 0; i < probes_num * num_avail_cpus; i++ )
    {
        int cpu_ind = 0;

        for ( ; cpu_ind < num_avail_cpus; cpu_ind++ )
        {
            wtmlib_TSCProbe_t *tsc_probe = &tsc_probes[cpu_ind][indexes[cpu_ind]];

            if ( tsc_probe->seq_num != i ) continue;

            if ( tsc_probe->tsc_val < prev_tsc_val )
            {
                is_monotonic = false;
                WTMLIB_OUT( "\t\tTSC value growth breaks at sequence number %lu\n", i);

                break;
            }

            indexes[cpu_ind]++;
            prev_tsc_val = tsc_probe->tsc_val;

            /* Have we found the new "full loop"? */
            if ( cpus_seen == num_avail_cpus && cpu_ind == first_cpu_ind )
            {
                num_loops++;
                cpus_seen = 0;
            }

            /* Do we see the current CPU for the first time while trying to find a new
               "full loop"? */
            if ( cpu_seen_num[cpu_ind] < num_loops + 1 )
            {
                WTMLIB_ASSERT( cpu_seen_num[cpu_ind] == num_loops);
                cpu_seen_num[cpu_ind]++;
                cpus_seen++;
                WTMLIB_ASSERT( cpus_seen <= num_avail_cpus);
            } else WTMLIB_ASSERT( cpu_seen_num[cpu_ind] == num_loops + 1);

            break;
        }

        if ( !is_monotonic ) break;

        if ( cpu_ind == num_avail_cpus )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Internal inconsistency: couldn't "
                             "find TSC probe with sequential number %lu", i);
            ret = WTMLIB_RET_GENERIC_ERR;

            goto is_probe_sequence_monotonic_out;
        }
    }

    if ( is_monotonic && num_loops < WTMLIB_FULL_LOOP_COUNT_THRESHOLD )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't observe the required amount of "
                         "TSC probe sub-sequences with desired properties (%lu required, "
                         "%lu found)", WTMLIB_FULL_LOOP_COUNT_THRESHOLD, num_loops);
        ret = WTMLIB_RET_POOR_STAT;

        goto is_probe_sequence_monotonic_out;
    }

    if ( is_monotonic )
    {
        WTMLIB_OUT( "\t\t\tThe collected TSC values DO monotonically increase\n");
    }

is_probe_sequence_monotonic_out:
    if ( cpu_seen_num ) free( cpu_seen_num);

    if ( indexes ) free( indexes);

    if ( !ret && is_monotonic_ret ) *is_monotonic_ret = is_monotonic;

    return ret;
}

/**
 * Check whether TSC values measured on same/different CPUs one after another
 * monotonically increase
 * 
 * The algorithm is the following:
 *   1) collect TSC probes using concurrently running threads (one thread per each
 *      available CPU). All the measurements are sequentially ordered by means of
 *      compare-and-swap operation
 *   2) check whether TSC values monotonically increase along the sequence of collected
 *      probes
 *   3) at the same time evaluate statistical significance of the result
 *
 * NOTE: if the function reports that collected TSC values do not monotonically increase,
 *       that doesn't necessarily imply that TSCs are unreliable. In some cases the
 *       observed decrease may be a result of TSC wrap
 */
static int wtmlib_EvalTSCMonotonicityCOP( int num_cpus,
                                          const cpu_set_t* const cpu_constraint,
                                          int cline_size,
                                          bool *is_monotonic_ret,
                                          char *err_msg,
                                          int err_msg_size)
{
    WTMLIB_ASSERT( cpu_constraint);

    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    cpu_set_t **cpu_sets = 0;
    wtmlib_TSCProbe_t **tsc_probes = 0;
    int cpu_set_size = CPU_ALLOC_SIZE( num_cpus);
    /* Number of CPUs available to the current thread */
    int num_cpus_avail = 0;
    bool is_monotonic = false;
    int ret = 0;

    WTMLIB_OUT( "\tEvaluating TSC monotonicity...\n");

    /* Calculate the number of CPUs available to the current thread */
    for ( int cpu_id = 0; cpu_id  < num_cpus; cpu_id++ )
    {
        if ( CPU_ISSET_S( cpu_id, cpu_set_size, cpu_constraint) ) num_cpus_avail++;
    }

    ret = wtmlib_AllocMemForCASOrderedProbes( cline_size, num_cpus, num_cpus_avail,
                                              WTMLIB_EVAL_TSC_MONOTCTY_PROBES_COUNT,
                                              &cpu_sets, &tsc_probes, local_err_msg,
                                              sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for CAS-"
                         "ordered probes: %s", local_err_msg);

        goto eval_tsc_monotonicity_cop_out;
    }

    /* Initialize CPU sets */
    for ( int cpu_id = 0, set_inx = 0; cpu_id < num_cpus; cpu_id++ )
    {
        if ( !CPU_ISSET_S( cpu_id, cpu_set_size, cpu_constraint) ) continue;

        WTMLIB_ASSERT( set_inx < num_cpus_avail);
        WTMLIB_OUT( "\t\tCPU ID %d maps to CPU index %d\n", cpu_id, set_inx);
        CPU_ZERO_S( cpu_set_size, cpu_sets[set_inx]);
        CPU_SET_S( cpu_id, cpu_set_size, cpu_sets[set_inx]);
        set_inx++;
    }

    ret = wtmlib_CollectCASOrderedTSCProbes( num_cpus_avail, cpu_sets,
                                             num_cpus, tsc_probes,
                                             WTMLIB_EVAL_TSC_MONOTCTY_PROBES_COUNT,
                                             local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while collecting CAS-ordered "
                         "TSC probes: %s", local_err_msg);

        goto eval_tsc_monotonicity_cop_out;
    }

#ifdef WTMLIB_LOG
    wtmlib_PrintTSCProbeSequence( num_cpus_avail, tsc_probes,
                                  WTMLIB_EVAL_TSC_MONOTCTY_PROBES_COUNT, "\t\t");
#endif
    ret = wtmlib_IsProbeSequenceMonotonic( tsc_probes,
                                           WTMLIB_EVAL_TSC_MONOTCTY_PROBES_COUNT,
                                           num_cpus_avail, &is_monotonic,
                                           local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while testing monotonicity of "
                         "the TSC values sequence: %s", local_err_msg);

        goto eval_tsc_monotonicity_cop_out;
    }

eval_tsc_monotonicity_cop_out:
    wtmlib_DeallocMemForCASOrderedProbes( num_cpus_avail, cpu_sets, tsc_probes);

    if ( !ret && is_monotonic_ret ) *is_monotonic_ret = is_monotonic;

    return ret;
}

/**                                       
 * Check whether time-stamp counters can be reliably used for measuring wall-clock time
 *
 * Data required by the calculations is collected using a method of "CAS-Ordered Probes" -
 * concurrently running threads (one per each available CPU) take all the needed
 * measurements. The measurements are sequentially ordered by means of compare-and-swap
 * operation
 */
int wtmlib_EvalTSCReliabilityCOP( int64_t *tsc_range_length_ret,
                                  bool *is_monotonic_ret,
                                  char *err_msg,
                                  int err_msg_size)
{
    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    /* Process and system state */
    wtmlib_ProcAndSysState_t ps_state;
    int64_t tsc_range_length = -1;
    bool is_monotonic = false;
    int ret = 0;

    WTMLIB_OUT( "Evaluating TSC reliability (the required data is collected using "
                "a method of \"CAS-ordered probes\")...\n");
    wtmlib_InitProcAndSysState( &ps_state);
    ret = wtmlib_GetProcAndSystemState( &ps_state, local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't obtain details of the "
                         "system and process state: %s", local_err_msg);

        goto eval_tsc_reliability_cop_out;
    }

    ret = wtmlib_CalcTSCEnclosingRangeCOP( ps_state.num_cpus, ps_state.initial_cpu,
                                           ps_state.initial_cpu_set,
                                           ps_state.cline_size, &tsc_range_length,
                                           local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while calculating enclosing "
                         "TSC range: %s", local_err_msg);

        goto eval_tsc_reliability_cop_out;
    }

    ret = wtmlib_EvalTSCMonotonicityCOP( ps_state.num_cpus, ps_state.initial_cpu_set,
                                         ps_state.cline_size, &is_monotonic,
                                         local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while evaluating TSC monotonicity"
                         ": %s", local_err_msg);

        goto eval_tsc_reliability_cop_out;
    }

    if ( tsc_range_length_ret ) *tsc_range_length_ret = tsc_range_length;

    if ( is_monotonic_ret) *is_monotonic_ret = is_monotonic;

eval_tsc_reliability_cop_out:
    wtmlib_DeallocProcAndSysState( &ps_state);

    return ret;
}

/**
 * Calculate delta in nanoseconds between two timespec values
 */
static int wtmlib_CalcDeltaInNsecs( const struct timespec *start_time,
                                    const struct timespec *end_time,
                                    uint64_t *delta,
                                    char *err_msg,
                                    int err_msg_size)
{
    WTMLIB_ASSERT( start_time && end_time);
    WTMLIB_ASSERT( start_time->tv_nsec < 1000000000 && end_time->tv_nsec < 1000000000);

    if ( start_time->tv_sec > end_time->tv_sec )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Start time has %lu seconds while "
                         "end time has a smaller amount of seconds (%lu)",
                         start_time->tv_sec, end_time->tv_sec);

        return WTMLIB_RET_GENERIC_ERR;
    }

    uint64_t num_nsecs = 0;
    uint64_t num_secs = end_time->tv_sec - start_time->tv_sec;

    if ( !num_secs && start_time->tv_nsec > end_time->tv_nsec )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Start and end time have equal "
                         "amounts of seconds but start time has more nanoseconds "
                         "(%lu > %lu)", start_time->tv_nsec, end_time->tv_nsec);

        return WTMLIB_RET_GENERIC_ERR;
    }

    if ( start_time->tv_nsec > end_time->tv_nsec )
    {
        num_secs--;
        num_nsecs = 1000000000 - start_time->tv_nsec + end_time->tv_nsec;
    } else
    {
        num_nsecs = end_time->tv_nsec - start_time->tv_nsec;
    }

    if ( UINT64_MAX / 1000000000 < num_secs
         || UINT64_MAX - num_nsecs < num_secs * 1000000000 )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "The resulting number of nanoseconds "
                         "exceeds the maximum value of %lu", UINT64_MAX);

        return WTMLIB_RET_GENERIC_ERR;
    }

    if ( delta ) *delta = (num_secs * 1000000000) + num_nsecs;

    return 0;
}

/**
 * Calculate how TSC changes during a second
 *
 * At first, it's measured how TSC changes during the specified period of time.
 * Then TSC-ticks-per-second is calculated based on the measured value
 */
static int wtmlib_CalcTSCCountPerSecond( uint64_t time_period_usecs,
                                         uint64_t *tsc_count,
                                         char *err_msg,
                                         int err_msg_size)
{
    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    struct timespec start_time = {.tv_sec = 0, .tv_nsec = 0};
    struct timespec end_time = {.tv_sec = 0, .tv_nsec = 0};
    const char *getting_time_failed_msg = "A call to 'clock_gettime()' failed: ";
    uint64_t elapsed_nsecs = 0;
    uint64_t end_tsc_val = 0;
    /* We first measure the start time and then start TSC value. The end values of
       time and TSC must be measured in the same order. That's because there exists
       a time gap between the start measurements. We don't know the value of this gap
       but we can - at least partially - compensate for the gap. We expect that the
       gap will be more or less the same each time we collect time and TSC values in
       the fixed order.
       Also we ensure that TSC and time values are measured one right after another.
       There must be no other operations in-between. E.g. we check the return value
       of 'clock_gettime()' only after the corresponding TSC value is measured */
    int ret = clock_gettime( CLOCK_MONOTONIC_RAW, &start_time);
    uint64_t start_tsc_val = WTMLIB_GET_TSC();

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "%s: %s", getting_time_failed_msg,
                         WTMLIB_STRERROR_R( local_err_msg, sizeof( local_err_msg)));

        return WTMLIB_RET_GENERIC_ERR;
    }

    do
    {
        ret = clock_gettime( CLOCK_MONOTONIC_RAW, &end_time);
        end_tsc_val = WTMLIB_GET_TSC();

        if ( ret )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "%s: %s", getting_time_failed_msg,
                             WTMLIB_STRERROR_R( local_err_msg, sizeof( local_err_msg)));

            return WTMLIB_RET_GENERIC_ERR;
        }

        ret = wtmlib_CalcDeltaInNsecs( &start_time, &end_time, &elapsed_nsecs,
                                       local_err_msg, sizeof( local_err_msg));

        if ( ret )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while calculating difference "
                             "between system time values: %s", local_err_msg);

            return WTMLIB_RET_GENERIC_ERR;
        }
    } while ( elapsed_nsecs < time_period_usecs * 1000 );

    /* Possibly TSC wrap has happened. But we don't guess here, just report
       the observed inconsistency as an error */
    if ( start_tsc_val >= end_tsc_val )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "End TSC value (%lu) is smaller then or "
                         "equal to start TSC value (%lu). TSC wrap might has happened",
                         end_tsc_val, start_tsc_val);

        return WTMLIB_RET_TSC_INCONSISTENCY;
    }

    if ( UINT64_MAX / 1000000000 < end_tsc_val - start_tsc_val )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Difference between end and start TSC "
                         "values is too big (%lu)", end_tsc_val - start_tsc_val);

        /* Well, the difference may be big not only because of TSC inconsistency but also
           because the elapsed time period is indeed very long. Nevertheless, we assume
           here that configuration parameters of the library are within "sane" bounds */
        return WTMLIB_RET_TSC_INCONSISTENCY;
    }

    if ( tsc_count ) *tsc_count = (end_tsc_val - start_tsc_val) * 1000000000 /
                                  elapsed_nsecs;

    return 0;
}

/**
 * Given a series of TSC-per-sec values and using some basic statistics concepts,
 * calculate a single TSC-per-sec value which would be free from random "noise"
 *
 * Relating TSC changes to system time changes is tricky because TSC and system time
 * cannot be measured simultaneously (at least on the same CPU). Some factors that can
 * affect the time gap between the measurements are: system call overhead, interrupts,
 * migration to a different CPU, and context switches. Here we assume that measurement
 * errors caused by these negative effects have normal distribution. Based on this
 * assumption, we first filter out statistical ouliers and then caclulate an average of
 * the remaining values. This method is borrowed from FIO (where it is used exactly in
 * the same way at the moment of writing).
 *
 * There are some reasons to expect that errors of the measurements are indeed random:
 *    this is how we do measurements (at conceptual level):
 *       1) measure system time
 *       2) measure TSC
 *       3) wait for a specified period of time
 *       4) measure system time
 *       5) measure TSC
 *    I.e. end system time and TSC values are measured in the same order as start
 *    system time and TSC values. Ideally, we would like to measure system time and
 *    TSC simultaneously. But in practice there exists a delay between these two
 *    measurements. In most cases this delay will be caused by the system call
 *    overhead. We expect that the named overhead will be more or less the same in
 *    both cases: while measuring start values and also while measuring end values.
 *    Thus, if we use the ordering shown above, then we can expect that time period
 *    between start and end TSC measurements will be more or less the same as time
 *    period between start and end system time measurements. Ideally, it would be
 *    exactly the same. But in practice, ratio of these two time periods can move to
 *    both sides from "one": sometimes one period will be longer and sometimes the
 *    other period will be longer. We expect these deviations from the mean value to
 *    be more or less random. All other negative effects (like interrupts and CPU
 *    migrations) can also move the ratio to both sides unpredictably and, thus, are
 *    also considered random.
 *
 * Alternatively, we could take the measurements in the following way:
 *       1) measure system time
 *       2) measure TSC
 *       3) wait for a specified period of time
 *       4) measure TSC
 *       5) measure system time
 *    Compared to the first approach, the ordering of the last two operations is
 *    reverted here. With this approach, one needs to hold several rounds of
 *    measurements and identify a round for which
 *    (TSC_end - TSC_start)/(time_end - time_start) is the biggest. Such a round is
 *    basically "the best". One can expect that during this round the non-simultaneity
 *    of TSC and system time measurements was affected only by THE MINIMAL POSSIBLE
 *    overhead of the system calls. Thus, to complete establishing "TSC<->system time"
 *    relation one needs to deal somehow with "the minimal possible overhead" of the
 *    time-measuring system calls. That could be done in - at least - two ways:
 *       1) wait for a sufficiently long period of time between start and end
 *          measurements (so that system call overhead becomes negligible compared to
 *          the overall duration of the experiment)
 *       2) calculate "the minimal possible overhead" somehow
 */
static int wtmlib_CalcFreeFromNoiseTSCPerSec( uint64_t *tsc_per_sec,
                                              uint64_t num_samples,
                                              uint64_t *tsc_per_sec_ret,
                                              char *err_msg,
                                              int err_msg_size)
{
    WTMLIB_ASSERT( tsc_per_sec && num_samples);
    WTMLIB_OUT( "\t\"Cleaning\" collected TSC-per-second values from random noise\n");

    /* Calculate "mean" and "standard deviation" of TSC-per-second observable value
       We use incremental formulas for computing both. Classical formulas are less
       stable. E.g. classical formula for calculating "mean" suffers from the
       necessity to summ up all the data points. That can result in overflow
       (especially when data set is large). Though, we need to admit that overflow
       is very unlikely in our case. Because to collect a lot of data points one
       needs to spend a lot of time measuring time intervals. Which not a very good
       use case for the library */
    double mean = 0.0, S = 0.0, delta = 0.0;

    for ( uint64_t i = 0; i < num_samples; i++ )
    {
        delta = tsc_per_sec[i] - mean;
        mean += delta / (i + 1.0);
        S += delta * (tsc_per_sec[i] - mean);
    }

    double sigma = 0.0;
    uint64_t max_sample = 0, min_sample = UINT64_MAX;
    uint64_t num_good_samples = 0, average = 0;

    /* We use "corrected sample standard deviation" here, and thus, "S" is divided
       not by the number of samples but by the number of samples minus 1 */
    sigma = num_samples > 1 ? sqrt( S / (num_samples - 1.0)) : sqrt( S);

    /* Find minimum and maximum samples */
    for ( uint64_t i = 0; i < num_samples; i++ )
    {
        max_sample = tsc_per_sec[i] > max_sample ? tsc_per_sec[i] : max_sample;
        min_sample = tsc_per_sec[i] < min_sample ? tsc_per_sec[i] : min_sample;
    }

    /* Filter out statistical outliers and calculate an average */
    for ( uint64_t i = 0; i < num_samples; i++ )
    {
        if ( ABS_DIFF( (double)tsc_per_sec[i], mean) > sigma ) continue;

        num_good_samples++;

        /* Samples can be pretty big (though, it's very-very unlikely). We don't want to
           get overflow while calculating their cumulative summ. That's why we summ up not
           them but their distances from the minimum sample */
        /* Still, check that we will not get overflow... */
        if ( UINT64_MAX - average < tsc_per_sec[i] - min_sample )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Got overflow while calculating an "
                             "average of \"good\" samples");

            return WTMLIB_RET_GENERIC_ERR;
        }

        average += (tsc_per_sec[i] - min_sample);
    }

    average /= num_good_samples;
    /* Take into account that the cumulative summ was "shifted" by
       "num_good_samples * min_sample"
       Cannot get overflow here (an average cannot be bigger than the maximum sample) */
    average += min_sample;
    WTMLIB_OUT( "\t\tMinimum sample: %lu, maximum sample: %lu\n", min_sample,
                max_sample);
    WTMLIB_OUT( "\t\tMean: %f, corrected sample standard deviation: %f\n",
                mean, sigma);
    WTMLIB_OUT( "\t\tAverage \"cleaned\" from statistical noise: %lu\n", average);

    if ( tsc_per_sec_ret ) *tsc_per_sec_ret = average;

    return 0;
}

/**
 * Calculate parameters that aid in converting measured TSC ticks into nanoseconds. Given
 * these parameters the TSC ticks can be converted into human-friendly time format using
 * fast division-free integer arithmetic
 *
 * Let us use an example to explain how time measured in TSC ticks is converted into
 * nanoseconds inside this library (the procedure described below is borrowed from FIO).
 *
 * Ideally, we'd like to do the following: ns_time = tsc_ticks / tsc_per_ns
 * Also we'd like to use only integer arithmetic to keep conversion overhead low. The
 * lower is the overhead the less it affects valuable computations.
 * If tsc_per_ns = 3, then simple integer division works fine: ns_time = tsc_ticks / 3
 * looks good. But what if tsc_per_ns = 3.333? The accuracy will be really poor if 3.333
 * is rounded down to 3.
 * We can mitigate this problem in the following way:
 *      ns_time = (tsc_ticks * factor) / (3.333 * factor)
 * If "factor" is "big enough" then accuracy must be good. What is not really good though,
 * is conversion overhead. Integer division is a pretty expensive operation (it takes 10+
 * clocks on x86 CPUs at the moment of writing; even worse, integer division operations
 * may not be pipelined in some cases).
 * Let's rewrite our expression in an equivalent form:
 *      ns_time = (tsc_ticks * factor / 3.333) / factor
 * First division is not a problem here. We can precompute (factor / 3.333) in advance.
 * But the second division is still pain. To deal with it, let's choose "factor" to be
 * equal to a power of 2. After that we can replace the last division by bitwise shift
 * which is really fast. Must work well as long as "factor" is "big enough".
 * But the problem is "factor" cannot be arbitrarily big. Namely, multiplication in the
 * numerator must not overflow 64-bit integer type (we want to stay using built-in types
 * because using wide arithmetic would negatively affect performance).
 * Let's see how big "factor" can be in our example. Assume that we want our conversion
 * to be valid for time periods up to 1 year. There is the following amount of TSC ticks
 * in 1 year: 3.333 * 1000000000 * 60 * 60 * 24 * 365 = 105109488000000000. Dividing the
 * maximum 64-bit value by this number, we get:
 * 18446744073709551615 / 105109488000000000 ~ 175.5. Thus, (factor / 3.333) cannot be
 * bigger than this value: factor <= 175.5 * 3.333 ~ 584.9. The biggest power of 2 that
 * doesn't exceed this value is 512. Hence, our conversion formula takes the form:
 *     ns_time = (tsc_ticks * 512 / 3.333) / 512
 * Remember, that we want to keep (512 / 3.333) precomputed. Taking that into account:
 *     ns_time = tsc_ticks * 153 / 512
 * Ok, let's evaluate how accurate this conversion formula is. There are
 * 1000000000 * 60 * 60 * 24 * 365 = 31536000000000000 nanoseconds in 1 year. While our
 * formula gives 105109488000000000 * 153 / 512 = 31409671218750000 nanoseconds. The
 * difference with the actual value is 126328781250000 nanoseconds which is
 * 126328781250000 / 1000000000 / 60 / 60 ~ 35 hours.
 * The error is pretty big. We want to do better.
 * What if we don't need to measure time periods longer than an hour? How big can "factor"
 * be in this case? Number of TSC ticks in one hour:
 * 3.333 * 1000000000 * 60 * 60 = 11998800000000. Dividing the maximum 64-bit value by
 * this number we get: 18446744073709551615 / 11998800000000 ~ 1537382.4. Thus,
 * factor <= 1537382.4 * 3.333 ~ 5124095.5. The biggest power of 2 that doesn't exceed
 * this value is 4194304. Hence, the conversion formula takes the form:
 *    ns_time = (tsc_ticks * 4194304 / 3.333) / 4194304 = tsc_ticks * 1258417 / 4194304
 * Let's evaluate its accuracy. There are 1000000000 * 60 * 60 = 3600000000000
 * nanoseconds in 1 hour. And our formula gives 11998800000000 * 1258417 / 4194304 =
 * = 3599999880695 nanoseconds. The absolute error is just 119305 nanoseconds (less
 * than 0.2 milliseconds per hour). Which is really good.
 * Now we can notice the following:
 *      tsc_ticks = (tsc_ticks_per_1_hour * number_of_hours) + tsc_ticks_remainder
 * If we pre-calculate tsc_ticks_per_1_hour than we will be able to extract
 * number_of_hours from tsc_ticks. Next, we know how many nanoseconds are in 1 hour. Thus,
 * to complete conversion of tsc_ticks to nanoseconds it remains only to convert
 * tsc_ticks_remainder to nanoseconds. To do that we can use the procedure described
 * above. We know that the conversion error will be really small (since
 * tsc_ticks_remainder represents a time period shorter than 1 hour).
 * This is the conversion mechanism that we're really going to use. Let's now generalize
 * and optimize the whole procedure. First of all, we'd like to have a flexible control
 * over the conversion error. Hence, we don't stick with 1 hour when calculating the
 * conversion parameters but use a configurable time period. We call this period "time
 * conversion modulus" (by analogy with modular arithmetic).
 *      tsc_ticks = (tsc_ticks_per_time_conversion_modulus * number_of_moduli_periods) +
 *                  + tsc_ticks_remainder
 * To convert tsc_ticks_remainder to nanoseconds we'll use the already familiar formula:
 *      ns_per_remainder = (tsc_ticks_remainder * factor / tsc_per_nsec) / factor
 * tsc_ticks_per_time_conversion_modulus * (factor / tsc_per_nsec) <= UINT64_MAX
 * factor <= (UINT64_MAX / tsc_ticks_per_time_conversion_modulus) * tsc_per_nsec
 * We choose the largest "shift" that satisfies the inequality:
 * 2 ^ shift <= (UINT64_MAX / tsc_ticks_per_time_conversion_modulus) * tsc_per_nsec
 * Now:
 *      factor = 2 ^ shift
 * Next, we precompute the "multiplier":
 *      mult = factor / tsc_per_nsec
 * After that the nanosecond worth of tsc_ticks_remainder can be calculated as follows:
 *      ns_per_remainder = (tsc_ticks_remainder * mult) >> shift
 * NOTE: in the code below instead of tsc_per_nsec we use (tsc_per_sec / 1000000000).
 *       E.g. mult = factor * 1000000000 / tsc_per_sec
 * NOTE: ideally we need the largest "factor" satisfying the following inequality:
 *       tsc_ticks_per_time_conversion_modulus * (factor / tsc_per_nsec) <= UINT64_MAX.
 *       But in reality - as you can see above - we choose the largest value satisfying:
 *       factor <= (UINT64_MAX / tsc_ticks_per_time_conversion_modulus) * tsc_per_nsec
 *       In integer arithmetic these inequalities are not equivalent. And the largest
 *       value satisfying the first inequality can be bigger than the largest value
 *       satisfying the second inequality. Though it's not clear whether this effect
 *       can affect the final result of our calculations: we need not just the largest
 *       value satisfying the inequality but the largest power of 2, also tsc_per_nsec is
 *       not arbitrary but (tsc_per_sec / 1000000000).
 *       Anyway, we don't care of that for now. Even if we choose "factor" 2 times smaller
 *       than the maximum allowed, that shouldn't be a problem as long as the value still
 *       remains "big enough" (which must be true if "time conversion modulus" stays in
 *       reasonable bounds).
 * The next problem to solve is extraction of tsc_ticks_remainder and
 * number_of_moduli_periods from tsc_ticks. Again, we'd like to do that fast. To avoid
 * division, instead of using tsc_ticks_per_time_conversion_modulus, we will use the
 * largest power of 2 that doesn't exceed tsc_ticks_per_time_conversion_modulus. We call
 * this value "TSC modulus". Thus:
 *      tsc_ticks = (tsc_modulus * number_of_tsc_moduli_periods) + tsc_modulus_remainder
 * Since tsc_modulus is a power of 2, extraction of number_of_tsc_moduli_periods and
 * tsc_modulus_remainder from tsc_ticks can be done easily: by doing a bit shift and
 * applying a bit mask. The power of 2 used to produce tsc_modulus is effectively a bit
 * length of tsc_modulus_remainder. Thus:
 *      tsc_modulus = 2 ^ tsc_modulus_remainder_bit_length
 *      number_of_tsc_moduli_periods = tsc_ticks >> tsc_modulus_remainder_bit_length
 *      tsc_modulus_remainder = tsc_ticks & (tsc_modulus - 1)
 * We already know how to convert tsc_modulus_remainder to nanoseconds. To convert TSC
 * moduli's worth of ticks to nanoseconds we do:
 *      ns_per_moduli = ns_per_tsc_modulus * number_of_tsc_moduli_periods
 * Since ns_per_tsc_modulus represents a time period which is not longer than "time
 * conversion modulus", then we can pre-calculate this value using the same formula as we
 * use for converting tsc_modulus_remainder to nanoseconds:
 *      ns_per_tsc_modulus = (tsc_modulus * mult) >> shift
 * That's it. Now we've fully described our two-stage conversion procedure. In summary,
 * the procedure is:
 *      1) convert "tsc moduli's" worth of ticks to nanoseconds using simple
 *         multiplication
 *      2) convert the remaining ticks using multiply-shift arithmetic
 *      3) summ up the results of the above conversions
 * NOTE: in the code below we use "time conversion modulus" to calculate "mult" and
 *       "shift". Instead, we could use "TSC modulus" and get slightly better accuracy in
 *       some cases (because "TSC modulus" corresponds to a time period which is not
 *       longer than "time conversion modulus"). But we don't do that. We want the
 *       accuracy to be driven by easily understood "time conversion modulus" which is
 *       measured in human-friendly seconds.
 */
static int wtmlib_CalcTSCToNsecConversionParams( uint64_t tsc_per_sec,
                                                 wtmlib_TSCConversionParams_t
                                                    *conv_params_ret,
                                                 char *err_msg,
                                                 int err_msg_size)
{
    WTMLIB_OUT( "\tCalculating TSC-to-nanoseconds conversion parameters...\n");

    if ( UINT64_MAX / WTMLIB_TIME_CONVERSION_MODULUS < tsc_per_sec )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Configured time conversion modulus is "
                         "too big. TSC worth of this period doesn't fit 64-bit cell");

        return WTMLIB_RET_GENERIC_ERR;
    }

    uint64_t tsc_worth_of_modulus = WTMLIB_TIME_CONVERSION_MODULUS * tsc_per_sec;
    uint64_t mult_bound = UINT64_MAX / tsc_worth_of_modulus;
    /* Multiplication here will not produce overflow because tsc_per_sec is smaller than
       tsc_worth_of_modulus */
    uint64_t factor_bound = mult_bound * tsc_per_sec / 1000000000;
    int shift = 0;

    /* Find "factor": the largest power of 2 that does not exceed factor_bound */
    while ( factor_bound > 1 )
    {
        factor_bound >>= 1;
        shift++;
    }

    uint64_t factor = 1ull << shift;
    /* Cannot get overflow here. By calculation this value is smaller than mult_bound */
    uint64_t mult = factor * 1000000000 / tsc_per_sec;

    WTMLIB_OUT( "\t\tShift: %d, multiplicator: %lu\n", shift, mult);

    /* Find the largest power of 2 that doesn't exceed tsc_worth_of_modulus. This number
       will play a role of "time conversion modulus" but in terms of TSC
       (WTMLIB_TIME_CONVERSION_MODULUS is measured in seconds) */
    int tsc_remainder_length = 0;

    while ( (tsc_worth_of_modulus >> tsc_remainder_length) > 1 )
    {
        tsc_remainder_length++;
    }

    WTMLIB_OUT( "\t\tLength of TSC remainder in bits: %d\n", tsc_remainder_length);

    uint64_t tsc_modulus = 1ull << tsc_remainder_length;
    /* Here we could use (tsc_modulus * 1000000000) / tsc_per_sec. But in this case the
       nanosecond worth of the last tick of every TSC modulus period would be excessively
       high compared to the worth of any other tick inside the same period. Though, seems
       that overall accuracy would be slightly better.
       Current decision is to keep accuracy at the same level for all measurements
       (instead of "recovering" it a little bit after each TSC modulus period). In this
       case the nanosecond worth of very similar TSC ranges will always be consistent.
       Hence, we calculate the nanosecond worth of TSC modulus period using exactly the
       same formula as we use to calculate the nanosecond worth of TSC remainder */
    uint64_t nsecs_per_tsc_modulus = (tsc_modulus * mult) >> shift;
    /* Calculate a bitmask used to extract TSC remainder. Applying this bitmask is the
       same as doing (tsc_ticks % tsc_modulus) */
    uint64_t tsc_remainder_bitmask = tsc_modulus - 1;

    WTMLIB_OUT( "\t\tTSC modulus: %lu\n", tsc_modulus);
    WTMLIB_OUT( "\t\tNanoseconds per TSC modulus: %lu\n", nsecs_per_tsc_modulus);
    WTMLIB_OUT( "\t\tBitmask to extract TSC remainder: %016lx\n", tsc_remainder_bitmask);

    if ( conv_params_ret )
    {
        conv_params_ret->mult = mult;
        conv_params_ret->shift = shift;
        conv_params_ret->nsecs_per_tsc_modulus = nsecs_per_tsc_modulus;
        conv_params_ret->tsc_remainder_length = tsc_remainder_length;
        conv_params_ret->tsc_remainder_bitmask = tsc_remainder_bitmask;
    }

    return 0;
}

/**
 * Calculate time (in seconds!) before the earliest TSC wrap
 *
 * All available CPUs are considered when calculating the time
 */
static int wtmlib_CalcTimeBeforeTSCWrap( wtmlib_TSCConversionParams_t *conv_params,
                                         uint64_t *secs_before_wrap_ret,
                                         char *err_msg,
                                         int err_msg_size)
{
    wtmlib_ProcAndSysState_t ps_state;
    uint64_t max_tsc_val = 0, curr_tsc_val = 0;
    uint64_t secs_before_wrap = 0;
    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";
    int ret = 0;

    WTMLIB_ASSERT( conv_params);
    WTMLIB_OUT( "\tCalculating time before the earliest TSC wrap...\n");
    wtmlib_InitProcAndSysState( &ps_state);
    
    if ( wtmlib_GetProcAndSystemState( &ps_state, local_err_msg, sizeof( local_err_msg)) )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't obtain details of the "
                         "system and process state: %s", local_err_msg);

        return WTMLIB_RET_GENERIC_ERR; 
    }

    int cpu_id = -1;
    pthread_t thread_self = pthread_self();
    int cpu_set_size = CPU_ALLOC_SIZE( ps_state.num_cpus);
    cpu_set_t *cpu_set = CPU_ALLOC( ps_state.num_cpus);

    if ( !cpu_set )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory for the "
                         "CPU set");

        return WTMLIB_RET_GENERIC_ERR;
    }

    CPU_ZERO_S( cpu_set_size, cpu_set);

    for ( cpu_id = 0; cpu_id < ps_state.num_cpus; cpu_id++ )
    {
        if ( !CPU_ISSET_S( cpu_id, cpu_set_size, ps_state.initial_cpu_set) ) continue;

        CPU_SET_S( cpu_id, cpu_set_size, cpu_set);
        
        if ( pthread_setaffinity_np( thread_self, cpu_set_size, cpu_set) )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't change CPU affinity of "
                             "the current thread: %s", WTMLIB_STRERROR_R( local_err_msg,
                             sizeof( local_err_msg)));

            break;
        }

        curr_tsc_val = WTMLIB_GET_TSC();
        WTMLIB_OUT( "\t\tTSC on CPU %d: %lu\n", cpu_id, curr_tsc_val);

        if ( curr_tsc_val > max_tsc_val ) max_tsc_val = curr_tsc_val;

        /* Return CPU mask to the "clean" state */
        CPU_CLR_S( cpu_id, cpu_set_size, cpu_set);
    }

    CPU_FREE( cpu_set);

    if ( cpu_id < ps_state.num_cpus ) return WTMLIB_RET_GENERIC_ERR;

    WTMLIB_OUT( "\t\tThe maximum TSC value: %lu\n", max_tsc_val);
    secs_before_wrap = WTMLIB_TSC_TO_NSEC( UINT64_MAX - max_tsc_val, conv_params) /
                       1000000000;
    WTMLIB_OUT( "\t\tSeconds before the maximum TSC will wrap: %lu\n", secs_before_wrap);
    ret = wtmlib_RestoreInitialProcState( &ps_state, local_err_msg,
                                          sizeof( local_err_msg));

    if ( ret )
    {
        /* This error is not critical. But we do treat it as critical for now. See
           the detailed comment to the identical condition inside
           "wtmlib_EvalTSCReliabilityCPUSW()" function */
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't restore initial state of "
                         "the current process: %s", local_err_msg);

        return WTMLIB_RET_GENERIC_ERR;
    }

    if ( secs_before_wrap_ret ) *secs_before_wrap_ret = secs_before_wrap;

    return 0;
}

/**
 * Calculate parameters used to convert TSC ticks into nanoseconds. Also
 * calculate time remaining before the earliest TSC wrap
 */
int wtmlib_GetTSCToNsecConversionParams( wtmlib_TSCConversionParams_t *conv_params_ret,
                                         uint64_t *secs_before_wrap_ret,
                                         char *err_msg,
                                         int err_msg_size)
{
    int ret = 0;
    uint64_t tsc_per_sec_golden = 0;
    uint64_t secs_before_wrap = 0;
    wtmlib_TSCConversionParams_t conv_params = {.mult = 0, .shift = 0,
                                                .nsecs_per_tsc_modulus = 0,
                                                .tsc_remainder_length = -1,
                                                .tsc_remainder_bitmask = 0};
    char local_err_msg[WTMLIB_MAX_ERR_MSG_SIZE] = "";

    WTMLIB_OUT( "Calculating TSC-to-nanoseconds conversion parameters...\n");

    /* Allocate array to keep tsc-per-second values calculated in different experiments */
    uint64_t *tsc_per_sec = (uint64_t*)calloc( sizeof( uint64_t),
                                               WTMLIB_TSC_PER_SEC_SAMPLE_COUNT);

    if ( !tsc_per_sec )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Couldn't allocate memory to keep "
                         "tsc-per-second values");
        ret = WTMLIB_RET_GENERIC_ERR;

        goto calc_tsc_to_nsec_conversion_params_out;
    }

    WTMLIB_OUT( "\tCalculating how TSC changes during a second-long time period\n");

    for ( uint64_t i = 0; i < WTMLIB_TSC_PER_SEC_SAMPLE_COUNT; i++ )
    {
        ret = wtmlib_CalcTSCCountPerSecond( WTMLIB_TIME_PERIOD_TO_MATCH_WITH_TSC,
                                            &tsc_per_sec[i], local_err_msg,
                                            sizeof( local_err_msg));

        if ( ret )
        {
            WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while calculating TSC worth "
                             "of a second: %s", local_err_msg);

            goto calc_tsc_to_nsec_conversion_params_out;
        }

        WTMLIB_OUT( "\t\t[Measurement %lu] TSC ticks per sec: %lu\n", i,
                    tsc_per_sec[i]);
    }

    ret = wtmlib_CalcFreeFromNoiseTSCPerSec( tsc_per_sec,
                                             WTMLIB_TSC_PER_SEC_SAMPLE_COUNT,
                                             &tsc_per_sec_golden, local_err_msg,
                                             sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while \"cleaning\" TSC-per-second "
                         "samples from random noise: %s", local_err_msg);

        goto calc_tsc_to_nsec_conversion_params_out;
    }

    ret = wtmlib_CalcTSCToNsecConversionParams( tsc_per_sec_golden, &conv_params,
                                                local_err_msg, sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while calculating TSC-to-"
                         "nanoseconds conversion parameters: %s", local_err_msg);

        goto calc_tsc_to_nsec_conversion_params_out;
    }

    ret = wtmlib_CalcTimeBeforeTSCWrap( &conv_params, &secs_before_wrap, local_err_msg,
                                        sizeof( local_err_msg));

    if ( ret )
    {
        WTMLIB_BUFF_MSG( err_msg, err_msg_size, "Error while calculating time "
                         "before the earliest TSC wrap: %s", local_err_msg);

        goto calc_tsc_to_nsec_conversion_params_out;
    }

    if ( conv_params_ret ) *conv_params_ret = conv_params;

    if ( secs_before_wrap_ret ) *secs_before_wrap_ret = secs_before_wrap;

calc_tsc_to_nsec_conversion_params_out:
    if ( tsc_per_sec ) free( tsc_per_sec);

    return ret;
}
