# Wall-clock Time Measurement library (WTMLIB)


## Table of contents
- [Description](#description)
- [How to use](#how-to-use)
- [Further usage notes](#further-usage-notes)
- [Building](#building)
- [Design and implementation](#design-and-implementation)
- [License](#license)

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
    wrap. TSC counter leaves in a microprocessor register of limited width. Thus, its
    value "wraps" from time to time (starts from zero after reaching the maximum). It's
    advised to ensure before starting actual time measurements that they will be completed
    before TSC on some of the available CPUs wraps. Another option is to track TSC wraps
    in the client code and behave accordingly
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

## Further usage notes
1. When converting TSC ticks to nanoseconds on the flight, please make sure that
pre-calculated conversion parameters can be found in cache (or even better - in CPU
registers) each time they are needed. Only in this case will the conversion procedure
be really efficient
2. `WTMLIB_GET_TSC()` is not protected from reordering. Neither from reordering done
by compiler, nor from reordering done at CPU level. Currently it is client's
responsibility to ensure that `WTMLIB_GET_TSC()` is properly ordered with the surrounding
code
3. When evaluating TSC reliability and pre-calculating TSC-to-nanoseconds conversion
parameters, the library considers only CPUs that are allowed by a CPU affinity mask of a
thread from which the library was called. WTMLIB assumes that time intervals will be
measured on those CPUs only and doesn't take all other CPUs into account. To see how it
works you may do the following:
    ```
    make log
    make example
    taskset -c 1,7,13 ./example
    ```
    You will see that WTMLIB will collect data only on CPUs 1, 7, and 13 (of course, if CPUs
    with these IDs do exist in your system).

## Building
There are two recommended ways of building WTMLIB:
1. build it as a standalone shared library (`.so`) using provided `Makefile`. Then link
client code with the library in a usual way
2. copy WTMLIB's complete source code to your project and build it as part of the project.
For example, you could first compile the library into a regular object file and then
statically link this object file with other object files in your project into a single
executable (or whatever you're trying to produce).

The second way is really viable because WTMLIB is small. Just one `c` file and two
headers. If you go this way, you may borrow command lines needed to compile the library
from the provided `Makefile`.

Examples given below in this section assume that the library needs to be packaged as a
standalone `.so` file.

Several build modes are available:
1. `make` builds release version of the library
2. `make log` builds a version that prints log messages to `stdout` while running. Logging
doesn't affect performance-critical sections of the library. There can be log prints
around them but never inside. Thus, a client can trust calculation results produced by
this version
3. `make debug` builds a version with internal consistency checks enabled. Some of these
checks do can be found inside performance-critical sections of the code. This build mode
is not meant for generating trusted calculation results, but for the purpose of finding
bugs in the code
4. `make log_debug` combines `log` and `debug` modes

After building the library you may also build example code. Just run:
```
make example
```
The example doesn't require any input parameters. Simply type `./example` and watch the
output

## Design and implementation
Using Time Stamp Counters for measuring wall-clock time promises high resolution and low
performance overhead. But in some cases TSC cannot serve as a reliable time source, or
its use for this purporse may be challenging. Possible reasons for that are:
1. frequency at which TSC increments may vary in time on some systems
2. TSCs may increment at different pace on different CPUs present in the system
3. there may exist a "shift" between TSCs running on different CPUs

Let us illustrate the third concern by an example. Assume there are two CPUs in the
system: CPU1 and CPU2. Assume next that TSC on CPU1 lags behind TSC on CPU2 by an
equivalent of 5 seconds. Suppose then that some thread running on the system needs to
measure duration of its own calculations. To do that it first reads TSC, then starts the
calculations, and after they finish it reads TSC again. If the thread spends all its life
on a single CPU, then everything is ok. But what if it reads start TSC value on CPU1, and
then it is moved in the middle of computation to CPU2, and the second TSC value is read on
CPU2? In this case the calculations will appear 5 seconds longer than they actually are.

The other two concerns can be illustrated in a similar way.

One or more of the three problems listed above do can be found on some systems. And as a
consequence, it will be impossible to use TSC as a reliable time source on some of those
systems. But on other systems "suffering" from the listed problems TSC does can serve as
a reliable time source. That becomes possible because of the following CPU architectural
features:
1. the hardware may provide an interrupt to software whenever the update frequency of TSC
changes, and a means to determine what the current update frequency is. Alternatively, the
update frequency of TSC may be under control of the system software. (See "Power ISA
Version 2.06 Revision B, Book II, Chapter 5")
2. the hardware may allow reading TSC value along with the corresponding CPU ID. (See
Intel's RDTSCP instruction. "Intel 64 and IA-32 Architectures Software Developer's
Manual", Volume 2)
3. on some systems software can adjust the value of time-stamp counter of every logical
CPU. (See Intel's WRMSR instruction and IA32_TIME_STAMP_COUNTER register. "Intel® 64 and
IA-32 Architectures Software Developer's Manual", Volume 3)

There is more to learn about time-stamp counters on different architectures. For example:
1. some architectures do not guarantee that TSC will necessarily provide high resolution
2. some architectures provide direct run-time information about TSC reliability

But the most valuable fact about TSC is the following: **modern hardware and operating
systems do tend to ensure that**:
1. TSC runs at the same pace on every CPU in the system
2. this pace doesn't change in time
3. there is no shift between TSCs running on different CPUs

WTMLIB is designed for such systems. It doesn't exploit deep hardware features of various
CPU architectures. Instead, it has a purely imperical focus:
1. WTMLIB allows experimentally verify reliability of TSC
2. it also allows exprimentally compute parameters required to efficiently convert TSC
ticks to nanoseconds
3. naturally, WTMLIB also provides convenient interfaces for reading TSC and converting
TSC ticks to nanoseconds on the fly

Let's first look at how WTMLIB allows assessing TSC reliability.

WTMLIB provides an interface for calculating two estimations:
1. maximum shift between time-stamp counters running on different CPUs available to the
process. The algorithm in outline is the following:
    - one of the CPUs is chosen as "base"
    - then the library iterates over all other CPUs and for each of them estimates a
      shift: `TSC_on_current_CPU - TSC_on_base_CPU`. The estimation is based on a fact
      that if `TSC_on_base_CPU_1`, `TSC_on_current_CPU`, `TSC_on_base_CPU_2` were
      successively measured, then the shift defined above must belong to the range
      `[TSC_on_current_CPU - TSC_on_base_CPU_2, TSC_on_current_CPU - TSC_on_base_CPU_1]`
      (assuming that TSCs run at the same pace on both CPUs)
    - when a shift relative to the base CPU is known for each available CPU, it is
      straightforward to calculate the maximum possible shift between time-stamp counters
      running on the available CPUs

    When a client has this estimation it can decide whether it finds the calculated
    maximum shift appropriate for its purporses.
2. whether TSC values measured successively on same/different CPUs monotonically increase.
The idea behind this estimation is the following. Assume several CPUs. Assume next that
they have synchronized clocks ticking at the same pace. If someone measures time on one
of those CPUs and then measures time again - on arbitrary of the CPUs - then the second
time value must be bigger than the first one.

    If `N` CPUs are available to the process, then clock synchronicity across the CPUs can
    be assessed in the following way:
    - measure TSC value on CPU_1
    - measure TSC value on CPU_2
    - ...
    - measure TSC value on CPU_N
    - measure TSC value on CPU_1 again
    - check whether the measured values monotonically increase

    This is a basic outline for how WTMLIB assesses clock synchronicity across the CPUs.

    Measuring the first and last values on the same CPU is really important here. Let's
    see why. Assume there are 3 CPUs. Assume next that TSC on CPU_2 is shifted by +100
    ticks relative to CPU_1. And TSC on CPU_3 is shifted by +100 ticks relative to CPU_2.
    Consider the following sequence of events:
    - get TSC on CPU_1. Let it be 10
    - 2 ticks passed
    - get TSC on CPU_2. It must be 112
    - 2 ticks passed
    - get TSC on CPU_3. It must be 214
    So far the clocks do look synchronized. But let's measure time on CPU_1 again:
    - 2 ticks passed
    - get TSC on CPU_1. It must be 16  
    Oooops! Monotonicity breaks. Thus, measuring first and last time values on the same
    CPU is important for detecting more or less big shifts between the clocks. Of course,
    the next question is "what does 'more or less big' mean"? Well, it depends on how much
    time passes between successive TSC measurements. In our example the measurements were
    separated by just 2 TSC ticks. Clock shifts bigger than 2 ticks will be detected.
    In general, clock shifts that are smaller than time that passes between successive
    measurements will not be detected.
    Thus, the "denser" measurements are the better. More on that below.

## License
Copyright © 2018 Andrey Nevolin, https://github.com/AndreyNevolin
 * Twitter: @Andrey_Nevolin
 * LinkedIn: https://www.linkedin.com/in/andrey-nevolin-76387328
  
This software is provided under the Apache 2.0 Software license provided in
the [LICENSE.md](LICENSE.md) file
