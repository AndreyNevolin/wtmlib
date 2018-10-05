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

See file [example.c](example.c) for the detailed examples of using all the library
interfaces.

See [src/wtmlib.h](src/wtmlib.h) for the API signatures, parameter descriptions, error
codes, and so on.

## Further usage notes
1. When converting TSC ticks to nanoseconds on the fly, please make sure that
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
    You will see that WTMLIB will collect data only on CPUs 1, 7, and 13 (of course, if
    CPUs with these IDs do exist in your system).

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
2. it also allows experimentally compute parameters required to efficiently convert TSC
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
    - measure TSC value on CPU1
    - measure TSC value on CPU2
    - ...
    - measure TSC value on CPUN
    - measure TSC value on CPU1 again
    - check whether the measured values monotonically increase

    This is a basic outline for how WTMLIB assesses clock synchronicity across the CPUs.

    Measuring the first and last values on the same CPU is really important here. Let's
    see why. Assume there are 3 CPUs. Assume next that TSC on CPU2 is shifted by +100
    ticks relative to CPU1. And TSC on CPU3 is shifted by +100 ticks relative to CPU2.
    Consider the following sequence of events:
    - get TSC on CPU1. Let it be 10
    - 2 ticks passed
    - get TSC on CPU2. It must be 112
    - 2 ticks passed
    - get TSC on CPU3. It must be 214

    So far the clocks do look synchronized. But let's measure time on CPU1 again:
    - 2 ticks passed
    - get TSC on CPU1. It must be 16

    Oooops! Monotonicity breaks. Thus, measuring first and last time values on the same
    CPU is important for detecting more or less big shifts between the clocks. Of course,
    the next question is "what does 'more or less big' mean"? Well, it depends on how much
    time passes between successive TSC measurements. In our example the measurements were
    separated by just 2 TSC ticks. Clock shifts bigger than 2 ticks will be detected.
    In general, clock shifts that are smaller than time that passes between successive
    measurements will not be detected.
    Thus, the "denser" measurements are the better. More on that below.

While calculating TSC reliability metrics explained above, WTMLIB also makes several more
simple reliability checks, e.g:
- a limited check of whether TSC counters run at the same pace on different CPUs
- whether TSC counters do change in time and don't just stay constant

Accuracy of "maximum shift" and "TSC monotonicity" estimations produced using the
algorithms outlined above highly depends on how close to each other in time are TSC
measurements taken on different CPUs. The "denser" are the measurements,
- the lower is the "maximum shift" bound
- the more trusted is "monotonicity" check

WTMLIB actually provides two interfaces for evaluating TSC reliability. Both of them rely
on the methods outlined above. The both produce estimations of the same type. What differs
significantly is the method used to collect TSC data.

1. `wtmlib_EvalTSCReliabilityCPUSW()`

    "CPUSW" in the name of the interface stands for "CPU Switching". If one calls this
    function, then all the data required to produce TSC reliability estimations will be
    collected by a single thread jumping from one CPU to another.

    `wtmlib_EvalTSCReliabilityCPUSW()` was implemented mostly for fun. It is not
    recommended for production use, since the time needed to move a thread from one CPU
    to another is pretty big. Thus, too much time passes between successive TSC
    measurements, and the accuracy of the estimations is low.

    Some of the advantages of "CPU Switching" data collection method are:
    - it is absolutely deterministic. If one needs to successively read TSC values on CPUs
    CPU1, CPU2, CPU3, then he/she can easily do that (first switch to CPU1, read TSC, then
    switch to CPU2, read TSC there, and finally switch to CPU3 and read TSC there)
    - supposedly, time needed to switch between CPUs in the system must not grow too fast
    even if the number of CPUs grows rapidly. Theoretically, there may exist a system - a
    pretty big system - where the use of the method may be justified. But currently it is
    very unlikely
2. `wtmlib_EvalTSCReliabilityCOP()`

    "COP" in the name stands for "CAS-ordered probes". All the required data is collected
    by concurrently running threads. One thread per each available CPU. The measurements
    taken be the threads are sequentially ordered by means of compare-and-swap operation.

    `wtmlib_EvalTSCReliabilityCOP()` is preferred for evaluating TSC realiability. It
    gives pretty nice accuraccy.

    But the method of "CAS-ordered probes" does have a small disadvantage:
    - it is non-deterministic. It doesn't allow taking successive TSC measurements in a
    predefined CPU order. Instead, a long sequence of TSC values must be collected at
    random, and then the algorithms will try to find in this sequence the values measured
    on the appropriate CPUs.

    Threoretically, on enormously big systems, the method may give poor results because
    the contention between the CPUs will be very intense, and it will be hard to produce a
    TSC sequence with good statistical properties. But currently such situation is
    unlikely.

    WTMLIB assesses statistical quality of TSC sequences produced using the method of
    "CAS-ordered probes". It is possible to set an acceptable level of statistical
    significance. The library provides several pretty simple controls for that. Please,
    refer to file [src/wtmlib_config.h](src/wtmlib_config.h) where all the configuration
    parameters of the library live.

