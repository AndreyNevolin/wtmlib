/**
 * Copyright © 2018 Andrey Nevolin, https://github.com/AndreyNevolin
 * Twitter: @Andrey_Nevolin
 * LinkedIn: https://www.linkedin.com/in/andrey-nevolin-76387328
 *
 * WTMLIB: a library for taking Wall-clock Time Measurements
 *
 * Header file of the library. Contains all external declarations
 */

#include <pthread.h>

/**
 * Get time-stamp counter (TSC)
 */
#ifdef __GNUC__
/*
   A separate implementation is provided for GNU environment, because in this environment
   the required functionality can be implemented as a macro. Using the macro gives a
   guarantee that there will be no unintended overhead in reading TSC value.

   For other environments the same code is put inside "static inline" function. Though
   it's HIGHLY probable that a compiler will make this code inline at the point of use,
   it is not strictly guaranteed.

   We could define a macro that would fit any environment. But the resulting macro would
   look strange from client perspective, because it would require arguments. So, for now
   the decision is to have a macro for GNU and a "static inline" function for all other
   environments.

   Need to think about introducing "reordering-safe" version of the macro. At the moment
   of writing GCC didn't provide any absolute non-reordering guarantees for ASM
   statements. Reordering must be controlled by specifying explicit dependencies (like
   "I can modify an arbitrary memory location").
   Without proper dependencies in place ASM statements can be potentially reordered with
   (almost) any surrounding code (including other ASM statements).
   Current version of the macro (that doesn't have reordering-blocking dependencies) can
   be protected from reordering by using various types of barriers around it in the client
   code. But that should be done with caution. Some direct approaches may not just work as
   expected. E.g. another ASM statement with some "barrier" instruction inside may not
   work, because it can be potentially reordered with TSC-retrival ASM statement (again,
   unless proper dependencies are in place). C-language-supported barriers will work best
*/
#define WTMLIB_GET_TSC()                                          \
    ({                                                            \
        uint32_t _eax, _edx;                                      \
                                                                  \
        __asm__ __volatile__( "rdtsc": "=a" (_eax), "=d" (_edx)); \
        ((uint64_t)_edx << 32U) | _eax;                           \
    })
#else /* __GNUC__ */
static inline uint64_t WTMLIB_GET_TSC() {
    uint32_t eax, edx;

    __asm__ __volatile__( "rdtsc" : "=a" (eax), "=d" (edx));

    return ((uint64_t)edx << 32) | eax;
}
#endif /* __GNUC__ */

/**
 * A set of parameters used to convert TSC ticks into nanoseconds in a fast and
 * accurate way
 */
typedef struct
{
    /* A multiplier: (tsc_remainder * mult) */
    uint64_t mult;
    /* A shift: nsecs_per_tsc_remainder = (tsc_remainder * mult) >> shift */
    int shift;
    /* Number of nanoseconds per TSC modulus: nsecs_per_tsc_moduli =
       = (tsc_ticks >> tsc_remainder_length) * nsecs_per_tsc_modulus */
    uint64_t nsecs_per_tsc_modulus;
    /* Length of TSC remainder in bits. Used to calculate the quotient of TSC ticks
       divided by TSC modulo: (tsc_ticks >> tsc_remainder_length) */
    int tsc_remainder_length;
    /* A bitmask used to extract TSC remainder:
       tsc_remainder = tsc_ticks & tsc_remainder_bitmask */
    uint64_t tsc_remainder_bitmask;
} wtmlib_TSCConversionParams_t;

/**
 * Convert TSC ticks to nanoseconds
 *
 * A pointer to a structure with the conversion parameters must be provided as the
 * second argument
 *
 * REALLY IMPORTANT: for the conversion to be fast, it must be ensured that the
 *                   structure with the conversion parameters always stays in cache
 */
#define WTMLIB_TSC_TO_NSEC( tsc_ticks_, cp_)                                          \
    (((tsc_ticks_) >> (cp_)->tsc_remainder_length) * ((cp_)->nsecs_per_tsc_modulus)   \
     + ((((tsc_ticks_) & ((cp_)->tsc_remainder_bitmask)) *                            \
      ((cp_)->mult)) >> (cp_)->shift))

/*
    Maximum size of human-readable error messages returned by the library functions
 */
#define WTMLIB_MAX_ERR_MSG_SIZE 2000

