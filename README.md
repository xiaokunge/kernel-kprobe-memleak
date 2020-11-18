### Need kernel config  

```
CONFIG_HAVE_KPROBES=y
CONFIG_KPROBES=y
CONFIG_TRACEPOINTS=y
# [optional, for syscall tracepoint]
CONFIG_HAVE_SYSCALL_TRACEPOINTS=y
```

### For android detect kernel memleak (light weight and simple)
