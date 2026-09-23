% bettertop(1) | User Commands
%
% 2026-09-23

# NAME

bettertop - monitor host resources, NVIDIA GPUs, and GPU processes

# SYNOPSIS

**bettertop** [**-c** _file_] [**-d**] [**-f** _filter_] [**-l**] [**-p** _id_] [**-t**] [**-u** _ms_] [**--classic**] [**--fun=cat|rocket**]

**bettertop** [**--default-config** | **--help** | **--version**]

# DESCRIPTION

BetterTop combines btop host and process monitoring with nvtop's NVIDIA GPU
metrics. Its default compact view shows host resource use, GPUs, and processes
using GPU memory. NVIDIA collection runs in a separate helper process; a slow
driver call can make GPU data stale without blocking the host view.

The `v` key switches between the compact view and the classic btop layout.
The `g` key toggles GPU-only and all-process rows. `x` cycles the optional
metric-driven cat and rocket footer modes.

# OPTIONS

**-c**, **--config _file_**
:   Read settings from _file_.

**-d**, **--debug**
:   Enable debug logging.

**-f**, **--filter _filter_**
:   Set an initial process filter.

**-l**, **--low-color**
:   Use 256 colors.

**-p**, **--preset _id_**
:   Start with preset 0-9.

**-t**, **--tty**
:   Force ANSI TTY mode and 16 colors.

**--no-tty**
:   Disable TTY mode.

**--classic**
:   Start with the classic btop layout.

**--fun=cat|rocket**
:   Start with an optional playful footer based on live GPU metrics.

**-u**, **--update _ms_**
:   Set the update interval in milliseconds.

**--default-config**
:   Print the default configuration.

**-h**, **--help**
:   Show command-line options.

**-V**, **--version**
:   Show version and build information.

# FILES

`~/.config/bettertop/bettertop.conf`
:   User configuration file.

# SEE ALSO

**top**(1), **htop**(1)