/*
   Macro that evaluates to 1 if the library should use non-negative return values. In
   the opposite case the macro evaluates to -1.
   The library takes care of its return values' sign to avoid interference with special
   return values. Only one such 'special value' is relevant for now. Namely,
   PTHREAD_CANCELED
*/
#define WTMLIB_RET_SIGN ((long)PTHREAD_CANCELED < 0 ? 1 : -1)
/* Return value that indicates generic error */
#define WTMLIB_RET_GENERIC_ERR (WTMLIB_RET_SIGN * 1)
/*
   Return value indicating that major TSC inconsistency was detected

   Getting this return value doesn't necessarily imply that TSC is unreliable and cannot
   be used as a wall-clock time measure. This type of error in some cases may be caused
   by TSC wrap (which could has happened on some CPU in the middle of calculations; or it
   might has happened on one CPU before WTMLIB was called but hasn't yet happened on the
   other available CPUs; and so on)
*/
#define WTMLIB_RET_TSC_INCONSISTENCY (WTMLIB_RET_SIGN * 2)
/*
   Return value indicating that configured statistical significance criteria were not met
   (data collected by the library doesn't contain enough specific patterns)
*/
#define WTMLIB_RET_POOR_STAT (WTMLIB_RET_SIGN * 3)

/**
 * Evaluate reliability of TSC (the required data is collected using "CPU Switching"
 * method - a single thread jumps from one CPU to another and takes all needed
 * measurements)
 *
 * Possible return codes:
 *      0 - in case of success
 *      WTMLIB_RET_TSC_INCONSISTENCY - major TSC inconsistency was detected
 *      WTMLIB_RET_GENERIC_ERR - all other errors
 *
 * Besides the regular return value the function returns (if the corresponding pointers
 * are non-zero):
 *      tsc_range_length - estimated maximum shift between TSC counters running on
 *                         different CPUs
 *      is_monotonic - whether TSC values measured successively on same or different CPUs
 *                     monotonically increase
 *      err_msg - human-readable error message
 *
 * Any of the pointer arguments can be zero.
 * In case of non-zero return code, the function doesn't modify memory referenced by
 * tsc_range_length and is_monotonic.
 * err_msg is modified only if the return code is non-zero
 *
 * NOTE: if the function sets is_monotonic to "false", that doesn't necessarily imply that
 *       TSCs are unreliable. In some cases that can be a result of TSC wrap
 */
int wtmlib_EvalTSCReliabilityCPUSW( int64_t *tsc_range_length, bool *is_monotonic,
                                    char *err_msg, int err_msg_size);

/**
 * Evaluate reliability of TSC (the required data is collected using a method of
 * CAS-Ordered Probes - the measurements are done by concurrently running threads; one per
 * each available CPU. The measurements are sequentially ordered by means of compare-and-
 * swap operation)
 *
 * Possible return codes:
 *      0 - in case of success
 *      WTMLIB_RET_TSC_INCONSISTENCY - major TSC inconsistency was detected
 *      WTMLIB_RET_POOR_STAT - configured statistical significance criteria were not met
 *                             (the number of specific patterns found in the collected
 *                             data is smaller than required by configuration parameters)
 *      WTMLIB_RET_GENERIC_ERR - all other errors
 *
 * Besides the regular return value the function returns (if the corresponding pointers
 * are non-zero):
 *      tsc_range_length - estimated maximum shift between TSC counters running on
 *                         different CPUs
 *      is_monotonic - whether TSC values measured successively on same or different CPUs
 *                     monotonically increase
 *      err_msg - human-readable error message
 *
 * Any of the pointer arguments can be zero.
 * In case of non-zero return code, the function doesn't modify memory referenced by
 * tsc_range_length and is_monotonic.
 * err_msg is modified only if the return code is non-zero
 */
int wtmlib_EvalTSCReliabilityCOP( int64_t *tsc_range_length, bool *is_monotonic,
                                  char *err_msg, int err_msg_size);

/**
 * Calculate parameters needed to perform fast and accurate conversion of TSC ticks to
 * nanoseconds. Also calculate time (in seconds) remaining before the earliest TSC wrap
 *
 * Possible return codes:
 *      0 - in case of success
 *      WTMLIB_RET_TSC_INCONSISTENCY - major TSC inconsistency was detected
 *      WTMLIB_RET_GENERIC_ERR - all other errors
 *
 * Besides the regular return value the function returns (if the corresponding pointers
 * are non-zero):
 *      conv_params - a set of TSC-to-nanoseconds conversion parameters
 *      secs_before_wrap - number of seconds remaining before the earliest TSC wrap
 *      err_msg - human-readable error message
 *
 * Any of the pointer arguments can be zero.
 * In case of non-zero return code, the function doesn't modify memory referenced by
 * conv_params and secs_before_wrap.
 * err_msg is modified only if the return code is non-zero
 */
int wtmlib_GetTSCToNsecConversionParams( wtmlib_TSCConversionParams_t *conv_params,
                                         uint64_t *secs_before_wrap_ret, char *err_msg,
                                         int err_msg_size);