Now, when we discussed evaluation of TSC reliability, let's lalk a bit about the second
big purporse of the library: on-the-fly conversion of TSC ticks to nanoseconds. The
implemented method is borrowed from [fio](https://github.com/axboe/fio) and in outline is
the following:
1. TSC ticks that are to be converted to nanoseconds are split into two parts:  
    `tsc_ticks = k * modulus + remainder`  
    `modulus` is a number of ticks that corresponds to some pre-defined time period  
    `modulus` is chosen to be a power of 2, so that `k * modulus` could be extracted from
    `tsc_ticks` using a binary shift, and `remainder` could be extracted using a bitmask
2. `k * modulus` worth of the ticks is converted to nanoseconds using integer
multiplication. WTMLIB pre-computes the number of nanoseconds "sitting" inside `modulus`
ticks. Thus, to convert `k * modulus` ticks to nanoseconds one only needs to multiply this
pre-computed value by `k`
3. `remainder` is converted to nanoseconds using integer multiply-shift arithmetic and
several pre-computed values

Let's now see what the conversion procedure for remainder looks like. The same formula
is also used to pre-compute the nanosecond worth of `modulus` ticks.

We start with the following trivial formula: `ns_time = tsc_ticks / tsc_per_ns`.

We want to use integer arithmetic only. But `tsc_per_ns` is small. If, for example, it is
3.333 and we take its integer part - just 3 - the precision will suffer significantly.
Thus, we introduce a big "factor":
```
ns_time = (tsc_ticks * factor) / (ticks_per_ns * factor)
```
Or, after rearranging:
```
ns_time = (tsc_ticks * factor / ticks_per_ns) / factor
```
Next, we pre-compute `mult = factor / ticks_per_ns`, and the formula turns into:
```
ns_time = (tsc_ticks * mult) / factor
```
`factor` must satisfy three requirements:
1. it must be "big enough" to ensure good accuracy
2. multiplication `tsc_ticks * mult` must not result in 64-bit overflow, as long as
`tsc_ticks` is smaller then or equal to `modulus`
3. it must be a power of 2, so that slow integer division can be replaced by a fast binary
shift

This is how `factor` is chosen by WTMLIB. After that the nanosecond worth of `remainder`
can be calculated in the following way: `(remainder * mult) >> shift`, where `shift` is
such a number that `factor = 2 ^ shift`.

It only remains to note that instead of `ticks_per_ns` WTMLIB uses
`tsc_per_sec / 1000000000`. `tsc_per_sec` is obtained by means of direct measurements:
- WTMLIB measures how many TSC ticks pass during a "predefined time period"
- this "predefined time period" may be smaller than, equal to, or bigger than 1 second
- the "predefined time period" is tracked using system calls
- since the "predefined time period" may not be equal to 1 second, the measured number
of ticks is scaled to 1 second
- several `tsc_per_sec` values are measured in this way
- then the collected values get "cleaned" from statistical noise to produce a single
trusted value

That's it. This section gives mostly outline of the algorithms implemented in WTMLIB.
There is a lot of details, as always. Please, refer to the source code for the in-depth
explanations. There is A LOT of comments in the code. You'll find there not only the
detailed explanations of all the algorithms, but also some useful discussions. Besides,
in case you understand Russian, you may read this in-depth article about the library:
[https://habr.com/post/425237/](https://habr.com/post/425237/)

All the controls over the library (again, with the detailed explanations) can be found in
[src/wtmlib_config.h](src/wtmlib_config.h).

## License
Copyright © 2018 Andrey Nevolin, https://github.com/AndreyNevolin
 * Twitter: @Andrey_Nevolin
 * LinkedIn: https://www.linkedin.com/in/andrey-nevolin-76387328
  
This software is provided under the Apache 2.0 Software license provided in
the [LICENSE.md](LICENSE.md) file
