# Query or Change Font Settings

This VT extension can be used to query the current font settings of the connected terminal
or to change them.

Mind, there is a similar VT extension (OSC 50) introduced by xterm, which is inferior.

## Syntax: Query Font

```
OSC 60 ST
```

## Syntax: Query Font

```
OSC 60 ; size ; regular ; bold ; italic ; bold italic ST
```

