# Wall-clock Time Measurement library (WTMLIB)


## Description
The library allows measuring wall-clock time intervals with nanosecond precision
and very low overhead (also at nanosecond scale).

High precision and low overhead stem from the following design principles underlying the
library:

1. no use of system calls when measuring time intervals
2. instead, the library uses microprocessor's Time Stamp Counter (TSC) for this purpose.
The counter is accessible - in most cases - directly from user space
3. TSC ticks are converted to nanoseconds by means of fast division-free integer
arithmetic

Here is a list of major capabilities provided by the library:

1. evaluation of reliability of TSC
2. reading current TSC value
3. conversion of elapsed TSC ticks to nanoseconds

The library builds on Linux only. Only x86 architecture is supported for now. Support for
Power will follow.

## How to use
The most typical scenario is the following:

1. include WTMLIB's API declarations to the client code:
    ```#include "wtmlib.h"```
2. check whether TSC can be reliably used for measuring wall-clock time (see
[Design and implementation](#design-and-implementation) section to learn about TSC
reliability considerations):
    ```
    ret = wtmlib_EvalTSCReliabilityCOP( &tsc_max_shift, &is_monotonic, err_msg,
                                        sizeof( err_msg));
    ```

    Here:  
        - `tsc_max_shift` is a maximum estimated shift between TSC counters running on
        different CPUs  
        - `is_monotonic` indicates whether TSC values collected one after another on
        same or different CPUs monotonically increase  
        - `err_msg` is a pointer to a buffer where human-readable error message will be
        stored (if the pointer is non-zero) in case of error
3. pre-calculate parameters needed to convert TSC ticks to nanoseconds on the fly:
    ```
    ret = wtmlib_GetTSCToNsecConversionParams( &conv_params, &secs_before_wrap, err_msg,
                                               sizeof( err_msg));
    ```
    Here:  
        - `conv_params` is a structure storing the conversion parameters  
        - `secs_before_wrap` is the number of seconds remaining before the earliest TSC
        wrap. TSC counter is stored in a microprocessor register of limited width. Thus,
        its value "wraps" from time to time (starts from zero after reaching the maximum).
        It's adviced to ensure before starting actual time measurements that they can
        be completed before TSC on some of the available CPUs wraps. Another option is to
        track TSC wraps in the client code and behave accordingly
4. get TSC value at the beggining of measured time interval:
    ```
    start_tsc_val = WTMLIB_GET_TSC();
    ```
5. get TSC value at the end of measured time interval:
    ```
    end_tsc_val = WTMLIB_GET_TSC();
    ```
6. convert measured ticks to nanoseconds:
    ```
    nsecs = WTMLIB_TSC_TO_NSEC( end_tsc_val - start_tsc_val, &conv_params));
    ```

    Here:  
        - `start_tsc_val` and `end_tsc_val` are the values collected at steps 4 and
        5  
        - `conv_params` holds conversion parameters pre-calculated at step 3

NOTE: if you don't need to convert TSC ticks to nanoseconds, you may omit steps 3 and 6.
There exist - at least - two good scenarios when you don't need to convert TSC ticks to
nanoseconds on the fly:
    1. you are interested not in absolute values of time intervals, but only in how
    different intervals relate to each other
    2. "online" conversion is not a requirement. In this case, you can first measure all
    time intervals in TSC ticks (storing these ticks somewhere for further processing).
    And later - when all the measurements have already been taken - TSC ticks may be
    converted to nanoseconds without any rush by using "slow" floating-point arithmetics
    or something

See file `example.c` for the detailed examples of using all the library interfaces.

See `src/wtmlib.h` for the API signatures, parameter descriptions, error codes, and
so on.

## License
Copyright © 2018 Andrey Nevolin, https://github.com/AndreyNevolin
 * Twitter: @Andrey_Nevolin
 * LinkedIn: https://www.linkedin.com/in/andrey-nevolin-76387328
  
This software is provided under the Apache 2.0 Software license provided in
the [LICENSE.md](LICENSE.md) file
