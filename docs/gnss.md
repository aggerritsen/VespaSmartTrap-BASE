# GNSS

The receiver enables GNSS with:

```text
AT+CGNSPWR=1
```

and samples position data with:

```text
AT+CGNSINF
```

POST samples for up to 10 seconds and exits early when a fix is available. No fix is a warning, not a boot failure.
