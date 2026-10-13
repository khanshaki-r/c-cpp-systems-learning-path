# dump_stacks.gdb - non-interactive stack dump of a running or hung tcpkv (module 07).
#   gdb -batch -x scripts/dump_stacks.gdb -p $(pgrep -n tcpkv) > stacks.txt 2>&1
# Attaching pauses the process for the duration of the dump; it resumes when gdb detaches.
# Needs ptrace permission: run as the same user with kernel.yama.ptrace_scope=0, or with sudo.
set pagination off
set print thread-events off
set print pretty on
info threads
thread apply all bt full
detach
quit
