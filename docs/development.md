## Building the code

You will need to install the userspace RCU development libraries. The package
name varies on different distros, but is probably one of:

- liburcu-dev
- userspace-rcu-devel

To install from source, see [https://liburcu.org/](https://liburcu.org/)

To build:

```
$ make
```

## Running the code

ngnfs development current uses two userspace programs: the devd
server, which serves block read/write requests, and the debugfs
command, which issues requests to the devd server to create and read
file system structures. Use the following steps to run them:

Make a fake device file and start the devd server:

```
$ truncate /tmp/dev -s 1G
$ ./devd/ngnfs-devd --device_path /tmp/dev --listen_addr 127.0.0.1:8080 --trace /tmp/trace
```

Use the debugfs command to mkfs and stat the root inode:

```
$ ./cli/ngnfs-cli debugfs --devd_addr 127.0.0.1:8080 --trace /tmp/trace2
<1> $ mkfs
<1> $ stat
```

## Code Layout

**{cli,devd}/**

These directories contain the source for utility binaries.  They'll have
their own private source files and will link with all the shared code.

**shared/**

This is all the code that's shared by each utility.  It's not a proper
library in that it doesn't need to remain API compatible with external
builds over time.

There's two kinds of shared code.  There's code that can run in either
userspace or the kernel (block.c) and shared code that only runs in
userspace (options.c).  It'd probably be worth making this distinction
more apparent.

**shared/lk/**

This is for userspace implementations of kernel interfaces.  This both
lets us use reasonably stand-alone kernel interfaces (list.h) in
userspace as well as share ngnfs code with the kernel module by
providing implementations of more complicated runtime services (RCU hash
tables, work queues).

**shared/format-{block,msg,trace}.h**

These headers contain the structures and protocol constants that are
exposed to the world through storage on persistent media or by sending
over the network.
